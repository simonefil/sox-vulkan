/* libSoX MPEG audio elementary stream: the frames of an MP3 or MP2 file.
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

/* An MP3 or MP2 file is MPEG audio frames back to back, of any layer and of
 * MPEG 1, 2 or 2.5, with nothing around them but tags: ID3v2 in front, and
 * at the end ID3v1, APEv2, Lyrics3 or another ID3v2.  A frame begins with a
 * four-byte header, which gives its length but for a free-format frame,
 * whose length is where the next frame begins.  Eleven bits of sync and a
 * header that holds together do not tell a frame from audio data or junk, so
 * the stream is taken to begin, at the start and wherever it is lost, only
 * where several frames follow one another with the same version, layer and
 * sample rate, as a demuxer does; from there each frame has to follow the
 * last.  A frame is handed to the MP3 codec as a packet, which libmad
 * decodes whatever the layer.
 *
 * LAME and FFmpeg put a Xing or Info frame first in an MP3 file, a Layer III
 * frame holding no audio but the count of frames and, in its LAME extension,
 * the encoder delay and padding.  The frame is not passed on; its delay and
 * the decoder's 529 frames make the codec delay, and its padding the discard
 * on the last frames, so that the signal comes out as it went in.  A VBRI
 * frame is passed over as well.  A file without the LAME extension is not
 * trimmed, as FFmpeg does not trim it either.
 *
 * The length is the signal's when the information frame gives the count of
 * frames, and otherwise, for a file, the frames counted by reading through
 * them once, which is exact since nothing is trimmed then.  On a pipe it is
 * unknown.  Seeking counts the frames up to the place sought, and starts the
 * codec a few frames ahead of it, enough for the data a frame takes from the
 * ones before, the bit reservoir, and for the overlap of the decoder's
 * filters; their samples are skipped. */

#include "container.h"

#if defined HAVE_MP3 && (defined STATIC_MP3 || !defined HAVE_LIBLTDL) && defined HAVE_MAD_H

#include "id3.h"

#include <string.h>

#define HEADER_MASK 0xfffe0c00 /* sync, version, layer and sample rate */
#define MAX_FRAME_SIZE 2881    /* MPEG 2.5 Layer II at 160 kbit/s and 8 kHz, padded */
#define SYNC_FRAMES 4          /* consecutive frames that are taken for the stream */
#define TAG_MAGIC_SIZE 11      /* enough for any tag's magic */
#define DECODER_DELAY 529
#define RESERVOIR_SIZE 511     /* the furthest back a frame's main data can begin */
#define PREROLL_FRAMES 64      /* the most a seek starts ahead of the place */

typedef struct {
  unsigned char * buffer;  /* the file from pos, between start and end */
  size_t buffer_size, start, end;
  sox_uint64_t pos;
  size_t taken;            /* the frame at start, handed out and done with */
  sox_uint32_t header;     /* the stream's, masked; 0 until it is found */
  size_t free_slots;       /* free format: a frame's slots, unpadded */
  sox_bool synced;         /* a frame is expected at start */
  sox_bool layer3;
  unsigned frame_samples;
  sox_uint64_t first;      /* where the first frame of audio is */
  sox_uint64_t next;       /* the frame of audio at start */
  sox_uint64_t delay;      /* decoder output ahead of the signal */
  sox_uint64_t stop;       /* decoder output where the signal ends; 0 if not known */
  sox_uint64_t counted;    /* decoder output of the frames the count covers */
  sox_uint64_t target;     /* decoder output before which a seek skips */
  sox_uint64_t length;     /* of the signal, in frames; 0 if not known */
} priv_t;

static sox_uint32_t get_be32(unsigned char const * b)
{
  return (sox_uint32_t)b[0] << 24 | (sox_uint32_t)b[1] << 16 | (sox_uint32_t)b[2] << 8 | b[3];
}

/* The length of the frame whose header is h, and the samples it decodes to,
 * or 0 if h is no frame header.  A free-format frame is free_slots slots,
 * and padding, long; free_slots 0 is not knowing how long. */
