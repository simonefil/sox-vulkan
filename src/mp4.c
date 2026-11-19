/* libSoX container format: MP4 (ISO base media file format), M4A and M4B.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Only the framing is done here; container.c binds it to the codec.
 *
 * Reading takes the one audio track.  Its sample table, in the moov box, is
 * read whole; the samples are then read in order, going forward by reading,
 * so that a pipe reads like a file as long as the moov box comes before the
 * media data.  A file whose moov comes after it can only be read where the
 * input can seek.  A fragmented file carries its samples in moof boxes,
 * each read as it comes.  The frames of signal are the edit list's, else
 * those of iTunes' iTunSMPB tag, else the sample table's, and each sample's
 * duration cuts off the padding of a short last packet.
 *
 * Writing produces one audio track.  To a file, the media data comes first
 * and is then moved up to make room for the moov box ahead of it.  To a
 * pipe, the file is fragmented: the moov box goes out with the first packet,
 * since some codecs are described only from it, and a moof box and its
 * media data follow for about every second. */

#include "container.h"
#include "ebml.h"

#include <stdio.h>
#include <string.h>

#define MAX_MOOV_SIZE ((size_t)256 << 20)
#define MAX_MOOF_SIZE ((size_t)64 << 20)
#define MAX_SAMPLE_SIZE ((sox_uint32_t)64 << 20)
#define MAX_SAMPLES ((size_t)1 << 26)
#define UNKNOWN_SIZE ((sox_uint64_t)-1)
#define SHIFT_CHUNK ((size_t)1 << 20)

typedef struct {
  unsigned char const * data;
  size_t size;
} span_t;

typedef struct {
  sox_uint64_t offset;   /* in the file, when reading */
  sox_uint32_t size, duration;
} sample_t;

typedef struct {
  sox_uint32_t id, timescale;
  sox_bool audio;
  span_t stsd, stts, stsz, stsc, stco, elst;
  sox_bool stz2, co64;
} track_t;

typedef struct {
  sox_uint64_t start, end; /* file positions; end is UNKNOWN_SIZE to the end of the file */
  char type[4];
} box_t;

typedef struct {
  sample_t * samples;
  size_t count, alloc, next;

  /* Reading */
  sox_uint32_t track_id, timescale;
  sox_bool fragmented;
  sox_uint32_t default_duration, default_size; /* from trex */
  sox_uint64_t box_end;          /* end of the top-level box being read through */
  sox_bool have_edit;
  sox_uint64_t edit_start, edit_duration;  /* media timescale; duration 0 if open */
  sox_bool have_smpb;
  sox_uint64_t smpb_priming, smpb_frames;
  sox_uint64_t table_duration;
  sox_rate_t rate;               /* the decoded rate, once known */
  unsigned char * packet;
  size_t packet_alloc;

  /* Writing */
  lsx_ebml_buffer_t buf, data;
  char const * tag_id;
  sox_uint32_t rate_out;
  sox_uint64_t mdat_pos, data_size, decode_time, fragment_frames;
  unsigned char * first;         /* the first packet, which some boxes describe */
  size_t first_size;
  sox_uint32_t sequence;
  sox_bool header_written;
} priv_t;

static sox_uint32_t rb16(unsigned char const * d)
{
  return (sox_uint32_t)d[0] << 8 | d[1];
}

static sox_uint32_t rb32(unsigned char const * d)
{
  return (sox_uint32_t)d[0] << 24 | (sox_uint32_t)d[1] << 16 | (sox_uint32_t)d[2] << 8 | d[3];
}

static sox_uint64_t rb64(unsigned char const * d)
{
  return (sox_uint64_t)rb32(d) << 32 | rb32(d + 4);
}

/* The next box among the children in s from *pos: 1 if there is one, its
 * content in body; 0 at the end or on a box that overruns its parent. */
static int next_box(span_t s, size_t * pos, char * type, span_t * body)
{
  size_t header = 8;
  sox_uint64_t size;

  if (s.size - *pos < 8)
    return 0;
  size = rb32(s.data + *pos);
  if (size == 1) {
    if (s.size - *pos < 16)
      return 0;
    size = rb64(s.data + *pos + 8);
    header = 16;
  }
  else if (size == 0)
    size = s.size - *pos;
  if (size < header || size > s.size - *pos)
    return 0;
  memcpy(type, s.data + *pos + 4, 4);
  body->data = s.data + *pos + header;
  body->size = (size_t)size - header;
  *pos += (size_t)size;
  return 1;
}

/* The first child of type in s, if any; an empty span if none. */
static sox_bool find_box(span_t s, char const * type, span_t * body)
{
  size_t pos = 0;
  char t[4];

  while (next_box(s, &pos, t, body))
    if (!memcmp(t, type, 4))
      return sox_true;
  body->data = NULL;
  body->size = 0;
  return sox_false;
}

/* Down a path of boxes such as "mdiaminf", four characters a level. */
static sox_bool find_path(span_t s, char const * path, span_t * body)
{
  for (; *path; path += 4, s = *body)
    if (!find_box(s, path, body))
      return sox_false;
  return sox_true;
}

/*--------------------------------- Reading ---------------------------------*/

/* The next top-level box: 1 if there is one, 0 at a clean end of file,
 * SOX_EOF on an error. */
static int read_box(sox_format_t * ft, box_t * box)
{
  unsigned char h[16];
  size_t got;
  sox_uint64_t size;

  box->start = (sox_uint64_t)lsx_tell(ft);
  if ((got = lsx_readbuf(ft, h, 8)) != 8) {
    if (!got && !ft->sox_errno)
      return 0;
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
    return SOX_EOF;
  }
  memcpy(box->type, h + 4, 4);
  size = rb32(h);
  if (size == 1) {
    if (lsx_readbuf(ft, h + 8, 8) != 8) {
      lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
      return SOX_EOF;
    }
    size = rb64(h + 8);
    if (size < 16) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid MP4 box size");
      return SOX_EOF;
    }
  }
  else if (size && size < 8) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid MP4 box size");
    return SOX_EOF;
  }
  box->end = size? box->start + size : UNKNOWN_SIZE;
  return 1;
}

/* Forward to pos, by reading where the input cannot seek. */
static int go_to(sox_format_t * ft, sox_uint64_t pos)
{
  sox_uint64_t here = (sox_uint64_t)lsx_tell(ft);

  if (pos == here)
    return SOX_SUCCESS;
  if (ft->seekable)
    return lsx_seeki(ft, (off_t)pos, SEEK_SET) == SOX_SUCCESS? SOX_SUCCESS : SOX_EOF;
  if (pos < here) {
    lsx_fail_errno(ft, SOX_EHDR, "MP4 samples out of order, which a pipe cannot be read in");
    return SOX_EOF;
  }
  return lsx_ebml_skip(ft, pos - here);
}

