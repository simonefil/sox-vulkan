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

#include <stddef.h>
#include <stdint.h>

/* Bit cursors over a buffer the caller owns, most significant bit first.
 * The handlers around these codecs all have to take apart or assemble a
 * transport header FFmpeg does not deal with itself -- ADTS, LOAS/LATM, an
 * AudioSpecificConfig -- and they all read it in the same bit order, so the
 * plumbing lives here once instead of in each of them.
 *
 * data is not owned; size_bits is its length in bits, not bytes; position is
 * the cursor, also in bits, and only ever advances.  A writer expects its
 * buffer to start zeroed: lsx_bit_write sets the one bits and leaves the zero
 * bits untouched. */
typedef struct {
  uint8_t const * data;
  size_t size_bits;
  size_t position;
} lsx_bit_reader_t;

typedef struct {
  uint8_t * data;
  size_t size_bits;
  size_t position;
} lsx_bit_writer_t;

/* Read count bits (at most 32) into *value, which may be NULL to skip them.
 * Returns SOX_EOF without moving the cursor if the field does not fit. */
int lsx_bit_read(lsx_bit_reader_t * reader, unsigned count, uint32_t * value);

/* Write the low count bits (at most 32) of value.  Returns SOX_EOF without
 * moving the cursor if the field does not fit. */
int lsx_bit_write(lsx_bit_writer_t * writer, unsigned count, uint32_t value);

typedef struct lsx_ffmpeg_codec_t lsx_ffmpeg_codec_t;

typedef int (*lsx_ffmpeg_codec_decoder_preparer_t)(sox_format_t * ft, AVCodecContext * context);

typedef int (*lsx_ffmpeg_codec_encoder_preparer_t)(AVCodecContext * context);

typedef int (*lsx_ffmpeg_codec_packet_reader_t)(sox_format_t * ft, AVPacket * packet);

typedef int (*lsx_ffmpeg_codec_packet_writer_t)(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet);

typedef int (*lsx_ffmpeg_codec_layout_selector_t)(unsigned channels, AVChannelLayout * layout);

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
  int required_decode_profile;
  lsx_ffmpeg_codec_decoder_preparer_t prepare_decoder;
  lsx_ffmpeg_codec_encoder_preparer_t prepare_encoder;
  lsx_ffmpeg_codec_packet_reader_t packet_reader;
  lsx_ffmpeg_codec_packet_writer_t packet_writer;
  sox_bool use_compression_level;
  int default_compression_level;
  int minimum_compression_level;
  int maximum_compression_level;
  lsx_ffmpeg_codec_layout_selector_t select_layout;
} lsx_ffmpeg_codec_definition_t;

int lsx_ffmpeg_codec_startread(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition);

size_t lsx_ffmpeg_codec_read(sox_format_t * ft, lsx_ffmpeg_codec_t * state, sox_sample_t * samples, size_t length);

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

int lsx_ffmpeg_codec_stopwrite(sox_format_t * ft, lsx_ffmpeg_codec_t ** state);

AVCodecContext const * lsx_ffmpeg_codec_context(lsx_ffmpeg_codec_t const * state);

void lsx_ffmpeg_codec_print_format_layouts(char const * format_name);

#endif