static size_t frame_length(sox_uint32_t h, size_t free_slots, unsigned * samples)
{
  static unsigned const bitrates[2][3][15] = {
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
  static unsigned const rates[3] = {44100, 48000, 32000};
  unsigned version = h >> 19 & 3, layer = 4 - (h >> 17 & 3), index = h >> 12 & 15, pad = h >> 9 & 1;
  sox_bool lsf = version != 3;
  unsigned rate;
  size_t slots;

  if ((h & 0xffe00000) != 0xffe00000 || version == 1 || layer == 4 || index == 15 || (h >> 10 & 3) == 3)
    return 0;
  rate = rates[h >> 10 & 3] >> (version == 3? 0 : version == 2? 1 : 2);
  *samples = layer == 1? 384 : layer == 3 && lsf? 576 : 1152;
  if (!index)
    slots = free_slots? free_slots + pad : 0;
  else slots = (layer == 1? 12 : layer == 3 && lsf? 72 : 144) * bitrates[lsf][layer - 1][index] * 1000 / rate + pad;
  return layer == 1? slots * 4 : slots;
}

/* The bytes of the file from start up to n, or fewer at the end of the
 * file; what precedes start is let go only when room is needed. */
static size_t fill(sox_format_t * ft, priv_t * p, size_t n)
{
  if (p->end - p->start < n) {
    memmove(p->buffer, p->buffer + p->start, p->end - p->start);
    p->end -= p->start;
    p->start = 0;
    if (n > p->buffer_size) {
      p->buffer_size = max(n, 2 * p->buffer_size);
      p->buffer = lsx_realloc(p->buffer, p->buffer_size);
    }
    p->end += lsx_readbuf(ft, p->buffer + p->end, n - p->end);
  }
  return min(n, p->end - p->start);
}

static void drop(priv_t * p, size_t n)
{
  p->start += n;
  p->pos += n;
}

/* The size of the ID3v2 tag at b, footer included, or 0 if there is none:
 * the size bytes being synchsafe keeps a stray "ID3" from passing for one. */
static size_t id3v2_size(unsigned char const * b, size_t n)
{
  if (n < 10 || memcmp(b, "ID3", 3) || b[3] == 0xff || b[4] == 0xff || ((b[6] | b[7] | b[8] | b[9]) & 0x80))
    return 0;
  return 10 + ((size_t)b[6] << 21) + ((size_t)b[7] << 14) + ((size_t)b[8] << 7) + b[9] +
      (b[3] == 4 && (b[5] & 0x10)? 10 : 0);
}

/* Whether a tag that only ends a file begins at b: ID3v1, APEv2 or Lyrics3. */
static sox_bool end_tag(unsigned char const * b, size_t n)
{
  return (n >= 3 && !memcmp(b, "TAG", 3)) || (n >= 8 && !memcmp(b, "APETAGEX", 8)) ||
      (n >= 11 && !memcmp(b, "LYRICSBEGIN", 11));
}

/* Whether the frame at start is followed by more with the same masked
 * header, up to SYNC_FRAMES in all or to the end of the file or a tag. */
static sox_bool frames_follow(sox_format_t * ft, priv_t * p, sox_uint32_t header, size_t free_slots)
{
  size_t off = 0, length, n;
  unsigned i, samples;

  for (i = 0; i < SYNC_FRAMES; ++i) {
    unsigned char const * b;

    n = fill(ft, p, off + TAG_MAGIC_SIZE);
    b = p->buffer + p->start + off;
    if (n < off)
      return sox_false;
    if (i && (n - off < 4 || end_tag(b, n - off) || id3v2_size(b, n - off)))
      return sox_true;
    if (n - off < 4 || (get_be32(b) & HEADER_MASK) != header ||
        !(length = frame_length(get_be32(b), free_slots, &samples)))
      return sox_false;
    off += length;
  }
  return sox_true;
}

/* Whether the stream begins, or goes on, with the frame header at start.
 * The stream's header must be kept to once found.  Without a bit rate, a
 * free-format frame is as long as up to where a frame with the same header
 * begins, the first such place that frames follow on from being taken. */
static sox_bool stream_starts(sox_format_t * ft, priv_t * p)
{
  sox_uint32_t h = get_be32(p->buffer + p->start), header = h & HEADER_MASK;
  sox_bool layer1 = (h >> 17 & 3) == 3;
  size_t off, n;

  if (p->header && (header != p->header || !(h >> 12 & 15) != !!p->free_slots))
    return sox_false;
  if ((h >> 12 & 15) || p->free_slots) {
    if (!frames_follow(ft, p, header, p->free_slots))
      return sox_false;
  }
  else {
    n = fill(ft, p, MAX_FRAME_SIZE + 4);
    for (off = 5; off + 4 <= n; ++off) {
      unsigned char const * b = p->buffer + p->start + off;

      if ((get_be32(b) & (HEADER_MASK | 0xf000)) == header && !(layer1 && off % 4) &&
          frames_follow(ft, p, header, (layer1? off / 4 : off) - (h >> 9 & 1))) {
        p->free_slots = (layer1? off / 4 : off) - (h >> 9 & 1);
        break;
      }
    }
    if (!p->free_slots)
      return sox_false;
  }
  p->header = header;
  return sox_true;
}

/* The next frame, left at start: SOX_SUCCESS with its length, or SOX_EOF at
 * the end of the stream.  A tag where a frame is expected is passed over if
 * it is ID3v2, and ends the stream otherwise.  What does not begin a frame
 * there is passed over until the stream is found again. */
static int next_frame(sox_format_t * ft, priv_t * p, size_t * length)
{
  sox_uint64_t junk = 0;

  drop(p, p->taken);
  p->taken = 0;
  for (;;) {
    size_t n = fill(ft, p, TAG_MAGIC_SIZE), size;
    unsigned char const * b = p->buffer + p->start;
    unsigned samples;

    if (n < 4)
      break;
    if (p->synced) {
      if (end_tag(b, n))
        break;
      if ((size = id3v2_size(b, n))) {
        drop(p, min(size, p->end - p->start));
        size -= min(size, p->end - p->start);
        if (size && lsx_skipbytes(ft, size) != SOX_SUCCESS)
          break;
        p->pos += size;
        continue;
      }
      if ((get_be32(b) & HEADER_MASK) == p->header &&
          (*length = frame_length(get_be32(b), p->free_slots, &samples))) {
        if (junk)
          lsx_report("%" PRIu64 " bytes skipped before the frame at %" PRIu64, junk, p->pos);
        if (fill(ft, p, *length) < *length) {
          lsx_report("the last frame is truncated");
          break;
        }
        p->taken = *length;
        return SOX_SUCCESS;
      }
      p->synced = sox_false;
    }
    if (b[0] == 0xff && (b[1] & 0xe0) == 0xe0 && stream_starts(ft, p))
      p->synced = sox_true;
    else {
      drop(p, 1);
      ++junk;
    }
  }
  return SOX_EOF;
}

/* Reading resumes at a frame, the stream's header being known. */
static int rewind_to(sox_format_t * ft, priv_t * p, sox_uint64_t pos)
{
  if (lsx_seeki(ft, (off_t)pos, SEEK_SET) != SOX_SUCCESS)
    return SOX_EOF;
  p->start = p->end = p->taken = 0;
  p->pos = pos;
  p->synced = sox_true;
  return SOX_SUCCESS;
}

/* The tags are read, from a file only, as they may be at either end.  The
 * information frame is where Layer III's main data would begin were there no
 * CRC, which LAME leaves out of the count even when it sets the flag for it,
 * as FFmpeg does in reading; the LAME extension follows the frame count and
 * what the flags say comes after it, and gives the delay and padding in 12
 * bits each, after the encoder's name and version. */
static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  sox_bool ignore_length = ft->signal.length == SOX_IGNORE_LENGTH;
  unsigned char magic[4];
  unsigned char const * b;
  size_t length, pos;
  sox_uint32_t h;
  sox_uint64_t count = 0, total = 0;
  sox_bool information = sox_false;

  if (ft->seekable) {
    lsx_id3_read_tag(ft, sox_true);
    if (lsx_seeki(ft, (off_t)0, SEEK_SET) != SOX_SUCCESS)
      return SOX_EOF;
  }
  if (lsx_container_read_magic(ft, magic, &p->pos) != SOX_SUCCESS)
    return SOX_EOF;
  p->buffer_size = MAX_FRAME_SIZE * SYNC_FRAMES + TAG_MAGIC_SIZE;
  p->buffer = lsx_malloc(p->buffer_size);
  memcpy(p->buffer, magic, 4);
  p->end = 4;
  if (next_frame(ft, p, &length) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "no MPEG audio frame found");
    return SOX_EOF;
  }
  b = p->buffer + p->start;
  h = get_be32(b);
  frame_length(h, p->free_slots, &p->frame_samples);
  p->layer3 = (h >> 17 & 3) == 1;
  params->rate = ((h >> 10 & 3) == 0? 44100 : (h >> 10 & 3) == 1? 48000 : 32000) >>
      ((h >> 19 & 3) == 3? 0 : (h >> 19 & 3) == 2? 1 : 2);
  params->channels = (h >> 6 & 3) == 3? 1 : 2;
  params->encoding = SOX_ENCODING_MP3;
  *codec_name = "mp3";

  if (p->layer3) {
    pos = 4 + ((h >> 19 & 3) == 3? ((h >> 6 & 3) == 3? 17 : 32) : ((h >> 6 & 3) == 3? 9 : 17));
    if (pos + 8 <= length && (!memcmp(b + pos, "Xing", 4) || !memcmp(b + pos, "Info", 4))) {
      sox_uint32_t flags = get_be32(b + pos + 4);

      information = sox_true;
      pos += 8;
      if ((flags & 1) && pos + 4 <= length) {
        count = get_be32(b + pos);
        pos += 4;
      }
      pos += (flags & 2? 4 : 0) + (flags & 4? 100 : 0) + (flags & 8? 4 : 0);
      if (pos + 24 <= length &&
          (!memcmp(b + pos, "LAME", 4) || !memcmp(b + pos, "Lavf", 4) || !memcmp(b + pos, "Lavc", 4))) {
        unsigned padding = (unsigned)(b[pos + 22] & 15) << 8 | b[pos + 23];

        p->delay = ((unsigned)b[pos + 21] << 4 | b[pos + 22] >> 4) + DECODER_DELAY;
        p->counted = count * p->frame_samples;
        if (count && p->counted + DECODER_DELAY > padding)
          p->stop = p->counted + DECODER_DELAY - padding;
      }
    }
    else if (4 + 32 + 4 <= length && !memcmp(b + 4 + 32, "VBRI", 4))
      information = sox_true;
  }
  p->first = p->pos + (information? length : 0);
  ft->data_start = p->first; /* after the tag and the information frame */
  if (!information)
    p->taken = 0;
  params->delay = p->delay;

  if (count)
    total = count * p->frame_samples;
  else if (ft->seekable && !ignore_length) {
    while (next_frame(ft, p, &length) == SOX_SUCCESS)
      total += p->frame_samples;
    if (rewind_to(ft, p, p->first) != SOX_SUCCESS)
      return SOX_EOF;
  }
  if (p->stop)
    total = min(total, p->stop);
  if (!ignore_length && total > p->delay)
    p->length = total - p->delay;
  return SOX_SUCCESS;
}