/* The rest of a box, read whole. */
static int read_body(sox_format_t * ft, box_t const * box, size_t limit, unsigned char * * data, size_t * size)
{
  sox_uint64_t here = (sox_uint64_t)lsx_tell(ft);

  if (box->end == UNKNOWN_SIZE || box->end - here > limit) {
    lsx_fail_errno(ft, SOX_EHDR, "MP4 `%.4s' box too large", box->type);
    return SOX_EOF;
  }
  *size = (size_t)(box->end - here);
  *data = lsx_malloc(max(*size, 1));
  if (lsx_readbuf(ft, *data, *size) != *size) {
    free(*data);
    *data = NULL;
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static void read_trak(span_t trak, track_t * t)
{
  span_t b;

  if (find_box(trak, "tkhd", &b) && b.size >= 24)
    t->id = rb32(b.data + (b.data[0] == 1? 20 : 12));
  if (find_path(trak, "mdiamdhd", &b) && b.size >= 24)
    t->timescale = rb32(b.data + (b.data[0] == 1? 20 : 12));
  t->audio = find_path(trak, "mdiahdlr", &b) && b.size >= 12 && !memcmp(b.data + 8, "soun", 4);
  find_path(trak, "edtselst", &t->elst);
  if (!find_path(trak, "mdiaminfstbl", &b))
    return;
  find_box(b, "stsd", &t->stsd);
  find_box(b, "stts", &t->stts);
  find_box(b, "stsc", &t->stsc);
  if (!find_box(b, "stsz", &t->stsz))
    t->stz2 = find_box(b, "stz2", &t->stsz);
  if (!find_box(b, "stco", &t->stco))
    t->co64 = find_box(b, "co64", &t->stco);
}

/* The size of the descriptor at d, and its tag and content. */
static size_t descriptor(unsigned char const * d, size_t n, unsigned * tag, span_t * content)
{
  size_t pos = 1, len = 0;
  int i;

  if (n < 2)
    return 0;
  *tag = d[0];
  for (i = 0; i < 4 && pos < n; ++i) {
    len = len << 7 | (d[pos] & 0x7f);
    if (!(d[pos++] & 0x80))
      break;
  }
  if (len > n - pos)
    return 0;
  content->data = d + pos;
  content->size = len;
  return pos + len;
}

/* From esds, the object type and the DecoderSpecificInfo. */
static sox_bool read_esds(span_t esds, unsigned * object_type, span_t * config)
{
  span_t es, dc, d;
  unsigned tag;
  size_t pos, n;

  config->size = 0;
  if (esds.size < 4 || !descriptor(esds.data + 4, esds.size - 4, &tag, &es) || tag != 3 || es.size < 3)
    return sox_false;
  pos = 3;
  if (es.data[2] & 0x80)
    pos += 2;
  if (es.data[2] & 0x40 && pos < es.size)
    pos += 1 + es.data[pos];
  if (es.data[2] & 0x20)
    pos += 2;
  if (pos >= es.size || !descriptor(es.data + pos, es.size - pos, &tag, &dc) || tag != 4 || dc.size < 13)
    return sox_false;
  *object_type = dc.data[0];
  for (pos = 13; pos < dc.size && (n = descriptor(dc.data + pos, dc.size - pos, &tag, &d)); pos += n)
    if (tag == 5) {
      *config = d;
      break;
    }
  return sox_true;
}

/* The codec boxes of a sample entry lie among its children, or in
 * QuickTime inside a wave box among them. */
static sox_bool find_codec_box(span_t children, char const * type, span_t * body)
{
  span_t wave;

  return find_box(children, type, body) ||
    (find_box(children, "wave", &wave) && find_box(wave, type, body));
}

/* The sample entry, as the codec identifier, parameters and private data:
 * Opus's dOps becomes the OpusHead its codec takes, and FLAC's dfLa the
 * stream header. */
static int read_entry(sox_format_t * ft, span_t stsd, lsx_codec_params_t * params, char * id, size_t id_size)
{
  span_t entry, children, b, config;
  size_t pos = 8, offset;
  unsigned version, object_type;
  char type[4];

  if (stsd.size < 8 || !next_box(stsd, &pos, type, &entry) || entry.size < 28) {
    lsx_fail_errno(ft, SOX_EHDR, "MP4 audio track has no sample description");
    return SOX_EOF;
  }
  if (rb32(stsd.data + 4) > 1) {
    lsx_fail_errno(ft, SOX_EFMT, "MP4 audio tracks with several sample descriptions are unsupported");
    return SOX_EOF;
  }
  version = rb16(entry.data + 8);
  params->channels = rb16(entry.data + 16);
  params->rate = !memcmp(type, "mlpa", 4)? rb32(entry.data + 24) : rb16(entry.data + 24);
  offset = version == 1? 44 : 28;
  if (version == 2 && entry.size >= 64) {
    double rate;
    sox_uint64_t bits = rb64(entry.data + 32);

    memcpy(&rate, &bits, sizeof(rate));
    params->rate = rate;
    params->channels = rb32(entry.data + 40);
    offset = 64;
  }
  children.data = entry.data + min(offset, entry.size);
  children.size = entry.size - min(offset, entry.size);
  snprintf(id, id_size, "%.4s", type);
  if (!memcmp(type, "mp4a", 4)) {
    if (!find_codec_box(children, "esds", &b) || !read_esds(b, &object_type, &config)) {
      lsx_fail_errno(ft, SOX_EHDR, "MP4 audio track has no elementary stream descriptor");
      return SOX_EOF;
    }
    snprintf(id, id_size, "mp4a.%02x", object_type);
    params->private_data = lsx_memdup(config.data, config.size);
    params->private_size = config.size;
  }
  else if (!memcmp(type, "alac", 4) && find_codec_box(children, "alac", &b) && b.size >= 4) {
    params->private_data = lsx_memdup(b.data + 4, b.size - 4);
    params->private_size = b.size - 4;
  }
  else if (!memcmp(type, "fLaC", 4) && find_codec_box(children, "dfLa", &b) && b.size >= 4) {
    params->private_size = b.size;
    params->private_data = lsx_malloc(b.size);
    memcpy(params->private_data, "fLaC", 4);
    memcpy(params->private_data + 4, b.data + 4, b.size - 4);
  }
  else if (!memcmp(type, "Opus", 4) && find_codec_box(children, "dOps", &b) && b.size >= 11) {
    unsigned char * head = params->private_data = lsx_malloc(8 + b.size);

    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = b.data[1];
    head[10] = b.data[3];
    head[11] = b.data[2];
    head[12] = b.data[7];
    head[13] = b.data[6];
    head[14] = b.data[5];
    head[15] = b.data[4];
    head[16] = b.data[9];
    head[17] = b.data[8];
    memcpy(head + 18, b.data + 10, b.size - 10);
    params->private_size = 8 + b.size;
  }
  return SOX_SUCCESS;
}

/* The track's samples from its sample table. */
static int read_sample_table(sox_format_t * ft, priv_t * p, track_t const * t)
{
  size_t count, i, chunks, entries, entry, field;
  sox_uint32_t constant;

  if (!t->stsz.data && !t->stco.data)
    return SOX_SUCCESS;
  if (t->stsz.size < 12 || t->stco.size < 8 || t->stsc.size < 8 || t->stts.size < 8)
    goto invalid;
  count = rb32(t->stsz.data + 8);
  constant = t->stz2? 0 : rb32(t->stsz.data + 4);
  field = t->stz2? t->stsz.data[7] : 32;
  if (count > MAX_SAMPLES || (field != 4 && field != 8 && field != 16 && field != 32) ||
      (!constant && (t->stsz.size - 12) * 8 / field < count))
    goto invalid;
  p->samples = lsx_calloc(max(count, 1), sizeof(*p->samples));
  p->count = p->alloc = count;
  for (i = 0; i < count; ++i) {
    unsigned char const * d = t->stsz.data + 12;

    p->samples[i].size = constant? constant : field == 32? rb32(d + 4 * i) : field == 16? rb16(d + 2 * i) :
        field == 8? d[i] : (d[i / 2] >> (i & 1? 0 : 4)) & 15;
  }

  entries = rb32(t->stts.data + 4);
  if ((t->stts.size - 8) / 8 < entries)
    goto invalid;
  for (i = entry = 0; entry < entries && i < count; ++entry) {
    sox_uint32_t n = rb32(t->stts.data + 8 + 8 * entry), delta = rb32(t->stts.data + 12 + 8 * entry);

    for (; n && i < count; --n, ++i) {
      p->samples[i].duration = delta;
      p->table_duration += delta;
    }
  }

  chunks = rb32(t->stco.data + 4);
  entries = rb32(t->stsc.data + 4);
  if ((t->stco.size - 8) / (t->co64? 8 : 4) < chunks || (t->stsc.size - 8) / 12 < entries)
    goto invalid;
  for (i = entry = 0; i < count && entry < entries; ++entry) {
    size_t chunk = rb32(t->stsc.data + 8 + 12 * entry) - 1;
    size_t end = entry + 1 < entries? rb32(t->stsc.data + 20 + 12 * entry) - 1 : chunks;
    sox_uint32_t per_chunk = rb32(t->stsc.data + 12 + 12 * entry);

    for (; chunk < min(end, chunks) && i < count; ++chunk) {
      sox_uint64_t offset = t->co64? rb64(t->stco.data + 8 + 8 * chunk) : rb32(t->stco.data + 8 + 4 * chunk);
      sox_uint32_t n;

      for (n = 0; n < per_chunk && i < count; ++n, ++i) {
        p->samples[i].offset = offset;
        offset += p->samples[i].size;
      }
    }
  }
  if (i < count)
    goto invalid;
  return SOX_SUCCESS;

invalid:
  lsx_fail_errno(ft, SOX_EHDR, "invalid MP4 sample table");
  return SOX_EOF;
}

/* The first edit that shows media, its media time and duration scaled from
 * the movie's timescale to the track's.  A movie timescale coarser than the
 * track's rounds the duration; where the sample table ends within that
 * rounding, its end is the exact one. */
static void read_edit_list(priv_t * p, span_t elst, sox_uint32_t movie_timescale)
{
  size_t i, count, width;

  if (elst.size < 8)
    return;
  width = elst.data[0] == 1? 8 : 4;
  count = rb32(elst.data + 4);
  for (i = 0; i < count && (elst.size - 8) / (2 * width + 4) > i; ++i) {
    unsigned char const * e = elst.data + 8 + i * (2 * width + 4);
    sox_uint64_t duration = width == 8? rb64(e) : rb32(e);
    sox_uint64_t time = width == 8? rb64(e + 8) : rb32(e + 4);

    if (width == 8? time == (sox_uint64_t)-1 : time == 0xffffffff)
      continue; /* an empty edit */
    p->have_edit = sox_true;
    p->edit_start = time;
    p->edit_duration = movie_timescale? (duration * p->timescale + movie_timescale / 2) / movie_timescale : 0;
    if (p->edit_duration && movie_timescale < p->timescale && p->table_duration > time) {
      sox_uint64_t rest = p->table_duration - time, tick = p->timescale / movie_timescale + 1;

      if (rest + tick >= p->edit_duration && rest <= p->edit_duration + tick)
        p->edit_duration = rest;
    }
    return;
  }
}

/* iTunes' gapless tag: priming, padding and frames of signal, counted in
 * the track's timescale. */
static void read_itunes_smpb(priv_t * p, span_t moov)
{
  span_t meta, ilst, item, b;
  size_t pos = 0;
  char type[4], text[200];
  unsigned priming, padding;
  sox_uint64_t frames;

  if (!find_path(moov, "udtameta", &meta))
    return;
  if (meta.size >= 8 && memcmp(meta.data + 4, "hdlr", 4)) { /* a full box, as in ISO */
    meta.data += 4;
    meta.size -= 4;
  }
  if (!find_box(meta, "ilst", &ilst))
    return;
  while (next_box(ilst, &pos, type, &item)) {
    if (memcmp(type, "----", 4) || !find_box(item, "name", &b) || b.size != 12 || memcmp(b.data + 4, "iTunSMPB", 8) ||
        !find_box(item, "data", &b) || b.size < 8)
      continue;
    memcpy(text, b.data + 8, min(b.size - 8, sizeof(text) - 1));
    text[min(b.size - 8, sizeof(text) - 1)] = '\0';
    if (sscanf(text, "%*x %x %x %" PRIx64, &priming, &padding, &frames) == 3) {
      p->have_smpb = sox_true;
      p->smpb_priming = priming;
      p->smpb_frames = frames;
    }
    return;
  }
}

/* The audio track to read, its sample table, and the defaults its
 * fragments take: the first whose codec SoX knows, or the first in the codec
 * the type names.  With several, the others are listed in a warning. */
static int read_moov(sox_format_t * ft, priv_t * p, span_t moov, lsx_codec_params_t * params, char const * * codec_name)
{
  char const * wanted = lsx_container_requested(ft->filetype);
  track_t * tracks = NULL, * audio = NULL;
  size_t count = 0, audio_count = 0, pos = 0, i;
  sox_uint32_t movie_timescale = 0;
  lsx_codec_tag_t const * tag = NULL;
  span_t b, mvex;
  char type[4], id[16], list[200] = "";
  int status = SOX_EOF;

  while (next_box(moov, &pos, type, &b)) {
    if (!memcmp(type, "mvhd", 4) && b.size >= 20)
      movie_timescale = rb32(b.data + (b.data[0] == 1? 20 : 12));
    else if (!memcmp(type, "trak", 4)) {
      tracks = lsx_realloc(tracks, (count + 1) * sizeof(*tracks));
      memset(&tracks[count], 0, sizeof(*tracks));
      read_trak(b, &tracks[count]);
      ++count;
    }
  }
  for (i = 0; i < count; ++i)
    if (tracks[i].audio) {
      lsx_codec_params_t entry = {0};
      lsx_codec_tag_t const * found;
      size_t len = strlen(list);

      if (read_entry(ft, tracks[i].stsd, &entry, id, sizeof(id)) != SOX_SUCCESS)
        goto end;
      found = lsx_codec_tag_by_config(lsx_container_mp4, id, entry.private_data, entry.private_size);
      snprintf(list + len, sizeof(list) - len, "%s#%lu %s %g Hz %u ch", len? "; " : "",
          (unsigned long)tracks[i].id, id, entry.rate, entry.channels);
      ++audio_count;
      if (!audio && found && (!wanted || !strcasecmp(found->codec, wanted))) {
        audio = &tracks[i];
        tag = found;
        *params = entry;
      }
      else free(entry.private_data);
    }
  if (!audio_count) {
    lsx_fail_errno(ft, SOX_EFMT, "no audio track");
    goto end;
  }
  if (!audio) {
    if (wanted)
      lsx_fail_errno(ft, SOX_EFMT, "no audio track in codec `%s': %s", wanted, list);
    else lsx_fail_errno(ft, SOX_EFMT, "no audio track in a supported codec: %s", list);
    goto end;
  }
  if (audio_count > 1)
    lsx_warn("%lu audio tracks, of which the %s one #%lu is read: %s", (unsigned long)audio_count,
        tag->codec, (unsigned long)audio->id, list);
  if (!audio->timescale) {
    lsx_fail_errno(ft, SOX_EHDR, "MP4 audio track has no timescale");
    goto end;
  }
  p->track_id = audio->id;
  p->timescale = audio->timescale;
  *codec_name = tag->codec;
  params->encoding = tag->encoding;
  if (!params->rate)
    params->rate = audio->timescale;
  read_itunes_smpb(p, moov);
  if (find_box(moov, "mvex", &mvex)) {
    p->fragmented = sox_true;
    for (pos = 0; next_box(mvex, &pos, type, &b); )
      if (!memcmp(type, "trex", 4) && b.size >= 24 && rb32(b.data + 4) == p->track_id) {
        p->default_duration = rb32(b.data + 12);
        p->default_size = rb32(b.data + 16);
      }
  }
  if ((status = read_sample_table(ft, p, audio)) == SOX_SUCCESS)
    read_edit_list(p, audio->elst, movie_timescale);

end:
  free(tracks);
  return status;
}

/* The samples of the audio track in a moof box starting at start. */
static int read_moof(sox_format_t * ft, priv_t * p, span_t moof, sox_uint64_t start)
{
  span_t traf, b;
  size_t pos = 0;
  char type[4];

  p->count = p->next = 0;
  while (next_box(moof, &pos, type, &traf)) {
    sox_uint32_t flags, duration = p->default_duration, size = p->default_size;
    sox_uint64_t base = start, offset;
    size_t tpos = 0, at;

    if (memcmp(type, "traf", 4) || !find_box(traf, "tfhd", &b) || b.size < 8 || rb32(b.data + 4) != p->track_id)
      continue;
    flags = rb32(b.data) & 0xffffff;
    at = 8;
    if (flags & 0x1) {
      if (b.size < at + 8)
        goto invalid;
      base = rb64(b.data + at);
      at += 8;
    }
    at += flags & 0x2? 4 : 0;
    if (flags & 0x8) {
      if (b.size < at + 4)
        goto invalid;
      duration = rb32(b.data + at);
      at += 4;
    }
    if (flags & 0x10) {
      if (b.size < at + 4)
        goto invalid;
      size = rb32(b.data + at);
    }
    offset = base;
    while (next_box(traf, &tpos, type, &b)) {
      sox_uint32_t n, i;
      size_t entry;

      if (memcmp(type, "trun", 4))
        continue;
      if (b.size < 8)
        goto invalid;
      flags = rb32(b.data) & 0xffffff;
      n = rb32(b.data + 4);
      at = 8;
      if (flags & 0x1) {
        if (b.size < at + 4)
          goto invalid;
        offset = base + (sox_uint64_t)(sox_int64_t)(sox_int32_t)rb32(b.data + at);
        at += 4;
      }
      at += flags & 0x4? 4 : 0;
      entry = 4 * (!!(flags & 0x100) + !!(flags & 0x200) + !!(flags & 0x400) + !!(flags & 0x800));
      if (at > b.size || (entry && (b.size - at) / entry < n) || p->count + n > MAX_SAMPLES)
        goto invalid;
      if (p->count + n > p->alloc) {
        p->alloc = max(p->count + n, 2 * p->alloc);
        p->samples = lsx_realloc(p->samples, p->alloc * sizeof(*p->samples));
      }
      for (i = 0; i < n; ++i) {
        sample_t * s = &p->samples[p->count++];

        s->duration = duration;
        s->size = size;
        if (flags & 0x100) {
          s->duration = rb32(b.data + at);
          at += 4;
        }
        if (flags & 0x200) {
          s->size = rb32(b.data + at);
          at += 4;
        }
        at += (flags & 0x400? 4 : 0) + (flags & 0x800? 4 : 0);
        s->offset = offset;
        offset += s->size;
      }
    }
  }
  return SOX_SUCCESS;

invalid:
  lsx_fail_errno(ft, SOX_EHDR, "invalid MP4 track fragment");
  return SOX_EOF;
}

/* On to the next moof box and into the media data after it. */
static int next_fragment(sox_format_t * ft, priv_t * p)
{
  box_t box;
  unsigned char * data;
  size_t size;
  int status;

  if (p->box_end != UNKNOWN_SIZE && p->box_end > (sox_uint64_t)lsx_tell(ft) && go_to(ft, p->box_end) != SOX_SUCCESS)
    return ft->sox_errno;
  do {
    if ((status = read_box(ft, &box)) != 1)
      return status? ft->sox_errno : SOX_EOF;
    if (memcmp(box.type, "moof", 4)) {
      if (box.end == UNKNOWN_SIZE)
        return SOX_EOF;
      if (go_to(ft, box.end) != SOX_SUCCESS)
        return ft->sox_errno;
    }
  } while (memcmp(box.type, "moof", 4));
  if (read_body(ft, &box, MAX_MOOF_SIZE, &data, &size) != SOX_SUCCESS)
    return ft->sox_errno;
  {
    span_t moof;

    moof.data = data;
    moof.size = size;
    status = read_moof(ft, p, moof, box.start);
  }
  free(data);
  if (status != SOX_SUCCESS)
    return ft->sox_errno;
  for (;;) {
    if ((status = read_box(ft, &box)) != 1) {
      if (!status)
        lsx_fail_errno(ft, SOX_EHDR, "MP4 fragment has no media data");
      return ft->sox_errno;
    }
    p->box_end = box.end;
    if (!memcmp(box.type, "mdat", 4))
      return SOX_SUCCESS;
    if (box.end == UNKNOWN_SIZE || go_to(ft, box.end) != SOX_SUCCESS) {
      if (!ft->sox_errno)
        lsx_fail_errno(ft, SOX_EHDR, "MP4 fragment has no media data");
      return ft->sox_errno;
    }
  }
}

/* The top-level boxes up to and through moov. */
static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  box_t box;
  int status;

  for (;;) {
    if ((status = read_box(ft, &box)) != 1) {
      if (!status)
        lsx_fail_errno(ft, SOX_EHDR, "MP4 file has no moov box");
      return SOX_EOF;
    }
    if (!memcmp(box.type, "moov", 4)) {
      unsigned char * data;
      span_t moov;

      if (read_body(ft, &box, MAX_MOOV_SIZE, &data, &moov.size) != SOX_SUCCESS)
        return SOX_EOF;
      moov.data = data;
      status = read_moov(ft, p, moov, params, codec_name);
      free(data);
      p->box_end = box.end;
      return status;
    }
    if (!memcmp(box.type, "mdat", 4) && !ft->seekable) {
      lsx_fail_errno(ft, SOX_EHDR, "the MP4 index (moov box) is at the end of the file, "
          "after the audio, and cannot be read from a pipe; read it from a file");
      return SOX_EOF;
    }
    if (box.end == UNKNOWN_SIZE) {
      lsx_fail_errno(ft, SOX_EHDR, "MP4 file has no moov box");
      return SOX_EOF;
    }
    if (go_to(ft, box.end) != SOX_SUCCESS)
      return SOX_EOF;
  }
}

/* frames of the track's timescale in frames of the decoded rate, or 0
 * where they do not come out whole. */
static size_t scale_frames(priv_t const * p, sox_uint64_t frames)
{
  sox_uint64_t rate = (sox_uint64_t)p->rate;

  return rate && (frames * rate) % p->timescale == 0? (size_t)(frames * rate / p->timescale) : 0;
}

static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec UNUSED, lsx_packet_t * packet)
{
  priv_t * p = v;
  sample_t const * s;

  while (p->next == p->count) {
    int status;

    if (!p->fragmented)
      return SOX_EOF;
    if ((status = next_fragment(ft, p)) != SOX_SUCCESS)
      return status;
  }
  s = &p->samples[p->next++];
  if (s->size > MAX_SAMPLE_SIZE) {
    lsx_fail_errno(ft, SOX_EHDR, "MP4 sample too large");
    return SOX_EOF;
  }
  if (go_to(ft, s->offset) != SOX_SUCCESS)
    return ft->sox_errno? ft->sox_errno : SOX_EOF;
  if (s->size > p->packet_alloc) {
    p->packet_alloc = s->size;
    p->packet = lsx_realloc(p->packet, p->packet_alloc);
  }
  if (lsx_readbuf(ft, p->packet, s->size) != s->size) {
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
    return ft->sox_errno;
  }
  packet->data = p->packet;
  packet->size = s->size;
  packet->frames = 0;
  packet->skip = 0;
  packet->discard = 0;
  packet->duration = scale_frames(p, s->duration);
  return SOX_SUCCESS;
}

