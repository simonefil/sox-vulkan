/* libSoX packet codecs backed by libavcodec: AAC, xHE-AAC, ALAC, AC-3,
 * E-AC-3, DTS, MLP and TrueHD.
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

/* The definitions below say what each codec is to the libavcodec adapter,
 * and serve both the elementary stream handlers (aac.c, ac3.c and the rest)
 * and the containers, through the packet codecs at the end of this file.
 * Only libavcodec is used: the containers frame the packets themselves.
 *
 * The private data a container carries is, for AAC and xHE-AAC, the
 * AudioSpecificConfig, and for ALAC the 24-byte ALACSpecificConfig, which
 * libavcodec wants behind the 12-byte header of the MP4 atom holding it.
 * The other codecs describe themselves in band. */

#include "codec.h"
#include "ffmpeg-codec.h"

#include <libavutil/opt.h>

#include <errno.h>
#include <string.h>

/* Keep the rest buildable with libavcodec 61, which has no USAC decoder;
 * the definition's version check turns that into a clear message. */
#ifndef AV_PROFILE_AAC_USAC
#define AV_PROFILE_AAC_USAC 41
#endif

#define ALAC_CONFIG_SIZE 24
#define ALAC_ATOM_HEADER_SIZE 12

/*------------------------------- Definitions -------------------------------*/

/* Force a programme config element for the layouts the numbered channel
 * configurations cannot describe, so the stream states its own channel order
 * instead of leaning on a default that means something else: quad and 6.1
 * have no number, and 7.1 has one that describes the different 7.1(wide). */
static int prepare_aac_encoder(AVCodecContext * context)
{
  if (context->ch_layout.nb_channels == 4 || context->ch_layout.nb_channels >= 7)
    return av_opt_set_int(context->priv_data, "aac_pce", 1, 0);
  return 0;
}

/* FFmpeg's DTS, MLP and TrueHD encoders are marked experimental and refuse
 * to open otherwise. */
static int prepare_experimental_encoder(AVCodecContext * context)
{
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  return 0;
}

/* Apple's channel layouts, which differ from SoX's canonical ones above four
 * channels: 4.0 rather than quad, 6.1 back rather than 6.1, and 7.1 wide back
 * rather than 7.1.  A file written with the canonical layouts would be read
 * by Apple software with its channels in the wrong places. */
static int select_alac_layout(unsigned channels, AVChannelLayout * layout)
{
  switch (channels) {
    case 1: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO; return 0;
    case 2: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO; return 0;
    case 3: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_SURROUND; return 0;
    case 4: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_4POINT0; return 0;
    case 5: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0_BACK; return 0;
    case 6: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK; return 0;
    case 7: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_6POINT1_BACK; return 0;
    case 8: *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1_WIDE_BACK; return 0;
    default: return AVERROR(EINVAL);
  }
}

/* The channel order, where Apple's choice is not the one SoX would assume,
 * since the samples are passed through unremixed and it is the user who has
 * to line them up.  Silent when --channel-layout was given. */
static void warn_alac_layout(sox_format_t * ft)
{
  if (ft->channel_layout != NULL)
    return;
  switch (ft->signal.channels) {
    case 4:
      lsx_warn("Encoding 4-channel ALAC as MPEG 4.0 B without remixing; channel order is FL FR FC BC");
      break;
    case 7:
      lsx_warn("Encoding 7-channel ALAC as Apple AAC 6.1 without remixing; "
          "channel order is FL FR FC LFE BL BR BC");
      break;
    case 8:
      lsx_warn("Encoding 8-channel ALAC as MPEG 7.1 B without remixing; "
          "channel order is FL FR FC LFE BL BR FLC FRC");
      break;
  }
}

/* DTS defines its layouts with side rather than back speakers. */
static char const * dts_encode_layout(unsigned channels)
{
  return channels == 4? "quad(side)" : channels == 5? "5.0(side)" : channels == 6? "5.1(side)" : NULL;
}

