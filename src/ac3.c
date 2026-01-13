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

static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AC3,
  SOX_ENCODING_AC3,
  "AC-3",
  6,
  sox_false,
  6,
  24,
  448000,
  32000,
  640000,
  AV_PROFILE_UNKNOWN,
  NULL,
  NULL,
  NULL,
  NULL
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);
}

static size_t read_samples(
    sox_format_t * ft,
    sox_sample_t * samples,
    size_t length)
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

LSX_FORMAT_HANDLER(ac3)
{
  static char const * const names[] = {"ac3", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_AC3, 0, 0};
  static sox_rate_t const rates[] = {32000, 44100, 48000, 0};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "ATSC A/52 AC-3 lossy audio compression",
    names,
    SOX_FILE_CODEC_OPTIONS,
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
