/* AAC elementary stream with ADTS framing.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "ffmpeg-codec.h"

#ifdef HAVE_FFMPEG_CODECS

#include <libavcodec/adts_parser.h>
#include <libavutil/opt.h>

#include <stdint.h>

#define ADTS_MAX_FRAME_SIZE 0x1fff
#define AAC_MAX_PCE_SIZE 40

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  sox_bool adts_configured;
  unsigned object_type;
  unsigned rate_index;
  unsigned channel_configuration;
  uint8_t pce[AAC_MAX_PCE_SIZE];
  size_t pce_size;
  sox_bool pce_pending;
} priv_t;

typedef struct {
  uint8_t const * data;
  size_t size_bits;
  size_t position;
} bit_reader_t;

typedef struct {
  uint8_t * data;
  size_t size_bits;
  size_t position;
} bit_writer_t;

static int read_exact(
    sox_format_t * ft,
    uint8_t * data,
    size_t size,
    sox_bool clean_eof)
{
  size_t done = 0;

  while (done < size) {
    size_t count = lsx_readbuf(ft, data + done, size - done);

    if (count == 0) {
      if (done == 0 && clean_eof)
        return 0;
      lsx_fail_errno(ft, SOX_EHDR,
          "Truncated AAC ADTS frame or metadata tag");
      return SOX_EOF;
    }
    done += count;
  }
  return 1;
}

static int discard_bytes(sox_format_t * ft, size_t size)
{
  uint8_t buffer[1024];

  while (size) {
    size_t count = min(size, sizeof(buffer));

    if (read_exact(ft, buffer, count, sox_false) != 1)
      return SOX_EOF;
    size -= count;
  }
  return SOX_SUCCESS;
}

static int read_adts_packet(
    sox_format_t * ft,
    AVPacket * packet)
{
  uint8_t header[10];

  for (;;) {
    size_t payload_size;
    unsigned frame_size;
    uint32_t samples;
    uint8_t frames;
    unsigned header_size;
    int result = read_exact(ft, header, 3, sox_true);

    if (result != 1)
      return result;
    if (!memcmp(header, "ID3", 3)) {
      result = read_exact(ft, header + 3, 7, sox_false);
      if (result != 1)
        return result;
      if (header[3] < 2 || header[3] > 4 || header[4] == 0xff ||
          (header[6] & 0x80) || (header[7] & 0x80) ||
          (header[8] & 0x80) || (header[9] & 0x80)) {
        lsx_fail_errno(ft, SOX_EHDR, "Invalid ID3v2 tag in AAC stream");
        return SOX_EOF;
      }
      payload_size =
          ((size_t)header[6] << 21) +
          ((size_t)header[7] << 14) +
          ((size_t)header[8] << 7) +
          header[9];
      if (header[3] == 4 && (header[5] & 0x10))
        payload_size += 10;
      if (discard_bytes(ft, payload_size) != SOX_SUCCESS)
        return SOX_EOF;
      continue;
    }
    if (!memcmp(header, "TAG", 3)) {
      if (discard_bytes(ft, 125) != SOX_SUCCESS)
        return SOX_EOF;
      continue;
    }

    result = read_exact(
        ft, header + 3, AV_AAC_ADTS_HEADER_SIZE - 3, sox_false);
    if (result != 1)
      return result;
    frame_size =
        ((header[3] & 3) << 11) | (header[4] << 3) | (header[5] >> 5);
    header_size = (header[1] & 1) ?
        AV_AAC_ADTS_HEADER_SIZE : AV_AAC_ADTS_HEADER_SIZE + 2;
    if (header[0] != 0xff || (header[1] & 0xf6) != 0xf0 ||
        frame_size < header_size ||
        av_adts_header_parse(header, &samples, &frames) < 0) {
      lsx_fail_errno(ft, SOX_EHDR, "Invalid AAC ADTS frame header");
      return SOX_EOF;
    }

    result = av_new_packet(packet, (int)frame_size);
    if (result < 0) {
      lsx_fail_errno(ft, SOX_ENOMEM,
          "Unable to allocate AAC compressed audio packet");
      return SOX_EOF;
    }
    memcpy(packet->data, header, AV_AAC_ADTS_HEADER_SIZE);
    result = read_exact(ft,
        packet->data + AV_AAC_ADTS_HEADER_SIZE,
        frame_size - AV_AAC_ADTS_HEADER_SIZE, sox_false);
    if (result != 1) {
      av_packet_unref(packet);
      return result;
    }
    return 1;
  }
}

static int sampling_frequency_index(int rate)
{
  static int const rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350
  };
  size_t i;

  for (i = 0; i < array_length(rates); ++i)
    if (rates[i] == rate)
      return (int)i;
  return -1;
}

static int read_bits(
    bit_reader_t * reader,
    unsigned count,
    unsigned * value)
{
  unsigned result = 0;
  unsigned i;

  if (count > 16 || reader->position > reader->size_bits ||
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

static int write_bits(
    bit_writer_t * writer,
    unsigned count,
    unsigned value)
{
  unsigned i;

  if (count > 16 || writer->position > writer->size_bits ||
      count > writer->size_bits - writer->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = writer->position++;
    unsigned bit = (value >> (count - i - 1)) & 1;

    if (bit)
      writer->data[position / 8] |=
          (uint8_t)(1U << (7 - position % 8));
  }
  return SOX_SUCCESS;
}

static int copy_bits(
    bit_reader_t * reader,
    bit_writer_t * writer,
    unsigned count,
    unsigned * value)
{
  unsigned copied;

  if (read_bits(reader, count, &copied) != SOX_SUCCESS ||
      write_bits(writer, count, copied) != SOX_SUCCESS)
    return SOX_EOF;
  if (value != NULL)
    *value = copied;
  return SOX_SUCCESS;
}

static int align_reader(bit_reader_t * reader)
{
  size_t aligned = (reader->position + 7) & ~(size_t)7;

  if (aligned > reader->size_bits)
    return SOX_EOF;
  reader->position = aligned;
  return SOX_SUCCESS;
}

static int align_writer(bit_writer_t * writer)
{
  while (writer->position & 7)
    if (write_bits(writer, 1, 0) != SOX_SUCCESS)
      return SOX_EOF;
  return SOX_SUCCESS;
}

static int extract_pce(
    bit_reader_t * reader,
    uint8_t * destination,
    size_t * destination_size)
{
  bit_writer_t writer = {
    destination,
    AAC_MAX_PCE_SIZE * 8,
    0
  };
  unsigned five_bit_elements;
  unsigned four_bit_elements;
  unsigned count;
  unsigned flag;
  unsigned comment_size;

  memset(destination, 0, AAC_MAX_PCE_SIZE);
  if (write_bits(&writer, 3, 5) != SOX_SUCCESS ||
      copy_bits(reader, &writer, 10, NULL) != SOX_SUCCESS ||
      copy_bits(reader, &writer, 4, &five_bit_elements) != SOX_SUCCESS ||
      copy_bits(reader, &writer, 4, &count) != SOX_SUCCESS)
    return SOX_EOF;
  five_bit_elements += count;
  if (copy_bits(reader, &writer, 4, &count) != SOX_SUCCESS)
    return SOX_EOF;
  five_bit_elements += count;
  if (copy_bits(reader, &writer, 2, &four_bit_elements) != SOX_SUCCESS ||
      copy_bits(reader, &writer, 3, &count) != SOX_SUCCESS)
    return SOX_EOF;
  four_bit_elements += count;
  if (copy_bits(reader, &writer, 4, &count) != SOX_SUCCESS)
    return SOX_EOF;
  five_bit_elements += count;

  if (copy_bits(reader, &writer, 1, &flag) != SOX_SUCCESS)
    return SOX_EOF;
  if (flag && copy_bits(reader, &writer, 4, NULL) != SOX_SUCCESS)
    return SOX_EOF;
  if (copy_bits(reader, &writer, 1, &flag) != SOX_SUCCESS)
    return SOX_EOF;
  if (flag && copy_bits(reader, &writer, 4, NULL) != SOX_SUCCESS)
    return SOX_EOF;
  if (copy_bits(reader, &writer, 1, &flag) != SOX_SUCCESS)
    return SOX_EOF;
  if (flag && copy_bits(reader, &writer, 3, NULL) != SOX_SUCCESS)
    return SOX_EOF;

  count = five_bit_elements * 5 + four_bit_elements * 4;
  while (count) {
    unsigned chunk = min(count, 16U);

    if (copy_bits(reader, &writer, chunk, NULL) != SOX_SUCCESS)
      return SOX_EOF;
    count -= chunk;
  }
  if (align_reader(reader) != SOX_SUCCESS ||
      align_writer(&writer) != SOX_SUCCESS ||
      copy_bits(reader, &writer, 8, &comment_size) != SOX_SUCCESS)
    return SOX_EOF;
  while (comment_size--) {
    if (copy_bits(reader, &writer, 8, NULL) != SOX_SUCCESS)
      return SOX_EOF;
  }

  *destination_size = writer.position / 8;
  return *destination_size ? SOX_SUCCESS : SOX_EOF;
}

static int configure_adts(
    sox_format_t * ft,
    AVCodecContext const * context,
    priv_t * p)
{
  bit_reader_t reader;
  unsigned frame_length_flag;
  unsigned depends_on_core_coder;
  unsigned extension_flag;
  int expected_rate_index = sampling_frequency_index(context->sample_rate);

  if (context->extradata == NULL || context->extradata_size < 2) {
    lsx_fail_errno(ft, SOX_EHDR,
        "AAC encoder did not provide an AudioSpecificConfig");
    return SOX_EOF;
  }
  reader.data = context->extradata;
  reader.size_bits = (size_t)context->extradata_size * 8;
  reader.position = 0;
  if (read_bits(&reader, 5, &p->object_type) != SOX_SUCCESS ||
      read_bits(&reader, 4, &p->rate_index) != SOX_SUCCESS ||
      read_bits(&reader, 4, &p->channel_configuration) != SOX_SUCCESS ||
      read_bits(&reader, 1, &frame_length_flag) != SOX_SUCCESS ||
      read_bits(&reader, 1, &depends_on_core_coder) != SOX_SUCCESS ||
      read_bits(&reader, 1, &extension_flag) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Invalid AAC AudioSpecificConfig from encoder");
    return SOX_EOF;
  }
  if (p->object_type < 1 || p->object_type > 4 ||
      expected_rate_index < 0 || p->rate_index > 12 ||
      p->rate_index != (unsigned)expected_rate_index ||
      p->channel_configuration > 7 ||
      frame_length_flag || depends_on_core_coder || extension_flag) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC encoder configuration cannot be represented in ADTS");
    return SOX_EOF;
  }
  if (!p->channel_configuration) {
    if (extract_pce(&reader, p->pce, &p->pce_size) != SOX_SUCCESS) {
      lsx_fail_errno(ft, SOX_EHDR,
          "Invalid Program Config Element from AAC encoder");
      return SOX_EOF;
    }
    p->pce_pending = sox_true;
  }
  else if (context->ch_layout.nb_channels == 4 ||
      context->ch_layout.nb_channels >= 7) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC encoder did not provide the required Program Config Element");
    return SOX_EOF;
  }
  p->adts_configured = sox_true;
  return SOX_SUCCESS;
}

static int prepare_aac_encoder(AVCodecContext * context)
{
  /* Quad and 6.1 require a PCE.  Force one for 7.1 as well because ADTS
   * channel_configuration 7 describes the different 7.1(wide) layout. */
  if (context->ch_layout.nb_channels == 4 ||
      context->ch_layout.nb_channels >= 7)
    return av_opt_set_int(
        context->priv_data, "aac_pce", 1, 0);
  return 0;
}

