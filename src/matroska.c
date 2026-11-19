/* libSoX container format: Matroska audio, and WebM.
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

/* Only the framing is done here; container.c binds it to the codec.  WebM
 * is Matroska with fewer codecs and its own document type.
 *
 * Reading goes front to back without seeking, so that a pipe reads like a
 * file.  The Segment and its Clusters are not bounded but read through:
 * their children are taken in turn wherever they lie, which is what makes
 * an unknown size, as a live stream has, need no special case.  Everything
 * but Tracks and the blocks of the audio track is skipped.  Sample counts
 * come from the decoded packets, never from timestamps.
 *
 * Writing produces one audio track.  To a file, the Segment and Cluster
 * sizes, the Duration, the Cues and the SeekHead are filled in at the end,
 * as is the codec private data, for codecs that only know it then.  To a
 * pipe, Segment and Clusters are written with unknown sizes and there is
 * no index. */

#include "container.h"
#include "ebml.h"

#include <math.h>
#include <string.h>

#define MAX_STRING_SIZE 4096
#define MAX_PRIVATE_SIZE ((size_t)16 << 20)
#define MAX_BLOCK_SIZE ((size_t)64 << 20)

/* Written files count time in milliseconds, and cut a cluster after this
 * long or this much data, well inside the 16-bit block timestamps. */
#define TIMESTAMP_SCALE 1000000
#define CLUSTER_MS 5000
#define CLUSTER_BYTES ((sox_uint64_t)5 << 20)

/* Room kept at the start of a written segment for the SeekHead. */
#define SEEKHEAD_SPACE 128

typedef struct {
  sox_uint64_t number, type, channels, bit_depth, codec_delay;
  double rate;
  char * codec_id;
  char * name;
  unsigned char * private_data;
  size_t private_size;
  sox_bool encoded;
} track_t;

typedef struct {
  /* Reading */
  sox_uint64_t segment_end;     /* file position, or EBML_UNKNOWN_SIZE */
  sox_uint64_t track;           /* number of the track being read */
  sox_rate_t rate;
  unsigned char * block;
  size_t lace_sizes[256];
  unsigned laces, lace;
  size_t lace_pos, block_discard;

  /* Writing */
  lsx_ebml_buffer_t buf;
  sox_uint64_t segment_start;   /* file position of the Segment's content */
  sox_uint64_t info_pos, tracks_pos; /* relative to segment_start */
  sox_uint64_t duration_pos, private_pos;
  sox_bool cluster_open;
  sox_uint64_t cluster_size_pos, cluster_time, cluster_bytes;
  sox_uint64_t frames, discarded;
  struct {sox_uint64_t time, position;} * cues;
  size_t cues_count, cues_alloc;
} priv_t;

/*--------------------------------- Reading ---------------------------------*/

/* The next child of a master element ending at end: 1 if there is one, 0 at
 * the end, SOX_EOF on an error. */
static int next_child(sox_format_t * ft, sox_uint64_t end, sox_uint32_t * id, sox_uint64_t * size)
{
  sox_uint64_t pos = (sox_uint64_t)lsx_tell(ft);

  if (pos >= end)
    return 0;
  if (lsx_ebml_read_id(ft, id) != SOX_SUCCESS || lsx_ebml_read_size(ft, size) != SOX_SUCCESS) {
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
    return SOX_EOF;
  }
  pos = (sox_uint64_t)lsx_tell(ft);
  if (*size == EBML_UNKNOWN_SIZE || pos > end || *size > end - pos) {
    lsx_fail_errno(ft, SOX_EHDR, "EBML element 0x%X overruns its parent", (unsigned)*id);
    return SOX_EOF;
  }
  return 1;
}

/* The end of a master element whose size has just been read. */
static int master_end(sox_format_t * ft, sox_uint64_t size, sox_uint64_t * end)
{
  if (size == EBML_UNKNOWN_SIZE) {
    lsx_fail_errno(ft, SOX_EHDR, "EBML element of unknown size where one is not allowed");
    return SOX_EOF;
  }
  *end = (sox_uint64_t)lsx_tell(ft) + size;
  return SOX_SUCCESS;
}

