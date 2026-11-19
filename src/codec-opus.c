/* libSoX packet codec: Opus, through libopus.
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

/* The private data is the identification header of RFC 7845, OpusHead:
 * channel count, pre-skip, output gain and channel mapping.  Opus codes at
 * 48 kHz whatever the rate of its input, so it decodes at 48 kHz, and encodes
 * only a 48 kHz signal, which SoX resamples to as it would for any format
 * with a fixed rate.  The pre-skip is the delay, priming frames the decoder
 * outputs ahead of the signal, and the container carries it, as Matroska's
 * codec delay, or takes it from the header, as Ogg does.  Mapping families 0
 * and 1 are supported: up to 8 channels in Vorbis order, which is put into
 * SoX's order and back. */

#include "codec.h"

#include <string.h>

#include <opus_multistream.h>

#define RATE 48000
#define MAX_FRAMES 5760        /* 120 ms, the longest an Opus packet lasts */
#define FRAMES_PER_PACKET 960  /* 20 ms */
#define HEAD_SIZE 19           /* up to the channel mapping table */
#define MAX_CHANNELS 8

typedef struct {
  OpusMSDecoder * decoder;
  OpusMSEncoder * encoder;
  float * pcm;
  size_t pcm_frames;          /* encoding: frames waiting in pcm */
  unsigned char * packet;
  size_t packet_size;
  sox_uint64_t input_frames;  /* encoding: frames given */
  sox_uint64_t coded_frames;  /* encoding: frames encoded, padding included */
} priv_t;

static unsigned le16(unsigned char const * b)
{
  return b[0] | (unsigned)b[1] << 8;
}

/* A layout SoX can carry: families 0 and 1, with 1 to 8 channels. */
static int check_layout(sox_format_t * ft, unsigned channels, unsigned family)
{
  if (channels < 1 || channels > MAX_CHANNELS) {
    lsx_fail_errno(ft, SOX_EFMT, "Opus supports standard layouts with 1 to 8 channels");
    return SOX_EOF;
  }
  if (family > 1) {
    lsx_fail_errno(ft, SOX_EFMT, "Unsupported Opus channel mapping family %u", family);
    return SOX_EOF;
  }
  if (family == 0 && channels > 2) {
    lsx_fail_errno(ft, SOX_EHDR, "Opus mapping family 0 is invalid for %u channels", channels);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  unsigned char const * head = params->private_data;
  unsigned char const stereo[2] = {0, 1};
  unsigned channels, family, streams, coupled;
  unsigned char const * mapping = stereo;
  int error;

  if (params->private_size < HEAD_SIZE || memcmp(head, "OpusHead", 8) || head[8] > 15) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "Opus private data holds no identification header");
    return SOX_EOF;
  }
  channels = head[9];
  family = head[18];
  if (check_layout(codec->ft, channels, family) != SOX_SUCCESS)
    return SOX_EOF;
  streams = 1;
  coupled = channels - 1;
  if (family) {
    if (params->private_size < HEAD_SIZE + 2 + channels) {
      lsx_fail_errno(codec->ft, SOX_EHDR, "Opus identification header cut short");
      return SOX_EOF;
    }
    streams = head[19];
    coupled = head[20];
    mapping = head + 21;
  }
  p->decoder = opus_multistream_decoder_create(RATE, (int)channels, (int)streams, (int)coupled,
      mapping, &error);
  if (!p->decoder) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "Opus: %s", opus_strerror(error));
    return SOX_EOF;
  }
  opus_multistream_decoder_ctl(p->decoder, OPUS_SET_GAIN((opus_int16)le16(head + 16)));
  p->pcm = lsx_malloc((size_t)MAX_FRAMES * channels * sizeof(*p->pcm));
  params->rate = RATE;
  params->channels = channels;
  params->encoding = SOX_ENCODING_OPUS;
  params->bits_per_sample = 0;
  if (!params->delay)
    params->delay = le16(head + 10);
  return SOX_SUCCESS;
}

static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  unsigned channels = codec->params.channels;
  sox_sample_t * out;
  int frames, i;
  unsigned channel;

  if (!data)
    return SOX_SUCCESS;
  frames = opus_multistream_decode_float(p->decoder, data, (opus_int32)size, p->pcm, MAX_FRAMES, 0);
  if (frames < 0) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "Opus: %s", opus_strerror(frames));
    return SOX_EOF;
  }
  out = lsx_codec_output(codec, (size_t)frames * channels);
  for (i = 0; i < frames; ++i, out += channels)
    for (channel = 0; channel < channels; ++channel)
      out[channels > 2? lsx_vorbis_channel_from_sox[channels][channel] : channel] =
        p->pcm[(size_t)i * channels + channel];
  return SOX_SUCCESS;
}

static size_t packet_frames(lsx_codec_t * codec UNUSED, unsigned char const * data, size_t size)
{
  int frames = opus_packet_get_nb_samples(data, (opus_int32)size, RATE);

  return frames > 0? (size_t)frames : 0;
}

static void stop_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  opus_multistream_decoder_destroy(p->decoder);
  free(p->pcm);
}

