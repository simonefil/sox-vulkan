/* libSoX packet codec: WavPack, through libwavpack.
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

/* A packet is a WavPack frame, the blocks that together carry every channel
 * for the same samples, stripped of most of their headers as Matroska stores
 * them: the sample count once, then for each block its flags, its checksum
 * and, when there are several, its size, then its data.  The private data is
 * the stream version, two bytes, which the stripped headers leave out.  Each
 * frame decodes on its own, and the word length and whether the samples are
 * floating point are learnt from the first. */

#include "codec.h"

#include <string.h>

#ifdef HAVE_WAVPACK_H
#include <wavpack.h>
#else
#include <wavpack/wavpack.h>
#endif

#define HEADER_SIZE 32 /* of a whole block */

typedef struct {
  /* Decoding */
  int version;
  int32_t * pcm;
  size_t pcm_size;
  sox_bool have_format;

  /* Encoding */
  WavpackContext * encoder;
  unsigned char * frame;
  size_t frame_size, frame_alloc, frame_blocks;
  sox_uint32_t frame_samples;
} priv_t;

static sox_uint32_t le32(unsigned char const * b)
{
  return b[0] | (sox_uint32_t)b[1] << 8 | (sox_uint32_t)b[2] << 16 | (sox_uint32_t)b[3] << 24;
}

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t const * params = &codec->params;

  if (params->private_size < 2) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "WavPack private data holds no stream version");
    return SOX_EOF;
  }
  p->version = params->private_data[0] | params->private_data[1] << 8;
  return SOX_SUCCESS;
}

static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  char error[80];
  WavpackContext * wpc;
  size_t frames, samples, i;
  unsigned bits;
  sox_bool is_float;
  sox_sample_t * out;

  if (!data)
    return SOX_SUCCESS;
  wpc = WavpackOpenRawDecoder((void *)data, (int32_t)size, NULL, 0, (int16_t)p->version,
      error, OPEN_NORMALIZE, 0);
  if (!wpc) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "WavPack: %s", error);
    return SOX_EOF;
  }
  bits = (unsigned)WavpackGetBytesPerSample(wpc) << 3;
  is_float = (WavpackGetMode(wpc) & MODE_FLOAT) != 0;
  if (!p->have_format) {
    params->channels = (unsigned)WavpackGetNumChannels(wpc);
    params->bits_per_sample = bits;
    params->encoding = is_float? SOX_ENCODING_WAVPACKF : SOX_ENCODING_WAVPACK;
    if (!params->rate)
      params->rate = WavpackGetSampleRate(wpc);
    p->have_format = sox_true;
  }
  else if ((unsigned)WavpackGetNumChannels(wpc) != params->channels || bits != params->bits_per_sample ||
      is_float != (params->encoding == SOX_ENCODING_WAVPACKF)) {
    WavpackCloseFile(wpc);
    lsx_fail_errno(codec->ft, SOX_EFMT, "WavPack frame parameters differ from the stream's");
    return SOX_EOF;
  }
  /* A raw decoder knows no stream length; the frame starts with its own. */
  frames = le32(data);
  samples = frames * params->channels;
  if (p->pcm_size < samples) {
    p->pcm_size = samples;
    free(p->pcm);
    p->pcm = lsx_malloc(samples * sizeof(*p->pcm));
  }
  if (WavpackUnpackSamples(wpc, p->pcm, (uint32_t)frames) != frames || WavpackGetNumErrors(wpc)) {
    WavpackCloseFile(wpc);
    lsx_fail_errno(codec->ft, SOX_EFMT, "WavPack frame is corrupt");
    return SOX_EOF;
  }
  WavpackCloseFile(wpc);
  out = lsx_codec_output(codec, samples);
  /* Whatever the word length, a sample is unpacked into an int32, and a
   * floating point one as its bit pattern. */
  for (i = 0; i < samples; ++i) switch (bits) {
    case  8: out[i] = SOX_SIGNED_8BIT_TO_SAMPLE(p->pcm[i],); break;
    case 16: out[i] = SOX_SIGNED_16BIT_TO_SAMPLE(p->pcm[i],); break;
    case 24: out[i] = SOX_SIGNED_24BIT_TO_SAMPLE(p->pcm[i],); break;
    default:
      if (is_float) {
        float value;

        memcpy(&value, &p->pcm[i], sizeof(value));
        out[i] = SOX_FLOAT_32BIT_TO_SAMPLE(value, codec->ft->clips);
      }
      else out[i] = SOX_SIGNED_32BIT_TO_SAMPLE(p->pcm[i],);
  }
  return SOX_SUCCESS;
}

static void stop_decode(lsx_codec_t * codec)
{
  free(((priv_t *)codec->priv)->pcm);
}

static void frame_put(priv_t * p, void const * data, size_t size)
{
  if (p->frame_size + size > p->frame_alloc) {
    p->frame_alloc = max(p->frame_size + size, p->frame_alloc * 2);
    p->frame = lsx_realloc(p->frame, p->frame_alloc);
  }
  memcpy(p->frame + p->frame_size, data, size);
  p->frame_size += size;
}

/* Each block as libwavpack finishes it.  The blocks of a frame are gathered,
 * each as its flags, checksum, size and data, and the frame goes out with the
 * last; a frame of one block then loses the size, which only tells several
 * apart. */
