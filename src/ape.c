/* libSoX Monkey's Audio container.
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

/* A Monkey's Audio file is a header, a table of where each frame starts, and
 * the frames, all little-endian; it begins "MAC ", or "MACF" when its
 * samples are 32-bit floating point.  Since version 3.98 the header is a
 * descriptor, which gives the sizes of the parts and an MD5 of the file, and
 * then the stream header; before, it was one header of its own layout, with
 * the original WAVE header ahead of the table.  A file holds blocks, a block
 * being a sample of every channel; every frame holds as many, the last
 * excepted.  Tags, APE or ID3v1, follow the frames and are left unread, as
 * is an ID3v2 tag in front.
 *
 * Frames are 32-bit words, counted from the first frame: a frame starts
 * part-way into the word where the frame before ends, and shares it, the
 * decoder being told how many bytes of the word to skip, and the decoder
 * reads a word beyond a frame's end.  The frames are read in order, keeping
 * what the next one shares, so that a pipe will do.  Frames written here are
 * padded to a whole word instead, which the decoders take as well since
 * they place every frame by the seek table.  The file ends with a word the
 * decoder may read ahead into.  Files before version 3.93 are not read: the
 * decoder no longer does.
 *
 * Writing reserves the seek table for the length expected, and moves the
 * frames further down at the end when more were written, so the output must
 * be a file.  A floating-point file keeps a WAVE header after the table, its
 * sizes known only at the end, and since the MD5 starts with it the frames
 * are read back for it. */

#include "container.h"
#include "ape-mac.h"

#include <string.h>

#define DESCRIPTOR_SIZE 52
#define HEADER_SIZE 24
#define OLD_HEADER_SIZE 32
#define MIN_VERSION 3930
#define MAX_VERSION 3990
#define WRITE_VERSION 3990
#define FLAG_8_BIT 1
#define FLAG_PEAK_LEVEL 4
#define FLAG_24_BIT 8
#define FLAG_SEEK_ELEMENTS 16
#define FLAG_CREATE_WAV_HEADER 32
#define FLAG_FLOATING_POINT 4096
#define PACKET_HEADER_SIZE 8
#define PRIVATE_SIZE 10
#define SHIFT_CHUNK (1 << 20)
#define WAV_HEADER_SIZE 58
#define WAVE_FORMAT_IEEE_FLOAT 3

typedef struct {
  /* Reading */
  unsigned version;
  sox_uint32_t frames, frame_blocks, final_blocks;
  sox_uint64_t * starts;      /* of the frames, in the file */
  sox_uint64_t first, end;    /* of the frame data; end 0 if not given */
  sox_uint32_t next;          /* frame */
  unsigned char * buffer;     /* the packet header, then the file from buffer_pos */
  size_t buffer_size, buffer_length;
  sox_uint64_t buffer_pos;
  size_t seek_skip;           /* blocks of the next frame before the place sought */

  /* Writing */
  sox_uint64_t * offsets;     /* of the frames, from the first */
  size_t offsets_size, reserved;
  sox_uint64_t data_bytes;
  sox_uint32_t blocks;
  unsigned char header[HEADER_SIZE];
  unsigned wav_header_size;   /* 0 when the decoder makes one up */
} priv_t;

static sox_uint32_t get_le(unsigned char const * b, int bytes)
{
  sox_uint32_t value = 0;

  while (bytes--)
    value = value << 8 | b[bytes];
  return value;
}

static void put_le(unsigned char * b, sox_uint32_t value, int bytes)
{
  int i;

  for (i = 0; i < bytes; ++i)
    b[i] = (unsigned char)(value >> (8 * i));
}

/*------------------------------- MD5 (RFC 1321) ----------------------------*/

#define ROTATE(x, n) ((x) << (n) | (x) >> (32 - (n)))

void lsx_md5_init(lsx_md5_t * md5)
{
  md5->state[0] = 0x67452301;
  md5->state[1] = 0xefcdab89;
  md5->state[2] = 0x98badcfe;
  md5->state[3] = 0x10325476;
  md5->length[0] = md5->length[1] = 0;
}