/* -C is a bit rate in kbit/s, from 6 to 256 per channel; absent, it is
 * libopus's own choice. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;
  unsigned channels = params->channels, family = channels > 2;
  unsigned char * head, mapping[MAX_CHANNELS];
  int streams, coupled, error;
  opus_int32 lookahead;

  if (check_layout(ft, channels, family) != SOX_SUCCESS)
    return SOX_EOF;
  head = params->private_data = lsx_calloc(1, HEAD_SIZE + (family? 2 + channels : 0));
  params->private_size = HEAD_SIZE + (family? 2 + channels : 0);
  p->encoder = opus_multistream_surround_encoder_create(RATE, (int)channels, (int)family,
      &streams, &coupled, mapping, OPUS_APPLICATION_AUDIO, &error);
  if (!p->encoder) {
    lsx_fail_errno(ft, SOX_EFMT, "Failed to initialize Opus encoder: %s", opus_strerror(error));
    return SOX_EOF;
  }
  if (ft->encoding.compression != HUGE_VAL) {
    double maximum = 256. * channels;

    if (ft->encoding.compression < 6 || ft->encoding.compression > maximum) {
      lsx_fail_errno(ft, SOX_EINVAL, "Opus bitrate must be between 6 and %.0f kbit/s for %u channel%s",
          maximum, channels, channels == 1? "" : "s");
      opus_multistream_encoder_destroy(p->encoder);
      return SOX_EOF;
    }
    opus_multistream_encoder_ctl(p->encoder,
        OPUS_SET_BITRATE((opus_int32)(ft->encoding.compression * 1000 + .5)));
  }
  opus_multistream_encoder_ctl(p->encoder, OPUS_GET_LOOKAHEAD(&lookahead));
  memcpy(head, "OpusHead", 8);
  head[8] = 1;
  head[9] = (unsigned char)channels;
  head[10] = (unsigned char)lookahead;
  head[11] = (unsigned char)(lookahead >> 8);
  head[12] = (unsigned char)RATE;
  head[13] = (unsigned char)(RATE >> 8);
  head[14] = (unsigned char)(RATE >> 16);
  head[18] = (unsigned char)family;
  if (family) {
    head[19] = (unsigned char)streams;
    head[20] = (unsigned char)coupled;
    memcpy(head + 21, mapping, channels);
  }
  params->delay = (sox_uint64_t)lookahead;
  /* Opus has no word length; 24 bits is what reading it reports too. */
  ft->signal.precision = 24;
  p->pcm = lsx_malloc((size_t)FRAMES_PER_PACKET * channels * sizeof(*p->pcm));
  p->packet_size = (size_t)1277 * (size_t)streams;
  p->packet = lsx_malloc(p->packet_size);
  return SOX_SUCCESS;
}

/* One packet from the frames waiting, padded out with silence. */
static int encode_packet(lsx_codec_t * codec, size_t discard)
{
  priv_t * p = codec->priv;
  unsigned channels = codec->params.channels;
  lsx_packet_t packet;
  opus_int32 bytes;

  memset(p->pcm + p->pcm_frames * channels, 0,
      (FRAMES_PER_PACKET - p->pcm_frames) * channels * sizeof(*p->pcm));
  p->pcm_frames = 0;
  bytes = opus_multistream_encode_float(p->encoder, p->pcm, FRAMES_PER_PACKET,
      p->packet, (opus_int32)p->packet_size);
  if (bytes < 0) {
    lsx_fail_errno(codec->ft, SOX_EOF, "Opus encoding failed: %s", opus_strerror(bytes));
    return SOX_EOF;
  }
  p->coded_frames += FRAMES_PER_PACKET;
  packet.data = p->packet;
  packet.size = (size_t)bytes;
  packet.frames = FRAMES_PER_PACKET;
  packet.skip = 0;
  packet.discard = discard;
  return codec->write_packet(codec, &packet);
}

/* At the end, packets go on until the decoder, lagging by the pre-skip, has
 * given out the last frame of the signal; what follows it in the last packet
 * is discard. */
static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  unsigned channels = codec->params.channels;
  size_t frames = len / channels, i;
  unsigned channel;

  if (!buf) {
    sox_uint64_t end = p->input_frames + codec->params.delay;

    while (p->coded_frames + FRAMES_PER_PACKET < end)
      if (encode_packet(codec, 0) != SOX_SUCCESS)
        return SOX_EOF;
    return encode_packet(codec, (size_t)(p->coded_frames + FRAMES_PER_PACKET - end));
  }
  for (i = 0; i < frames; ++i) {
    float * frame = p->pcm + p->pcm_frames * channels;

    for (channel = 0; channel < channels; ++channel)
      frame[channel] = SOX_SAMPLE_TO_FLOAT_32BIT(
          buf[i * channels + (channels > 2? lsx_vorbis_channel_from_sox[channels][channel] : channel)],
          codec->ft->clips);
    if (++p->pcm_frames == FRAMES_PER_PACKET && encode_packet(codec, 0) != SOX_SUCCESS)
      return SOX_EOF;
  }
  p->input_frames += frames;
  return SOX_SUCCESS;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  opus_multistream_encoder_destroy(p->encoder);
  free(p->pcm);
  free(p->packet);
}

lsx_codec_handler_t const * lsx_opus_codec_fn(void)
{
  static unsigned const write_formats[] = {SOX_ENCODING_OPUS, 0, 0};
  static sox_rate_t const write_rates[] = {RATE, 0};
  static lsx_codec_handler_t const handler = {
    "opus", write_formats, write_rates, sizeof(priv_t),
    start_decode, decode, packet_frames, stop_decode,
    start_encode, encode, stop_encode
  };
  return &handler;
}