static void warn_dts_layout(sox_format_t * ft)
{
  if (ft->channel_layout != NULL)
    return;
  switch (ft->signal.channels) {
    case 4:
      lsx_warn("Encoding 4-channel DTS as quad(side) without remixing; channel order is FL FR SL SR");
      break;
    case 5:
      lsx_warn("Encoding 5-channel DTS as 5.0(side) without remixing; channel order is FL FR FC SL SR");
      break;
    case 6:
      lsx_warn("Encoding 6-channel DTS as 5.1(side) without remixing; "
          "channel order is FL FR FC LFE SL SR");
      break;
  }
}

/* FFmpeg's ALAC, MLP and TrueHD encoders implement only these two depths. */
static int check_lossless_depth(sox_format_t * ft, lsx_ffmpeg_codec_definition_t const * definition)
{
  if (ft->encoding.bits_per_sample != 16 && ft->encoding.bits_per_sample != 24) {
    lsx_fail_errno(ft, SOX_EFMT, "%s encoding supports 16-bit or 24-bit PCM", definition->name);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* -C is rejected rather than ignored: for MLP and TrueHD it would mean
 * nothing, and silently dropping it would look like it had taken effect. */
static int check_mlp_encoder(sox_format_t * ft, lsx_ffmpeg_codec_definition_t const * definition)
{
  if (check_lossless_depth(ft, definition) != SOX_SUCCESS)
    return SOX_EOF;
  if (ft->encoding.compression != HUGE_VAL) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "%s is lossless and does not accept a bitrate or compression level with -C", definition->name);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* No fixed bit rate range: what is usable depends on the rate and channel
 * count, so the encoder is left to reject what it cannot do.  FFmpeg keeps
 * the canonical PCE sample order but reports 6.1 and 7.1 frames with no
 * speaker names, hence the unspecified layouts. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_aac_definition = {
  AV_CODEC_ID_AAC, SOX_ENCODING_AAC, "AAC",
  8, sox_true, 8, 24,
  128000, 0, 0,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  prepare_aac_encoder, NULL, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

/* Decode only: FFmpeg has no USAC encoder.  The codec is shared with plain
 * AAC, hence the required profile, which is what stops an AAC-LC stream from
 * being taken for xHE-AAC.  USAC states its own channel order in the config,
 * so decoder order is the right order even when FFmpeg names no speakers. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_usac_definition = {
  AV_CODEC_ID_AAC, SOX_ENCODING_USAC, "xHE-AAC",
  8, sox_true, 0, 24,
  0, 0, 0,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_AAC_USAC, 62,
  NULL, NULL, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

/* Lossless: the depth comes from the stream on the way in and from -b on the
 * way out, and -C is a compression level, 0 to 2, 2 being FFmpeg's default. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_alac_definition = {
  AV_CODEC_ID_ALAC, SOX_ENCODING_ALAC, "ALAC",
  8, sox_false, 8, 0,
  0, 0, 0,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  NULL, check_lossless_depth, NULL, warn_alac_layout, sox_false,
  sox_true, 2, 0, 2, select_alac_layout
};

/* A/52 fixes the channel order, so the canonical layouts apply as they are;
 * the bit rate bounds are the ones it allows. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_ac3_definition = {
  AV_CODEC_ID_AC3, SOX_ENCODING_AC3, "AC-3",
  6, sox_false, 6, 24,
  448000, 32000, 640000,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  NULL, NULL, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_eac3_definition = {
  AV_CODEC_ID_EAC3, SOX_ENCODING_EAC3, "E-AC-3",
  8, sox_false, 6, 24,
  640000, 32000, 6144000,
  AV_PROFILE_EAC3_DDP_ATMOS, "Dolby Atmos JOC/OAMD", AV_PROFILE_UNKNOWN, 0,
  NULL, NULL, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

/* Decoding covers every DTS profile, up to the 8 channels an extension
 * substream can carry; FFmpeg's encoder writes a 5.1 core at most.  The
 * depth reported is the one the stream was mastered at. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_dts_definition = {
  AV_CODEC_ID_DTS, SOX_ENCODING_DTS, "DTS",
  8, sox_false, 6, 24,
  768000, 32000, 3840000,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  prepare_experimental_encoder, NULL, dts_encode_layout, warn_dts_layout, sox_true,
  sox_false, 0, 0, 0, NULL
};

/* MLP is a DVD-Audio format and stops at 5.1; TrueHD decodes up to 8
 * channels but FFmpeg's encoder writes at most 5.1.  An Atmos stream is
 * decoded as its channel bed with the object metadata warned about. */
lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_mlp_definition = {
  AV_CODEC_ID_MLP, SOX_ENCODING_MLP, "MLP",
  6, sox_false, 6, 0,
  0, 0, 0,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  prepare_experimental_encoder, check_mlp_encoder, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_truehd_definition = {
  AV_CODEC_ID_TRUEHD, SOX_ENCODING_TRUEHD, "Dolby TrueHD",
  8, sox_false, 6, 0,
  0, 0, 0,
  AV_PROFILE_TRUEHD_ATMOS, "Dolby TrueHD with Dolby Atmos", AV_PROFILE_UNKNOWN, 0,
  prepare_experimental_encoder, check_mlp_encoder, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

/*------------------------------ Packet codecs ------------------------------*/

typedef struct {
  lsx_ffmpeg_codec_definition_t const * definition;
  lsx_ffmpeg_codec_t * state;

  /* Encoding.  The packets the flush gives are held until it is over, when
   * the padding at the end of the stream is known and can be marked as
   * discard on the last of them. */
  sox_uint64_t input_frames, coded_frames;
  sox_bool flushing;
  struct {unsigned char * data; size_t size, frames, discard;} * held;
  size_t held_count, held_alloc;
} priv_t;

static lsx_ffmpeg_codec_definition_t const * definition_of(lsx_codec_handler_t const * handler);

/* The configuration the decoder is given: ALAC's behind its atom header,
 * unless the container kept the header. */
static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  unsigned char alac[ALAC_ATOM_HEADER_SIZE + ALAC_CONFIG_SIZE];
  unsigned char const * extradata = params->private_data;
  size_t extradata_size = params->private_size;
  AVCodecContext const * context;

  p->definition = definition_of(codec->handler);
  if (p->definition->codec_id == AV_CODEC_ID_ALAC) {
    if (extradata_size < ALAC_CONFIG_SIZE) {
      lsx_fail_errno(codec->ft, SOX_EHDR, "ALAC configuration missing or too short");
      return SOX_EOF;
    }
    if (extradata_size < ALAC_ATOM_HEADER_SIZE + ALAC_CONFIG_SIZE || memcmp(extradata + 4, "alac", 4)) {
      memcpy(alac, "\0\0\0\x24" "alac\0\0\0\0", ALAC_ATOM_HEADER_SIZE);
      memcpy(alac + ALAC_ATOM_HEADER_SIZE, extradata, ALAC_CONFIG_SIZE);
      extradata = alac;
      extradata_size = sizeof(alac);
    }
  }
  if (lsx_ffmpeg_codec_open_decoder(codec->ft, &p->state, p->definition, extradata, extradata_size) != SOX_SUCCESS)
    return SOX_EOF;
  context = lsx_ffmpeg_codec_context(p->state);
  if (context->sample_rate > 0)
    params->rate = context->sample_rate;
  if (context->ch_layout.nb_channels > 0)
    params->channels = (unsigned)context->ch_layout.nb_channels;
  params->encoding = p->definition->encoding;
  params->bits_per_sample = p->definition->precision? 0 : (unsigned)context->bits_per_raw_sample;
  if (p->definition->encoding == SOX_ENCODING_USAC)
    lsx_warn("`%s': xHE-AAC loudness and dynamic-range metadata, if present, will be ignored",
        codec->ft->filename);
  return SOX_SUCCESS;
}

/* The rate, channels and, where the decoder knows it only from the
 * stream, the depth are settled by the frames themselves. */
static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  sox_sample_t const * samples;
  size_t length;
  int result;

  if (lsx_ffmpeg_codec_send_packet(codec->ft, p->state, data, size) != SOX_SUCCESS)
    return SOX_EOF;
  while ((result = lsx_ffmpeg_codec_receive_samples(codec->ft, p->state, &samples, &length)) == 1) {
    AVCodecContext const * context = lsx_ffmpeg_codec_context(p->state);

    codec->params.rate = context->sample_rate;
    codec->params.channels = (unsigned)context->ch_layout.nb_channels;
    if (p->definition->decoded_depth) {
      int bits = context->bits_per_raw_sample;

      codec->params.bits_per_sample = bits == 16 || bits == 20 || bits == 24? (unsigned)bits : 16;
    }
    else if (!p->definition->precision && context->bits_per_raw_sample > 0)
      codec->params.bits_per_sample = (unsigned)context->bits_per_raw_sample;
    memcpy(lsx_codec_output(codec, length), samples, length * sizeof(*samples));
  }
  return result == SOX_EOF? SOX_EOF : SOX_SUCCESS;
}

static void stop_decode(lsx_codec_t * codec)
{
  lsx_ffmpeg_codec_stopread(&((priv_t *)codec->priv)->state);
}

static int deliver(lsx_codec_t * codec, unsigned char const * data, size_t size, size_t frames, size_t discard)
{
  lsx_packet_t packet;

  packet.data = data;
  packet.size = size;
  packet.frames = frames;
  packet.skip = 0;
  packet.discard = discard;
  return codec->write_packet(codec, &packet);
}

/* What a packet decodes to: the frame size, or for an encoder with no delay,
 * which may end on a short frame, the packet's own duration. */
static int sink(void * opaque, AVCodecContext const * context, AVPacket const * packet)
{
  lsx_codec_t * codec = opaque;
  priv_t * p = codec->priv;
  size_t frames = packet->duration > 0 && !(context->codec->capabilities & AV_CODEC_CAP_DELAY)?
      (size_t)packet->duration : (size_t)context->frame_size;

  if (!packet->size)
    return SOX_SUCCESS;
  p->coded_frames += frames;
  if (!p->flushing)
    return deliver(codec, packet->data, (size_t)packet->size, frames, 0);
  if (p->held_count == p->held_alloc) {
    p->held_alloc = max(8, p->held_alloc * 2);
    p->held = lsx_realloc(p->held, p->held_alloc * sizeof(*p->held));
  }
  p->held[p->held_count].data = lsx_memdup(packet->data, (size_t)packet->size);
  p->held[p->held_count].size = (size_t)packet->size;
  p->held[p->held_count++].frames = frames;
  return SOX_SUCCESS;
}

static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  AVCodecContext const * context;

  p->definition = definition_of(codec->handler);
  if (lsx_ffmpeg_codec_open_encoder(codec->ft, &p->state, p->definition, sink, codec) != SOX_SUCCESS)
    return SOX_EOF;
  context = lsx_ffmpeg_codec_context(p->state);
  params->delay = context->initial_padding > 0? (sox_uint64_t)context->initial_padding : 0;
  params->bits_per_sample = codec->ft->encoding.bits_per_sample;
  if (p->definition->codec_id == AV_CODEC_ID_ALAC) {
    if (context->extradata_size < ALAC_ATOM_HEADER_SIZE + ALAC_CONFIG_SIZE) {
      lsx_fail_errno(codec->ft, SOX_EFMT, "ALAC encoder did not provide its configuration");
      lsx_ffmpeg_codec_stopread(&p->state);
      return SOX_EOF;
    }
    params->private_data = lsx_memdup(context->extradata + ALAC_ATOM_HEADER_SIZE, ALAC_CONFIG_SIZE);
    params->private_size = ALAC_CONFIG_SIZE;
  }
  else if (context->extradata_size > 0) {
    params->private_data = lsx_memdup(context->extradata, (size_t)context->extradata_size);
    params->private_size = (size_t)context->extradata_size;
  }
  return SOX_SUCCESS;
}

/* At the end, the frames coded beyond the delay and the input are padding,
 * marked as discard from the last packet back. */
static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  sox_uint64_t excess;
  size_t i;
  int result = SOX_SUCCESS;

  if (buf) {
    p->input_frames += len / codec->params.channels;
    return lsx_ffmpeg_codec_write(codec->ft, p->state, buf, len) == len? SOX_SUCCESS : SOX_EOF;
  }
  p->flushing = sox_true;
  if (lsx_ffmpeg_codec_flush(codec->ft, p->state) != SOX_SUCCESS)
    return SOX_EOF;
  excess = p->coded_frames - min(p->coded_frames, codec->params.delay + p->input_frames);
  for (i = p->held_count; i--; excess -= p->held[i].discard)
    p->held[i].discard = (size_t)min(excess, p->held[i].frames);
  for (i = 0; i < p->held_count && result == SOX_SUCCESS; ++i)
    result = deliver(codec, p->held[i].data, p->held[i].size, p->held[i].frames, p->held[i].discard);
  return result;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  size_t i;

  lsx_ffmpeg_codec_stopread(&p->state);
  for (i = 0; i < p->held_count; ++i)
    free(p->held[i].data);
  free(p->held);
}

static unsigned const aac_formats[] = {SOX_ENCODING_AAC, 0, 0};
static unsigned const usac_formats[] = {SOX_ENCODING_USAC, 0, 0};
static unsigned const alac_formats[] = {SOX_ENCODING_ALAC, 16, 24, 0, 0};
static unsigned const ac3_formats[] = {SOX_ENCODING_AC3, 0, 0};
static unsigned const eac3_formats[] = {SOX_ENCODING_EAC3, 0, 0};
static unsigned const dts_formats[] = {SOX_ENCODING_DTS, 0, 0};
static unsigned const mlp_formats[] = {SOX_ENCODING_MLP, 16, 24, 0, 0};
static unsigned const truehd_formats[] = {SOX_ENCODING_TRUEHD, 16, 24, 0, 0};
static sox_rate_t const aac_rates[] = {
  7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 0
};
static sox_rate_t const ac3_rates[] = {32000, 44100, 48000, 0};
static sox_rate_t const dts_rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0};
static sox_rate_t const mlp_rates[] = {44100, 48000, 88200, 96000, 176400, 192000, 0};