static int read_ebml_header(sox_format_t * ft, sox_uint64_t size)
{
  sox_uint64_t end;
  sox_uint32_t id;
  unsigned char * doctype = NULL;
  int more;

  if (master_end(ft, size, &end) != SOX_SUCCESS)
    return SOX_EOF;
  while ((more = next_child(ft, end, &id, &size)) == 1) {
    if (id != EBML_ID_DOCTYPE) {
      if (lsx_ebml_skip(ft, size) != SOX_SUCCESS)
        break;
    }
    else if (lsx_ebml_read_binary(ft, size, MAX_STRING_SIZE, &doctype) != SOX_SUCCESS)
      break;
    else if (strcmp((char *)doctype, "matroska") && strcmp((char *)doctype, "webm")) {
      lsx_fail_errno(ft, SOX_EHDR, "EBML document type `%s' is neither Matroska nor WebM", doctype);
      break;
    }
  }
  free(doctype);
  return more? SOX_EOF : SOX_SUCCESS;
}

static int read_audio(sox_format_t * ft, sox_uint64_t size, track_t * t)
{
  sox_uint64_t end;
  sox_uint32_t id;
  int more, status = SOX_SUCCESS;

  if (master_end(ft, size, &end) != SOX_SUCCESS)
    return SOX_EOF;
  while (status == SOX_SUCCESS && (more = next_child(ft, end, &id, &size)) == 1)
    switch (id) {
      case MKV_ID_SAMPLING_FREQUENCY: status = lsx_ebml_read_float(ft, size, &t->rate); break;
      case MKV_ID_CHANNELS: status = lsx_ebml_read_uint(ft, size, &t->channels); break;
      case MKV_ID_BIT_DEPTH: status = lsx_ebml_read_uint(ft, size, &t->bit_depth); break;
      default: status = lsx_ebml_skip(ft, size);
    }
  return status == SOX_SUCCESS && !more? SOX_SUCCESS : SOX_EOF;
}

static int read_track_entry(sox_format_t * ft, sox_uint64_t size, track_t * t)
{
  sox_uint64_t end;
  sox_uint32_t id;
  int more, status = SOX_SUCCESS;

  /* The defaults Matroska gives elements that are left out. */
  t->rate = 8000;
  t->channels = 1;
  if (master_end(ft, size, &end) != SOX_SUCCESS)
    return SOX_EOF;
  while (status == SOX_SUCCESS && (more = next_child(ft, end, &id, &size)) == 1)
    switch (id) {
      case MKV_ID_TRACK_NUMBER: status = lsx_ebml_read_uint(ft, size, &t->number); break;
      case MKV_ID_TRACK_TYPE: status = lsx_ebml_read_uint(ft, size, &t->type); break;
      case MKV_ID_CODEC_DELAY: status = lsx_ebml_read_uint(ft, size, &t->codec_delay); break;
      case MKV_ID_CODEC_ID:
        status = lsx_ebml_read_binary(ft, size, MAX_STRING_SIZE, (unsigned char * *)&t->codec_id);
        break;
      case MKV_ID_NAME:
        status = lsx_ebml_read_binary(ft, size, MAX_STRING_SIZE, (unsigned char * *)&t->name);
        break;
      case MKV_ID_CODEC_PRIVATE:
        status = lsx_ebml_read_binary(ft, size, MAX_PRIVATE_SIZE, &t->private_data);
        t->private_size = (size_t)size;
        break;
      case MKV_ID_AUDIO: status = read_audio(ft, size, t); break;
      case MKV_ID_CONTENT_ENCODINGS:
        t->encoded = sox_true;
        status = lsx_ebml_skip(ft, size);
        break;
      default: status = lsx_ebml_skip(ft, size);
    }
  return status == SOX_SUCCESS && !more? SOX_SUCCESS : SOX_EOF;
}

/* The one audio track, as the parameters and codec to decode it with.  A
 * file with several is refused, listing them, until there is a rule for
 * choosing among them. */
