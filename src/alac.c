/* Apple Lossless audio in an M4A container.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "ffmpeg-codec.h"
#include "ffmpeg-container.h"

#ifdef HAVE_FFMPEG_FORMATS

#include <libavutil/error.h>

#include <errno.h>

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  lsx_ffmpeg_container_t * container;
} priv_t;

static int select_alac_layout(unsigned channels, AVChannelLayout * layout)
{
  switch (channels) {
    case 1:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
      return 0;
    case 2:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
      return 0;
    case 3:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_SURROUND;
      return 0;
    case 4:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_4POINT0;
      return 0;
    case 5:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0_BACK;
      return 0;
    case 6:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK;
      return 0;
    case 7:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_6POINT1_BACK;
      return 0;
    case 8:
      *layout =
          (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1_WIDE_BACK;
      return 0;
    default:
      return AVERROR(EINVAL);
  }
}

static void warn_alac_layout(sox_format_t * ft, char const * operation)
{
  if (ft->channel_layout != NULL)
    return;
  switch (ft->signal.channels) {
    case 4:
      lsx_warn("%s 4-channel ALAC as MPEG 4.0 B without remixing; " "channel order is FL FR FC BC", operation);
      break;
    case 7:
      lsx_warn("%s 7-channel ALAC as Apple AAC 6.1 without remixing; "
          "channel order is FL FR FC LFE BL BR BC", operation);
      break;
    case 8:
      lsx_warn("%s 8-channel ALAC as MPEG 7.1 B without remixing; "
          "channel order is FL FR FC LFE BL BR FLC FRC", operation);
      break;
    default:
      break;
  }
}

static int prepare_alac_decoder(sox_format_t * ft, AVCodecContext * context)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_container_startread(ft, &p->container, "mov", AV_CODEC_ID_ALAC, context);
}

static int read_alac_packet(sox_format_t * ft, AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_container_read_packet(ft, p->container, packet);
}

static int write_alac_packet(sox_format_t * ft, AVCodecContext const * context, AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;

  if (p->container == NULL) {
    lsx_fail_errno(ft, SOX_EFMT, "M4A output container is unavailable");
    return SOX_EOF;
  }
  return lsx_ffmpeg_container_write_packet(ft, p->container, context, packet);
}

static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_ALAC,
  SOX_ENCODING_ALAC,
  "ALAC",
  8,
  sox_false,
  8,
  0,
  0,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  prepare_alac_decoder,
  NULL,
  read_alac_packet,
  write_alac_packet,
  sox_true,
  2,
  0,
  2,
  select_alac_layout
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);

  if (result != SOX_SUCCESS)
    lsx_ffmpeg_container_stopread(&p->container);
  return result;
}

static size_t read_samples(sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_read(ft, p->codec, samples, length);
}

static int stopread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_stopread(&p->codec);

  lsx_ffmpeg_container_stopread(&p->container);
  return result;
}

static int startwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context;
  int result;

  if (ft->encoding.bits_per_sample != 16 && ft->encoding.bits_per_sample != 24) {
    lsx_fail_errno(ft, SOX_EFMT, "ALAC encoding supports 16-bit or 24-bit PCM");
    return SOX_EOF;
  }
  result = lsx_ffmpeg_codec_startwrite(ft, &p->codec, &definition);
  if (result != SOX_SUCCESS)
    return result;
  warn_alac_layout(ft, "Encoding");
  context = lsx_ffmpeg_codec_context(p->codec);
  result = lsx_ffmpeg_container_startwrite(ft, &p->container, "ipod", context);
  if (result != SOX_SUCCESS) {
    lsx_ffmpeg_codec_stopwrite(ft, &p->codec);
    lsx_ffmpeg_container_stopwrite(ft, &p->container);
  }
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
  int codec_result = lsx_ffmpeg_codec_stopwrite(ft, &p->codec);
  int container_result = lsx_ffmpeg_container_stopwrite(ft, &p->container);

  return codec_result != SOX_SUCCESS ? codec_result : container_result;
}

LSX_FORMAT_HANDLER(alac)
{
  static char const * const names[] = {"m4a", NULL};
  static unsigned const encodings[] = {
    SOX_ENCODING_ALAC, 16, 24, 0, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "Apple Lossless audio in an M4A container",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread,
    read_samples,
    stopread,
    startwrite,
    write_samples,
    stopwrite,
    NULL,
    encodings,
    NULL,
    sizeof(priv_t)
  };

  return &handler;
}

#endif
