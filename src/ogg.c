/* This library is free software; you can redistribute it and/or modify it
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

/* libSoX container format: Ogg, holding Vorbis, Opus or FLAC.
 *
 * Only the framing is done here; container.c binds it to the codec.
 *
 * An Ogg stream is pages of packets, and the logical streams it multiplexes
 * each begin with a page marked as the first, whose first packet names the
 * codec.  Of the streams that begin together, one audio stream in a codec
 * SoX has is read and the others are skipped.  Streams may be chained one
 * after another, each group beginning where the last has ended; the stream
 * read goes on into one in each group that follows, which must be in its
 * codec, at its rate and with its channels.  The header packets each codec
 * opens with are turned into the private data it takes in Matroska, and back
 * when writing.
 *
 * A page's granule position is where in the stream, in frames, its last
 * packet ends.  The frames each packet decodes to come from the codec, and
 * against the granule positions they tell where the stream starts and ends
 * short of its packets: on the first page, frames beyond its granule
 * position precede the signal, and on the last, frames beyond it are padding.
 *
 * On a file, the chained streams are found at the start by bisection, each
 * with its length from its last granule position, and a seek bisects the
 * stream it falls in for the last page that ends before it, then decodes
 * from there, dropping the frames up to it.
 *
 * Writing produces one logical stream: the header packets, flushed so that
 * the audio starts on a page of its own, then the audio packets, each page
 * given out as libogg completes it.  The serial number comes from SoX's
 * random numbers, so -R makes the output repeatable.  A FLAC stream's header
 * is rewritten at the end on a file, as Matroska rewrites it, with the sample
 * count and checksum. */

#include "container.h"

#include <string.h>

#include <ogg/ogg.h>

#define READ_SIZE 4096
#define END_SCAN 65536          /* what is read from the end at first for the length */
#define FLAC_MAPPING_SIZE 9     /* 0x7F, "FLAC", version, header packet count */
#define FLAC_HEADER_SIZE (4 + 4 + 34)
#define OPUS_PREROLL 3840       /* frames an Opus decoder takes to settle, from the specification */

typedef struct {
  size_t offset, size, frames, skip, discard;
} queued_t;

typedef struct {
  off_t start;               /* of the first pages of the streams that begin together */
  off_t headers_end;         /* the start of the last page holding header packets */
  off_t end;                 /* where the streams chained next begin */
  ogg_int64_t signal;        /* the granule position the signal starts at */
  sox_uint64_t first;        /* frame of the signal, counting from the first stream */
} link_t;

typedef struct {
  /* Reading */
  ogg_sync_state sync;
  off_t pos;                 /* where the bytes the sync holds start */
  ogg_stream_state stream;
  sox_bool stream_open, ended;
  long serial;
  long * serials;            /* of the streams that begin with the one read */
  size_t serials_count, serials_alloc;
  ogg_page pending;          /* a page read ahead, of the stream or the first of a chained one */
  off_t pending_start;
  sox_bool has_pending;
  off_t page_start;          /* of the page queued last */
  unsigned char * packets;   /* the packets completed on the last page, back to back */
  size_t packets_size, packets_alloc;
  queued_t * queue;
  size_t queued, queue_alloc, next;
  ogg_int64_t granule;       /* of the last page */
  sox_bool last_page, placed, started;
  ogg_int64_t position;      /* where the packets placed so far end */
  ogg_int64_t base;          /* where the packets decoded start, by the first page */
  ogg_int64_t origin;        /* where the signal is to start, at the least */
  ogg_int64_t to_skip;       /* frames still to drop to reach it */
  link_t * links;
  size_t links_count;

  /* Writing */
  ogg_stream_state out;
  unsigned char * held;      /* the last packet, until it is known whether it ends the stream */
  size_t held_size, held_alloc, held_frames, held_discard;
  sox_bool holding;
  ogg_int64_t packetno, written_granule;
  unsigned char * first_page;
  size_t first_page_size, header_len;
  sox_uint64_t first_page_pos;
} priv_t;

/*--------------------------------- Reading ---------------------------------*/

/* The next page and where it starts: 1, 0 at the end of the input, SOX_EOF
 * on an error. */
static int read_page(sox_format_t * ft, ogg_sync_state * sync, off_t * pos, ogg_page * page,
    off_t * start)
{
  long n;

  while ((n = ogg_sync_pageseek(sync, page)) <= 0) {
    char * buffer;
    size_t got;

    if (n < 0) {
      *pos -= n; /* bytes skipped to find a page */
      continue;
    }
    buffer = ogg_sync_buffer(sync, READ_SIZE);
    got = lsx_readbuf(ft, buffer, READ_SIZE);
    if (lsx_error(ft)) {
      lsx_fail_errno(ft, SOX_EHDR, "read error in Ogg stream");
      return SOX_EOF;
    }
    if (!got)
      return 0;
    ogg_sync_wrote(sync, (long)got);
  }
  *start = *pos;
  *pos += n;
  return 1;
}