static int choose_track(sox_format_t * ft, priv_t * p, track_t const * tracks, size_t count,
    lsx_codec_params_t * params, char const * * codec_name)
{
  track_t const * audio = NULL;
  lsx_codec_tag_t const * tag;
  size_t i, audio_count = 0;
  char list[200] = "";

  for (i = 0; i < count; ++i)
    if (tracks[i].type == MKV_TRACK_TYPE_AUDIO) {
      size_t len = strlen(list);

      audio = &tracks[i];
      snprintf(list + len, sizeof(list) - len, "%s#%lu %s %g Hz %lu ch%s%s%s", len? "; " : "",
          (unsigned long)audio->number, audio->codec_id? audio->codec_id : "?", audio->rate,
          (unsigned long)audio->channels, audio->name? " `" : "", audio->name? audio->name : "",
          audio->name? "'" : "");
      ++audio_count;
    }
  if (!audio_count) {
    lsx_fail_errno(ft, SOX_EFMT, "no audio track");
    return SOX_EOF;
  }
  if (audio_count > 1) {
    lsx_fail_errno(ft, SOX_EFMT, "%lu audio tracks, and choosing one is not supported: %s",
        (unsigned long)audio_count, list);
    return SOX_EOF;
  }
  if (!audio->codec_id || !(tag = lsx_codec_tag_by_config(lsx_container_matroska, audio->codec_id,
      audio->private_data, audio->private_size))) {
    lsx_fail_errno(ft, SOX_EFMT, "audio codec `%s' is not supported", audio->codec_id? audio->codec_id : "");
    return SOX_EOF;
  }
  if (audio->encoded) {
    lsx_fail_errno(ft, SOX_EFMT, "compressed or encrypted tracks are not supported");
    return SOX_EOF;
  }
  p->track = audio->number;
  p->rate = audio->rate;
  params->rate = audio->rate;
  params->channels = (unsigned)audio->channels;
  params->bits_per_sample = (unsigned)audio->bit_depth;
  /* 8-bit integer PCM is unsigned, whatever the byte order says. */
  params->encoding = tag->encoding == SOX_ENCODING_SIGN2 && audio->bit_depth == 8?
      SOX_ENCODING_UNSIGNED : tag->encoding;
  params->big_endian = tag->endian == lsx_endian_big;
  params->private_data = lsx_memdup(audio->private_data, audio->private_size);
  params->private_size = audio->private_size;
  params->delay = (sox_uint64_t)floor(audio->codec_delay * audio->rate / 1e9 + .5);
  *codec_name = tag->codec;
  return SOX_SUCCESS;
}

static int read_tracks(sox_format_t * ft, priv_t * p, sox_uint64_t size,
    lsx_codec_params_t * params, char const * * codec_name)
{
  track_t * tracks = NULL;
  size_t count = 0, i;
  sox_uint64_t end;
  sox_uint32_t id;
  int more = SOX_EOF, status = master_end(ft, size, &end);

  while (status == SOX_SUCCESS && (more = next_child(ft, end, &id, &size)) == 1)
    if (id == MKV_ID_TRACK_ENTRY) {
      tracks = lsx_realloc(tracks, (count + 1) * sizeof(*tracks));
      memset(&tracks[count], 0, sizeof(*tracks));
      status = read_track_entry(ft, size, &tracks[count++]);
    }
    else status = lsx_ebml_skip(ft, size);
  if (status == SOX_SUCCESS && !more)
    status = choose_track(ft, p, tracks, count, params, codec_name);
  else status = SOX_EOF;
  for (i = 0; i < count; ++i) {
    free(tracks[i].codec_id);
    free(tracks[i].name);
    free(tracks[i].private_data);
  }
  free(tracks);
  return status;
}

/* Header, Segment, and on to the first Cluster, which is entered.  A
 * segment that ends with no Cluster holds an empty track. */