void lsx_md5_add(lsx_md5_t * md5, void const * data, size_t size)
{
  static unsigned char const shifts[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
  static sox_uint32_t const sines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
  };
  unsigned char const * bytes = data;
  sox_uint64_t length = (sox_uint64_t)md5->length[1] << 32 | md5->length[0];
  size_t used = (size_t)(length % 64);

  length += size;
  md5->length[0] = (sox_uint32_t)length;
  md5->length[1] = (sox_uint32_t)(length >> 32);
  while (size) {
    size_t n = min(size, 64 - used);
    sox_uint32_t a, b, c, d, words[16];
    int i;

    memcpy(md5->block + used, bytes, n);
    bytes += n;
    size -= n;
    used += n;
    if (used < 64)
      break;
    used = 0;
    a = md5->state[0], b = md5->state[1], c = md5->state[2], d = md5->state[3];
    for (i = 0; i < 16; ++i)
      words[i] = get_le(md5->block + 4 * i, 4);
    for (i = 0; i < 64; ++i) {
      sox_uint32_t f, t;
      int word;

      switch (i / 16) {
        case 0: f = (b & c) | (~b & d); word = i; break;
        case 1: f = (d & b) | (~d & c); word = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d; word = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d); word = 7 * i % 16; break;
      }
      t = d;
      d = c;
      c = b;
      f += a + sines[i] + words[word];
      b += ROTATE(f, shifts[i / 16][i % 4]);
      a = t;
    }
    md5->state[0] += a;
    md5->state[1] += b;
    md5->state[2] += c;
    md5->state[3] += d;
  }
}

void lsx_md5_final(lsx_md5_t * md5, unsigned char digest[16])
{
  static unsigned char const padding[64] = {0x80};
  unsigned char bits[8];
  int i;

  put_le(bits, md5->length[0] << 3, 4);
  put_le(bits + 4, md5->length[1] << 3 | md5->length[0] >> 29, 4);
  lsx_md5_add(md5, padding, 1 + (119 - md5->length[0] % 64) % 64);
  lsx_md5_add(md5, bits, 8);
  for (i = 0; i < 4; ++i)
    put_le(digest + 4 * i, md5->state[i], 4);
}

/*--------------------------------- Reading ---------------------------------*/