/* Known once the first packet has settled the decoded rate, which for
 * HE-AAC is twice the track's timescale. */
static int length(sox_format_t * ft UNUSED, void * v, lsx_codec_t * codec, sox_uint64_t * frames)
{
  priv_t * p = v;
  double scale = codec->params.rate / p->timescale;

  p->rate = codec->params.rate;
  if (p->have_edit) {
    codec->params.delay = (sox_uint64_t)(p->edit_start * scale + .5);
    if (p->edit_duration) {
      *frames = (sox_uint64_t)(p->edit_duration * scale + .5);
      return SOX_SUCCESS;
    }
  }
  else if (p->have_smpb) {
    codec->params.delay = (sox_uint64_t)(p->smpb_priming * scale + .5);
    *frames = (sox_uint64_t)(p->smpb_frames * scale + .5);
    return SOX_SUCCESS;
  }
  if (!p->fragmented && p->table_duration) {
    sox_uint64_t total = (sox_uint64_t)(p->table_duration * scale + .5);

    *frames = total > codec->params.delay? total - codec->params.delay : 0;
  }
  return SOX_SUCCESS;
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  priv_t * p = v;

  free(p->samples);
  free(p->packet);
}

/*--------------------------------- Writing ---------------------------------*/

static void put_be(lsx_ebml_buffer_t * b, sox_uint64_t value, unsigned bytes)
{
  unsigned char d[8];
  unsigned i;

  for (i = 0; i < bytes; ++i)
    d[i] = (unsigned char)(value >> (8 * (bytes - 1 - i)));
  lsx_ebml_put_bytes(b, d, bytes);
}

