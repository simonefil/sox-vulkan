/* libSoX packet codec: Monkey's Audio, with the Monkey's Audio SDK.
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

/* A packet is a frame: the blocks it holds and the bytes of its first
 * 32-bit word that precede it, both 32-bit little-endian, then the whole
 * 32-bit words of the file that hold it and the word after, which the
 * decoder may read into; at the end of the file that word may be missing.
 * Frames written here start on a word, so they skip nothing.  The private
 * data is what the decoder needs from the file header: the file version,
 * the compression level and the format flags, 16-bit little-endian each,
 * and the blocks of a frame, 32-bit. */

#include "codec.h"
#include "ape-mac.h"

#include <math.h>
#include <string.h>

#define PRIVATE_SIZE 10
#define PACKET_HEADER_SIZE 8
#define FILE_VERSION 3990
#define MIN_FILE_VERSION 3930
#define FLAG_CREATE_WAV_HEADER 32
#define FLAG_FLOATING_POINT 4096

typedef struct {
  lsx_ape_decoder_t * decoder;
  lsx_ape_encoder_t * encoder;
  unsigned frame_blocks, block_bytes;
  sox_bool floating_point;
  unsigned char * pcm;      /* the frame being filled, as a WAVE file holds it */
  size_t pcm_size;
  unsigned char * packet;
  size_t packet_alloc;
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

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t const * params = &codec->params;
  unsigned version, bits = params->bits_per_sample;

  if (params->private_size != PRIVATE_SIZE) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "Monkey's Audio header missing");
    return SOX_EOF;
  }
  version = get_le(params->private_data, 2);
  if (version < MIN_FILE_VERSION) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "Monkey's Audio file version %u.%02u is older than the decoder reads",
        version / 1000, version % 1000 / 10);
    return SOX_EOF;
  }
  p->floating_point = (get_le(params->private_data + 4, 2) & FLAG_FLOATING_POINT) != 0;
  if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) || (p->floating_point && bits != 32) ||
      !params->channels || params->channels > 32) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "invalid Monkey's Audio header");
    return SOX_EOF;
  }
  p->frame_blocks = get_le(params->private_data + 6, 4);
  p->block_bytes = params->channels * bits / 8;
  p->decoder = lsx_ape_decoder_create(version, (int)get_le(params->private_data + 2, 2), params->channels, bits,
      (unsigned)params->rate, p->frame_blocks, p->floating_point);
  if (!p->decoder) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "can't start the Monkey's Audio decoder for this file");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  unsigned bytes = codec->params.bits_per_sample / 8;
  unsigned char const * pcm;
  sox_sample_t * out;
  size_t blocks, i;

  if (!data)
    return SOX_SUCCESS;
  blocks = size < PACKET_HEADER_SIZE? 0 : get_le(data, 4);
  if (!blocks || lsx_ape_decoder_decode(p->decoder, data + PACKET_HEADER_SIZE, size - PACKET_HEADER_SIZE,
        get_le(data + 4, 4), (unsigned)blocks, &pcm)) {
    lsx_fail_errno(codec->ft, SOX_EOF, "Monkey's Audio frame does not decode");
    return SOX_EOF;
  }
  out = lsx_codec_output(codec, blocks * codec->params.channels);
  for (i = 0; i < blocks * codec->params.channels; ++i, pcm += bytes) {
    if (p->floating_point) {
      sox_uint32_t word = get_le(pcm, 4);
      float value;

      memcpy(&value, &word, sizeof(value));
      out[i] = SOX_FLOAT_32BIT_TO_SAMPLE(value, codec->ft->clips);
    }
    else out[i] = bytes == 1? SOX_UNSIGNED_8BIT_TO_SAMPLE(*pcm,) : SOX_SIGNED_TO_SAMPLE(8 * bytes, get_le(pcm, (int)bytes));
  }
  return SOX_SUCCESS;
}

static void stop_decode(lsx_codec_t * codec)
{
  lsx_ape_decoder_destroy(((priv_t *)codec->priv)->decoder);
}