static int read_bytes(sox_format_t * ft, unsigned char * data, size_t size)
{
  if (lsx_readbuf(ft, data, size) != size) {
    lsx_fail_errno(ft, SOX_EHDR, "Monkey's Audio file truncated");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* The header comes in either layout, the WAVE header data stored after it
 * in the new one being read through with the frames.  On a file, the frame
 * data of an old one is taken to end where the file does, short of the WAVE
 * trailer: what tags follow, the decoder leaves. */
static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  unsigned char h[DESCRIPTOR_SIZE + HEADER_SIZE];
  sox_uint64_t start, at;
  sox_uint32_t tail = 0;
  unsigned level, flags, bits;
  size_t table_size, i;

  *codec_name = "ape";
  if (lsx_container_read_magic(ft, h, &start) != SOX_SUCCESS)
    return SOX_EOF;
  if (memcmp(h, "MAC ", 4) && memcmp(h, "MACF", 4)) {
    lsx_fail_errno(ft, SOX_EHDR, "not a Monkey's Audio file");
    return SOX_EOF;
  }
  if (read_bytes(ft, h + 4, 2) != SOX_SUCCESS)
    return SOX_EOF;
  p->version = get_le(h + 4, 2);
  if (p->version < MIN_VERSION || p->version > MAX_VERSION) {
    lsx_fail_errno(ft, SOX_EHDR, "Monkey's Audio file version %u.%02u is unsupported",
        p->version / 1000, p->version % 1000 / 10);
    return SOX_EOF;
  }
  if (p->version >= 3980) {
    sox_uint32_t descriptor_size, header_size, header_data_size;
    sox_uint64_t data_size;

    if (read_bytes(ft, h + 6, DESCRIPTOR_SIZE - 6) != SOX_SUCCESS)
      return SOX_EOF;
    descriptor_size = get_le(h + 8, 4);
    header_size = get_le(h + 12, 4);
    table_size = get_le(h + 16, 4);
    header_data_size = get_le(h + 20, 4);
    data_size = (sox_uint64_t)get_le(h + 28, 4) << 32 | get_le(h + 24, 4);
    if (descriptor_size < DESCRIPTOR_SIZE || header_size < HEADER_SIZE) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid Monkey's Audio header");
      return SOX_EOF;
    }
    if (lsx_skipbytes(ft, descriptor_size - DESCRIPTOR_SIZE) != SOX_SUCCESS ||
        read_bytes(ft, h + DESCRIPTOR_SIZE, HEADER_SIZE) != SOX_SUCCESS ||
        lsx_skipbytes(ft, header_size - HEADER_SIZE) != SOX_SUCCESS)
      return SOX_EOF;
    level = get_le(h + DESCRIPTOR_SIZE, 2);
    flags = get_le(h + DESCRIPTOR_SIZE + 2, 2);
    p->frame_blocks = get_le(h + DESCRIPTOR_SIZE + 4, 4);
    p->final_blocks = get_le(h + DESCRIPTOR_SIZE + 8, 4);
    p->frames = get_le(h + DESCRIPTOR_SIZE + 12, 4);
    bits = get_le(h + DESCRIPTOR_SIZE + 16, 2);
    params->channels = get_le(h + DESCRIPTOR_SIZE + 18, 2);
    params->rate = get_le(h + DESCRIPTOR_SIZE + 20, 4);
    at = start + descriptor_size + header_size;
    p->first = at + table_size + header_data_size;
    p->end = data_size? p->first + data_size : 0;
  }
  else {
    sox_uint32_t header_data_size;

    if (read_bytes(ft, h + 6, OLD_HEADER_SIZE - 6) != SOX_SUCCESS)
      return SOX_EOF;
    level = get_le(h + 6, 2);
    flags = get_le(h + 8, 2);
    params->channels = get_le(h + 10, 2);
    params->rate = get_le(h + 12, 4);
    header_data_size = get_le(h + 16, 4);
    tail = get_le(h + 20, 4);
    p->frames = get_le(h + 24, 4);
    p->final_blocks = get_le(h + 28, 4);
    at = start + OLD_HEADER_SIZE;
    if (flags & FLAG_PEAK_LEVEL) {
      if (lsx_skipbytes(ft, 4) != SOX_SUCCESS)
        return SOX_EOF;
      at += 4;
    }
    table_size = (size_t)p->frames * 4;
    if (flags & FLAG_SEEK_ELEMENTS) {
      if (read_bytes(ft, h, 4) != SOX_SUCCESS)
        return SOX_EOF;
      table_size = (size_t)get_le(h, 4) * 4;
      at += 4;
    }
    bits = flags & FLAG_8_BIT? 8 : flags & FLAG_24_BIT? 24 : 16;
    p->frame_blocks = p->version >= 3950? 73728 * 4 : 73728;
    if (!(flags & FLAG_CREATE_WAV_HEADER)) {
      if (lsx_skipbytes(ft, header_data_size) != SOX_SUCCESS)
        return SOX_EOF;
      at += header_data_size;
    }
    p->first = at + table_size;
  }
  params->bits_per_sample = bits;
  params->encoding = flags & FLAG_FLOATING_POINT? SOX_ENCODING_APEF : SOX_ENCODING_APE;
  params->private_size = PRIVATE_SIZE;
  params->private_data = lsx_malloc(params->private_size);
  put_le(params->private_data, p->version, 2);
  put_le(params->private_data + 2, level, 2);
  put_le(params->private_data + 4, flags, 2);
  put_le(params->private_data + 6, p->frame_blocks, 4);
  if (!p->frames || !p->frame_blocks || p->final_blocks > p->frame_blocks || table_size / 4 < p->frames) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Monkey's Audio header");
    return SOX_EOF;
  }
  p->starts = lsx_malloc(p->frames * sizeof(*p->starts));
  for (i = 0; i < table_size / 4; ++i) {
    if (read_bytes(ft, h, 4) != SOX_SUCCESS)
      return SOX_EOF;
    if (i < p->frames)
      p->starts[i] = i? start + get_le(h, 4) : p->first;
    if (i && i < p->frames && p->starts[i] < p->starts[i - 1]) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid Monkey's Audio seek table");
      return SOX_EOF;
    }
  }
  at += table_size;
  if (p->end && p->end < p->starts[p->frames - 1]) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Monkey's Audio header");
    return SOX_EOF;
  }
  if (!p->end && ft->seekable) {
    sox_uint64_t size = lsx_filelength(ft);

    if (size > p->starts[p->frames - 1] + tail)
      p->end = size - tail - (size - tail - p->starts[p->frames - 1]) % 4;
  }
  p->buffer_pos = at;
  return SOX_SUCCESS;
}

/* A frame from where the word it starts in begins to the word after the
 * one it ends in, the buffer keeping what was read beyond the frame
 * before. */
