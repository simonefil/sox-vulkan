/* xHE-AAC/USAC elementary stream with LOAS/LATM framing.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "ffmpeg-codec.h"
#include "latm-common.h"

#ifdef HAVE_FFMPEG_CODECS

#include <libavutil/mem.h>

#include <stdint.h>
#include <string.h>

#define USAC_MAX_CONFIG_SIZE 1024

/* Keep the other FFmpeg-backed formats buildable with libavcodec 61.
 * The xHE-AAC handler rejects that version before opening the decoder. */
#ifndef AV_PROFILE_AAC_USAC
#define AV_PROFILE_AAC_USAC 41
#endif

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  uint8_t loas_frame[LSX_LOAS_MAX_PACKET_SIZE];
  uint8_t pending_packet[LSX_LOAS_MAX_FRAME_SIZE];
  size_t pending_size;
  uint8_t config[USAC_MAX_CONFIG_SIZE];
  size_t config_size;
  size_t config_bits;
  sox_bool configured;
} priv_t;

typedef struct {
  uint8_t const * data;
  size_t size_bits;
  size_t position;
} bit_reader_t;

static int read_bits(
    bit_reader_t * reader,
    unsigned count,
    uint32_t * value)
{
  uint32_t result = 0;
  unsigned i;

  if (count > 32 || reader->position > reader->size_bits ||
      count > reader->size_bits - reader->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = reader->position++;

    result = (result << 1) |
        ((reader->data[position / 8] >> (7 - position % 8)) & 1);
  }
  if (value != NULL)
    *value = result;
  return SOX_SUCCESS;
}

static int read_latm_value(
    bit_reader_t * reader,
    uint32_t * value)
{
  uint32_t bytes_for_value;
  uint32_t result = 0;
  uint32_t byte;
  unsigned i;

  if (read_bits(reader, 2, &bytes_for_value) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = 0; i <= bytes_for_value; ++i) {
    if (read_bits(reader, 8, &byte) != SOX_SUCCESS)
      return SOX_EOF;
    result = (result << 8) | byte;
  }
  *value = result;
  return SOX_SUCCESS;
}

static int copy_bits(
    bit_reader_t * reader,
    uint8_t * destination,
    size_t count)
{
  size_t i;

  memset(destination, 0, (count + 7) / 8);
  if (reader->position > reader->size_bits ||
      count > reader->size_bits - reader->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t source_position = reader->position++;

    if ((reader->data[source_position / 8] >>
          (7 - source_position % 8)) & 1)
      destination[i / 8] |= (uint8_t)(1U << (7 - i % 8));
  }
  return SOX_SUCCESS;
}

static int audio_object_type(
    uint8_t const * config,
    size_t config_bits,
    uint32_t * object_type)
{
  bit_reader_t reader = {config, config_bits, 0};
  uint32_t value;

  if (read_bits(&reader, 5, &value) != SOX_SUCCESS)
    return SOX_EOF;
  if (value == 31) {
    uint32_t extension;

    if (read_bits(&reader, 6, &extension) != SOX_SUCCESS)
      return SOX_EOF;
    value = 32 + extension;
  }
  *object_type = value;
  return SOX_SUCCESS;
}

