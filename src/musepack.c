/* libSoX Musepack container, stream versions 7 and 8.
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

/* An SV7 file is a header of six little-endian 32-bit words, the first
 * holding "MP+" and the version, then the frames back to back as a stream of
 * bits, 32-bit words in file order and each read from its most significant
 * bit: a frame begins with its length in bits, 20 bits wide, and ends
 * part-way into a word, which the next frame begins in.  The header gives
 * the frame count, and whether the last frame holds fewer than 1152 samples,
 * and how many.
 *
 * An SV8 file is "MPCK" and then blocks, each a two-letter key and its size,
 * which counts the key and the size itself; the size, like every count in
 * the format, is written seven bits a byte, most significant first, with the
 * top bit set on all bytes but the last.  The stream header block, SH, gives
 * the samples, those at the start that precede the signal, and the stream
 * parameters; each audio packet block, AP, holds a power of four frames,
 * fewer in the last; and the stream end block, SE, closes the stream.  The
 * other blocks, for seeking, replay gain and the encoder, are passed over.
 *
 * Every frame decodes to 1152 samples, the decoder's synthesis filter
 * putting 481 of delay ahead of the signal, and the samples after its end
 * are padding; the decoder leaves both out, knowing the samples from the
 * header.  Tags, APE or ID3v1, follow the stream and are left unread, as is
 * an ID3v2 tag in front.  Reading goes on in file order, so that a pipe will
 * do.
 *
 * SV8 is written, with the stream header rewritten at the end once the
 * samples are known, so the output must be a file. */

#include "container.h"

#include <string.h>

#define FRAME_SAMPLES 1152
#define SYNTHESIS_DELAY 481
#define SV7_HEADER_SIZE 24
#define SV7_PACKET_HEADER_SIZE 4
#define SV8_PARAMETERS_SIZE 2
#define SH_PAYLOAD_SIZE 16  /* as written: the sample count eight bytes wide */

static sox_rate_t const rates[] = {44100, 48000, 37800, 32000};

typedef struct {
  sox_bool sv8;
  sox_uint64_t frames;      /* SV7 */
  sox_uint64_t samples;     /* of the signal */
  unsigned bits;            /* SV7: bits of the next frame's first word that precede it */
  sox_uint64_t next;        /* SV7: frame */
  unsigned char * buffer;   /* SV7: the packet header, then the file */
  size_t buffer_size, buffer_length, buffer_start;
  sox_bool ended;
} priv_t;

static sox_uint32_t get_le32(unsigned char const * b)
{
  return b[0] | (sox_uint32_t)b[1] << 8 | (sox_uint32_t)b[2] << 16 | (sox_uint32_t)b[3] << 24;
}

/* A count as SV8 writes it, n bytes wide, or as few as it takes if n is 0;
 * the bytes it took. */
static size_t put_count(unsigned char * b, sox_uint64_t value, size_t n)
{
  size_t i;

  if (!n)
    for (n = 1; n < 10 && value >> (7 * n); ++n);
  for (i = 0; i < n; ++i)
    b[i] = (unsigned char)((value >> (7 * (n - 1 - i)) & 0x7f) | (i + 1 < n? 0x80 : 0));
  return n;
}

/*--------------------------------- Reading ---------------------------------*/

static int truncated(sox_format_t * ft)
{
  lsx_fail_errno(ft, SOX_EHDR, "Musepack file truncated");
  return SOX_EOF;
}

static int read_count(sox_format_t * ft, sox_uint64_t * value, size_t * size)
{
  unsigned char b;

  *value = 0;
  *size = 0;
  do {
    if (lsx_readbuf(ft, &b, 1) != 1 || ++*size > 9)
      return truncated(ft);
    *value = *value << 7 | (b & 0x7f);
  } while (b & 0x80);
  return SOX_SUCCESS;
}