/* Pages read from another place in the file. */
static int reposition(sox_format_t * ft, ogg_sync_state * sync, off_t * pos, off_t at)
{
  lsx_clearerr(ft);
  if (lsx_seeki(ft, at, SEEK_SET) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "can't seek in the Ogg file");
    return SOX_EOF;
  }
  ogg_sync_reset(sync);
  *pos = at;
  return SOX_SUCCESS;
}

static void keep_page(priv_t * p, ogg_page const * page, off_t start)
{
  p->pending = *page;
  p->pending.header = lsx_memdup(page->header, (size_t)page->header_len);
  p->pending.body = lsx_memdup(page->body, (size_t)page->body_len);
  p->pending_start = start;
  p->has_pending = sox_true;
}

static void drop_pending(priv_t * p)
{
  if (p->has_pending) {
    free(p->pending.header);
    free(p->pending.body);
    p->has_pending = sox_false;
  }
}

/* The packets completed on a page of the stream, queued apart from libogg,
 * which may move them on the next page. */
static void queue_page(priv_t * p, ogg_page * page, off_t start)
{
  ogg_packet op;
  int status;

  ogg_stream_pagein(&p->stream, page);
  p->queued = p->next = p->packets_size = 0;
  while ((status = ogg_stream_packetout(&p->stream, &op)) != 0) {
    if (status < 0) {
      lsx_warn("Warning: hole in stream; probably harmless");
      continue;
    }
    if (p->queued == p->queue_alloc) {
      p->queue_alloc = max(16, p->queue_alloc * 2);
      p->queue = lsx_realloc(p->queue, p->queue_alloc * sizeof(*p->queue));
    }
    if (p->packets_size + (size_t)op.bytes > p->packets_alloc) {
      p->packets_alloc = max(p->packets_size + (size_t)op.bytes, p->packets_alloc * 2);
      p->packets = lsx_realloc(p->packets, p->packets_alloc);
    }
    memcpy(p->packets + p->packets_size, op.packet, (size_t)op.bytes);
    p->queue[p->queued].offset = p->packets_size;
    p->queue[p->queued++].size = (size_t)op.bytes;
    p->packets_size += (size_t)op.bytes;
  }
  p->granule = ogg_page_granulepos(page);
  p->last_page = p->ended = ogg_page_eos(page) != 0;
  p->placed = sox_false;
  p->page_start = start;
}

/* The next page of the stream: 1, 0 at its end, SOX_EOF on an error.  The
 * first page of a chained stream also ends it, and is kept. */
static int next_page(sox_format_t * ft, priv_t * p)
{
  ogg_page page;
  off_t start;
  int status;

  if (p->ended)
    return 0;
  if (p->has_pending) {
    queue_page(p, &p->pending, p->pending_start);
    drop_pending(p);
    return 1;
  }
  while ((status = read_page(ft, &p->sync, &p->pos, &page, &start)) == 1) {
    if (ogg_page_bos(&page)) {
      keep_page(p, &page, start);
      p->ended = sox_true;
      return 0;
    }
    if (ogg_page_serialno(&page) == p->serial) {
      queue_page(p, &page, start);
      return 1;
    }
  }
  return status;
}

/* The streams that begin together each start with a page of their own,
 * read from page on; of them, the first in the codec wanted is
 * chosen, or failing that the first in a codec SoX has. */
static int select_stream(sox_format_t * ft, priv_t * p, ogg_page * page, off_t start,
    char const * wanted, lsx_codec_tag_t const * * tag)
{
  ogg_page chosen;
  off_t chosen_start = 0;
  char list[200] = "";
  size_t count = 0;
  int status = 1;

  *tag = NULL;
  p->serials_count = 0;
  memset(&chosen, 0, sizeof(chosen));
  while (status == 1 && ogg_page_bos(page)) {
    ogg_stream_state stream;
    ogg_packet op;
    lsx_codec_tag_t const * found = NULL;

    if (p->serials_count == p->serials_alloc) {
      p->serials_alloc = max(4, p->serials_alloc * 2);
      p->serials = lsx_realloc(p->serials, p->serials_alloc * sizeof(*p->serials));
    }
    p->serials[p->serials_count++] = ogg_page_serialno(page);
    ogg_stream_init(&stream, ogg_page_serialno(page));
    ogg_stream_pagein(&stream, page);
    if (ogg_stream_packetout(&stream, &op) == 1)
      found = lsx_codec_tag_by_magic(lsx_container_ogg, op.packet, (size_t)op.bytes);
    ogg_stream_clear(&stream);
    if (found) {
      snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s#%lx %s", count++? "; " : "",
          (unsigned long)(sox_uint32_t)ogg_page_serialno(page), found->codec);
      if (!*tag || (wanted && strcasecmp((*tag)->codec, wanted) && !strcasecmp(found->codec, wanted))) {
        *tag = found;
        free(chosen.header);
        free(chosen.body);
        chosen = *page;
        chosen.header = lsx_memdup(page->header, (size_t)page->header_len);
        chosen.body = lsx_memdup(page->body, (size_t)page->body_len);
        chosen_start = start;
      }
    }
    status = read_page(ft, &p->sync, &p->pos, page, &start);
  }
  if (status != SOX_EOF && !*tag)
    lsx_fail_errno(ft, SOX_EFMT, "no Vorbis, Opus or FLAC stream in the Ogg input");
  if (status == SOX_EOF || !*tag)
    return SOX_EOF;
  if (count > 1 && !p->stream_open) /* once, at the start */
    lsx_warn("%lu audio streams, of which the %s one #%lx is read: %s", (unsigned long)count,
        (*tag)->codec, (unsigned long)(sox_uint32_t)ogg_page_serialno(&chosen), list);
  p->serial = ogg_page_serialno(&chosen);
  if (p->stream_open)
    ogg_stream_reset_serialno(&p->stream, (int)p->serial);
  else ogg_stream_init(&p->stream, (int)p->serial);
  p->stream_open = sox_true;
  queue_page(p, &chosen, chosen_start);
  free(chosen.header);
  free(chosen.body);
  /* The page that ended the first pages may belong to the chosen stream,
   * and is kept until the packets of its first page are taken. */
  if (status == 1 && ogg_page_serialno(page) == p->serial)
    keep_page(p, page, start);
  return SOX_SUCCESS;
}

