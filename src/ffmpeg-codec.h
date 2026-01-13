/* Common libavcodec adapter for SoX format handlers.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_FFMPEG_CODEC_H
#define LSX_FFMPEG_CODEC_H

#include "sox.h"

#include <libavcodec/avcodec.h>

typedef struct lsx_ffmpeg_codec_t lsx_ffmpeg_codec_t;

typedef int (*lsx_ffmpeg_codec_encoder_preparer_t)(
    AVCodecContext * context);

typedef int (*lsx_ffmpeg_codec_packet_reader_t)(
    sox_format_t * ft,
    AVPacket * packet);

typedef int (*lsx_ffmpeg_codec_packet_writer_t)(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet);

typedef struct {
  enum AVCodecID codec_id;
  sox_encoding_t encoding;
  char const * name;
  unsigned max_decode_channels;
  sox_bool accept_unspecified_decode_layout;
  unsigned max_encode_channels;
  unsigned precision;
  int64_t default_bit_rate;
  int64_t minimum_bit_rate;
  int64_t maximum_bit_rate;
  int ignored_metadata_profile;
  char const * ignored_metadata_name;
  lsx_ffmpeg_codec_encoder_preparer_t prepare_encoder;
  lsx_ffmpeg_codec_packet_reader_t packet_reader;
  lsx_ffmpeg_codec_packet_writer_t packet_writer;
} lsx_ffmpeg_codec_definition_t;

int lsx_ffmpeg_codec_startread(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition);

size_t lsx_ffmpeg_codec_read(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t * samples,
    size_t length);

int lsx_ffmpeg_codec_stopread(lsx_ffmpeg_codec_t ** state);

int lsx_ffmpeg_codec_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition);

size_t lsx_ffmpeg_codec_write(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t const * samples,
    size_t length);

int lsx_ffmpeg_codec_stopwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state);

#endif
