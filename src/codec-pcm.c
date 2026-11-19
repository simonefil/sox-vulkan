/* libSoX packet codec: linear PCM, integer and floating point.
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

#include "codec.h"

#include <string.h>

/* Frames per packet written.  Fixed, so that the packets do not depend on
 * how the samples happen to arrive. */
#define FRAMES_PER_PACKET 4096

typedef struct {
  unsigned bytes_per_sample;
  unsigned char * packet;
  size_t packet_samples; /* samples held in packet */
} priv_t;

static int start_decode(lsx_codec_t * codec)
{
  lsx_codec_params_t const * params = &codec->params;
  sox_bool ok;

  switch (params->encoding) {
    case SOX_ENCODING_SIGN2:
    case SOX_ENCODING_UNSIGNED:
      ok = params->bits_per_sample >= 8 && params->bits_per_sample <= 32 &&
          !(params->bits_per_sample & 7);
      break;
    case SOX_ENCODING_FLOAT:
      ok = params->bits_per_sample == 32 || params->bits_per_sample == 64;
      break;
    default:
      ok = sox_false;
  }
  if (!ok || !params->channels) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "PCM with %u-bit %s samples and %u channels is not supported",
        params->bits_per_sample, sox_encodings_info[params->encoding].desc, params->channels);
    return SOX_EOF;
  }
  ((priv_t *)codec->priv)->bytes_per_sample = params->bits_per_sample >> 3;
  return SOX_SUCCESS;
}

static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t const * p = codec->priv;
  lsx_codec_params_t const * params = &codec->params;
  unsigned width = p->bytes_per_sample;
  size_t len = size / width;
  sox_sample_t * out;
  size_t i;

  if (!data)
    return SOX_SUCCESS;
  if (size % ((size_t)width * params->channels)) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "PCM packet of %lu bytes does not hold whole frames",
        (unsigned long)size);
    return SOX_EOF;
  }
  out = lsx_codec_output(codec, len);
  for (i = 0; i < len; ++i, data += width) {
    sox_uint64_t word = 0;
    unsigned b;

    for (b = 0; b < width; ++b)
      word |= (sox_uint64_t)data[params->big_endian? width - 1 - b : b] << (8 * b);
    if (params->encoding == SOX_ENCODING_FLOAT) {
      if (width == 4) {
        sox_uint32_t word32 = (sox_uint32_t)word;
        float value;

        memcpy(&value, &word32, sizeof(value));
        out[i] = SOX_FLOAT_32BIT_TO_SAMPLE(value,);
      } else {
        double value;

        memcpy(&value, &word, sizeof(value));
        out[i] = SOX_FLOAT_64BIT_TO_SAMPLE(value,);
      }
    }
    else if (params->encoding == SOX_ENCODING_UNSIGNED)
      out[i] = SOX_UNSIGNED_TO_SAMPLE(params->bits_per_sample, word);
    else out[i] = SOX_SIGNED_TO_SAMPLE(params->bits_per_sample, word);
  }
  return SOX_SUCCESS;
}

static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  if (start_decode(codec) != SOX_SUCCESS)
    return SOX_EOF;
  p->packet = lsx_malloc((size_t)FRAMES_PER_PACKET * codec->params.channels * p->bytes_per_sample);
  return SOX_SUCCESS;
}

static int emit_packet(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_packet_t packet;

  packet.data = p->packet;
  packet.size = p->packet_samples * p->bytes_per_sample;
  packet.frames = p->packet_samples / codec->params.channels;
  packet.discard = 0;
  p->packet_samples = 0;
  return codec->write_packet(codec, &packet);
}

static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t const * params = &codec->params;
  unsigned width = p->bytes_per_sample;
  size_t packet_samples = (size_t)FRAMES_PER_PACKET * params->channels;
  size_t i;

  if (!buf)
    return p->packet_samples? emit_packet(codec) : SOX_SUCCESS;
  for (i = 0; i < len; ++i) {
    unsigned char * dst = p->packet + p->packet_samples * width;
    sox_uint64_t word;
    unsigned b;
    SOX_SAMPLE_LOCALS;

    if (params->encoding == SOX_ENCODING_FLOAT) {
      if (width == 4) {
        float value = SOX_SAMPLE_TO_FLOAT_32BIT(buf[i], codec->ft->clips);
        sox_uint32_t word32;

        memcpy(&word32, &value, sizeof(word32));
        word = word32;
      } else {
        double value = SOX_SAMPLE_TO_FLOAT_64BIT(buf[i], codec->ft->clips);

        memcpy(&word, &value, sizeof(word));
      }
    }
    else switch (params->bits_per_sample) {
      case 8: word = SOX_SAMPLE_TO_UNSIGNED_8BIT(buf[i], codec->ft->clips); break;
      case 16: word = (sox_uint16_t)SOX_SAMPLE_TO_SIGNED_16BIT(buf[i], codec->ft->clips); break;
      case 24: word = (sox_uint32_t)SOX_SAMPLE_TO_SIGNED_24BIT(buf[i], codec->ft->clips); break;
      default: word = (sox_uint32_t)SOX_SAMPLE_TO_SIGNED_32BIT(buf[i], codec->ft->clips); break;
    }
    for (b = 0; b < width; ++b)
      dst[params->big_endian? width - 1 - b : b] = (unsigned char)(word >> (8 * b));
    if (++p->packet_samples == packet_samples && emit_packet(codec) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static void stop_encode(lsx_codec_t * codec)
{
  free(((priv_t *)codec->priv)->packet);
}

lsx_codec_handler_t const * lsx_pcm_codec_fn(void)
{
  /* Unsigned at 8 bits only, which is how containers store 8-bit PCM. */
  static unsigned const write_formats[] = {
    SOX_ENCODING_SIGN2, 16, 24, 32, 0,
    SOX_ENCODING_UNSIGNED, 8, 0,
    SOX_ENCODING_FLOAT, 32, 64, 0,
    0};
  static lsx_codec_handler_t const handler = {
    "pcm", write_formats, sizeof(priv_t),
    start_decode, decode, NULL,
    start_encode, encode, stop_encode
  };
  return &handler;
}