static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec UNUSED, lsx_packet_t * packet)
{
  priv_t * p = v;
  sox_uint32_t i = p->next;
  sox_uint64_t start, end;
  size_t skip, want, blocks = i + 1 < p->frames? p->frame_blocks : p->final_blocks;

  if (i >= p->frames)
    return SOX_EOF;
  skip = (size_t)((p->starts[i] - p->first) & 3);
  start = p->starts[i] - skip;
  end = i + 1 < p->frames? p->starts[i + 1] : p->end? p->end : start + (sox_uint64_t)p->final_blocks * 8;
  end = start + ((end - start + 3) & ~(sox_uint64_t)3) + (i + 1 < p->frames? 4 : 0);
  if (start < p->buffer_pos) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Monkey's Audio seek table");
    return SOX_EOF;
  }
  if (start - p->buffer_pos >= p->buffer_length) {
    if (lsx_skipbytes(ft, (size_t)(start - p->buffer_pos - p->buffer_length)) != SOX_SUCCESS) {
      lsx_fail_errno(ft, SOX_EHDR, "Monkey's Audio file truncated");
      return SOX_EOF;
    }
    p->buffer_length = 0;
  }
  else {
    size_t drop = (size_t)(start - p->buffer_pos);

    p->buffer_length -= drop;
    memmove(p->buffer + PACKET_HEADER_SIZE, p->buffer + PACKET_HEADER_SIZE + drop, p->buffer_length);
  }
  p->buffer_pos = start;
  want = (size_t)(end - start);
  if (PACKET_HEADER_SIZE + want > p->buffer_size) {
    p->buffer_size = PACKET_HEADER_SIZE + want;
    p->buffer = lsx_realloc(p->buffer, p->buffer_size);
  }
  if (want > p->buffer_length)
    p->buffer_length += lsx_readbuf(ft, p->buffer + PACKET_HEADER_SIZE + p->buffer_length,
        want - p->buffer_length);
  if (p->buffer_length < want && (i + 1 < p->frames || p->end)) {
    lsx_fail_errno(ft, SOX_EHDR, "Monkey's Audio file truncated");
    return SOX_EOF;
  }
  put_le(p->buffer, (sox_uint32_t)blocks, 4);
  put_le(p->buffer + 4, (sox_uint32_t)skip, 4);
  packet->data = p->buffer;
  packet->size = PACKET_HEADER_SIZE + (min(want, p->buffer_length) & ~(size_t)3);
  packet->frames = blocks;
  packet->skip = p->seek_skip;
  packet->discard = 0;
  packet->duration = blocks;
  p->seek_skip = 0;
  ++p->next;
  return SOX_SUCCESS;
}

static int length(sox_format_t * ft UNUSED, void * v, lsx_codec_t * codec UNUSED, sox_uint64_t * frames)
{
  priv_t * p = v;

  *frames = (sox_uint64_t)(p->frames - 1) * p->frame_blocks + p->final_blocks;
  return SOX_SUCCESS;
}

/* The codec starts over at the frame that holds the place sought, the
 * blocks before it being skipped. */
static int seek(sox_format_t * ft, void * v, lsx_codec_t * codec, sox_uint64_t frame)
{
  priv_t * p = v;
  sox_uint32_t i = (sox_uint32_t)min(frame / p->frame_blocks, p->frames - 1);
  lsx_codec_params_t params = codec->params;
  sox_uint64_t start = p->starts[i] - ((p->starts[i] - p->first) & 3);

  if (lsx_seeki(ft, (off_t)start, SEEK_SET) != SOX_SUCCESS)
    return SOX_EOF;
  p->buffer_pos = start;
  p->buffer_length = 0;
  p->next = i;
  p->seek_skip = (size_t)(frame - (sox_uint64_t)i * p->frame_blocks);
  params.private_data = lsx_memdup(params.private_data, params.private_size);
  return lsx_container_restart(ft, &params, sox_false);
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  priv_t * p = v;

  free(p->starts);
  free(p->buffer);
}

/*--------------------------------- Writing ---------------------------------*/