static int parse_stream_mux_config(
    sox_format_t * ft,
    priv_t * p,
    bit_reader_t * reader,
    sox_bool * config_changed)
{
  uint8_t config[USAC_MAX_CONFIG_SIZE];
  uint32_t value;
  uint32_t asc_bits;
  uint32_t object_type;
  size_t config_size;
  sox_bool was_configured = p->configured;

  if (read_bits(reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      read_bits(reader, 1, &value) != SOX_SUCCESS || value != 0 ||
      read_latm_value(reader, &value) != SOX_SUCCESS ||
      read_bits(reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      read_bits(reader, 6, &value) != SOX_SUCCESS || value != 0 ||
      read_bits(reader, 4, &value) != SOX_SUCCESS || value != 0 ||
      read_bits(reader, 3, &value) != SOX_SUCCESS || value != 0) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Unsupported xHE-AAC LATM configuration; expected one "
        "synchronous program and layer with audioMuxVersion 1");
    return SOX_EOF;
  }
  if (read_latm_value(reader, &asc_bits) != SOX_SUCCESS ||
      asc_bits == 0 ||
      asc_bits > USAC_MAX_CONFIG_SIZE * 8U) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Invalid xHE-AAC AudioSpecificConfig length");
    return SOX_EOF;
  }

  config_size = (asc_bits + 7) / 8;
  if (copy_bits(reader, config, asc_bits) != SOX_SUCCESS ||
      audio_object_type(config, asc_bits, &object_type) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Truncated xHE-AAC AudioSpecificConfig");
    return SOX_EOF;
  }
  if (object_type != 42) {
    lsx_fail_errno(ft, SOX_EFMT,
        "LOAS/LATM AudioSpecificConfig is not xHE-AAC/USAC");
    return SOX_EOF;
  }

  if (read_bits(reader, 3, &value) != SOX_SUCCESS || value != 0 ||
      read_bits(reader, 8, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Unsupported xHE-AAC LATM frame length configuration");
    return SOX_EOF;
  }
  if (read_bits(reader, 1, &value) != SOX_SUCCESS || value != 0) {
    lsx_fail_errno(ft, SOX_EHDR,
        "xHE-AAC LATM otherData is unsupported");
    return SOX_EOF;
  }
  if (read_bits(reader, 1, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Truncated xHE-AAC StreamMuxConfig");
    return SOX_EOF;
  }
  if (value && read_bits(reader, 8, NULL) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Truncated xHE-AAC StreamMuxConfig CRC");
    return SOX_EOF;
  }

  *config_changed = was_configured &&
      (p->config_bits != asc_bits ||
       p->config_size != config_size ||
       memcmp(p->config, config, config_size));
  memcpy(p->config, config, config_size);
  p->config_size = config_size;
  p->config_bits = asc_bits;
  p->configured = sox_true;
  return SOX_SUCCESS;
}

static int parse_audio_mux_element(
    sox_format_t * ft,
    priv_t * p,
    uint8_t * destination,
    size_t * destination_size,
    sox_bool * config_changed,
    size_t frame_size)
{
  bit_reader_t reader = {
    p->loas_frame + LSX_LOAS_HEADER_SIZE,
    (frame_size - LSX_LOAS_HEADER_SIZE) * 8,
    0
  };
  uint32_t value;
  size_t payload_size = 0;

  *config_changed = sox_false;
  if (read_bits(&reader, 1, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated xHE-AAC AudioMuxElement");
    return SOX_EOF;
  }
  if (!value) {
    if (parse_stream_mux_config(
          ft, p, &reader, config_changed) != SOX_SUCCESS)
      return SOX_EOF;
  }
  else if (!p->configured) {
    lsx_fail_errno(ft, SOX_EHDR,
        "xHE-AAC LATM stream starts without a StreamMuxConfig");
    return SOX_EOF;
  }

  do {
    if (read_bits(&reader, 8, &value) != SOX_SUCCESS ||
        payload_size > LSX_LOAS_MAX_FRAME_SIZE - value) {
      lsx_fail_errno(ft, SOX_EHDR,
          "Invalid xHE-AAC LATM payload length");
      return SOX_EOF;
    }
    payload_size += value;
  } while (value == 255);

  if (payload_size == 0 ||
      copy_bits(&reader, destination, payload_size * 8) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Truncated or empty xHE-AAC access unit");
    return SOX_EOF;
  }
  *destination_size = payload_size;
  return SOX_SUCCESS;
}

static int read_loas_access_unit(
    sox_format_t * ft,
    priv_t * p,
    uint8_t * destination,
    size_t * destination_size,
    sox_bool * config_changed,
    sox_bool clean_eof)
{
  size_t frame_size;
  int result = lsx_loas_read_packet(ft, p->loas_frame,
      sizeof(p->loas_frame), &frame_size, clean_eof, "xHE-AAC");

  if (result != 1)
    return result;
  return parse_audio_mux_element(ft, p, destination,
      destination_size, config_changed, frame_size) == SOX_SUCCESS ? 1 :
      SOX_EOF;
}

static int prepare_usac_decoder(
    sox_format_t * ft,
    AVCodecContext * context)
{
  priv_t * p = (priv_t *)ft->priv;
  sox_bool config_changed;
  int result;

  if ((avcodec_version() >> 16) < 62) {
    lsx_fail_errno(ft, SOX_EFMT,
        "xHE-AAC decoding requires FFmpeg 8.0 or later");
    return SOX_EOF;
  }
  result = read_loas_access_unit(ft, p, p->pending_packet,
      &p->pending_size, &config_changed, sox_true);
  if (result != 1) {
    if (result == 0)
      lsx_fail_errno(ft, SOX_EHDR, "xHE-AAC stream contains no audio");
    return SOX_EOF;
  }

  context->extradata = av_mallocz(
      p->config_size + AV_INPUT_BUFFER_PADDING_SIZE);
  if (context->extradata == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate xHE-AAC decoder configuration");
    return SOX_EOF;
  }
  memcpy(context->extradata, p->config, p->config_size);
  context->extradata_size = (int)p->config_size;
  lsx_warn("`%s': xHE-AAC loudness and dynamic-range metadata, if "
      "present, will be ignored", ft->filename);
  return SOX_SUCCESS;
}

static int read_usac_packet(
    sox_format_t * ft,
    AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint8_t access_unit[LSX_LOAS_MAX_FRAME_SIZE];
  uint8_t * new_config;
  size_t access_unit_size;
  sox_bool config_changed = sox_false;
  int result;

  if (p->pending_size) {
    memcpy(access_unit, p->pending_packet, p->pending_size);
    access_unit_size = p->pending_size;
    p->pending_size = 0;
  }
  else {
    result = read_loas_access_unit(ft, p, access_unit,
        &access_unit_size, &config_changed, sox_true);
    if (result != 1)
      return result;
  }

  result = av_new_packet(packet, (int)access_unit_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate xHE-AAC compressed audio packet");
    return SOX_EOF;
  }
  memcpy(packet->data, access_unit, access_unit_size);
  if (!config_changed)
    return 1;

  new_config = av_packet_new_side_data(packet,
      AV_PKT_DATA_NEW_EXTRADATA, p->config_size);
  if (new_config == NULL) {
    av_packet_unref(packet);
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to attach updated xHE-AAC decoder configuration");
    return SOX_EOF;
  }
  memcpy(new_config, p->config, p->config_size);
  return 1;
}

static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_USAC,
  "xHE-AAC",
  8,
  sox_true,
  0,
  24,
  0,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_AAC_USAC,
  prepare_usac_decoder,
  NULL,
  read_usac_packet,
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

LSX_FORMAT_HANDLER(usac)
{
  static char const * const names[] = {
    "usac", "xheaac", "xhe-aac", NULL
  };
  static unsigned const encodings[] = {SOX_ENCODING_USAC, 0, 0};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "xHE-AAC/USAC with LOAS/LATM framing (decode only)",
    names,
    SOX_FILE_CODEC_OPTIONS,
    startread,
    read_samples,
    stopread,
    NULL,
    NULL,
    NULL,
    NULL,
    encodings,
    NULL,
    sizeof(priv_t)
  };

  return &handler;
}

#endif