static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  sox_uint32_t id;
  sox_uint64_t size;
  sox_bool have_track = sox_false;

  p->segment_end = EBML_UNKNOWN_SIZE;

  if (lsx_ebml_read_id(ft, &id) != SOX_SUCCESS || id != EBML_ID_HEADER) {
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "not a Matroska file");
    return SOX_EOF;
  }
  if (lsx_ebml_read_size(ft, &size) != SOX_SUCCESS || read_ebml_header(ft, size) != SOX_SUCCESS)
    return SOX_EOF;
  for (;;) {
    if (have_track && p->segment_end != EBML_UNKNOWN_SIZE && (sox_uint64_t)lsx_tell(ft) >= p->segment_end)
      return SOX_SUCCESS;
    if (lsx_ebml_read_id(ft, &id) != SOX_SUCCESS) {
      if (have_track && !ft->sox_errno)
        return SOX_SUCCESS;
      if (!ft->sox_errno)
        lsx_fail_errno(ft, SOX_EHDR, "no track list before the end of the file");
      return SOX_EOF;
    }
    if (lsx_ebml_read_size(ft, &size) != SOX_SUCCESS)
      return SOX_EOF;
    switch (id) {
      case MKV_ID_SEGMENT:
        p->segment_end = size == EBML_UNKNOWN_SIZE? size : (sox_uint64_t)lsx_tell(ft) + size;
        continue; /* its children are read in turn */
      case MKV_ID_TRACKS:
        if (!have_track) {
          if (read_tracks(ft, p, size, params, codec_name) != SOX_SUCCESS)
            return SOX_EOF;
          have_track = sox_true;
          continue;
        }
        break;
      case MKV_ID_CLUSTER:
        if (have_track)
          return SOX_SUCCESS;
        lsx_fail_errno(ft, SOX_EHDR, "audio data comes before the track list");
        return SOX_EOF;
    }
    if (lsx_ebml_skip(ft, size) != SOX_SUCCESS)
      return SOX_EOF;
  }
}

/* The block just read, split into its laced frames; laces stays 0 for a
 * block of another track. */
static int split_block(sox_format_t * ft, priv_t * p, size_t size)
{
  unsigned char const * b = p->block;
  sox_uint64_t track, sum = 0;
  size_t pos = lsx_ebml_parse_vint(b, size, &track);
  unsigned i, count;

  p->laces = p->lace = 0;
  if (!pos || pos + 3 > size) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Matroska block");
    return SOX_EOF;
  }
  if (track != p->track)
    return SOX_SUCCESS;
  pos += 3; /* timestamp and flags */
  if (!(b[pos - 1] & 0x06)) {
    p->lace_sizes[0] = size - pos;
    count = 1;
  }
  else {
    if (pos >= size)
      goto invalid;
    count = b[pos++] + 1u;
    switch (b[pos - 2] & 0x06) {
      case 0x02: /* Xiph lacing: sizes as runs of 255 ending in a smaller byte */
        for (i = 0; i + 1 < count; sum += p->lace_sizes[i++]) {
          p->lace_sizes[i] = 0;
          do {
            if (pos >= size)
              goto invalid;
            p->lace_sizes[i] += b[pos];
          } while (b[pos++] == 255);
        }
        break;
      case 0x06: /* EBML lacing: a size, then signed differences */
        for (i = 0; i + 1 < count; sum += p->lace_sizes[i++]) {
          sox_uint64_t value;
          size_t n = lsx_ebml_parse_vint(b + pos, size - pos, &value);

          if (!n)
            goto invalid;
          if (i)
            value = p->lace_sizes[i - 1] + value - ((1ull << (7 * n - 1)) - 1);
          if (value > size)
            goto invalid;
          p->lace_sizes[i] = (size_t)value;
          pos += n;
        }
        break;
      default: /* fixed-size lacing */
        if ((size - pos) % count)
          goto invalid;
        for (i = 0; i + 1 < count; sum += p->lace_sizes[i++])
          p->lace_sizes[i] = (size - pos) / count;
    }
    if (sum > size - pos)
      goto invalid;
    p->lace_sizes[count - 1] = size - pos - (size_t)sum;
  }
  p->lace_pos = pos;
  p->laces = count;
  return SOX_SUCCESS;

invalid:
  lsx_fail_errno(ft, SOX_EHDR, "invalid Matroska block lacing");
  return SOX_EOF;
}

