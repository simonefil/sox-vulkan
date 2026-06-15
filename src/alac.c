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

/* ALAC is the one format here that needs both adapters: the codec adapter
 * drives the encoder and decoder, and the container adapter supplies its
 * packet hooks from the surrounding M4A.  The two are started and stopped in
 * a fixed order below, since the codec's decoder cannot be configured until
 * the container has produced its parameters, while on the write side the
 * container cannot be described until the encoder is open. */
typedef struct {
  lsx_ffmpeg_codec_t * codec;
  lsx_ffmpeg_container_t * container;
} priv_t;

/* Apple's channel layouts, which differ from SoX's canonical ones above four
 * channels: 4.0 rather than quad, 6.1 back rather than 6.1, and 7.1 wide back
 * rather than 7.1.  A file written with the canonical layouts would be read
 * by Apple software with its channels in the wrong places, so the layouts the
 * format actually uses are what this handler selects.  Follows the libavutil
 * convention: 0 on success, negative AVERROR otherwise. */
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

/* Spell out the channel order for the layouts where Apple's choice is not the
 * one SoX would otherwise assume, since the samples are passed through
 * unremixed and it is the user who has to line them up.  Silent when
 * --channel-layout was given: the order was then stated deliberately. */
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

/* Open the M4A and configure the decoder from the stream inside it.  An ALAC
 * packet says nothing about its own sample rate, channel count or bit depth,
 * so the container's magic cookie is the only source for them; this hook runs
 * before the decoder is opened, which is exactly when it is needed. */
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

/* Mux one encoded packet.  The container is opened after the encoder, so a
 * packet could in principle arrive before it exists; nothing in the current
 * ordering produces one, and the guard turns that into a clear failure rather
 * than a null dereference. */
static int write_alac_packet(sox_format_t * ft, AVCodecContext const * context, AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;

  if (p->container == NULL) {
    lsx_fail_errno(ft, SOX_EFMT, "M4A output container is unavailable");
    return SOX_EOF;
  }
  return lsx_ffmpeg_container_write_packet(ft, p->container, context, packet);
}

/* Being lossless, ALAC has no bit rate to set and no fixed precision to
 * report: the depth comes from the file on the way in and from -b on the way
 * out, so precision stays 0 and the adapter reports the real bits per sample.
 * -C is a compression level instead, 0 to 2, where 2 is FFmpeg's default. */
static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_ALAC,
  SOX_ENCODING_ALAC,
  "ALAC",
  8,                            /* max_decode_channels */
  sox_false,                    /* accept_unspecified_decode_layout */
  8,                            /* max_encode_channels */
  0,                            /* precision */
  0,                            /* default_bit_rate */
  0,                            /* minimum_bit_rate */
  0,                            /* maximum_bit_rate */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  prepare_alac_decoder,         /* prepare_decoder */
  NULL,                         /* prepare_encoder */
  read_alac_packet,             /* packet_reader */
  write_alac_packet,            /* packet_writer */
  sox_true,                     /* use_compression_level */
  2,                            /* default_compression_level */
  0,                            /* minimum_compression_level */
  2,                            /* maximum_compression_level */
  select_alac_layout            /* select_layout */
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);

  /* prepare_alac_decoder may have opened the container before the codec went
   * on to fail, and the codec adapter knows nothing about it, so the cleanup
   * belongs here. */
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

  /* FFmpeg's ALAC encoder implements only these two depths. */
  if (ft->encoding.bits_per_sample != 16 && ft->encoding.bits_per_sample != 24) {
    lsx_fail_errno(ft, SOX_EFMT, "ALAC encoding supports 16-bit or 24-bit PCM");
    return SOX_EOF;
  }
  result = lsx_ffmpeg_codec_startwrite(ft, &p->codec, &definition);
  if (result != SOX_SUCCESS)
    return result;
  warn_alac_layout(ft, "Encoding");
  /* The container has to describe the stream it will carry, which is only
   * settled once the encoder is open -- hence this order, and hence the
   * codec's context being handed over rather than rebuilt.  "ipod" is the
   * MP4 variant whose brand and tags Apple software expects for M4A. */
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
  /* The codec is stopped first so its final packets reach the muxer before
   * the trailer is written.  Both run whatever happens -- stopping only one
   * would leak the other, and a file without its trailer is unplayable -- so
   * the first failure is the one reported. */
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