/* The key of the next SV8 block and the size of what follows its header. */
static int read_block(sox_format_t * ft, char key[2], sox_uint64_t * size)
{
  size_t count_size;

  if (lsx_readbuf(ft, key, 2) != 2 || read_count(ft, size, &count_size) != SOX_SUCCESS)
    return truncated(ft);
  if (*size < 2 + count_size) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid Musepack block");
    return SOX_EOF;
  }
  *size -= 2 + count_size;
  return SOX_SUCCESS;
}

/* The bytes of the file from the buffer's start up to size, or fewer at the
 * end of the file; what precedes the start is let go. */
static size_t fill(sox_format_t * ft, priv_t * p, size_t size)
{
  if (p->buffer_start) {
    p->buffer_length -= p->buffer_start;
    memmove(p->buffer + SV7_PACKET_HEADER_SIZE, p->buffer + SV7_PACKET_HEADER_SIZE + p->buffer_start,
        p->buffer_length);
    p->buffer_start = 0;
  }
  if (SV7_PACKET_HEADER_SIZE + size > p->buffer_size) {
    p->buffer_size = SV7_PACKET_HEADER_SIZE + size;
    p->buffer = lsx_realloc(p->buffer, p->buffer_size);
  }
  if (size > p->buffer_length)
    p->buffer_length += lsx_readbuf(ft, p->buffer + SV7_PACKET_HEADER_SIZE + p->buffer_length,
        size - p->buffer_length);
  return min(size, p->buffer_length);
}

/* The stream header of either version, of which the decoder is given what
 * follows the magic and, for SV8, the CRC and the version. */
static int open_read(sox_format_t * ft, void * v, lsx_codec_params_t * params, char const * * codec_name)
{
  priv_t * p = v;
  unsigned char h[SV7_HEADER_SIZE];
  sox_uint64_t start, signal = 0, silence = 0;

  if (lsx_container_read_magic(ft, h, &start) != SOX_SUCCESS)
    return SOX_EOF;
  if (!memcmp(h, "MP+", 3)) {
    sox_uint32_t word;

    if ((h[3] & 0x0f) != 7) {
      lsx_fail_errno(ft, SOX_EFMT, "Musepack SV%u is unsupported", h[3] & 0x0f);
      return SOX_EOF;
    }
    if (lsx_readbuf(ft, h + 4, SV7_HEADER_SIZE - 4) != SV7_HEADER_SIZE - 4)
      return truncated(ft);
    *codec_name = "mpc7";
    p->frames = get_le32(h + 4);
    if (!p->frames) {
      lsx_fail_errno(ft, SOX_EHDR, "Musepack file holds no frames");
      return SOX_EOF;
    }
    params->rate = rates[h[10] & 3];
    params->channels = 2;
    params->private_size = SV7_HEADER_SIZE - 4;
    params->private_data = lsx_memdup(h + 4, params->private_size);
    word = get_le32(h + 20);
    signal = p->frames * FRAME_SAMPLES - (word >> 31? FRAME_SAMPLES - ((word >> 20 & 0x7ff)? word >> 20 & 0x7ff :
        FRAME_SAMPLES) : SYNTHESIS_DELAY);
    p->bits = 8;
  }
  else if (!memcmp(h, "MPCK", 4)) {
    char key[2];
    sox_uint64_t size;
    size_t n, count_size;

    *codec_name = "mpc8";
    p->sv8 = sox_true;
    do {
      if (read_block(ft, key, &size) != SOX_SUCCESS)
        return SOX_EOF;
      if (!memcmp(key, "AP", 2) || !memcmp(key, "SE", 2)) {
        lsx_fail_errno(ft, SOX_EHDR, "Musepack stream header missing");
        return SOX_EOF;
      }
      if (memcmp(key, "SH", 2) && lsx_skipbytes(ft, (size_t)size) != SOX_SUCCESS)
        return truncated(ft);
    } while (memcmp(key, "SH", 2));
    if (size < 5 || lsx_readbuf(ft, h, 5) != 5)
      return truncated(ft);
    if (h[4] != 8) {
      lsx_fail_errno(ft, SOX_EFMT, "Musepack SV8 stream version %u is unsupported", h[4]);
      return SOX_EOF;
    }
    if (read_count(ft, &signal, &n) != SOX_SUCCESS || read_count(ft, &silence, &count_size) != SOX_SUCCESS)
      return SOX_EOF;
    n += 5 + count_size;
    if (size < n + SV8_PARAMETERS_SIZE || lsx_readbuf(ft, h, SV8_PARAMETERS_SIZE) != SV8_PARAMETERS_SIZE ||
        lsx_skipbytes(ft, (size_t)(size - n - SV8_PARAMETERS_SIZE)) != SOX_SUCCESS)
      return truncated(ft);
    if (h[0] >> 5 >= array_length(rates)) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid Musepack sample rate");
      return SOX_EOF;
    }
    params->rate = rates[h[0] >> 5];
    params->channels = (h[1] >> 4) + 1;
    params->private_data = lsx_malloc(20 + SV8_PARAMETERS_SIZE);
    params->private_size = put_count(params->private_data, signal, 0);
    params->private_size += put_count(params->private_data + params->private_size, silence, 0);
    memcpy(params->private_data + params->private_size, h, SV8_PARAMETERS_SIZE);
    params->private_size += SV8_PARAMETERS_SIZE;
    if (silence > signal) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid Musepack stream header");
      return SOX_EOF;
    }
  }
  else {
    lsx_fail_errno(ft, SOX_EHDR, "not a Musepack file");
    return SOX_EOF;
  }
  params->encoding = SOX_ENCODING_MUSEPACK;
  p->samples = signal - silence;
  return SOX_SUCCESS;
}