/* The header packets as the codec takes them: Vorbis's three laced Xiph
 * fashion, Opus's identification header alone with its comments read here,
 * FLAC's stream marker and metadata blocks out of their Ogg mapping. */
static void read_comments(sox_format_t * ft, unsigned char const * data, size_t size)
{
  size_t pos = 8, count;

  if (size < pos + 4)
    return;
  pos += 4 + (data[pos] | (size_t)data[pos + 1] << 8 | (size_t)data[pos + 2] << 16 | (size_t)data[pos + 3] << 24);
  if (pos + 4 > size)
    return;
  count = data[pos] | (size_t)data[pos + 1] << 8 | (size_t)data[pos + 2] << 16 | (size_t)data[pos + 3] << 24;
  for (pos += 4; count-- && pos + 4 <= size; ) {
    size_t len = data[pos] | (size_t)data[pos + 1] << 8 | (size_t)data[pos + 2] << 16 | (size_t)data[pos + 3] << 24;
    char * comment;

    if ((pos += 4) + len > size || len > size)
      break;
    comment = lsx_malloc(len + 1);
    memcpy(comment, data + pos, len);
    comment[len] = '\0';
    sox_append_comment(&ft->oob.comments, comment);
    free(comment);
    pos += len;
  }
}

static void append(lsx_codec_params_t * params, void const * data, size_t size)
{
  params->private_data = lsx_realloc(params->private_data, params->private_size + size);
  memcpy(params->private_data + params->private_size, data, size);
  params->private_size += size;
}

/* Vorbis's header packets, laced into the private data. */
static void lace(lsx_codec_params_t * params, unsigned char * const * headers, size_t const * sizes)
{
  unsigned char byte = 2;
  size_t i, n;

  append(params, &byte, 1);
  for (i = 0; i < 2; ++i) {
    for (n = sizes[i], byte = 255; n >= 255; n -= 255)
      append(params, &byte, 1);
    byte = (unsigned char)n;
    append(params, &byte, 1);
  }
  for (i = 0; i < 3; ++i)
    append(params, headers[i], sizes[i]);
}

/* Header packets go into the private data until the codec has them all, and
 * any packets after them on the same page stay queued as audio.  FLAC gives
 * the count of its header packets, or none, and then the last metadata block
 * is marked as such.  Comments are the file's only from the first stream. */
static int read_headers(sox_format_t * ft, priv_t * p, lsx_codec_tag_t const * tag,
    lsx_codec_params_t * params, sox_bool comments)
{
  unsigned char const * first = p->packets + p->queue[0].offset;
  sox_bool vorbis = !strcmp(tag->codec, "vorbis"), opus = !strcmp(tag->codec, "opus");
  sox_bool done = sox_false;
  size_t count = 0, wanted = vorbis? 3 : opus? 2 : 0, i;
  unsigned char * headers[3];
  size_t sizes[3];
  int status = SOX_SUCCESS;

  if (!p->queued) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Ogg stream");
    return SOX_EOF;
  }
  if (!vorbis && !opus) {
    if (p->queue[0].size < FLAC_MAPPING_SIZE + FLAC_HEADER_SIZE || first[5] != 1) {
      lsx_fail_errno(ft, SOX_EHDR, "unsupported Ogg FLAC mapping");
      return SOX_EOF;
    }
    wanted = (first[7] << 8 | first[8])? 1 + (first[7] << 8 | first[8]) : 0;
  }
  while (!done) {
    if (p->next == p->queued && next_page(ft, p) != 1) {
      if (!ft->sox_errno)
        lsx_fail_errno(ft, SOX_EHDR, "Ogg stream ends within its headers");
      status = SOX_EOF;
      break;
    }
    for (; !done && p->next < p->queued; ++count) {
      unsigned char const * data = p->packets + p->queue[p->next].offset;
      size_t size = p->queue[p->next++].size;

      if (vorbis) {
        headers[count] = lsx_memdup(data, size);
        sizes[count] = size;
      }
      else if (opus) {
        if (!count)
          append(params, data, size);
        else if (comments)
          read_comments(ft, data, size);
      }
      else {
        if (!count) {
          data += FLAC_MAPPING_SIZE;
          size -= FLAC_MAPPING_SIZE;
        }
        append(params, data, size);
        /* the last-block flag, of STREAMINFO in the first */
        done = !wanted && size > (count? 0u : 4u) && (data[count? 0 : 4] & 0x80);
      }
      done |= count + 1 == wanted;
    }
  }
  if (vorbis) {
    if (status == SOX_SUCCESS)
      lace(params, headers, sizes);
    for (i = 0; i < count; ++i)
      free(headers[i]);
  }
  return status;
}

