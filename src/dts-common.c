/* Shared DTS and DTS-HD elementary stream format implementation.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "dts-common.h"
#include "ffmpeg-codec.h"

#ifdef HAVE_FFMPEG_CODECS

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  sox_bool spatial_metadata_warning_shown;
} priv_t;

static int prepare_encoder(AVCodecContext * context)
{
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  return 0;
}

static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_DTS,
  SOX_ENCODING_DTS,
  "DTS",
  8,
  sox_false,
  6,
  24,
  768000,
  32000,
  3840000,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  NULL,
  prepare_encoder,
  NULL,
  NULL,
  sox_false,
  0,
  0,
  0,
  NULL
};

static sox_bool is_dtshd_profile(int profile)
{
  switch (profile) {
    case AV_PROFILE_DTS_HD_HRA:
    case AV_PROFILE_DTS_HD_MA:
    case AV_PROFILE_DTS_HD_MA_X:
    case AV_PROFILE_DTS_HD_MA_X_IMAX:
    case AV_PROFILE_DTS_EXPRESS:
      return sox_true;
    default:
      return sox_false;
  }
}

static char const * profile_name(int profile)
{
  switch (profile) {
    case AV_PROFILE_DTS:
      return "DTS core";
    case AV_PROFILE_DTS_ES:
      return "DTS-ES";
    case AV_PROFILE_DTS_96_24:
      return "DTS 96/24";
    case AV_PROFILE_DTS_HD_HRA:
      return "DTS-HD High Resolution Audio";
    case AV_PROFILE_DTS_HD_MA:
      return "DTS-HD Master Audio";
    case AV_PROFILE_DTS_HD_MA_X:
      return "DTS-HD Master Audio with DTS:X";
    case AV_PROFILE_DTS_HD_MA_X_IMAX:
      return "DTS-HD Master Audio with DTS:X IMAX";
    case AV_PROFILE_DTS_EXPRESS:
      return "DTS Express";
    default:
      return "unknown DTS";
  }
}

static void warn_spatial_metadata(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context =
      lsx_ffmpeg_codec_context(p->codec);
  char const * name;

  if (p->spatial_metadata_warning_shown || context == NULL)
    return;
  if (context->profile == AV_PROFILE_DTS_HD_MA_X)
    name = "DTS:X";
  else if (context->profile == AV_PROFILE_DTS_HD_MA_X_IMAX)
    name = "DTS:X IMAX";
  else
    return;
  lsx_warn("`%s': %s detected; spatial metadata will be ignored and "
      "only the channel-based audio presentation will be decoded",
      ft->filename, name);
  p->spatial_metadata_warning_shown = sox_true;
}

static int startread_common(
    sox_format_t * ft,
    sox_bool require_dtshd)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context;
  int result;

  p->spatial_metadata_warning_shown = sox_false;
  result = lsx_ffmpeg_codec_startread(
      ft, &p->codec, &definition);
  if (result != SOX_SUCCESS)
    return result;
  context = lsx_ffmpeg_codec_context(p->codec);
  if (context != NULL &&
      (context->bits_per_raw_sample == 16 ||
       context->bits_per_raw_sample == 20 ||
       context->bits_per_raw_sample == 24))
    ft->signal.precision = ft->encoding.bits_per_sample =
        (unsigned)context->bits_per_raw_sample;
  else
    ft->signal.precision = ft->encoding.bits_per_sample = 16;
  if (require_dtshd &&
      (context == NULL || !is_dtshd_profile(context->profile))) {
    char const * name = context == NULL ?
        "unknown DTS" : profile_name(context->profile);

    lsx_ffmpeg_codec_stopread(&p->codec);
    lsx_fail_errno(ft, SOX_EFMT,
        "The input is %s rather than DTS-HD", name);
    return SOX_EOF;
  }
  warn_spatial_metadata(ft);
  return SOX_SUCCESS;
}

static int startread_dts(sox_format_t * ft)
{
  return startread_common(ft, sox_false);
}

static int startread_dtshd(sox_format_t * ft)
{
  return startread_common(ft, sox_true);
}

static size_t read_samples(
    sox_format_t * ft,
    sox_sample_t * samples,
    size_t length)
{
  priv_t * p = (priv_t *)ft->priv;
  size_t result =
      lsx_ffmpeg_codec_read(ft, p->codec, samples, length);

  warn_spatial_metadata(ft);
  return result;
}

static int stopread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopread(&p->codec);
}

static void warn_encoded_layout(sox_format_t * ft)
{
  if (ft->channel_layout != NULL)
    return;
  switch (ft->signal.channels) {
    case 4:
      lsx_warn("Encoding 4-channel DTS as quad(side) without remixing; "
          "channel order is FL FR SL SR");
      break;
    case 5:
      lsx_warn("Encoding 5-channel DTS as 5.0(side) without remixing; "
          "channel order is FL FR FC SL SR");
      break;
    case 6:
      lsx_warn("Encoding 6-channel DTS as 5.1(side) without remixing; "
          "channel order is FL FR FC LFE SL SR");
      break;
    default:
      break;
  }
}

static int startwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  char const * requested_layout = ft->channel_layout;
  char const * default_layout = NULL;
  int result;

  if (requested_layout == NULL) {
    switch (ft->signal.channels) {
      case 4:
        default_layout = "quad(side)";
        break;
      case 5:
        default_layout = "5.0(side)";
        break;
      case 6:
        default_layout = "5.1(side)";
        break;
      default:
        break;
    }
    ft->channel_layout = default_layout;
  }
  result = lsx_ffmpeg_codec_startwrite(
      ft, &p->codec, &definition);
  ft->channel_layout = requested_layout;

  if (result == SOX_SUCCESS)
    warn_encoded_layout(ft);
  return result;
}

static size_t write_samples(
    sox_format_t * ft,
    sox_sample_t const * samples,
    size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_write(ft, p->codec, samples, length);
}

static int stopwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopwrite(ft, &p->codec);
}

sox_format_handler_t const * lsx_dts_format_handler(void)
{
  static char const * const names[] = {"dts", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_DTS, 0, 0};
  static sox_rate_t const rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "DTS Coherent Acoustics elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread_dts,
    read_samples,
    stopread,
    startwrite,
    write_samples,
    stopwrite,
    NULL,
    encodings,
    rates,
    sizeof(priv_t)
  };

  return &handler;
}

sox_format_handler_t const * lsx_dtshd_format_handler(void)
{
  static char const * const names[] = {"dtshd", NULL};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "DTS-HD elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS,
    startread_dtshd,
    read_samples,
    stopread,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    sizeof(priv_t)
  };

  return &handler;
}

#endif