/* The place a seek went to is skipped up to, and the padding is discarded,
 * both counted in the decoder's output, every frame giving as many samples.
 * The padding ends with the frames the information frame counts: frames
 * beyond them, as where files were joined, are read as they come, as FFmpeg
 * reads them. */
static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec UNUSED, lsx_packet_t * packet)
{
  priv_t * p = v;
  sox_uint64_t start = p->next * p->frame_samples, end = start + p->frame_samples;
  size_t length;

  if (next_frame(ft, p, &length) != SOX_SUCCESS)
    return SOX_EOF;
  packet->data = p->buffer + p->start;
  packet->size = length;
  packet->frames = p->frame_samples;
  packet->skip = (size_t)(min(max(p->target, start), end) - start);
  packet->discard = p->stop && start < p->counted && end > p->stop?
      (size_t)(min(end, p->counted) - max(start, p->stop)) : 0;
  packet->duration = 0;
  ++p->next;
  return SOX_SUCCESS;
}

static int length(sox_format_t * ft UNUSED, void * v, lsx_codec_t * codec UNUSED, sox_uint64_t * frames)
{
  *frames = ((priv_t *)v)->length;
  return SOX_SUCCESS;
}

/* The frames are counted from the first up to the one that holds the place,
 * remembering where the last few were and how much main data they hold.
 * Layer III decodes a frame right when the two before it were, whose
 * filters overlap into it, and when the frames before those hold all the
 * main data the two can take from them; the other layers need only the
 * frame before.  A place beyond the end leaves nothing to read, as seeking
 * past the end of any file does. */