static int read_block(sox_format_t * ft, priv_t * p, sox_uint64_t size)
{
  if (size == EBML_UNKNOWN_SIZE || lsx_ebml_read_binary(ft, size, MAX_BLOCK_SIZE, &p->block) != SOX_SUCCESS) {
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "invalid Matroska block");
    return SOX_EOF;
  }
  return split_block(ft, p, (size_t)size);
}

/* A BlockGroup is read whole, since its DiscardPadding may follow the Block. */
static int read_block_group(sox_format_t * ft, priv_t * p, sox_uint64_t size)
{
  sox_uint64_t end;
  sox_uint32_t id;
  sox_int64_t discard = 0;
  unsigned laces = 0;
  int more = SOX_EOF, status = master_end(ft, size, &end);

  while (status == SOX_SUCCESS && (more = next_child(ft, end, &id, &size)) == 1)
    switch (id) {
      case MKV_ID_BLOCK:
        status = read_block(ft, p, size);
        laces = p->laces;
        break;
      case MKV_ID_DISCARD_PADDING: status = lsx_ebml_read_sint(ft, size, &discard); break;
      default: status = lsx_ebml_skip(ft, size);
    }
  if (status != SOX_SUCCESS || more)
    return SOX_EOF;
  p->laces = laces;
  p->block_discard = discard > 0? (size_t)floor(discard * p->rate / 1e9 + .5) : 0;
  return SOX_SUCCESS;
}

/* On through the segment to the next block of the audio track. */
static int next_block(sox_format_t * ft, priv_t * p)
{
  for (;;) {
    sox_uint32_t id;
    sox_uint64_t size;
    int status;

    if (p->segment_end != EBML_UNKNOWN_SIZE && (sox_uint64_t)lsx_tell(ft) >= p->segment_end)
      return SOX_EOF;
    if (lsx_ebml_read_id(ft, &id) != SOX_SUCCESS)
      return ft->sox_errno? ft->sox_errno : SOX_EOF;
    if (lsx_ebml_read_size(ft, &size) != SOX_SUCCESS)
      return ft->sox_errno;
    switch (id) {
      case MKV_ID_CLUSTER:
        continue; /* its children are read in turn */
      case EBML_ID_HEADER:
        return SOX_EOF; /* a further segment, chained to this one */
      case MKV_ID_SIMPLE_BLOCK:
        status = read_block(ft, p, size);
        p->block_discard = 0;
        break;
      case MKV_ID_BLOCK_GROUP:
        status = read_block_group(ft, p, size);
        break;
      default:
        status = lsx_ebml_skip(ft, size);
    }
    if (status != SOX_SUCCESS)
      return ft->sox_errno;
    if (p->laces)
      return SOX_SUCCESS;
  }
}

static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec UNUSED, lsx_packet_t * packet)
{
  priv_t * p = v;

  while (p->lace == p->laces) {
    int status = next_block(ft, p);

    if (status != SOX_SUCCESS)
      return status;
  }
  packet->data = p->block + p->lace_pos;
  packet->size = p->lace_sizes[p->lace];
  packet->frames = 0;
  packet->skip = 0;
  packet->discard = ++p->lace == p->laces? p->block_discard : 0;
  packet->duration = 0;
  p->lace_pos += packet->size;
  return SOX_SUCCESS;
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  free(((priv_t *)v)->block);
}

/*--------------------------------- Writing ---------------------------------*/

static int flush(sox_format_t * ft, priv_t * p)
{
  size_t size = p->buf.size;

  p->buf.size = 0;
  return lsx_writebuf(ft, p->buf.data, size) == size? SOX_SUCCESS : SOX_EOF;
}

/* Overwrite what was written at pos, then go back to the end. */
static int patch(sox_format_t * ft, sox_uint64_t pos, void const * data, size_t size)
{
  if (lsx_seeki(ft, (off_t)pos, SEEK_SET) != SOX_SUCCESS || lsx_writebuf(ft, data, size) != size)
    return SOX_EOF;
  return lsx_seeki(ft, (off_t)0, SEEK_END);
}

static void be64(unsigned char * out, sox_uint64_t value)
{
  int i;

  for (i = 0; i < 8; ++i)
    out[i] = (unsigned char)(value >> (8 * (7 - i)));
}

