/* AAC elementary stream with LOAS/LATM framing.
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

#include <libavutil/opt.h>

#include <stdint.h>
#include <string.h>

#define LATM_CONFIG_INTERVAL 20
#define LATM_MAX_ASC_SIZE 1024

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  uint8_t packet[LSX_LOAS_MAX_PACKET_SIZE];
  sox_bool configured;
  unsigned config_counter;
} priv_t;

typedef struct {
  uint8_t * data;
  size_t size_bits;
  size_t position;
} bit_writer_t;

static int write_bits(
    bit_writer_t * writer,
    unsigned count,
    uint32_t value)
{
  unsigned i;

  if (count > 32 || writer->position > writer->size_bits ||
      count > writer->size_bits - writer->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = writer->position++;
    uint32_t bit = (value >> (count - i - 1)) & 1;

    if (bit)
      writer->data[position / 8] |=
          (uint8_t)(1U << (7 - position % 8));
  }
  return SOX_SUCCESS;
}

static int copy_bits(
    bit_writer_t * writer,
    uint8_t const * source,
    size_t count)
{
  size_t i;

  if (writer->position > writer->size_bits ||
      count > writer->size_bits - writer->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    uint32_t bit =
        (source[i / 8] >> (7 - i % 8)) & 1;

    if (write_bits(writer, 1, bit) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int write_latm_value(
    bit_writer_t * writer,
    uint32_t value)
{
  unsigned bytes = 1;
  unsigned i;

  if (value > 0xffffff)
    bytes = 4;
  else if (value > 0xffff)
    bytes = 3;
  else if (value > 0xff)
    bytes = 2;
  if (write_bits(writer, 2, bytes - 1) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = bytes; i; --i)
    if (write_bits(writer, 8,
          value >> ((i - 1) * 8)) != SOX_SUCCESS)
      return SOX_EOF;
  return SOX_SUCCESS;
}

static int audio_object_type(
    uint8_t const * config,
    size_t config_size,
    uint32_t * object_type)
{
  uint32_t value;

  if (config_size == 0)
    return SOX_EOF;
  value = config[0] >> 3;
  if (value == 31) {
    if (config_size < 2)
      return SOX_EOF;
    value = 32 + (((uint32_t)config[0] & 7) << 3) +
        (config[1] >> 5);
  }
  *object_type = value;
  return SOX_SUCCESS;
}

static sox_bool supported_object_type(uint32_t object_type)
{
  return object_type == 2 ||
      object_type == 5 ||
      object_type == 29;
}

static int read_latm_packet(
    sox_format_t * ft,
    AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint32_t object_type;
  size_t packet_size;
  int config;
  int result = lsx_loas_read_packet(ft, p->packet,
      sizeof(p->packet), &packet_size, sox_true, "AAC");

  if (result != 1)
    return result;
  config = lsx_latm_config_object_type(
      p->packet, packet_size, &object_type);
  if (config == SOX_EOF) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Invalid or unsupported AAC LATM StreamMuxConfig");
    return SOX_EOF;
  }
  if (config == 1) {
    if (!supported_object_type(object_type)) {
      lsx_fail_errno(ft, SOX_EFMT,
          "AAC LATM object type %u is unsupported; expected "
          "AAC-LC, HE-AAC or HE-AACv2", object_type);
      return SOX_EOF;
    }
    p->configured = sox_true;
  }
  else if (!p->configured) {
    lsx_fail_errno(ft, SOX_EHDR,
        "AAC LATM stream starts without a StreamMuxConfig");
    return SOX_EOF;
  }

  result = av_new_packet(packet, (int)packet_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate AAC LATM packet");
    return SOX_EOF;
  }
  memcpy(packet->data, p->packet, packet_size);
  return 1;
}

static int prepare_latm_encoder(AVCodecContext * context)
{
  /* Quad and 6.1 require a PCE.  Force one for 7.1 as well because channel
   * configuration 7 describes the different 7.1(wide) layout. */
  if (context->ch_layout.nb_channels == 4 ||
      context->ch_layout.nb_channels >= 7)
    return av_opt_set_int(
        context->priv_data, "aac_pce", 1, 0);
  return 0;
}