static size_t open_box(lsx_ebml_buffer_t * b, char const * type)
{
  size_t start = b->size;

  put_be(b, 0, 4);
  lsx_ebml_put_bytes(b, type, 4);
  return start;
}

static size_t open_full_box(lsx_ebml_buffer_t * b, char const * type, unsigned version, sox_uint32_t flags)
{
  size_t start = open_box(b, type);

  put_be(b, (sox_uint64_t)version << 24 | flags, 4);
  return start;
}

static void close_box(lsx_ebml_buffer_t * b, size_t start)
{
  size_t size = b->size - start;

  b->data[start] = (unsigned char)(size >> 24);
  b->data[start + 1] = (unsigned char)(size >> 16);
  b->data[start + 2] = (unsigned char)(size >> 8);
  b->data[start + 3] = (unsigned char)size;
}

static void put_zeros(lsx_ebml_buffer_t * b, size_t n)
{
  while (n--)
    put_be(b, 0, 1);
}

static void put_matrix(lsx_ebml_buffer_t * b)
{
  static unsigned char const matrix[36] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0
  };

  lsx_ebml_put_bytes(b, matrix, sizeof(matrix));
}

/* A field of a 32-bit version 0 box, or of a 64-bit version 1 one. */
static void put_time(lsx_ebml_buffer_t * b, sox_uint64_t value, sox_bool wide)
{
  put_be(b, value, wide? 8 : 4);
}