static int open_write(sox_format_t * ft, void * v, lsx_codec_t const * codec,
    lsx_codec_tag_t const * tag)
{
  priv_t * p = v;
  lsx_codec_params_t const * params = &codec->params;
  lsx_ebml_buffer_t * b = &p->buf;
  sox_uint64_t start = (sox_uint64_t)lsx_tell(ft);
  size_t master, entry, audio;
  char app[64];

  master = lsx_ebml_open_master(b, EBML_ID_HEADER);
  lsx_ebml_put_uint(b, EBML_ID_VERSION, 1);
  lsx_ebml_put_uint(b, EBML_ID_READ_VERSION, 1);
  lsx_ebml_put_uint(b, EBML_ID_MAX_ID_LENGTH, 4);
  lsx_ebml_put_uint(b, EBML_ID_MAX_SIZE_LENGTH, 8);
  lsx_ebml_put_string(b, EBML_ID_DOCTYPE,
      lsx_container_family(ft->filetype) == lsx_container_webm? "webm" : "matroska");
  lsx_ebml_put_uint(b, EBML_ID_DOCTYPE_VERSION, 4);
  lsx_ebml_put_uint(b, EBML_ID_DOCTYPE_READ_VERSION, 2);
  lsx_ebml_close_master(b, master);

  lsx_ebml_put_id(b, MKV_ID_SEGMENT);
  lsx_ebml_put_size(b, ft->seekable? 0 : EBML_UNKNOWN_SIZE, 8);
  p->segment_start = start + b->size;
  if (ft->seekable)
    lsx_ebml_put_void(b, SEEKHEAD_SPACE);

  p->info_pos = start + b->size - p->segment_start;
  master = lsx_ebml_open_master(b, MKV_ID_INFO);
  lsx_ebml_put_uint(b, MKV_ID_TIMESTAMP_SCALE, TIMESTAMP_SCALE);
  sprintf(app, "SoX %.40s", sox_version());
  lsx_ebml_put_string(b, MKV_ID_MUXING_APP, app);
  lsx_ebml_put_string(b, MKV_ID_WRITING_APP, app);
  if (ft->seekable) {
    lsx_ebml_put_float(b, MKV_ID_DURATION, 0);
    p->duration_pos = start + b->size - 8;
  }
  lsx_ebml_close_master(b, master);

  p->tracks_pos = start + b->size - p->segment_start;
  master = lsx_ebml_open_master(b, MKV_ID_TRACKS);
  entry = lsx_ebml_open_master(b, MKV_ID_TRACK_ENTRY);
  lsx_ebml_put_uint(b, MKV_ID_TRACK_NUMBER, 1);
  lsx_ebml_put_uint(b, MKV_ID_TRACK_UID, 1);
  lsx_ebml_put_uint(b, MKV_ID_TRACK_TYPE, MKV_TRACK_TYPE_AUDIO);
  lsx_ebml_put_uint(b, MKV_ID_FLAG_LACING, 0);
  lsx_ebml_put_string(b, MKV_ID_LANGUAGE, "und");
  lsx_ebml_put_string(b, MKV_ID_CODEC_ID, tag->id);
  if (params->private_size) {
    lsx_ebml_put_binary(b, MKV_ID_CODEC_PRIVATE, params->private_data, params->private_size);
    p->private_pos = start + b->size - params->private_size;
  }
  if (params->delay)
    lsx_ebml_put_uint(b, MKV_ID_CODEC_DELAY, (sox_uint64_t)floor(params->delay * 1e9 / params->rate + .5));
  audio = lsx_ebml_open_master(b, MKV_ID_AUDIO);
  lsx_ebml_put_float(b, MKV_ID_SAMPLING_FREQUENCY, params->rate);
  lsx_ebml_put_uint(b, MKV_ID_CHANNELS, params->channels);
  if (params->bits_per_sample)
    lsx_ebml_put_uint(b, MKV_ID_BIT_DEPTH, params->bits_per_sample);
  lsx_ebml_close_master(b, audio);
  lsx_ebml_close_master(b, entry);
  lsx_ebml_close_master(b, master);
  return flush(ft, p);
}