/* A chained stream, from its first page on, read up to its first audio
 * packet; it must be in the codec of the stream it follows. */
static int read_chained(sox_format_t * ft, priv_t * p, ogg_page * page, off_t start,
    char const * codec_name, lsx_codec_params_t * params)
{
  lsx_codec_tag_t const * tag;

  p->ended = p->started = sox_false;
  p->origin = 0;
  if (select_stream(ft, p, page, start, codec_name, &tag) != SOX_SUCCESS)
    return SOX_EOF;
  if (strcasecmp(tag->codec, codec_name)) {
    lsx_fail_errno(ft, SOX_EFMT, "chained Ogg streams in different codecs are unsupported");
    return SOX_EOF;
  }
  if (read_headers(ft, p, tag, params, sox_false) != SOX_SUCCESS) {
    free(params->private_data);
    params->private_data = NULL;
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Back to the streams that begin at start, read again up to the first audio
 * packet of the one read. */
static int rewind_to(sox_format_t * ft, priv_t * p, off_t start, char const * codec_name,
    lsx_codec_params_t * params)
{
  ogg_page page;
  off_t at;
  int status;

  memset(params, 0, sizeof(*params));
  drop_pending(p);
  if (reposition(ft, &p->sync, &p->pos, start) != SOX_SUCCESS)
    return SOX_EOF;
  if ((status = read_page(ft, &p->sync, &p->pos, &page, &at)) != 1) {
    if (!status)
      lsx_fail_errno(ft, SOX_EHDR, "Ogg stream not found again");
    return SOX_EOF;
  }
  return read_chained(ft, p, &page, at, codec_name, params);
}

static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  lsx_codec_tag_t const * tag;
  ogg_page page;
  off_t start;
  int status;

  ogg_sync_init(&p->sync);
  if ((status = read_page(ft, &p->sync, &p->pos, &page, &start)) != 1) {
    if (!status)
      lsx_fail_errno(ft, SOX_EFMT, "no Vorbis, Opus or FLAC stream in the Ogg input");
    return SOX_EOF;
  }
  if (select_stream(ft, p, &page, start, lsx_container_requested(ft->filetype), &tag) != SOX_SUCCESS ||
      read_headers(ft, p, tag, params, sox_true) != SOX_SUCCESS)
    return SOX_EOF;
  *codec_name = tag->codec;
  return SOX_SUCCESS;
}

/* Spread over a page's packets, from the first for frames that precede
 * where the signal is to start and from the last for padding.  A stream of
 * one page starts at 0, its granule position marking where it ends. */
static void place_page(priv_t * p, lsx_codec_t * codec)
{
  ogg_int64_t frames = 0, excess;
  size_t i;

  for (i = p->next; i < p->queued; ++i) {
    queued_t * q = &p->queue[i];

    q->frames = codec->handler->packet_frames(codec, p->packets + q->offset, q->size);
    q->skip = q->discard = 0;
    frames += (ogg_int64_t)q->frames;
  }
  p->placed = sox_true;
  if (p->granule < 0) {
    p->position += frames;
    return;
  }
  if (!p->started) {
    p->base = p->last_page? max(p->granule - frames, 0) : p->granule - frames;
    p->position = p->base;
    p->to_skip = max(p->origin, p->base) - p->base;
    p->started = sox_true;
  }
  if (p->last_page && p->granule < p->position + frames)
    for (excess = p->position + frames - p->granule, i = p->queued; i-- > p->next && excess > 0;
        excess -= (ogg_int64_t)p->queue[i].discard)
      p->queue[i].discard = (size_t)min(excess, (ogg_int64_t)p->queue[i].frames);
  for (i = p->next; i < p->queued && p->to_skip > 0; p->to_skip -= (ogg_int64_t)p->queue[i++].skip)
    p->queue[i].skip = (size_t)min(p->to_skip, (ogg_int64_t)p->queue[i].frames);
  p->position = p->granule;
}

static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec, lsx_packet_t * packet)
{
  priv_t * p = v;
  queued_t const * q;

  while (p->next == p->queued) {
    int status = next_page(ft, p);

    if (status != 1)
      return status? ft->sox_errno : SOX_EOF;
  }
  if (!p->placed)
    place_page(p, codec);
  q = &p->queue[p->next++];
  packet->data = p->packets + q->offset;
  packet->size = q->size;
  packet->frames = q->frames;
  packet->skip = q->skip;
  packet->discard = q->discard;
  packet->duration = 0;
  return SOX_SUCCESS;
}

