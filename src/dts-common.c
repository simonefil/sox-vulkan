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

/* The dts and dtshd handlers share everything but one check.  A DTS-HD file
 * is a DTS core with extension substreams appended, so the same decoder reads
 * both and the profile it reports is what tells them apart -- which is why
 * this file exists rather than two independent handlers. */
typedef struct {
  lsx_ffmpeg_codec_t * codec;
  sox_bool spatial_metadata_warning_shown;
} priv_t;

/* FFmpeg's DTS encoder is marked experimental and refuses to open otherwise.
 * It only ever writes a core stream, which is why encoding is offered by the
 * dts handler alone. */
static int prepare_encoder(AVCodecContext * context)
{
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  return 0;
}

/* Decoding covers every DTS profile, up to the 8 channels an extension
 * substream can carry; encoding is core only, hence 5.1.  The bit rate range
 * is what the core stream allows. */
static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_DTS,
  SOX_ENCODING_DTS,
  "DTS",
  8,                            /* max_decode_channels */
  sox_false,                    /* accept_unspecified_decode_layout */
  6,                            /* max_encode_channels */
  24,                           /* precision */
  768000,                       /* default_bit_rate */
  32000,                        /* minimum_bit_rate */
  3840000,                      /* maximum_bit_rate */
  /* The Atmos-style warning is issued by warn_spatial_metadata instead,
   * which distinguishes the two DTS:X profiles by name. */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  NULL,                         /* prepare_decoder */
  prepare_encoder,              /* prepare_encoder */
  NULL,                         /* packet_reader */
  NULL,                         /* packet_writer */
  sox_false,                    /* use_compression_level */
  0,                            /* default_compression_level */
  0,                            /* minimum_compression_level */
  0,                            /* maximum_compression_level */
  NULL                          /* select_layout */
};

/* The profiles that carry an extension substream, and so count as DTS-HD.
 * DTS Express is included: it is a low-rate substream profile, not a core. */
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

/* Name a profile for the "this is X, not DTS-HD" message, so a rejected file
 * says what it actually is. */
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

/* Warn once that a DTS:X file is being decoded as its channel bed, the object
 * metadata being dropped because SoX cannot carry it.  Called from startread
 * and from every read because the profile is only known once the decoder has
 * seen the substream, which for some files is not on the first frame. */
static void warn_spatial_metadata(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context = lsx_ffmpeg_codec_context(p->codec);
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

/* Open a DTS stream for reading, optionally insisting that it be DTS-HD.
 *
 * The precision is corrected afterwards because the adapter reports what the
 * definition declares, whereas DTS carries a real source word length: the
 * decoder's value is used when it is one DTS can express, and 16 bits stands
 * in when it says nothing, that being the core's own depth.
 *
 * A file rejected for not being DTS-HD has already been opened, so the codec
 * state is released before failing. */
static int startread_common(sox_format_t * ft, sox_bool require_dtshd)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context;
  int result;

  p->spatial_metadata_warning_shown = sox_false;
  result = lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);
  if (result != SOX_SUCCESS)
    return result;
  context = lsx_ffmpeg_codec_context(p->codec);
  if (context != NULL &&
      (context->bits_per_raw_sample == 16 ||
       context->bits_per_raw_sample == 20 ||
       context->bits_per_raw_sample == 24))
    ft->signal.precision = ft->encoding.bits_per_sample = (unsigned)context->bits_per_raw_sample;
  else
    ft->signal.precision = ft->encoding.bits_per_sample = 16;
  if (require_dtshd && (context == NULL || !is_dtshd_profile(context->profile))) {
    char const * name = context == NULL ? "unknown DTS" : profile_name(context->profile);

    lsx_ffmpeg_codec_stopread(&p->codec);
    lsx_fail_errno(ft, SOX_EFMT, "The input is %s rather than DTS-HD", name);
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

static size_t read_samples(sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;
  size_t result = lsx_ffmpeg_codec_read(ft, p->codec, samples, length);

  warn_spatial_metadata(ft);
  return result;
}

static int stopread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopread(&p->codec);
}

/* Spell out the channel order for the layouts DTS defines with side rather
 * than back speakers, since the samples are written unremixed and the order
 * differs from the one SoX assumes.  Silent when --channel-layout was given. */
static void warn_encoded_layout(sox_format_t * ft)
{
  if (ft->channel_layout != NULL)
    return;
  switch (ft->signal.channels) {
    case 4:
      lsx_warn("Encoding 4-channel DTS as quad(side) without remixing; " "channel order is FL FR SL SR");
      break;
    case 5:
      lsx_warn("Encoding 5-channel DTS as 5.0(side) without remixing; " "channel order is FL FR FC SL SR");
      break;
    case 6:
      lsx_warn("Encoding 6-channel DTS as 5.1(side) without remixing; " "channel order is FL FR FC LFE SL SR");
      break;
    default:
      break;
  }
}

/* Open a DTS stream for writing, defaulting to the side-speaker layouts DTS
 * actually uses where the canonical back-speaker ones would be wrong.
 *
 * This is done by substituting into ft->channel_layout rather than through a
 * select_layout hook, so that a layout the user asked for still takes
 * precedence and still reaches the adapter's validation unchanged.  The field
 * is restored afterwards, whatever the outcome, since it belongs to the
 * caller and is read again later. */
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
  result = lsx_ffmpeg_codec_startwrite(ft, &p->codec, &definition);
  ft->channel_layout = requested_layout;

  if (result == SOX_SUCCESS)
    warn_encoded_layout(ft);
  return result;
}

static size_t write_samples(sox_format_t * ft, sox_sample_t const * samples, size_t length)
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