/* On a file, the cluster's eight-byte size is written over its placeholder. */
static int close_cluster(sox_format_t * ft, priv_t * p)
{
  unsigned char size[8];

  if (!p->cluster_open || !ft->seekable)
    return SOX_SUCCESS;
  p->cluster_open = sox_false;
  be64(size, ((sox_uint64_t)lsx_tell(ft) - p->cluster_size_pos - 8) | 1ull << 56);
  return patch(ft, p->cluster_size_pos, size, sizeof(size));
}

static int open_cluster(sox_format_t * ft, priv_t * p, sox_uint64_t time)
{
  sox_uint64_t pos = (sox_uint64_t)lsx_tell(ft);

  if (close_cluster(ft, p) != SOX_SUCCESS)
    return SOX_EOF;
  if (ft->seekable) {
    if (p->cues_count == p->cues_alloc) {
      p->cues_alloc = max(16, p->cues_alloc * 2);
      p->cues = lsx_realloc(p->cues, p->cues_alloc * sizeof(*p->cues));
    }
    p->cues[p->cues_count].time = time;
    p->cues[p->cues_count++].position = pos - p->segment_start;
  }
  lsx_ebml_put_id(&p->buf, MKV_ID_CLUSTER);
  lsx_ebml_put_size(&p->buf, ft->seekable? 0 : EBML_UNKNOWN_SIZE, 8);
  lsx_ebml_put_uint(&p->buf, MKV_ID_TIMESTAMP, time);
  p->cluster_size_pos = pos + 4;
  p->cluster_open = sox_true;
  p->cluster_time = time;
  p->cluster_bytes = 0;
  return SOX_SUCCESS;
}

/* A packet with padding needs a BlockGroup to carry its DiscardPadding;
 * any other goes in a SimpleBlock. */
static int write_packet(sox_format_t * ft, void * v, lsx_packet_t const * packet)
{
  priv_t * p = v;
  lsx_ebml_buffer_t * b = &p->buf;
  sox_uint64_t time = (sox_uint64_t)floor(p->frames * 1000. / ft->signal.rate + .5);
  unsigned char header[4];
  size_t group = 0;

  if ((!p->cluster_open || time - p->cluster_time > CLUSTER_MS || p->cluster_bytes > CLUSTER_BYTES) &&
      open_cluster(ft, p, time) != SOX_SUCCESS)
    return SOX_EOF;
  header[0] = 0x81; /* track 1 */
  header[1] = (unsigned char)((time - p->cluster_time) >> 8);
  header[2] = (unsigned char)(time - p->cluster_time);
  header[3] = packet->discard? 0 : 0x80; /* a SimpleBlock is marked as a keyframe */
  if (packet->discard)
    group = lsx_ebml_open_master(b, MKV_ID_BLOCK_GROUP);
  lsx_ebml_put_id(b, packet->discard? MKV_ID_BLOCK : MKV_ID_SIMPLE_BLOCK);
  lsx_ebml_put_size(b, sizeof(header) + packet->size, 0);
  lsx_ebml_put_bytes(b, header, sizeof(header));
  lsx_ebml_put_bytes(b, packet->data, packet->size);
  if (packet->discard) {
    lsx_ebml_put_sint(b, MKV_ID_DISCARD_PADDING,
        (sox_int64_t)floor(packet->discard * 1e9 / ft->signal.rate + .5));
    lsx_ebml_close_master(b, group);
  }
  p->cluster_bytes += b->size;
  p->frames += packet->frames;
  p->discarded += packet->discard;
  return flush(ft, p);
}

static void put_seek(lsx_ebml_buffer_t * b, sox_uint32_t id, sox_uint64_t position)
{
  size_t seek = lsx_ebml_open_master(b, MKV_ID_SEEK);
  unsigned char bytes[4];

  bytes[0] = (unsigned char)(id >> 24);
  bytes[1] = (unsigned char)(id >> 16);
  bytes[2] = (unsigned char)(id >> 8);
  bytes[3] = (unsigned char)id;
  lsx_ebml_put_binary(b, MKV_ID_SEEK_ID, bytes, sizeof(bytes));
  lsx_ebml_put_uint(b, MKV_ID_SEEK_POSITION, position);
  lsx_ebml_close_master(b, seek);
}