/* After the stream's last page, the pages of the streams that began with it
 * are passed over up to the first page of a chained stream. */
static int next_stream(sox_format_t * ft, void * v, lsx_codec_t const * codec,
    lsx_codec_params_t * params)
{
  priv_t * p = v;
  ogg_page page;
  off_t start;
  int status = 1;

  if (p->has_pending && !ogg_page_bos(&p->pending))
    drop_pending(p);
  if (p->has_pending) {
    ogg_page first = p->pending;

    p->has_pending = sox_false;
    status = read_chained(ft, p, &first, p->pending_start, codec->handler->name, params);
    free(first.header);
    free(first.body);
    return status == SOX_SUCCESS? SOX_SUCCESS : ft->sox_errno;
  }
  while ((status = read_page(ft, &p->sync, &p->pos, &page, &start)) == 1 && !ogg_page_bos(&page));
  if (status != 1)
    return status? ft->sox_errno : SOX_EOF;
  return read_chained(ft, p, &page, start, codec->handler->name, params) == SOX_SUCCESS?
    SOX_SUCCESS : ft->sox_errno;
}

static sox_bool begins_with_stream(priv_t const * p, long serial)
{
  size_t i;

  for (i = 0; i < p->serials_count; ++i)
    if (p->serials[i] == serial)
      return sox_true;
  return sox_false;
}

/* Where the streams that begin with the one read end: by bisection, the
 * first page of none of them, which must begin a chained stream; the end of
 * the file otherwise. */
static int streams_end(sox_format_t * ft, priv_t * p, off_t start, off_t * end)
{
  ogg_sync_state sync;
  ogg_page page;
  off_t lo = start, hi = (off_t)lsx_filelength(ft), pos, at;
  int status = SOX_SUCCESS, found;

  ogg_sync_init(&sync);
  while (status == SOX_SUCCESS && hi - lo > READ_SIZE) {
    off_t mid = lo + (hi - lo) / 2;

    if ((status = reposition(ft, &sync, &pos, mid)) != SOX_SUCCESS ||
        (found = read_page(ft, &sync, &pos, &page, &at)) < 0)
      status = SOX_EOF;
    else if (!found || at >= hi || !begins_with_stream(p, ogg_page_serialno(&page)))
      hi = mid;
    else lo = at;
  }
  if (status == SOX_SUCCESS && (status = reposition(ft, &sync, &pos, lo)) == SOX_SUCCESS) {
    while ((found = read_page(ft, &sync, &pos, &page, &at)) == 1 &&
        begins_with_stream(p, ogg_page_serialno(&page)));
    if (found < 0)
      status = SOX_EOF;
    *end = found == 1 && ogg_page_bos(&page)? at : (off_t)lsx_filelength(ft);
  }
  ogg_sync_clear(&sync);
  return status;
}

/* The last granule position of the stream before end, from the pages of
 * ever longer stretches before it, or -1 if it has none. */
static ogg_int64_t last_granule(sox_format_t * ft, priv_t * p, off_t start, off_t end)
{
  off_t from = end, size = END_SCAN, pos, at;
  ogg_int64_t last = -1;

  while (last < 0 && from > start) {
    ogg_sync_state sync;
    ogg_page page;

    from = end - start > size? end - size : start;
    size *= 2;
    ogg_sync_init(&sync);
    if (reposition(ft, &sync, &pos, from) == SOX_SUCCESS)
      while (read_page(ft, &sync, &pos, &page, &at) == 1 && at < end)
        if (ogg_page_serialno(&page) == p->serial && ogg_page_granulepos(&page) >= 0)
          last = ogg_page_granulepos(&page);
    ogg_sync_clear(&sync);
  }
  return last;
}

/* The signal from frame t of a chained stream: its first pages read again,
 * the codec restarted on its headers, and reading resumed at the last page
 * whose granule position is at most where decoding must begin, if one is
 * past the headers, found by bisection.  The frames up to t are dropped as
 * they are decoded. */
