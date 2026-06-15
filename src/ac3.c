/* ATSC A/52 AC-3 elementary stream format.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "ffmpeg-codec.h"

#ifdef HAVE_FFMPEG_CODECS

typedef struct {
  lsx_ffmpeg_codec_t * codec;
} priv_t;

/* AC-3 needs nothing beyond the adapter's defaults: its elementary stream is
 * self-synchronising, so FFmpeg's own bitstream parser finds the frames, and
 * A/52 fixes the channel order, so the canonical layouts apply as they are.
 * The bit rate bounds are the ones A/52 allows. */
static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AC3,
  SOX_ENCODING_AC3,
  "AC-3",
  6,                            /* max_decode_channels */
  sox_false,                    /* accept_unspecified_decode_layout */
  6,                            /* max_encode_channels */
  24,                           /* precision */
  448000,                       /* default_bit_rate */
  32000,                        /* minimum_bit_rate */
  640000,                       /* maximum_bit_rate */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  NULL,                         /* prepare_decoder */
  NULL,                         /* prepare_encoder */
  NULL,                         /* packet_reader */
  NULL,                         /* packet_writer */
  sox_false,                    /* use_compression_level */
  0,                            /* default_compression_level */
  0,                            /* minimum_compression_level */
  0,                            /* maximum_compression_level */
  NULL                          /* select_layout */
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);
}

static size_t read_samples(sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_read(ft, p->codec, samples, length);
}

static int stopread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopread(&p->codec);
}

static int startwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startwrite(ft, &p->codec, &definition);
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

LSX_FORMAT_HANDLER(ac3)
{
  static char const * const names[] = {"ac3", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_AC3, 0, 0};
  static sox_rate_t const rates[] = {32000, 44100, 48000, 0};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "ATSC A/52 AC-3 lossy audio compression",
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
    rates,
    sizeof(priv_t)
  };

  return &handler;
}

#endif