/* Cues, then the sizes, SeekHead, Duration and private data left open. */
static int finish_file(sox_format_t * ft, priv_t * p, lsx_codec_t const * codec)
{
  lsx_ebml_buffer_t * b = &p->buf;
  sox_uint64_t cues_pos = (sox_uint64_t)lsx_tell(ft) - p->segment_start;
  unsigned char bytes[8];
  sox_uint64_t bits;
  double ms;
  size_t i, master;

  if (p->cues_count) {
    master = lsx_ebml_open_master(b, MKV_ID_CUES);
    for (i = 0; i < p->cues_count; ++i) {
      size_t point = lsx_ebml_open_master(b, MKV_ID_CUE_POINT);
      size_t positions;

      lsx_ebml_put_uint(b, MKV_ID_CUE_TIME, p->cues[i].time);
      positions = lsx_ebml_open_master(b, MKV_ID_CUE_TRACK_POSITIONS);
      lsx_ebml_put_uint(b, MKV_ID_CUE_TRACK, 1);
      lsx_ebml_put_uint(b, MKV_ID_CUE_CLUSTER_POSITION, p->cues[i].position);
      lsx_ebml_close_master(b, positions);
      lsx_ebml_close_master(b, point);
    }
    lsx_ebml_close_master(b, master);
    if (flush(ft, p) != SOX_SUCCESS)
      return SOX_EOF;
  }
  be64(bytes, ((sox_uint64_t)lsx_tell(ft) - p->segment_start) | 1ull << 56);
  if (patch(ft, p->segment_start - 8, bytes, sizeof(bytes)) != SOX_SUCCESS)
    return SOX_EOF;

  master = lsx_ebml_open_master(b, MKV_ID_SEEKHEAD);
  put_seek(b, MKV_ID_INFO, p->info_pos);
  put_seek(b, MKV_ID_TRACKS, p->tracks_pos);
  if (p->cues_count)
    put_seek(b, MKV_ID_CUES, cues_pos);
  lsx_ebml_close_master(b, master);
  lsx_ebml_put_void(b, SEEKHEAD_SPACE - b->size);
  if (patch(ft, p->segment_start, b->data, b->size) != SOX_SUCCESS)
    return SOX_EOF;
  b->size = 0;

  ms = (double)(p->frames - min(p->frames, codec->params.delay + p->discarded)) * 1000. / ft->signal.rate;
  memcpy(&bits, &ms, sizeof(bits));
  be64(bytes, bits);
  if (patch(ft, p->duration_pos, bytes, sizeof(bytes)) != SOX_SUCCESS)
    return SOX_EOF;
  return codec->params.private_size?
    patch(ft, p->private_pos, codec->params.private_data, codec->params.private_size) : SOX_SUCCESS;
}

static int close_write(sox_format_t * ft, void * v, lsx_codec_t const * codec)
{
  priv_t * p = v;
  int status = close_cluster(ft, p);

  if (status == SOX_SUCCESS && ft->seekable)
    status = finish_file(ft, p, codec);
  free(p->buf.data);
  free(p->cues);
  return status;
}

static lsx_container_ops_t const matroska_ops = {
  sizeof(priv_t),
  open_read, read_packet, NULL, NULL, NULL, close_read,
  open_write, write_packet, close_write
};

static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &matroska_ops);
}

static int startwrite(sox_format_t * ft)
{
  return lsx_container_startwrite(ft, &matroska_ops);
}

LSX_FORMAT_HANDLER(mka)
{
  static char const * const names[] = {"mka", "mkv", "webm", NULL};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Matroska audio; the codec is given as in -t mka:flac", names,
    SOX_FILE_CONTAINER | SOX_FILE_LIT_END,
    startread, lsx_container_read, lsx_container_stopread,
    startwrite, lsx_container_write, lsx_container_stopwrite,
    NULL, NULL, NULL, sizeof(lsx_container_t)
  };
  return &handler;
}