static int go_to(sox_format_t * ft, priv_t * p, lsx_codec_t * codec, link_t const * link,
    sox_uint64_t t)
{
  lsx_codec_params_t params;
  ogg_sync_state sync;
  ogg_page page;
  ogg_int64_t target;
  off_t lo = link->headers_end, hi = link->end, best = link->headers_end, pos, at;
  int status;

  if (rewind_to(ft, p, link->start, codec->handler->name, &params) != SOX_SUCCESS ||
      lsx_container_restart(ft, &params, !t) != SOX_SUCCESS)
    return SOX_EOF;
  if (!t)
    return SOX_SUCCESS;
  p->origin = link->signal + (ogg_int64_t)t;
  target = p->origin - (strcmp(codec->handler->name, "opus")? 0 : OPUS_PREROLL);
  ogg_sync_init(&sync);
  while (hi - lo > READ_SIZE) {
    off_t mid = lo + (hi - lo) / 2;

    if (reposition(ft, &sync, &pos, mid) != SOX_SUCCESS)
      break;
    while ((status = read_page(ft, &sync, &pos, &page, &at)) == 1 && at < hi &&
        (ogg_page_serialno(&page) != p->serial || ogg_page_granulepos(&page) < 0));
    if (status != 1 || at >= hi || ogg_page_granulepos(&page) > target)
      hi = mid;
    else best = lo = at;
  }
  if (reposition(ft, &sync, &pos, lo) == SOX_SUCCESS)
    while (read_page(ft, &sync, &pos, &page, &at) == 1 && at < hi)
      if (ogg_page_serialno(&page) == p->serial && at > link->headers_end &&
          ogg_page_granulepos(&page) >= 0 && ogg_page_granulepos(&page) <= target)
        best = at;
  ogg_sync_clear(&sync);
  if (best == link->headers_end) /* decoding begins after the headers, as already read */
    return reposition(ft, &p->sync, &p->pos, p->pos) == SOX_SUCCESS? SOX_SUCCESS : SOX_EOF;
  ogg_stream_reset(&p->stream);
  p->queued = p->next = 0;
  p->ended = sox_false;
  drop_pending(p);
  return reposition(ft, &p->sync, &p->pos, best);
}

/* On a file, the chained streams are found and checked against the first,
 * and their lengths added up.  The first audio pages of each are placed
 * with a codec of their own to find where the signal starts.  Reading then
 * starts over. */
static int length(sox_format_t * ft, void * v, lsx_codec_t * codec, sox_uint64_t * frames)
{
  priv_t * p = v;
  off_t start = 0, file_end = (off_t)lsx_filelength(ft);
  int status = SOX_SUCCESS;

  *frames = 0;
  if (!ft->seekable)
    return SOX_SUCCESS;
  while (status == SOX_SUCCESS && start < file_end) {
    lsx_codec_t probe;
    link_t link;
    ogg_int64_t last;
    int got = 1;

    memset(&probe, 0, sizeof(probe));
    probe.ft = ft;
    probe.handler = codec->handler;
    if (rewind_to(ft, p, start, codec->handler->name, &probe.params) != SOX_SUCCESS)
      return SOX_EOF;
    probe.priv = lsx_calloc(1, probe.handler->priv_size);
    if ((status = lsx_codec_start_decode(&probe, sox_false)) == SOX_SUCCESS) {
      status = lsx_container_check_chained(ft, &codec->params, &probe.params);
      link.start = start;
      link.headers_end = p->page_start;
      while (status == SOX_SUCCESS && !p->started && got == 1) {
        if (p->next < p->queued) {
          place_page(p, &probe);
          p->next = p->queued;
        }
        else got = next_page(ft, p);
      }
      if (got < 0)
        status = SOX_EOF;
      link.signal = p->started? max(p->base, 0) + (ogg_int64_t)probe.params.delay : 0;
      if (status == SOX_SUCCESS)
        status = streams_end(ft, p, start, &link.end);
      if (status == SOX_SUCCESS) {
        last = last_granule(ft, p, start, link.end);
        link.first = *frames;
        *frames += last > link.signal? (sox_uint64_t)(last - link.signal) : 0;
        p->links = lsx_realloc(p->links, (p->links_count + 1) * sizeof(*p->links));
        p->links[p->links_count++] = link;
        start = link.end;
      }
      if (probe.handler->stop_decode)
        probe.handler->stop_decode(&probe);
    }
    free(probe.priv);
    free(probe.samples);
    free(probe.params.private_data);
  }
  return status == SOX_SUCCESS? go_to(ft, p, codec, &p->links[0], 0) : SOX_EOF;
}

/* Seeking is on a file only, whose chained streams length() has found. */
static int seek(sox_format_t * ft, void * v, lsx_codec_t * codec, sox_uint64_t frame)
{
  priv_t * p = v;
  size_t i = 0;

  while (i + 1 < p->links_count && frame >= p->links[i + 1].first)
    ++i;
  return go_to(ft, p, codec, &p->links[i], frame - p->links[i].first);
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  priv_t * p = v;

  if (p->stream_open)
    ogg_stream_clear(&p->stream);
  ogg_sync_clear(&p->sync);
  drop_pending(p);
  free(p->packets);
  free(p->queue);
  free(p->serials);
  free(p->links);
}