static int seek(sox_format_t * ft, void * v, lsx_codec_t * codec, sox_uint64_t frame)
{
  priv_t * p = v;
  sox_uint64_t target = frame + p->delay, i, j, starts[PREROLL_FRAMES];
  size_t main_sizes[PREROLL_FRAMES], length, data = 0;
  lsx_codec_params_t params = codec->params;

  if (rewind_to(ft, p, p->first) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = 0; ; ++i) {
    unsigned char const * b;
    size_t side;

    if (next_frame(ft, p, &length) != SOX_SUCCESS) {
      p->next = i;
      p->target = target;
      return lsx_container_restart(ft, &params, sox_false);
    }
    b = p->buffer + p->start;
    side = 4 + (b[1] & 1? 0 : 2) + ((b[1] >> 3 & 3) == 3? ((b[3] >> 6) == 3? 17 : 32) : ((b[3] >> 6) == 3? 9 : 17));
    starts[i % PREROLL_FRAMES] = p->pos;
    main_sizes[i % PREROLL_FRAMES] = length > side? length - side : 0;
    if ((i + 1) * p->frame_samples > target)
      break;
  }
  j = p->layer3? (i > 2? i - 2 : 0) : (i? i - 1 : 0);
  while (p->layer3 && j && data < RESERVOIR_SIZE && i - j + 1 < PREROLL_FRAMES)
    data += main_sizes[--j % PREROLL_FRAMES];
  if (rewind_to(ft, p, starts[j % PREROLL_FRAMES]) != SOX_SUCCESS)
    return SOX_EOF;
  p->next = j;
  p->target = target;
  return lsx_container_restart(ft, &params, sox_false);
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  free(((priv_t *)v)->buffer);
}

lsx_container_ops_t const lsx_mpeg_audio_ops = {
  sizeof(priv_t),
  open_read, read_packet, NULL, length, seek, close_read,
  NULL, NULL, NULL
};

#endif
