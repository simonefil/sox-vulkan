/* AAC elementary stream with LOAS/LATM framing.
 *
 * (c) Simone Filippini <info@simonefilippini.it> 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "codec.h"
#include "ffmpeg-codec.h"
#include "latm-common.h"

#ifdef HAVE_FFMPEG_CODECS

#include <stdint.h>
#include <string.h>

/* How often the StreamMuxConfig is repeated when writing, in frames.  LOAS is
 * meant to be joinable part-way through, which only works if a decoder that
 * starts anywhere meets a configuration soon after; the cost is a few dozen
 * bytes every interval. */
#define LATM_CONFIG_INTERVAL 20

/* Sanity limit on the AudioSpecificConfig the encoder hands back, so a
 * nonsensical extradata size cannot drive the bit writer below. */
#define LATM_MAX_ASC_SIZE 1024

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  /* One whole LOAS frame; used for reading or for writing, never both. */
  uint8_t packet[LSX_LOAS_MAX_PACKET_SIZE];
  sox_bool configured;          /* Read side: a StreamMuxConfig has been seen. */
  unsigned config_counter;      /* Write side: frames until the config repeats. */
} priv_t;

/* Append count bits from a byte-aligned source to the writer, which is not
 * itself aligned -- the config lands at whatever bit offset the preceding
 * fields left.  Fails without writing if the field would not fit. */
static int copy_bits(lsx_bit_writer_t * writer, uint8_t const * source, size_t count)
{
  size_t i;

  if (writer->position > writer->size_bits || count > writer->size_bits - writer->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    uint32_t bit = (source[i / 8] >> (7 - i % 8)) & 1;

    if (lsx_bit_write(writer, 1, bit) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Write a LATM variable-length integer, the inverse of lsx_latm_read_value:
 * a two-bit count of the bytes that follow, then the value in as few bytes as
 * carry it, most significant first. */
static int write_latm_value(lsx_bit_writer_t * writer, uint32_t value)
{
  unsigned bytes = 1;
  unsigned i;

  if (value > 0xffffff)
    bytes = 4;
  else if (value > 0xffff)
    bytes = 3;
  else if (value > 0xff)
    bytes = 2;
  if (lsx_bit_write(writer, 2, bytes - 1) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = bytes; i; --i)
    if (lsx_bit_write(writer, 8, value >> ((i - 1) * 8)) != SOX_SUCCESS)
      return SOX_EOF;
  return SOX_SUCCESS;
}

/* AAC-LC (2), HE-AAC (5) and HE-AACv2 (29): the object types this handler
 * claims.  USAC in the same framing belongs to the usac handler, and the
 * older or more exotic types are refused by name rather than handed to a
 * decoder that would not produce what the file describes. */
static sox_bool supported_object_type(uint32_t object_type)
{
  return object_type == 2 || object_type == 5 || object_type == 29;
}

/* Supply the next LOAS frame as an AVPacket, framing header included: the
 * AAC_LATM decoder takes whole frames, so unlike the usac handler this one
 * does not unwrap the transport itself.  It still looks inside far enough to
 * reject an object type it does not claim, and to insist that a
 * configuration arrives before any payload. */
static int read_latm_packet(sox_format_t * ft, AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint32_t object_type;
  size_t packet_size;
  int config;
  int result = lsx_loas_read_packet(ft, p->packet, sizeof(p->packet), &packet_size, sox_true, "AAC");

  if (result != 1)
    return result;
  config = lsx_latm_config_object_type(p->packet, packet_size, &object_type);
  if (config == SOX_EOF) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid or unsupported AAC LATM StreamMuxConfig");
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
    lsx_fail_errno(ft, SOX_EHDR, "AAC LATM stream starts without a StreamMuxConfig");
    return SOX_EOF;
  }

  result = av_new_packet(packet, (int)packet_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate AAC LATM packet");
    return SOX_EOF;
  }
  memcpy(packet->data, p->packet, packet_size);
  return 1;
}

/* Emit a StreamMuxConfig describing one synchronous programme and layer,
 * carrying the encoder's AudioSpecificConfig verbatim.  The whole structure
 * is constant apart from that config, so the fields are written as literals
 * rather than derived; a single failed write aborts the lot, which the caller
 * turns into "config too large". */
static int write_stream_mux_config(lsx_bit_writer_t * writer, AVCodecContext const * context)
{
  uint32_t asc_bits = (uint32_t)context->extradata_size * 8;

  /* audioMuxVersion 1 makes the AudioSpecificConfig length explicit, so
   * the complete encoder configuration, including any PCE, is preserved.
   *
   * In order: audioMuxVersion 1, audioMuxVersionA 0, taraBufferFullness 0,
   * allStreamsSameTimeFraming, numSubFrames 0, numProgram 0, numLayer 0,
   * the config length and the config itself, then frameLengthType 0 with
   * latmBufferFullness 0xff (the "unknown" value, since SoX does not model
   * the decoder buffer), otherDataPresent 0 and crcCheckPresent 0. */
  if (lsx_bit_write(writer, 1, 1) != SOX_SUCCESS ||
      lsx_bit_write(writer, 1, 0) != SOX_SUCCESS ||
      write_latm_value(writer, 0) != SOX_SUCCESS ||
      lsx_bit_write(writer, 1, 1) != SOX_SUCCESS ||
      lsx_bit_write(writer, 6, 0) != SOX_SUCCESS ||
      lsx_bit_write(writer, 4, 0) != SOX_SUCCESS ||
      lsx_bit_write(writer, 3, 0) != SOX_SUCCESS ||
      write_latm_value(writer, asc_bits) != SOX_SUCCESS ||
      copy_bits(writer, context->extradata, asc_bits) != SOX_SUCCESS ||
      lsx_bit_write(writer, 3, 0) != SOX_SUCCESS ||
      lsx_bit_write(writer, 8, 0xff) != SOX_SUCCESS ||
      lsx_bit_write(writer, 1, 0) != SOX_SUCCESS ||
      lsx_bit_write(writer, 1, 0) != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

/* Wrap one encoded AAC frame in LOAS/LATM and write it out.
 *
 * The packet is the encoder's raw access unit; this builds the frame in
 * p->packet and emits header plus body.  The config is written into every
 * LATM_CONFIG_INTERVAL-th frame, the rest setting useSameStreamMux instead.
 * The payload length that follows is coded as a chain of 255s ending in a
 * shorter byte.
 *
 * Everything after the first bit is written at an arbitrary bit offset, so
 * the frame is zeroed first -- lsx_bit_write only ever sets bits -- and the
 * trailing partial byte ends up padded with zeros. */
static int write_latm_packet(sox_format_t * ft, AVCodecContext const * context, AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  lsx_bit_writer_t writer;
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
      lsx_aac_object_type(context->extradata,
          (size_t)context->extradata_size,
          &object_type) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "AAC encoder did not provide a valid AudioSpecificConfig");
    return SOX_EOF;
  }
  /* Encoding is AAC-LC only, even though all three types can be read: the
   * SBR and PS tools change how the config and the access units relate, and
   * FFmpeg's native encoder does not produce them anyway. */
  if (object_type != 2) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC LATM encoding supports AAC-LC only");
    return SOX_EOF;
  }
  if (packet->size < 0 || (size_t)packet->size > LSX_LOAS_MAX_FRAME_SIZE) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC frame is too large for the 13-bit LOAS frame length");
    return SOX_EOF;
  }

  memset(p->packet, 0, sizeof(p->packet));
  writer.data = p->packet + LSX_LOAS_HEADER_SIZE;
  writer.size_bits = LSX_LOAS_MAX_FRAME_SIZE * 8;
  writer.position = 0;
  write_config = p->config_counter == 0;
  if (lsx_bit_write(&writer, 1, !write_config) != SOX_SUCCESS ||
      (write_config &&
       write_stream_mux_config(&writer, context) != SOX_SUCCESS)) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC StreamMuxConfig is too large for LOAS/LATM");
    return SOX_EOF;
  }

  remaining = (size_t)packet->size;
  while (remaining >= 255) {
    if (lsx_bit_write(&writer, 8, 255) != SOX_SUCCESS)
      goto too_large;
    remaining -= 255;
  }
  if (lsx_bit_write(&writer, 8, (uint32_t)remaining) != SOX_SUCCESS)
    goto too_large;

  /* FFmpeg's AAC encoder opens each access unit with a data stream element
   * whose data_byte_align_flag is set, which is right for ADTS, where the
   * payload starts on a byte boundary.  In LATM it does not: the config and
   * the length bytes leave the payload at an arbitrary bit offset, so a
   * decoder honouring that flag would align to the wrong place and read the
   * rest of the frame as rubbish.  The flag is the low bit of the first
   * byte, and clearing it turns the element into a plain DSE, which is why
   * the first byte is copied separately from the rest.
   *
   * The test matches that element and no other: syn_ele 100 is a DSE, and
   * the low bit must be the set alignment flag. */
  if (packet->size && (packet->data[0] & 0xe1) == 0x81) {
    uint8_t first = packet->data[0] & 0xfe;

    if (copy_bits(&writer, &first, 8) != SOX_SUCCESS ||
        copy_bits(&writer, packet->data + 1,
            ((size_t)packet->size - 1) * 8) != SOX_SUCCESS)
      goto too_large;
  }
  else if (copy_bits(&writer, packet->data, (size_t)packet->size * 8) != SOX_SUCCESS)
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
  p->config_counter = (p->config_counter + 1) % LATM_CONFIG_INTERVAL;
  return SOX_SUCCESS;