/*--------------------------------- Writing ---------------------------------*/

static int write_page(sox_format_t * ft, ogg_page const * page)
{
  return lsx_writebuf(ft, page->header, (size_t)page->header_len) == (size_t)page->header_len &&
    lsx_writebuf(ft, page->body, (size_t)page->body_len) == (size_t)page->body_len?
    SOX_SUCCESS : SOX_EOF;
}

static int packet_in(sox_format_t * ft, priv_t * p, unsigned char const * data, size_t size,
    ogg_int64_t granule, sox_bool eos)
{
  ogg_packet op;
  ogg_page page;

  op.packet = (unsigned char *)data;
  op.bytes = (long)size;
  op.b_o_s = !p->packetno;
  op.e_o_s = eos;
  op.granulepos = granule;
  op.packetno = p->packetno++;
  ogg_stream_packetin(&p->out, &op);
  while (ogg_stream_pageout(&p->out, &page))
    if (write_page(ft, &page) != SOX_SUCCESS)
      return SOX_EOF;
  return SOX_SUCCESS;
}

/* Opus's comment header, from SoX's comments, each made a NAME=value pair
 * under the name Comment when it is not one already. */
static unsigned char * opus_tags(sox_format_t * ft, size_t * size)
{
  char vendor[64];
  size_t count = sox_num_comments(ft->oob.comments), i, pos;
  unsigned char * b;

  sprintf(vendor, "SoX %.40s", sox_version());
  for (*size = 8 + 4 + strlen(vendor) + 4, i = 0; i < count; ++i)
    *size += 4 + (strchr(ft->oob.comments[i], '=')? 0 : 8) + strlen(ft->oob.comments[i]);
  b = lsx_malloc(*size);
  memcpy(b, "OpusTags", 8);
  pos = 8;
  for (i = 0; i <= count; ++i) {
    char const * text = i? ft->oob.comments[i - 1] : vendor;
    sox_bool named = !i || strchr(text, '=');
    size_t len = strlen(text) + (named? 0 : 8);

    b[pos] = (unsigned char)len;
    b[pos + 1] = (unsigned char)(len >> 8);
    b[pos + 2] = (unsigned char)(len >> 16);
    b[pos + 3] = (unsigned char)(len >> 24);
    pos += 4;
    if (!named) {
      memcpy(b + pos, "Comment=", 8);
      pos += 8;
    }
    memcpy(b + pos, text, strlen(text));
    pos += strlen(text);
    if (!i) {
      b[pos] = (unsigned char)count;
      b[pos + 1] = (unsigned char)(count >> 8);
      b[pos + 2] = (unsigned char)(count >> 16);
      b[pos + 3] = (unsigned char)(count >> 24);
      pos += 4;
    }
  }
  return b;
}

/* FLAC's first header packet: the Ogg mapping, the stream marker and
 * STREAMINFO. */
static void flac_first_packet(unsigned char * b, lsx_codec_params_t const * params, unsigned headers)
{
  memcpy(b, "\x7f" "FLAC\x01\x00", 7);
  b[7] = (unsigned char)(headers >> 8);
  b[8] = (unsigned char)headers;
  memcpy(b + FLAC_MAPPING_SIZE, params->private_data, FLAC_HEADER_SIZE);
}

static int open_write(sox_format_t * ft, void * v, lsx_codec_t const * codec,
    lsx_codec_tag_t const * tag)
{
  priv_t * p = v;
  lsx_codec_params_t const * params = &codec->params;
  unsigned char const * b = params->private_data;
  ogg_page page;
  int status = SOX_SUCCESS;

  ogg_stream_init(&p->out, INT_MAX & (int)RANQD1);
  if (!strcmp(tag->codec, "vorbis")) {
    size_t sizes[3], pos = 1, i;

    for (i = 0; i < 2; ++i)
      for (sizes[i] = 0; sizes[i] += b[pos], b[pos++] == 255; );
    sizes[2] = params->private_size - pos - sizes[0] - sizes[1];
    for (i = 0; i < 3; pos += sizes[i++])
      ogg_stream_packetin(&p->out, &(ogg_packet){(unsigned char *)b + pos, (long)sizes[i],
          !i, 0, 0, p->packetno++});
  }
  else if (!strcmp(tag->codec, "opus")) {
    size_t size;
    unsigned char * tags = opus_tags(ft, &size);

    ogg_stream_packetin(&p->out, &(ogg_packet){(unsigned char *)b, (long)params->private_size,
        1, 0, 0, p->packetno++});
    ogg_stream_packetin(&p->out, &(ogg_packet){tags, (long)size, 0, 0, 0, p->packetno++});
    free(tags);
  }
  else {
    unsigned char first[FLAC_MAPPING_SIZE + FLAC_HEADER_SIZE];
    size_t pos, headers = 0;

    for (pos = FLAC_HEADER_SIZE; pos + 4 <= params->private_size; ++headers)
      pos += 4 + ((size_t)b[pos + 1] << 16 | (size_t)b[pos + 2] << 8 | b[pos + 3]);
    flac_first_packet(first, params, (unsigned)headers);
    ogg_stream_packetin(&p->out, &(ogg_packet){first, sizeof(first), 1, 0, 0, p->packetno++});
    for (pos = FLAC_HEADER_SIZE; pos + 4 <= params->private_size; ) {
      size_t size = 4 + ((size_t)b[pos + 1] << 16 | (size_t)b[pos + 2] << 8 | b[pos + 3]);

      ogg_stream_packetin(&p->out, &(ogg_packet){(unsigned char *)b + pos,
          (long)min(size, params->private_size - pos), 0, 0, 0, p->packetno++});
      pos += size;
    }
  }
  while (status == SOX_SUCCESS && ogg_stream_flush(&p->out, &page)) {
    if (!p->first_page && !strcmp(tag->codec, "flac") && ft->seekable) {
      p->first_page_pos = (sox_uint64_t)lsx_tell(ft);
      p->header_len = (size_t)page.header_len;
      p->first_page_size = (size_t)(page.header_len + page.body_len);
      p->first_page = lsx_malloc(p->first_page_size);
      memcpy(p->first_page, page.header, (size_t)page.header_len);
      memcpy(p->first_page + page.header_len, page.body, (size_t)page.body_len);
    }
    status = write_page(ft, &page);
  }
  if (status != SOX_SUCCESS) {
    ogg_stream_clear(&p->out);
    free(p->first_page);
  }
  return status;
}