static int read_packet(sox_format_t * ft, void * v, lsx_codec_t * codec UNUSED, lsx_packet_t * packet)
{
  priv_t * p = v;

  if (p->ended)
    return SOX_EOF;
  if (p->sv8) {
    char key[2];
    sox_uint64_t size;

    do {
      if (read_block(ft, key, &size) != SOX_SUCCESS)
        return SOX_EOF;
      if (!memcmp(key, "SE", 2)) {
        p->ended = sox_true;
        return SOX_EOF;
      }
      if (memcmp(key, "AP", 2) && lsx_skipbytes(ft, (size_t)size) != SOX_SUCCESS)
        return truncated(ft);
    } while (memcmp(key, "AP", 2));
    p->buffer_start = p->buffer_length;
    if (fill(ft, p, (size_t)size) != size)
      return truncated(ft);
    packet->data = p->buffer + SV7_PACKET_HEADER_SIZE;
    packet->size = (size_t)size;
  }
  else {
    sox_uint32_t length;
    unsigned bits = p->bits + 20;
    size_t size, got;

    if (p->next == p->frames)
      return SOX_EOF;
    if (fill(ft, p, 8) < (p->bits <= 12? 4u : 8u))
      return truncated(ft);
    length = (p->bits <= 12? get_le32(p->buffer + SV7_PACKET_HEADER_SIZE) >> (12 - p->bits) :
        get_le32(p->buffer + SV7_PACKET_HEADER_SIZE) << (p->bits - 12) |
        get_le32(p->buffer + SV7_PACKET_HEADER_SIZE + 4) >> (44 - p->bits)) & 0xfffff;
    size = ((length + bits + 31) & ~(sox_uint32_t)31) >> 3;
    got = fill(ft, p, size + 4);
    if (got < size)
      return truncated(ft);
    p->buffer[0] = (unsigned char)bits;
    p->buffer[1] = p->buffer[2] = p->buffer[3] = 0;
    packet->data = p->buffer;
    packet->size = SV7_PACKET_HEADER_SIZE + (got & ~(size_t)3);
    p->bits = (bits + length) & 31;
    p->buffer_start = size - (p->bits? 4 : 0);
    ++p->next;
  }
  packet->frames = packet->skip = packet->discard = packet->duration = 0;
  return SOX_SUCCESS;
}