typedef struct {
  unsigned char const * d;
  size_t size, pos; /* in bits */
} bits_t;

static unsigned get_bits(bits_t * b, unsigned n)
{
  unsigned value = 0;

  for (; n--; ++b->pos)
    value = value << 1 | (b->pos < b->size? (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0);
  return value;
}

/* What dac3 or dec3 says of a frame, and the frame's size. */
typedef struct {
  unsigned type, substream, fscod, bsid, bsmod, acmod, lfeon, bit_rate_code, chan_loc;
  sox_bool chan_map;
  size_t size;
  unsigned long data_rate;
} ac3_frame_t;

static sox_bool parse_ac3(unsigned char const * d, size_t n, ac3_frame_t * f)
{
  static unsigned const rates[3] = {48000, 44100, 32000};
  static unsigned const blocks[4] = {1, 2, 3, 6};
  bits_t b;
  unsigned i, frmsizecod, num_blocks = 6, rate;

  memset(f, 0, sizeof(*f));
  if (n < 8 || d[0] != 0x0b || d[1] != 0x77)
    return sox_false;
  b.d = d;
  b.size = n * 8;
  b.pos = 16;
  f->bsid = d[5] >> 3;
  if (f->bsid <= 10) {
    get_bits(&b, 16);
    f->fscod = get_bits(&b, 2);
    frmsizecod = get_bits(&b, 6);
    if (f->fscod == 3 || frmsizecod > 37)
      return sox_false;
    f->bit_rate_code = frmsizecod >> 1;
    get_bits(&b, 5);
    f->bsmod = get_bits(&b, 3);
    f->acmod = get_bits(&b, 3);
    if (f->acmod == 2)
      get_bits(&b, 2);
    else {
      if ((f->acmod & 1) && f->acmod != 1)
        get_bits(&b, 2);
      if (f->acmod & 4)
        get_bits(&b, 2);
    }
    f->lfeon = get_bits(&b, 1);
    return sox_true;
  }
  if (f->bsid > 16)
    return sox_false;
  f->type = get_bits(&b, 2);
  f->substream = get_bits(&b, 3);
  f->size = ((size_t)get_bits(&b, 11) + 1) * 2;
  f->fscod = get_bits(&b, 2);
  if (f->fscod == 3) {
    unsigned fscod2 = get_bits(&b, 2);

    if (fscod2 == 3)
      return sox_false;
    rate = rates[fscod2] / 2;
  }
  else {
    num_blocks = blocks[get_bits(&b, 2)];
    rate = rates[f->fscod];
  }
  f->acmod = get_bits(&b, 3);
  f->lfeon = get_bits(&b, 1);
  f->data_rate = (unsigned long)(8. * f->size * rate / (num_blocks * 256) / 1000);
  get_bits(&b, 5);
  for (i = 0; i < (f->acmod? 1u : 2u); ++i) {
    get_bits(&b, 5);
    if (get_bits(&b, 1))
      get_bits(&b, 8);
  }
  if (f->type == 1 && (f->chan_map = get_bits(&b, 1)))
    f->chan_loc = (get_bits(&b, 16) >> 5) & 0x1f;
  if (get_bits(&b, 1)) { /* mixing metadata */
    if (f->acmod > 2) {
      get_bits(&b, 2);
      if (f->acmod & 1)
        get_bits(&b, 6);
      if (f->acmod & 4)
        get_bits(&b, 6);
    }
    if (f->lfeon && get_bits(&b, 1))
      get_bits(&b, 5);
    if (f->type == 0) {
      for (i = 0; i < (f->acmod? 1u : 2u); ++i)
        if (get_bits(&b, 1))
          get_bits(&b, 6);
      if (get_bits(&b, 1))
        get_bits(&b, 6);
      switch (get_bits(&b, 2)) {
        case 1: get_bits(&b, 5); break;
        case 2: get_bits(&b, 12); break;
        case 3: b.pos += ((size_t)get_bits(&b, 5) + 2) * 8; break;
      }
      if (f->acmod < 2)
        for (i = 0; i < (f->acmod? 1u : 2u); ++i)
          if (get_bits(&b, 1))
            get_bits(&b, 14);
      if (get_bits(&b, 1))
        for (i = 0; i < num_blocks; ++i)
          if (num_blocks == 1 || get_bits(&b, 1))
            get_bits(&b, 5);
    }
  }
  if (get_bits(&b, 1)) /* informational metadata */
    f->bsmod = get_bits(&b, 3);
  return b.pos <= b.size;
}

/* dac3, or dec3 with the dependent substreams of the first frame. */
static sox_bool put_dolby_box(lsx_ebml_buffer_t * b, unsigned char const * d, size_t n, sox_bool eac3)
{
  ac3_frame_t f, dep;
  size_t box, pos;
  unsigned dependents = 0, chan_loc = 0;

  if (!parse_ac3(d, n, &f) || (f.bsid <= 10) == eac3)
    return sox_false;
  if (!eac3) {
    box = open_box(b, "dac3");
    put_be(b, (sox_uint64_t)f.fscod << 22 | f.bsid << 17 | f.bsmod << 14 | f.acmod << 11 |
        f.lfeon << 10 | f.bit_rate_code << 5, 3);
    close_box(b, box);
    return sox_true;
  }
  if (f.type == 1 || f.substream)
    return sox_false;
  for (pos = f.size; pos < n && parse_ac3(d + pos, n - pos, &dep) && dep.type == 1 && dep.size; pos += dep.size) {
    ++dependents;
    chan_loc |= dep.chan_map? dep.chan_loc : dep.acmod;
  }
  box = open_box(b, "dec3");
  put_be(b, (sox_uint64_t)min(f.data_rate, 8191) << 3, 2);
  if (dependents)
    put_be(b, (sox_uint64_t)f.fscod << 30 | f.bsid << 25 | f.bsmod << 20 | f.acmod << 17 |
        f.lfeon << 16 | min(dependents, 15) << 9 | chan_loc, 4);
  else put_be(b, (sox_uint64_t)f.fscod << 22 | f.bsid << 17 | f.bsmod << 12 | f.acmod << 9 | f.lfeon << 8, 3);
  close_box(b, box);
  return sox_true;
}

static void put_descriptor(lsx_ebml_buffer_t * b, unsigned tag, size_t size)
{
  put_be(b, tag, 1);
  put_be(b, 0x80808000 | (size >> 21 & 0x7f) << 24 | (size >> 14 & 0x7f) << 16 | (size >> 7 & 0x7f) << 8 |
      (size & 0x7f), 4);
}

/* The bit rates are left 0 where the samples are not all known. */
static void put_esds(priv_t const * p, lsx_ebml_buffer_t * b, unsigned object_type,
    lsx_codec_params_t const * params, sox_uint64_t duration)
{
  size_t box = open_full_box(b, "esds", 0, 0), config = params->private_size? 5 + params->private_size : 0, i;
  sox_uint64_t max_rate = 0, max_size = 0;

  for (i = 0; i < p->count; ++i) {
    max_size = max(max_size, p->samples[i].size);
    if (p->samples[i].duration)
      max_rate = max(max_rate, (sox_uint64_t)p->samples[i].size * 8 * p->rate_out / p->samples[i].duration);
  }
  put_descriptor(b, 3, 3 + 5 + 13 + config + 5 + 1);
  put_be(b, 1, 2); /* ES_ID */
  put_be(b, 0, 1);
  put_descriptor(b, 4, 13 + config);
  put_be(b, object_type, 1);
  put_be(b, 0x15, 1); /* an audio stream */
  put_be(b, max_size, 3);
  put_be(b, min(max_rate, 0xffffffff), 4);
  put_be(b, duration? min(p->data_size * 8 * p->rate_out / duration, 0xffffffff) : 0, 4);
  if (params->private_size) {
    put_descriptor(b, 5, params->private_size);
    lsx_ebml_put_bytes(b, params->private_data, params->private_size);
  }
  put_descriptor(b, 6, 1);
  put_be(b, 2, 1);
  close_box(b, box);
}

/* The sample entry; for a QuickTime-style sound description, a rate over
 * 16 bits is halved until it fits, its true value being in the codec's own
 * configuration, but TrueHD's takes it whole. */
static int put_sample_entry(sox_format_t * ft, priv_t const * p, lsx_ebml_buffer_t * b,
    lsx_codec_params_t const * params, sox_uint64_t duration)
{
  sox_bool mp4a = !strncmp(p->tag_id, "mp4a.", 5);
  size_t entry = open_box(b, mp4a? "mp4a" : p->tag_id), box;
  sox_uint32_t rate = p->rate_out;
  sox_bool lossless = !strcmp(p->tag_id, "alac") || !strcmp(p->tag_id, "fLaC");

  put_zeros(b, 6);
  put_be(b, 1, 2); /* data reference index */
  put_zeros(b, 8);
  put_be(b, params->channels, 2);
  put_be(b, lossless && params->bits_per_sample? params->bits_per_sample : 16, 2);
  put_zeros(b, 4);
  if (!strcmp(p->tag_id, "mlpa"))
    put_be(b, rate, 4);
  else {
    while (rate > 0xffff)
      rate >>= 1;
    put_be(b, (sox_uint64_t)rate << 16, 4);
  }
  if (mp4a)
    put_esds(p, b, (unsigned)strtoul(p->tag_id + 5, NULL, 16), params, duration);
  else if (!strcmp(p->tag_id, "alac")) {
    box = open_full_box(b, "alac", 0, 0);
    lsx_ebml_put_bytes(b, params->private_data, params->private_size);
    close_box(b, box);
  }
  else if (!strcmp(p->tag_id, "fLaC")) {
    box = open_full_box(b, "dfLa", 0, 0);
    if (params->private_size > 4)
      lsx_ebml_put_bytes(b, params->private_data + 4, params->private_size - 4);
    close_box(b, box);
  }
  else if (!strcmp(p->tag_id, "Opus")) {
    unsigned char const * h = params->private_data;

    if (params->private_size < 19) {
      lsx_fail_errno(ft, SOX_EFMT, "Opus private data holds no identification header");
      return SOX_EOF;
    }
    box = open_box(b, "dOps");
    put_be(b, 0, 1);
    put_be(b, h[9], 1);
    put_be(b, (sox_uint64_t)h[11] << 8 | h[10], 2);
    put_be(b, (sox_uint64_t)h[15] << 24 | (sox_uint64_t)h[14] << 16 | (sox_uint64_t)h[13] << 8 | h[12], 4);
    put_be(b, (sox_uint64_t)h[17] << 8 | h[16], 2);
    lsx_ebml_put_bytes(b, h + 18, params->private_size - 18);
    close_box(b, box);
  }
  else if (p->first && (!strcmp(p->tag_id, "ac-3") || !strcmp(p->tag_id, "ec-3"))) {
    if (!put_dolby_box(b, p->first, p->first_size, !strcmp(p->tag_id, "ec-3"))) {
      lsx_fail_errno(ft, SOX_EFMT, "invalid %s stream", p->tag_id[0] == 'a'? "AC-3" : "E-AC-3");
      return SOX_EOF;
    }
  }
  else if (p->first && !strcmp(p->tag_id, "mlpa")) {
    if (p->first_size < 20 || rb32(p->first + 4) != 0xf8726fba) {
      lsx_fail_errno(ft, SOX_EFMT, "invalid TrueHD stream");
      return SOX_EOF;
    }
    box = open_box(b, "dmlp");
    lsx_ebml_put_bytes(b, p->first + 8, 4); /* format_info */
    put_be(b, rb16(p->first + 18) << 1 & 0xffff, 2); /* peak_data_rate */
    put_zeros(b, 4);
    close_box(b, box);
  }
  close_box(b, entry);
  return SOX_SUCCESS;
}

/* moov, for samples at base on in the file, or for a fragmented file. */
static int put_moov(sox_format_t * ft, priv_t const * p, lsx_ebml_buffer_t * b,
    lsx_codec_params_t const * params, sox_uint64_t base, sox_bool fragmented)
{
  sox_uint64_t duration = 0, shown;
  sox_bool co64 = base + p->data_size > 0xffffffff;
  sox_bool wide;
  size_t moov, trak, mdia, minf, stbl, box, runs = 0, i, run;
  sox_bool constant = p->count > 0;

  for (i = 0; i < p->count; ++i) {
    duration += p->samples[i].duration;
    constant = constant && p->samples[i].size == p->samples[0].size;
    runs += !i || p->samples[i].duration != p->samples[i - 1].duration;
  }
  shown = duration > params->delay? duration - params->delay : 0;
  wide = duration > 0xffffffff;

  moov = open_box(b, "moov");
  box = open_full_box(b, "mvhd", wide, 0);
  put_time(b, 0, wide);
  put_time(b, 0, wide);
  put_be(b, p->rate_out, 4);
  put_time(b, shown, wide);
  put_be(b, 0x00010000, 4);
  put_be(b, 0x0100, 2);
  put_zeros(b, 10);
  put_matrix(b);
  put_zeros(b, 24);
  put_be(b, 2, 4); /* next track ID */
  close_box(b, box);

  trak = open_box(b, "trak");
  box = open_full_box(b, "tkhd", wide, 3);
  put_time(b, 0, wide);
  put_time(b, 0, wide);
  put_be(b, 1, 4); /* track ID */
  put_zeros(b, 4);
  put_time(b, shown, wide);
  put_zeros(b, 8 + 2 + 2);
  put_be(b, 0x0100, 2);
  put_zeros(b, 2);
  put_matrix(b);
  put_zeros(b, 8);
  close_box(b, box);
  if (params->delay) {
    size_t edts = open_box(b, "edts");

    box = open_full_box(b, "elst", wide, 0);
    put_be(b, 1, 4);
    put_time(b, shown, wide);
    put_time(b, params->delay, wide);
    put_be(b, 0x00010000, 4);
    close_box(b, box);
    close_box(b, edts);
  }
  mdia = open_box(b, "mdia");
  box = open_full_box(b, "mdhd", wide, 0);
  put_time(b, 0, wide);
  put_time(b, 0, wide);
  put_be(b, p->rate_out, 4);
  put_time(b, duration, wide);
  put_be(b, 0x55c4, 2); /* "und" */
  put_zeros(b, 2);
  close_box(b, box);
  box = open_full_box(b, "hdlr", 0, 0);
  put_zeros(b, 4);
  lsx_ebml_put_bytes(b, "soun", 4);
  put_zeros(b, 12);
  lsx_ebml_put_bytes(b, "SoundHandler", 13);
  close_box(b, box);
  minf = open_box(b, "minf");
  box = open_full_box(b, "smhd", 0, 0);
  put_zeros(b, 4);
  close_box(b, box);
  box = open_box(b, "dinf");
  i = open_full_box(b, "dref", 0, 0);
  put_be(b, 1, 4);
  close_box(b, open_full_box(b, "url ", 0, 1));
  close_box(b, i);
  close_box(b, box);
  stbl = open_box(b, "stbl");
  box = open_full_box(b, "stsd", 0, 0);
  put_be(b, 1, 4);
  if (put_sample_entry(ft, p, b, params, duration) != SOX_SUCCESS)
    return SOX_EOF;
  close_box(b, box);
  box = open_full_box(b, "stts", 0, 0);
  put_be(b, runs, 4);
  for (i = 0; i < p->count; i = run) {
    for (run = i + 1; run < p->count && p->samples[run].duration == p->samples[i].duration; ++run);
    put_be(b, run - i, 4);
    put_be(b, p->samples[i].duration, 4);
  }
  close_box(b, box);
  box = open_full_box(b, "stsc", 0, 0);
  put_be(b, p->count? 1 : 0, 4);
  if (p->count) {
    put_be(b, 1, 4);
    put_be(b, p->count, 4);
    put_be(b, 1, 4);
  }
  close_box(b, box);
  box = open_full_box(b, "stsz", 0, 0);
  put_be(b, constant? p->samples[0].size : 0, 4);
  put_be(b, p->count, 4);
  for (i = 0; !constant && i < p->count; ++i)
    put_be(b, p->samples[i].size, 4);
  close_box(b, box);
  box = open_full_box(b, co64? "co64" : "stco", 0, 0);
  put_be(b, p->count? 1 : 0, 4);
  if (p->count)
    put_be(b, base, co64? 8 : 4);
  close_box(b, box);
  if (p->count && !strcmp(p->tag_id, "mp4a.40")) {
    /* Every AAC frame needs the one before it to decode; Apple's readers
     * honour the edit list only where this says so. */
    box = open_full_box(b, "sgpd", 1, 0);
    lsx_ebml_put_bytes(b, "roll", 4);
    put_be(b, 2, 4); /* default length */
    put_be(b, 1, 4);
    put_be(b, 0xffff, 2); /* roll distance -1 */
    close_box(b, box);
    box = open_full_box(b, "sbgp", 0, 0);
    lsx_ebml_put_bytes(b, "roll", 4);
    put_be(b, 1, 4);
    put_be(b, p->count, 4);
    put_be(b, 1, 4);
    close_box(b, box);
  }
  close_box(b, stbl);
  close_box(b, minf);
  close_box(b, mdia);
  close_box(b, trak);
  if (fragmented) {
    size_t mvex = open_box(b, "mvex");

    box = open_full_box(b, "trex", 0, 0);
    put_be(b, 1, 4);
    put_be(b, 1, 4);
    put_zeros(b, 12);
    close_box(b, box);
    close_box(b, mvex);
  }
  close_box(b, moov);
  return SOX_SUCCESS;
}

static int flush(sox_format_t * ft, lsx_ebml_buffer_t * b)
{
  size_t size = b->size;

  b->size = 0;
  return lsx_writebuf(ft, b->data, size) == size? SOX_SUCCESS : SOX_EOF;
}

/* Brands as Apple writes them for M4A and M4B, and as ISO's otherwise. */
static int open_write(sox_format_t * ft, void * v, lsx_codec_t const * codec, lsx_codec_tag_t const * tag)
{
  priv_t * p = v;
  lsx_ebml_buffer_t * b = &p->buf;
  sox_bool apple = !strcmp(tag->codec, "aac") || !strcmp(tag->codec, "alac");
  sox_bool m4b = !strcasecmp(ft->filetype, "m4b") || !strncasecmp(ft->filetype, "m4b:", 4);
  size_t box;

  if (codec->params.rate != (sox_uint32_t)codec->params.rate || codec->params.rate < 1) {
    lsx_fail_errno(ft, SOX_EFMT, "MP4 needs a whole sample rate");
    return SOX_EOF;
  }
  p->rate_out = (sox_uint32_t)codec->params.rate;
  p->tag_id = tag->id;
  box = open_box(b, "ftyp");
  lsx_ebml_put_bytes(b, apple? m4b? "M4B " : "M4A " : "isom", 4);
  put_be(b, 0x200, 4);
  if (apple)
    lsx_ebml_put_bytes(b, m4b? "M4B " : "M4A ", 4);
  lsx_ebml_put_bytes(b, "isomiso2", 8);
  lsx_ebml_put_bytes(b, ft->seekable? "mp41" : "iso6", 4);
  close_box(b, box);
  if (ft->seekable) {
    p->mdat_pos = (sox_uint64_t)lsx_tell(ft) + b->size;
    put_be(b, 1, 4);
    lsx_ebml_put_bytes(b, "mdat", 4);
    put_be(b, 0, 8);
  }
  return flush(ft, b);
}

/* moof and its media data. */
static int write_fragment(sox_format_t * ft, priv_t * p)
{
  lsx_ebml_buffer_t * b = &p->buf;
  size_t moof, traf, box, offset, i;

  if (!p->count)
    return SOX_SUCCESS;
  moof = open_box(b, "moof");
  box = open_full_box(b, "mfhd", 0, 0);
  put_be(b, ++p->sequence, 4);
  close_box(b, box);
  traf = open_box(b, "traf");
  box = open_full_box(b, "tfhd", 0, 0x020000); /* the moof is the base */
  put_be(b, 1, 4);
  close_box(b, box);
  box = open_full_box(b, "tfdt", 1, 0);
  put_be(b, p->decode_time, 8);
  close_box(b, box);
  box = open_full_box(b, "trun", 0, 0x000301); /* data offset, durations, sizes */
  put_be(b, p->count, 4);
  offset = b->size;
  put_be(b, 0, 4);
  for (i = 0; i < p->count; ++i) {
    put_be(b, p->samples[i].duration, 4);
    put_be(b, p->samples[i].size, 4);
    p->decode_time += p->samples[i].duration;
  }
  close_box(b, box);
  close_box(b, traf);
  close_box(b, moof);
  box = b->size - moof + 8;
  b->data[offset] = (unsigned char)(box >> 24);
  b->data[offset + 1] = (unsigned char)(box >> 16);
  b->data[offset + 2] = (unsigned char)(box >> 8);
  b->data[offset + 3] = (unsigned char)box;
  put_be(b, 8 + p->data.size, 4);
  lsx_ebml_put_bytes(b, "mdat", 4);
  p->count = 0;
  p->fragment_frames = 0;
  if (flush(ft, b) != SOX_SUCCESS)
    return SOX_EOF;
  return flush(ft, &p->data);
}

static int write_packet(sox_format_t * ft, void * v, lsx_packet_t const * packet)
{
  priv_t * p = v;
  lsx_container_t const * c = ft->priv;
  sample_t * s;

  if (packet->size > MAX_SAMPLE_SIZE) {
    lsx_fail_errno(ft, SOX_EFMT, "packet too large for MP4");
    return SOX_EOF;
  }
  if (!p->first) {
    p->first = lsx_memdup(packet->data, max(packet->size, 1));
    p->first_size = packet->size;
  }
  if (!ft->seekable && !p->header_written) {
    if (put_moov(ft, p, &p->buf, &c->codec.params, 0, sox_true) != SOX_SUCCESS || flush(ft, &p->buf) != SOX_SUCCESS)
      return SOX_EOF;
    p->header_written = sox_true;
  }
  if (p->count == p->alloc) {
    p->alloc = max(256, 2 * p->alloc);
    p->samples = lsx_realloc(p->samples, p->alloc * sizeof(*p->samples));
  }
  s = &p->samples[p->count++];
  s->size = (sox_uint32_t)packet->size;
  s->duration = (sox_uint32_t)(packet->frames - min(packet->discard, packet->frames));
  p->data_size += packet->size;
  if (ft->seekable)
    return lsx_writebuf(ft, packet->data, packet->size) == packet->size? SOX_SUCCESS : SOX_EOF;
  lsx_ebml_put_bytes(&p->data, packet->data, packet->size);
  p->fragment_frames += s->duration;
  return p->fragment_frames >= p->rate_out? write_fragment(ft, p) : SOX_SUCCESS;
}

/* The media data moves up by the size of moov, from the end back, and moov
 * takes its place.  Standard output sent to a file can seek but not be read
 * back, and there moov follows the media data instead. */
static int finish_file(sox_format_t * ft, priv_t * p, lsx_codec_params_t const * params)
{
  lsx_ebml_buffer_t * b = &p->buf;
  sox_uint64_t data_start = p->mdat_pos + 16, end = data_start + p->data_size, pos;
  unsigned char * chunk;
  size_t moov_size, n;
  unsigned char size[8];
  int i;

  for (i = 0; i < 8; ++i)
    size[i] = (unsigned char)((16 + p->data_size) >> (8 * (7 - i)));
  if (lsx_seeki(ft, (off_t)(p->mdat_pos + 8), SEEK_SET) != SOX_SUCCESS || lsx_writebuf(ft, size, 8) != 8)
    return SOX_EOF;
  if (put_moov(ft, p, b, params, 0, sox_false) != SOX_SUCCESS)
    return SOX_EOF;
  moov_size = b->size;
  b->size = 0;
  put_moov(ft, p, b, params, data_start + moov_size, sox_false);
  if (b->size != moov_size) { /* the offsets went to 64 bits */
    moov_size = b->size;
    b->size = 0;
    put_moov(ft, p, b, params, data_start + moov_size, sox_false);
  }
  chunk = lsx_malloc(SHIFT_CHUNK);
  n = (size_t)min(SHIFT_CHUNK, end - p->mdat_pos);
  if (lsx_seeki(ft, (off_t)(end - n), SEEK_SET) != SOX_SUCCESS || lsx_readbuf(ft, chunk, n) != n) {
    free(chunk);
    clearerr((FILE *)ft->fp);
    ft->sox_errno = SOX_SUCCESS;
    b->size = 0;
    put_moov(ft, p, b, params, data_start, sox_false);
    return lsx_seeki(ft, (off_t)end, SEEK_SET) == SOX_SUCCESS? flush(ft, b) : SOX_EOF;
  }
  for (pos = end; ; ) {
    pos -= n;
    if (lsx_seeki(ft, (off_t)(pos + moov_size), SEEK_SET) != SOX_SUCCESS || lsx_writebuf(ft, chunk, n) != n)
      break;
    if (pos == p->mdat_pos) {
      free(chunk);
      return lsx_seeki(ft, (off_t)p->mdat_pos, SEEK_SET) == SOX_SUCCESS? flush(ft, b) : SOX_EOF;
    }
    n = (size_t)min(SHIFT_CHUNK, pos - p->mdat_pos);
    if (lsx_seeki(ft, (off_t)(pos - n), SEEK_SET) != SOX_SUCCESS || lsx_readbuf(ft, chunk, n) != n)
      break;
  }
  free(chunk);
  if (!ft->sox_errno)
    lsx_fail_errno(ft, SOX_EOF, "can't move the MP4 media data");
  return SOX_EOF;
}

static int close_write(sox_format_t * ft, void * v, lsx_codec_t const * codec)
{
  priv_t * p = v;
  int status;

  if (ft->seekable)
    status = finish_file(ft, p, &codec->params);
  else if (!p->header_written &&
      (put_moov(ft, p, &p->buf, &codec->params, 0, sox_true) != SOX_SUCCESS || flush(ft, &p->buf) != SOX_SUCCESS))
    status = SOX_EOF;
  else status = write_fragment(ft, p);
  free(p->buf.data);
  free(p->data.data);
  free(p->samples);
  free(p->first);
  return status;
}

static lsx_container_ops_t const mp4_ops = {
  sizeof(priv_t),
  open_read, read_packet, NULL, length, NULL, close_read,
  open_write, write_packet, close_write
};

static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &mp4_ops);
}

static int startwrite(sox_format_t * ft)
{
  return lsx_container_startwrite(ft, &mp4_ops);
}

LSX_FORMAT_HANDLER(mp4)
{
  static char const * const names[] = {"m4a", "mp4", "m4b", "xhe-m4a", "usac-m4a", NULL};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "MP4, M4A and M4B; the codec is given as in -t m4a:aac, ALAC if not, "
    "and -t xhe-m4a reads xHE-AAC only",
    names, SOX_FILE_CONTAINER | SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread, lsx_container_read, lsx_container_stopread,
    startwrite, lsx_container_write, lsx_container_stopwrite,
    NULL, NULL, NULL, sizeof(lsx_container_t)
  };
  return &handler;
}