/* The granule position of a packet is where the frames it decodes to end,
 * less all the padding so far. */
static int submit_held(sox_format_t * ft, priv_t * p, sox_bool eos)
{
  p->written_granule += (ogg_int64_t)p->held_frames - (ogg_int64_t)p->held_discard;
  p->holding = sox_false;
  return packet_in(ft, p, p->held, p->held_size, p->written_granule, eos);
}

/* Each packet is held until the next arrives, since the last must be marked
 * as ending the stream. */
static int write_packet(sox_format_t * ft, void * v, lsx_packet_t const * packet)
{
  priv_t * p = v;

  if (p->holding && submit_held(ft, p, sox_false) != SOX_SUCCESS)
    return SOX_EOF;
  if (packet->size > p->held_alloc) {
    p->held_alloc = max(packet->size, p->held_alloc * 2);
    p->held = lsx_realloc(p->held, p->held_alloc);
  }
  memcpy(p->held, packet->data, packet->size);
  p->held_size = packet->size;
  p->held_frames = packet->frames;
  p->held_discard = packet->discard;
  p->holding = sox_true;
  return SOX_SUCCESS;
}

/* A FLAC stream's first page again, with the final STREAMINFO. */
static int rewrite_flac_header(sox_format_t * ft, priv_t * p, lsx_codec_params_t const * params)
{
  ogg_page page;
  unsigned char * body = p->first_page + p->header_len;

  flac_first_packet(body, params, (unsigned)(body[7] << 8 | body[8]));
  page.header = p->first_page;
  page.header_len = (long)p->header_len;
  page.body = body;
  page.body_len = (long)(p->first_page_size - p->header_len);
  ogg_page_checksum_set(&page);
  if (lsx_seeki(ft, (off_t)p->first_page_pos, SEEK_SET) != SOX_SUCCESS ||
      write_page(ft, &page) != SOX_SUCCESS)
    return SOX_EOF;
  return lsx_seeki(ft, (off_t)0, SEEK_END);
}

static int close_write(sox_format_t * ft, void * v, lsx_codec_t const * codec)
{
  priv_t * p = v;
  int status = p->holding? submit_held(ft, p, sox_true) : SOX_SUCCESS;

  if (status == SOX_SUCCESS && p->first_page)
    status = rewrite_flac_header(ft, p, &codec->params);
  ogg_stream_clear(&p->out);
  free(p->held);
  free(p->first_page);
  return status;
}

static lsx_container_ops_t const ogg_ops = {
  sizeof(priv_t),
  open_read, read_packet, next_stream, length, seek, close_read,
  open_write, write_packet, close_write
};

static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &ogg_ops);
}

static int startwrite(sox_format_t * ft)
{
  return lsx_container_startwrite(ft, &ogg_ops);
}

LSX_FORMAT_HANDLER(ogg)
{
  static char const * const names[] = {"ogg", "vorbis", "opus", NULL};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Ogg holding Vorbis, Opus or FLAC; the codec is given as in -t ogg:opus, Vorbis if not, "
    "and -t vorbis and -t opus are Ogg in that codec",
    names, SOX_FILE_CONTAINER,
    startread, lsx_container_read, lsx_container_stopread,
    startwrite, lsx_container_write, lsx_container_stopwrite,
    lsx_container_seek, NULL, NULL, sizeof(lsx_container_t)
  };
  return &handler;
}