static int block_out(void * id, void * data, int32_t bcount)
{
  lsx_codec_t * codec = id;
  priv_t * p = codec->priv;
  unsigned char const * block = data;
  unsigned char size[4];
  sox_uint32_t flags;
  lsx_packet_t packet;

  if (bcount < HEADER_SIZE || memcmp(block, "wvpk", 4) ||
      (block[8] | block[9] << 8) != (codec->params.private_data[0] | codec->params.private_data[1] << 8)) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "WavPack wrote a block of an unexpected kind");
    return 0;
  }
  flags = le32(block + 24);
  if (flags & INITIAL_BLOCK) {
    p->frame_size = p->frame_blocks = 0;
    p->frame_samples = le32(block + 20);
    frame_put(p, block + 20, 4);
  }
  size[0] = (unsigned char)(bcount - HEADER_SIZE);
  size[1] = (unsigned char)((bcount - HEADER_SIZE) >> 8);
  size[2] = (unsigned char)((bcount - HEADER_SIZE) >> 16);
  size[3] = (unsigned char)((bcount - HEADER_SIZE) >> 24);
  frame_put(p, block + 24, 8);
  frame_put(p, size, sizeof(size));
  frame_put(p, block + HEADER_SIZE, (size_t)(bcount - HEADER_SIZE));
  ++p->frame_blocks;
  if (!(flags & FINAL_BLOCK))
    return 1;
  if (p->frame_blocks == 1) {
    memmove(p->frame + 12, p->frame + 16, p->frame_size - 16);
    p->frame_size -= 4;
  }
  packet.data = p->frame;
  packet.size = p->frame_size;
  packet.frames = p->frame_samples;
  packet.skip = packet.discard = 0;
  return codec->write_packet(codec, &packet) == SOX_SUCCESS;
}

/* Settings as the wv format has them.  The private data is the stream
 * version WavPack writes unless told to keep to the older, compatible one. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  WavpackConfig config;

  if (!(p->encoder = WavpackOpenFileOutput(block_out, codec, NULL))) {
    lsx_fail_errno(codec->ft, SOX_ENOMEM, "WavPack error creating output instance");
    return SOX_EOF;
  }
  memset(&config, 0, sizeof(config));
  config.bytes_per_sample = (int)params->bits_per_sample >> 3;
  config.bits_per_sample = (int)params->bits_per_sample;
  config.channel_mask = params->channels == 1? 4 :
      params->channels == 2? 3 : (1 << params->channels) - 1;
  config.num_channels = (int)params->channels;
  config.sample_rate = (int32_t)(params->rate + .5);
  config.flags = CONFIG_VERY_HIGH_FLAG;
  if (params->encoding == SOX_ENCODING_WAVPACKF)
    config.float_norm_exp = 127;
  if (!WavpackSetConfiguration64(p->encoder, &config, -1, NULL) || !WavpackPackInit(p->encoder)) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "%s", WavpackGetErrorMessage(p->encoder));
    WavpackCloseFile(p->encoder);
    return SOX_EOF;
  }
  params->private_data = lsx_malloc(2);
  params->private_data[0] = MAX_STREAM_VERS & 0xff;
  params->private_data[1] = MAX_STREAM_VERS >> 8;
  params->private_size = 2;
  return SOX_SUCCESS;
}

static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t const * params = &codec->params;
  sox_bool ok;
  size_t i;

  if (!buf) {
    if (WavpackFlushSamples(p->encoder))
      return SOX_SUCCESS;
    if (!codec->ft->sox_errno)
      lsx_fail_errno(codec->ft, SOX_EINVAL, "%s", WavpackGetErrorMessage(p->encoder));
    return SOX_EOF;
  }
  if (p->pcm_size < len) {
    p->pcm_size = len;
    free(p->pcm);
    p->pcm = lsx_malloc(len * sizeof(*p->pcm));
  }
  for (i = 0; i < len; ++i) {
    SOX_SAMPLE_LOCALS;

    switch (params->bits_per_sample) {
      case  8: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_8BIT(buf[i], codec->ft->clips); break;
      case 16: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_16BIT(buf[i], codec->ft->clips); break;
      case 24: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_24BIT(buf[i], codec->ft->clips); break;
      default:
        if (params->encoding == SOX_ENCODING_WAVPACKF) {
          float value = SOX_SAMPLE_TO_FLOAT_32BIT(buf[i], codec->ft->clips);

          memcpy(&p->pcm[i], &value, sizeof(value));
        }
        else p->pcm[i] = SOX_SAMPLE_TO_SIGNED_32BIT(buf[i], codec->ft->clips);
    }
  }
  ok = WavpackPackSamples(p->encoder, p->pcm, (uint32_t)(len / params->channels));
  if (ok)
    return SOX_SUCCESS;
  if (!codec->ft->sox_errno)
    lsx_fail_errno(codec->ft, SOX_EINVAL, "%s", WavpackGetErrorMessage(p->encoder));
  return SOX_EOF;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  WavpackCloseFile(p->encoder);
  free(p->frame);
  free(p->pcm);
}

lsx_codec_handler_t const * lsx_wavpack_codec_fn(void)
{
  static unsigned const write_formats[] = {
    SOX_ENCODING_WAVPACK, 8, 16, 24, 32, 0,
    SOX_ENCODING_WAVPACKF, 32, 0,
    0};
  static lsx_codec_handler_t const handler = {
    "wavpack", write_formats, NULL, sizeof(priv_t),
    start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode
  };
  return &handler;
}