static int write_adts_packet(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint8_t header[AV_AAC_ADTS_HEADER_SIZE];
  size_t frame_size;

  if (packet->size == 0)
    return SOX_SUCCESS;
  if (!p->adts_configured &&
      configure_adts(ft, context, p) != SOX_SUCCESS)
    return SOX_EOF;
  frame_size = sizeof(header) + (size_t)packet->size +
      (p->pce_pending ? p->pce_size : 0);
  if (frame_size > ADTS_MAX_FRAME_SIZE) {
    lsx_fail_errno(ft, SOX_EFMT,
        "AAC frame is too large for the 13-bit ADTS frame length");
    return SOX_EOF;
  }

  header[0] = 0xff;
  header[1] = 0xf1;
  header[2] = (uint8_t)(
      (((p->object_type - 1) & 3) << 6) |
      (p->rate_index << 2) |
      (p->channel_configuration >> 2));
  header[3] = (uint8_t)(
      ((p->channel_configuration & 3) << 6) |
      (frame_size >> 11));
  header[4] = (uint8_t)(frame_size >> 3);
  header[5] = (uint8_t)(((frame_size & 7) << 5) | 0x1f);
  header[6] = 0xfc;

  if (lsx_writebuf(ft, header, sizeof(header)) != sizeof(header))
    return SOX_EOF;
  if (p->pce_pending &&
      lsx_writebuf(ft, p->pce, p->pce_size) != p->pce_size)
    return SOX_EOF;
  if (lsx_writebuf(ft, packet->data,
          (size_t)packet->size) != (size_t)packet->size)
    return SOX_EOF;
  p->pce_pending = sox_false;
  return SOX_SUCCESS;
}

static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_AAC,
  "AAC",
  8,
  /* FFmpeg preserves canonical PCE sample order but reports 6.1 and 7.1
   * frames with AV_CHANNEL_ORDER_UNSPEC. */
  sox_true,
  8,
  24,
  128000,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  NULL,
  prepare_aac_encoder,
  read_adts_packet,
  write_adts_packet
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

LSX_FORMAT_HANDLER(aac)
{
  static char const * const names[] = {"aac", "adts", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_AAC, 0, 0};
  static sox_rate_t const rates[] = {
    7350, 8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 64000, 88200, 96000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "AAC-LC, HE-AAC and HE-AACv2 with ADTS framing",
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