static int length(sox_format_t * ft UNUSED, void * v, lsx_codec_t * codec UNUSED, sox_uint64_t * frames)
{
  *frames = ((priv_t *)v)->samples;
  return SOX_SUCCESS;
}

static void close_read(sox_format_t * ft UNUSED, void * v)
{
  free(((priv_t *)v)->buffer);
}

/*--------------------------------- Writing ---------------------------------*/

static int write_block(sox_format_t * ft, char const * key, unsigned char const * data, size_t size)
{
  unsigned char header[12];
  size_t n = 1;

  memcpy(header, key, 2);
  while (n < 10 && (2 + n + size) >> (7 * n))
    ++n;
  put_count(header + 2, 2 + n + size, n);
  return lsx_writebuf(ft, header, 2 + n) == 2 + n && lsx_writebuf(ft, data, size) == size?
      SOX_SUCCESS : SOX_EOF;
}

/* The stream header goes in blank, to be written at the end from the codec's
 * private data, which the encoder completes then. */
static int open_write(sox_format_t * ft, void * v UNUSED, lsx_codec_t const * codec UNUSED,
    lsx_codec_tag_t const * tag UNUSED)
{
  unsigned char payload[SH_PAYLOAD_SIZE];

  if (!ft->seekable) {
    lsx_fail_errno(ft, SOX_EINVAL, "Musepack output must be a file, not a pipe");
    return SOX_EOF;
  }
  memset(payload, 0, sizeof(payload));
  return lsx_writebuf(ft, "MPCK", 4) == 4 && write_block(ft, "SH", payload, sizeof(payload)) == SOX_SUCCESS?
      SOX_SUCCESS : SOX_EOF;
}

static int write_packet(sox_format_t * ft, void * v UNUSED, lsx_packet_t const * packet)
{
  return write_block(ft, "AP", packet->data, packet->size);
}

/* The header's CRC is the one of PNG, over what follows it in the block,
 * and is stored most significant byte first. */
static int close_write(sox_format_t * ft, void * v UNUSED, lsx_codec_t const * codec)
{
  lsx_codec_params_t const * params = &codec->params;
  unsigned char payload[SH_PAYLOAD_SIZE];
  sox_uint32_t crc = 0xffffffff;
  size_t i;
  int bit;

  if (params->private_size != SH_PAYLOAD_SIZE - 5 || write_block(ft, "SE", NULL, 0) != SOX_SUCCESS)
    return SOX_EOF;
  payload[4] = 8;
  memcpy(payload + 5, params->private_data, params->private_size);
  for (i = 4; i < SH_PAYLOAD_SIZE; ++i)
    for (crc ^= payload[i], bit = 0; bit < 8; ++bit)
      crc = crc >> 1 ^ (crc & 1? 0xedb88320 : 0);
  crc = ~crc;
  for (i = 0; i < 4; ++i)
    payload[i] = (unsigned char)(crc >> (24 - 8 * i));
  return lsx_seeki(ft, 4 + 3, SEEK_SET) == SOX_SUCCESS &&
      lsx_writebuf(ft, payload, sizeof(payload)) == sizeof(payload)? SOX_SUCCESS : SOX_EOF;
}

static lsx_container_ops_t const musepack_ops = {
  sizeof(priv_t),
  open_read, read_packet, NULL, length, NULL, close_read,
  open_write, write_packet, close_write
};

static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &musepack_ops);
}

static int startwrite(sox_format_t * ft)
{
  return lsx_container_startwrite(ft, &musepack_ops);
}

LSX_FORMAT_HANDLER(mpc)
{
  static char const * const names[] = {"mpc", NULL};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Musepack, SV7 and SV8, writing SV8; -C is the quality, 0 to 10, as mpcenc's --quality: "
    "4 radio, 5 standard (the default), 6 extreme, 7 insane",
    names, SOX_FILE_CONTAINER,
    startread, lsx_container_read, lsx_container_stopread,
    startwrite, lsx_container_write, lsx_container_stopwrite,
    NULL, NULL, NULL, sizeof(lsx_container_t)
  };
  return &handler;
}