static int open_write(sox_format_t * ft, void * v, lsx_codec_t const * codec, lsx_codec_tag_t const * tag UNUSED)
{
  priv_t * p = v;
  lsx_codec_params_t const * params = &codec->params;
  sox_uint64_t blocks = ft->signal.length / params->channels;

  if (!ft->seekable) {
    lsx_fail_errno(ft, SOX_EINVAL, "Monkey's Audio output must be a file, not a pipe");
    return SOX_EOF;
  }
  p->frame_blocks = get_le(params->private_data + 6, 4);
  p->reserved = (size_t)((blocks + p->frame_blocks - 1) / p->frame_blocks);
  put_le(p->header, get_le(params->private_data + 2, 2), 2);
  put_le(p->header + 2, get_le(params->private_data + 4, 2), 2);
  put_le(p->header + 4, p->frame_blocks, 4);
  put_le(p->header + 16, params->bits_per_sample, 2);
  put_le(p->header + 18, params->channels, 2);
  put_le(p->header + 20, (sox_uint32_t)params->rate, 4);
  p->wav_header_size = get_le(params->private_data + 4, 2) & FLAG_CREATE_WAV_HEADER? 0 : WAV_HEADER_SIZE;
  return lsx_padbytes(ft, DESCRIPTOR_SIZE + HEADER_SIZE + p->reserved * 4 + p->wav_header_size);
}

static int write_packet(sox_format_t * ft, void * v, lsx_packet_t const * packet)
{
  priv_t * p = v;
  size_t size = packet->size - PACKET_HEADER_SIZE;

  if (p->frames && p->blocks != p->frame_blocks) {
    lsx_fail_errno(ft, SOX_EOF, "Monkey's Audio frame of %lu blocks before the last", (unsigned long)p->blocks);
    return SOX_EOF;
  }
  if (p->frames == p->offsets_size) {
    p->offsets_size = max(64, p->offsets_size * 2);
    p->offsets = lsx_realloc(p->offsets, p->offsets_size * sizeof(*p->offsets));
  }
  p->offsets[p->frames++] = p->data_bytes;
  p->blocks = get_le(packet->data, 4);
  p->data_bytes += size;
  return lsx_writebuf(ft, packet->data + PACKET_HEADER_SIZE, size) == size? SOX_SUCCESS : SOX_EOF;
}

/* The WAVE header that floating-point files keep, as SoX writes one: the
 * SDK makes up an integer one for files without, and decoded float would be
 * labelled integer. */