static int write_stream_mux_config(
    bit_writer_t * writer,
    AVCodecContext const * context)
{
  uint32_t asc_bits =
      (uint32_t)context->extradata_size * 8;

  /* audioMuxVersion 1 makes the AudioSpecificConfig length explicit, so
   * the complete encoder configuration, including any PCE, is preserved. */
  if (write_bits(writer, 1, 1) != SOX_SUCCESS ||
      write_bits(writer, 1, 0) != SOX_SUCCESS ||
      write_latm_value(writer, 0) != SOX_SUCCESS ||
      write_bits(writer, 1, 1) != SOX_SUCCESS ||
      write_bits(writer, 6, 0) != SOX_SUCCESS ||
      write_bits(writer, 4, 0) != SOX_SUCCESS ||
      write_bits(writer, 3, 0) != SOX_SUCCESS ||
      write_latm_value(writer, asc_bits) != SOX_SUCCESS ||
      copy_bits(writer, context->extradata, asc_bits) != SOX_SUCCESS ||
      write_bits(writer, 3, 0) != SOX_SUCCESS ||
      write_bits(writer, 8, 0xff) != SOX_SUCCESS ||
      write_bits(writer, 1, 0) != SOX_SUCCESS ||
      write_bits(writer, 1, 0) != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int write_latm_packet(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  bit_writer_t writer;
  uint32_t object_type;
  size_t remaining;
  size_t body_size;
  size_t packet_size;
  sox_bool write_config;

  if (packet->size == 0)
    return SOX_SUCCESS;
  if (context->extradata == NULL ||
      context->extradata_size < 2 ||
      context->extradata_size > LATM_MAX_ASC_SIZE ||
      audio_object_type(context->extradata,
          (size_t)context->extradata_size,
          &object_type) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "AAC encoder did not provide a valid AudioSpecificConfig");
    return SOX_EOF;
  }
  if (object_type != 2) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC LATM encoding supports AAC-LC only");
    return SOX_EOF;
  }
  if (packet->size < 0 ||
      (size_t)packet->size > LSX_LOAS_MAX_FRAME_SIZE) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC frame is too large for the 13-bit LOAS frame length");
    return SOX_EOF;
  }

  memset(p->packet, 0, sizeof(p->packet));
  writer.data = p->packet + LSX_LOAS_HEADER_SIZE;
  writer.size_bits = LSX_LOAS_MAX_FRAME_SIZE * 8;
  writer.position = 0;
  write_config = p->config_counter == 0;
  if (write_bits(&writer, 1, !write_config) != SOX_SUCCESS ||
      (write_config &&
       write_stream_mux_config(&writer, context) != SOX_SUCCESS)) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC StreamMuxConfig is too large for LOAS/LATM");
    return SOX_EOF;
  }

  remaining = (size_t)packet->size;
  while (remaining >= 255) {
    if (write_bits(&writer, 8, 255) != SOX_SUCCESS)
      goto too_large;
    remaining -= 255;
  }
  if (write_bits(&writer, 8, (uint32_t)remaining) != SOX_SUCCESS)
    goto too_large;

  if (packet->size && (packet->data[0] & 0xe1) == 0x81) {
    uint8_t first = packet->data[0] & 0xfe;

    if (copy_bits(&writer, &first, 8) != SOX_SUCCESS ||
        copy_bits(&writer, packet->data + 1,
            ((size_t)packet->size - 1) * 8) != SOX_SUCCESS)
      goto too_large;
  }
  else if (copy_bits(&writer, packet->data,
        (size_t)packet->size * 8) != SOX_SUCCESS)
    goto too_large;

  body_size = (writer.position + 7) / 8;
  if (body_size > LSX_LOAS_MAX_FRAME_SIZE)
    goto too_large;
  p->packet[0] = 0x56;
  p->packet[1] = (uint8_t)(0xe0 | (body_size >> 8));
  p->packet[2] = (uint8_t)body_size;
  packet_size = LSX_LOAS_HEADER_SIZE + body_size;
  if (lsx_writebuf(ft, p->packet, packet_size) != packet_size)
    return SOX_EOF;
  p->config_counter =
      (p->config_counter + 1) % LATM_CONFIG_INTERVAL;
  return SOX_SUCCESS;

too_large:
  lsx_fail_errno(ft, SOX_EFMT,
      "AAC packet is too large for the 13-bit LOAS frame length");
  return SOX_EOF;
}

static lsx_ffmpeg_codec_definition_t const read_definition = {
  AV_CODEC_ID_AAC_LATM,
  SOX_ENCODING_AAC,
  "AAC LATM",
  8,
  sox_true,
  0,
  24,
  0,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  NULL,
  NULL,
  read_latm_packet,
  NULL
};

static lsx_ffmpeg_codec_definition_t const write_definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_AAC,
  "AAC LATM",
  0,
  sox_false,
  8,
  24,
  128000,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  NULL,
  prepare_latm_encoder,
  NULL,
  write_latm_packet
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startread(
      ft, &p->codec, &read_definition);
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

  return lsx_ffmpeg_codec_startwrite(
      ft, &p->codec, &write_definition);
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

LSX_FORMAT_HANDLER(latm)
{
  static char const * const names[] = {"latm", "loas", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_AAC, 0, 0};
  static sox_rate_t const rates[] = {
    7350, 8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 64000, 88200, 96000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "AAC-LC, HE-AAC and HE-AACv2 with LOAS/LATM framing",
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