/* -C is the SDK's level divided by a thousand: 1 fast, 2 normal, which is
 * the default as in the SDK's own program, 3 high, 4 extra high, 5 insane.
 * The two highest make frames four and sixteen times longer. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;
  int level = 2;

  if (ft->encoding.compression != HUGE_VAL) {
    level = (int)ft->encoding.compression;
    if (level != ft->encoding.compression || level < 1 || level > 5) {
      lsx_fail_errno(ft, SOX_EINVAL, "Monkey's Audio compression level must be a whole number from 1 to 5");
      return SOX_EOF;
    }
  }
  if (params->channels > 32) {
    lsx_fail_errno(ft, SOX_EFMT, "Monkey's Audio holds at most 32 channels");
    return SOX_EOF;
  }
  p->frame_blocks = 73728 * (level == 4? 4 : level == 5? 16 : 1);
  p->block_bytes = params->channels * params->bits_per_sample / 8;
  p->floating_point = params->encoding == SOX_ENCODING_APEF;
  p->encoder = lsx_ape_encoder_create(params->channels, params->bits_per_sample, (unsigned)params->rate,
      level * 1000, p->frame_blocks, p->floating_point);
  if (!p->encoder) {
    lsx_fail_errno(ft, SOX_ENOMEM, "can't start the Monkey's Audio encoder");
    return SOX_EOF;
  }
  p->pcm = lsx_malloc((size_t)p->frame_blocks * p->block_bytes);
  params->private_data = lsx_malloc(PRIVATE_SIZE);
  params->private_size = PRIVATE_SIZE;
  put_le(params->private_data, FILE_VERSION, 2);
  put_le(params->private_data + 2, (sox_uint32_t)level * 1000, 2);
  put_le(params->private_data + 4, p->floating_point? FLAG_FLOATING_POINT : FLAG_CREATE_WAV_HEADER, 2);
  put_le(params->private_data + 6, p->frame_blocks, 4);
  params->delay = 0;
  return SOX_SUCCESS;
}

static int encode_frame(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  unsigned char const * frame;
  size_t frame_size, blocks = p->pcm_size / p->block_bytes;
  lsx_packet_t packet;

  if (lsx_ape_encoder_encode(p->encoder, p->pcm, p->pcm_size, &frame, &frame_size)) {
    lsx_fail_errno(codec->ft, SOX_EOF, "Monkey's Audio encoder failed");
    return SOX_EOF;
  }
  p->pcm_size = 0;
  if (PACKET_HEADER_SIZE + frame_size > p->packet_alloc) {
    p->packet_alloc = PACKET_HEADER_SIZE + frame_size;
    p->packet = lsx_realloc(p->packet, p->packet_alloc);
  }
  put_le(p->packet, (sox_uint32_t)blocks, 4);
  put_le(p->packet + 4, 0, 4);
  memcpy(p->packet + PACKET_HEADER_SIZE, frame, frame_size);
  packet.data = p->packet;
  packet.size = PACKET_HEADER_SIZE + frame_size;
  packet.frames = blocks;
  packet.skip = packet.discard = packet.duration = 0;
  return codec->write_packet(codec, &packet);
}

/* Samples go into the frame as a WAVE file would hold them, and a frame is
 * encoded when full, or at the end however short. */
static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  unsigned bytes = codec->params.bits_per_sample / 8;
  size_t i;
  SOX_SAMPLE_LOCALS;

  if (!buf)
    return p->pcm_size? encode_frame(codec) : SOX_SUCCESS;
  for (i = 0; i < len; ++i) {
    unsigned char * out = p->pcm + p->pcm_size;

    if (p->floating_point) {
      float value = SOX_SAMPLE_TO_FLOAT_32BIT(buf[i], codec->ft->clips);
      sox_uint32_t word;

      memcpy(&word, &value, sizeof(word));
      put_le(out, word, 4);
    }
    else switch (bytes) {
      case 1: *out = SOX_SAMPLE_TO_UNSIGNED_8BIT(buf[i], codec->ft->clips); break;
      case 2: put_le(out, (sox_uint32_t)SOX_SAMPLE_TO_SIGNED_16BIT(buf[i], codec->ft->clips), 2); break;
      case 3: put_le(out, (sox_uint32_t)SOX_SAMPLE_TO_SIGNED_24BIT(buf[i], codec->ft->clips), 3); break;
      default: put_le(out, (sox_uint32_t)SOX_SAMPLE_TO_SIGNED_32BIT(buf[i], codec->ft->clips), 4); break;
    }
    p->pcm_size += bytes;
    if (p->pcm_size == (size_t)p->frame_blocks * p->block_bytes && encode_frame(codec) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  lsx_ape_encoder_destroy(p->encoder);
  free(p->pcm);
  free(p->packet);
}

lsx_codec_handler_t const * lsx_ape_codec_fn(void)
{
  static unsigned const formats[] = {SOX_ENCODING_APE, 16, 24, 8, 32, 0, SOX_ENCODING_APEF, 32, 0, 0};
  static lsx_codec_handler_t const handler = {
    "ape", formats, NULL, sizeof(priv_t),
    start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode
  };

  return &handler;
}