too_large:
  lsx_fail_errno(ft, SOX_EFMT, "AAC packet is too large for the 13-bit LOAS frame length");
  return SOX_EOF;
}

/* Reading and writing use different libavcodec codecs: AAC_LATM is a
 * decoder that takes whole LOAS frames, whereas encoding goes through the
 * plain AAC encoder with this file adding the framing. */
static lsx_ffmpeg_codec_definition_t const latm_definition = {
  AV_CODEC_ID_AAC_LATM, SOX_ENCODING_AAC, "AAC LATM",
  /* A PCE-configured stream is decoded in the order the file states, which
   * FFmpeg may report without naming speakers; the back pair is taken as for
   * the plain AAC definition. */
  8, sox_true, sox_true, 0, 24,
  0, 0, 0,
  AV_PROFILE_UNKNOWN, NULL, AV_PROFILE_UNKNOWN, 0,
  NULL, NULL, NULL, NULL, sox_false,
  sox_false, 0, 0, 0, NULL
};

static lsx_ffmpeg_stream_definition_t const read_definition = {
  &latm_definition, NULL, read_latm_packet, NULL
};

static lsx_ffmpeg_stream_definition_t const write_definition = {
  &lsx_ffmpeg_aac_definition, NULL, NULL, write_latm_packet
};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startread(ft, &p->codec, &read_definition);
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

  return lsx_ffmpeg_codec_startwrite(ft, &p->codec, &write_definition);
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