static void put_wav_header(priv_t const * p, unsigned char * h)
{
  sox_uint32_t channels = get_le(p->header + 18, 2), rate = get_le(p->header + 20, 4);
  sox_uint32_t blocks = p->frames? (p->frames - 1) * p->frame_blocks + p->blocks : 0;
  sox_uint32_t align = channels * 4;

  memcpy(h, "RIFF", 4);
  put_le(h + 4, WAV_HEADER_SIZE - 8 + blocks * align, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  put_le(h + 16, 18, 4);
  put_le(h + 20, WAVE_FORMAT_IEEE_FLOAT, 2);
  put_le(h + 22, channels, 2);
  put_le(h + 24, rate, 4);
  put_le(h + 28, rate * align, 4);
  put_le(h + 32, align, 2);
  put_le(h + 34, 32, 2);
  put_le(h + 36, 0, 2);
  memcpy(h + 38, "fact", 4);
  put_le(h + 42, 4, 4);
  put_le(h + 46, blocks, 4);
  memcpy(h + 50, "data", 4);
  put_le(h + 54, blocks * align, 4);
}

/* The final word goes after the frames, and if more frames were written
 * than the seek table has room for, they are moved down to make room, last
 * chunk first.  The MD5 is of the WAVE header kept, the frame data, read
 * back, the stream header and the seek table, in that order. */
static int close_write(sox_format_t * ft, void * v, lsx_codec_t const * codec UNUSED)
{
  static unsigned char const final_word[4];
  priv_t * p = v;
  unsigned char descriptor[DESCRIPTOR_SIZE], wav[WAV_HEADER_SIZE], * table = NULL, * chunk = lsx_malloc(SHIFT_CHUNK);
  sox_uint64_t data_start = DESCRIPTOR_SIZE + HEADER_SIZE + (sox_uint64_t)p->reserved * 4 + p->wav_header_size, pos;
  lsx_md5_t md5;
  size_t i;
  int status = SOX_EOF;

  if (lsx_writebuf(ft, final_word, 4) != 4)
    goto end;
  p->data_bytes += 4;
  if (p->frames > p->reserved) {
    size_t shift = (p->frames - p->reserved) * 4;
    sox_bool moved = sox_true;

    pos = data_start + p->data_bytes;
    while (moved && pos > data_start) {
      size_t n = (size_t)min(SHIFT_CHUNK, pos - data_start);

      pos -= n;
      if (lsx_seeki(ft, (off_t)pos, SEEK_SET) != SOX_SUCCESS || lsx_readbuf(ft, chunk, n) != n ||
          lsx_seeki(ft, (off_t)(pos + shift), SEEK_SET) != SOX_SUCCESS || lsx_writebuf(ft, chunk, n) != n)
        moved = sox_false;
    }
    if (!moved) {
      lsx_fail_errno(ft, SOX_EOF, "can't make room for the Monkey's Audio seek table");
      goto end;
    }
    p->reserved = p->frames;
    data_start += shift;
  }
  if (p->frames && data_start + p->offsets[p->frames - 1] > 0xffffffff) {
    lsx_fail_errno(ft, SOX_EOF, "Monkey's Audio file too long");
    goto end;
  }
  put_le(p->header + 8, p->frames? p->blocks : 0, 4);
  put_le(p->header + 12, (sox_uint32_t)p->frames, 4);
  table = lsx_malloc(p->reserved * 4);
  for (i = 0; i < p->reserved; ++i)
    put_le(table + 4 * i, i < p->frames? (sox_uint32_t)(data_start + p->offsets[i]) : 0, 4);
  if (p->wav_header_size)
    put_wav_header(p, wav);
  if (lsx_seeki(ft, (off_t)DESCRIPTOR_SIZE, SEEK_SET) != SOX_SUCCESS ||
      lsx_writebuf(ft, p->header, HEADER_SIZE) != HEADER_SIZE ||
      lsx_writebuf(ft, table, p->reserved * 4) != p->reserved * 4 ||
      lsx_writebuf(ft, wav, p->wav_header_size) != p->wav_header_size)
    goto end;
  lsx_md5_init(&md5);
  lsx_md5_add(&md5, wav, p->wav_header_size);
  for (pos = 0; pos < p->data_bytes; pos += SHIFT_CHUNK) {
    size_t n = (size_t)min(SHIFT_CHUNK, p->data_bytes - pos);

    if (lsx_readbuf(ft, chunk, n) != n) {
      lsx_fail_errno(ft, SOX_EOF, "can't read back the Monkey's Audio frames");
      goto end;
    }
    lsx_md5_add(&md5, chunk, n);
  }
  lsx_md5_add(&md5, p->header, HEADER_SIZE);
  lsx_md5_add(&md5, table, p->reserved * 4);
  memset(descriptor, 0, sizeof(descriptor));
  memcpy(descriptor, get_le(p->header + 2, 2) & FLAG_FLOATING_POINT? "MACF" : "MAC ", 4);
  put_le(descriptor + 4, WRITE_VERSION, 2);
  put_le(descriptor + 8, DESCRIPTOR_SIZE, 4);
  put_le(descriptor + 12, HEADER_SIZE, 4);
  put_le(descriptor + 16, (sox_uint32_t)(p->reserved * 4), 4);
  put_le(descriptor + 20, p->wav_header_size, 4);
  put_le(descriptor + 24, (sox_uint32_t)p->data_bytes, 4);
  put_le(descriptor + 28, (sox_uint32_t)(p->data_bytes >> 32), 4);
  lsx_md5_final(&md5, descriptor + 36);
  if (lsx_seeki(ft, 0, SEEK_SET) == SOX_SUCCESS && lsx_writebuf(ft, descriptor, DESCRIPTOR_SIZE) == DESCRIPTOR_SIZE)
    status = SOX_SUCCESS;

end:
  free(table);
  free(chunk);
  free(p->offsets);
  return status;
}

static lsx_container_ops_t const ape_ops = {
  sizeof(priv_t),
  open_read, read_packet, NULL, length, seek, close_read,
  open_write, write_packet, close_write
};

static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &ape_ops);
}

static int startwrite(sox_format_t * ft)
{
  return lsx_container_startwrite(ft, &ape_ops);
}

LSX_FORMAT_HANDLER(ape)
{
  static char const * const names[] = {"ape", NULL};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Monkey's Audio; -C is the compression level, 1 fast, 2 normal (the default), "
    "3 high, 4 extra high, 5 insane",
    names, SOX_FILE_CONTAINER,
    startread, lsx_container_read, lsx_container_stopread,
    startwrite, lsx_container_write, lsx_container_stopwrite,
    lsx_container_seek, NULL, NULL, sizeof(lsx_container_t)
  };
  return &handler;
}