static struct {
  lsx_codec_handler_t handler;
  lsx_ffmpeg_codec_definition_t const * definition;
} const codecs[] = {
  {{"aac", aac_formats, aac_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_aac_definition},
  {{"usac", usac_formats, NULL, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    NULL, NULL, NULL}, &lsx_ffmpeg_usac_definition},
  {{"alac", alac_formats, NULL, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_alac_definition},
  {{"ac3", ac3_formats, ac3_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_ac3_definition},
  {{"eac3", eac3_formats, ac3_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_eac3_definition},
  {{"dts", dts_formats, dts_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_dts_definition},
  {{"mlp", mlp_formats, mlp_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_mlp_definition},
  {{"truehd", truehd_formats, mlp_rates, sizeof(priv_t), start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode}, &lsx_ffmpeg_truehd_definition},
};

static lsx_ffmpeg_codec_definition_t const * definition_of(lsx_codec_handler_t const * handler)
{
  size_t i;

  for (i = 0; &codecs[i].handler != handler; ++i);
  return codecs[i].definition;
}

lsx_codec_handler_t const * lsx_aac_codec_fn(void) {return &codecs[0].handler;}
lsx_codec_handler_t const * lsx_usac_codec_fn(void) {return &codecs[1].handler;}
lsx_codec_handler_t const * lsx_alac_codec_fn(void) {return &codecs[2].handler;}
lsx_codec_handler_t const * lsx_ac3_codec_fn(void) {return &codecs[3].handler;}
lsx_codec_handler_t const * lsx_eac3_codec_fn(void) {return &codecs[4].handler;}
lsx_codec_handler_t const * lsx_dts_codec_fn(void) {return &codecs[5].handler;}
lsx_codec_handler_t const * lsx_mlp_codec_fn(void) {return &codecs[6].handler;}
lsx_codec_handler_t const * lsx_truehd_codec_fn(void) {return &codecs[7].handler;}
