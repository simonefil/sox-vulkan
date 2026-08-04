/* xHE-AAC/USAC elementary stream with LOAS/LATM framing.
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

#include "sox_i.h"
#include "ffmpeg-codec.h"
#include "latm-common.h"
#ifdef HAVE_FFMPEG_FORMATS
#include "ffmpeg-container.h"
#endif

#ifdef HAVE_FFMPEG_CODECS

#include <libavutil/mem.h>

#include <stdint.h>
#include <string.h>

/* Upper bound on an AudioSpecificConfig, in bytes.  A USAC config is a few
 * dozen bytes in practice; this is a sanity limit that keeps the buffers
 * below fixed-size and rejects an absurd length field before it is used. */
#define USAC_MAX_CONFIG_SIZE 1024

/* Keep the other FFmpeg-backed formats buildable with libavcodec 61.
 * The xHE-AAC handler rejects that version before opening the decoder. */
#ifndef AV_PROFILE_AAC_USAC
#define AV_PROFILE_AAC_USAC 41
#endif

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  uint8_t loas_frame[LSX_LOAS_MAX_PACKET_SIZE]; /* One LOAS frame as read from the file. */

  /* The first access unit, pulled early so that prepare_usac_decoder can see
   * the config that precedes it.  read_usac_packet hands it out before
   * reading any more, which is what keeps it from being decoded twice. */
  uint8_t pending_packet[LSX_LOAS_MAX_FRAME_SIZE];
  size_t pending_size;

  /* The current AudioSpecificConfig, kept because LATM sends it in band and
   * may repeat or change it: it becomes the decoder's extradata at open time
   * and is re-attached to a packet whenever it changes mid-stream.
   * config_bits is the exact length and config_size its rounding up to whole
   * bytes; both are needed, since a config need not end on a byte boundary
   * and a change in the trailing bits alone still counts as a change. */
  uint8_t config[USAC_MAX_CONFIG_SIZE];
  size_t config_size;
  size_t config_bits;
  sox_bool configured;
} priv_t;

/* Copy count bits out of reader into destination, left-aligned from bit 0 and
 * zero-padded to the next byte.  A plain memcpy will not do: the config sits
 * at an arbitrary bit offset inside the LATM element, but FFmpeg wants it
 * byte-aligned.  Leaves the cursor unmoved and destination zeroed if the
 * field does not fit. */
static int copy_bits(lsx_bit_reader_t * reader, uint8_t * destination, size_t count)
{
  size_t i;

  memset(destination, 0, (count + 7) / 8);
  if (reader->position > reader->size_bits || count > reader->size_bits - reader->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t source_position = reader->position++;

    if ((reader->data[source_position / 8] >> (7 - source_position % 8)) & 1)
      destination[i / 8] |= (uint8_t)(1U << (7 - i % 8));
  }
  return SOX_SUCCESS;
}

/* Read the audioObjectType from the front of an AudioSpecificConfig.  It is
 * 5 bits, with the escape value 31 meaning "6 more bits, biased by 32", which
 * is how types above 30 -- USAC among them, at 42 -- are spelled. */
static int audio_object_type(uint8_t const * config, size_t config_bits, uint32_t * object_type)
{
  lsx_bit_reader_t reader = {config, config_bits, 0};
  uint32_t value;

  if (lsx_bit_read(&reader, 5, &value) != SOX_SUCCESS)
    return SOX_EOF;
  if (value == 31) {
    uint32_t extension;

    if (lsx_bit_read(&reader, 6, &extension) != SOX_SUCCESS)
      return SOX_EOF;
    value = 32 + extension;
  }
  *object_type = value;
  return SOX_SUCCESS;
}

/* Parse a StreamMuxConfig and latch the AudioSpecificConfig it carries, with
 * the cursor left just past the config so the caller can go on to the payload
 * lengths.  *config_changed says whether this replaced a config already in
 * force, which is what makes the difference between the decoder's initial
 * extradata and a mid-stream reconfiguration.  The first config seen is never
 * a change: it is what the decoder is opened with.
 *
 * Only the one LATM shape xHE-AAC is defined for is accepted -- audioMux
 * version 1, a single synchronous program and layer, no otherData -- so the
 * fields below are read and required to be their fixed values rather than
 * being interpreted.  Everything else is refused with a clear message
 * instead of being misparsed. */
static int parse_stream_mux_config(sox_format_t * ft, priv_t * p, lsx_bit_reader_t * reader, sox_bool * config_changed)
{
  uint8_t config[USAC_MAX_CONFIG_SIZE];
  uint32_t value;
  uint32_t asc_bits;
  uint32_t object_type;
  size_t config_size;
  sox_bool was_configured = p->configured;

  /* audioMuxVersion, audioMuxVersionA, taraBufferFullness, allStreamsSameTimeFraming,
   * numSubFrames, numProgram, numLayer. */
  if (lsx_bit_read(reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      lsx_bit_read(reader, 1, &value) != SOX_SUCCESS || value != 0 ||
      lsx_latm_read_value(reader, &value) != SOX_SUCCESS ||
      lsx_bit_read(reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      lsx_bit_read(reader, 6, &value) != SOX_SUCCESS || value != 0 ||
      lsx_bit_read(reader, 4, &value) != SOX_SUCCESS || value != 0 ||
      lsx_bit_read(reader, 3, &value) != SOX_SUCCESS || value != 0) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Unsupported xHE-AAC LATM configuration; expected one "
        "synchronous program and layer with audioMuxVersion 1");
    return SOX_EOF;
  }
  if (lsx_latm_read_value(reader, &asc_bits) != SOX_SUCCESS || asc_bits == 0 || asc_bits > USAC_MAX_CONFIG_SIZE * 8U) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid xHE-AAC AudioSpecificConfig length");
    return SOX_EOF;
  }

  config_size = (asc_bits + 7) / 8;
  if (copy_bits(reader, config, asc_bits) != SOX_SUCCESS ||
      audio_object_type(config, asc_bits, &object_type) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated xHE-AAC AudioSpecificConfig");
    return SOX_EOF;
  }
  /* Object type 42 is USAC.  Plain AAC in the same framing is handled by the
   * latm format, so anything else here is a file for that handler, not a
   * malformed one for this. */
  if (object_type != 42) {
    lsx_fail_errno(ft, SOX_EFMT, "LOAS/LATM AudioSpecificConfig is not xHE-AAC/USAC");
    return SOX_EOF;
  }

  /* frameLengthType 0 and its latmBufferFullness, then otherDataPresent.
   * Only type 0 puts explicit payload lengths in each frame, which is what
   * parse_audio_mux_element goes on to read. */
  if (lsx_bit_read(reader, 3, &value) != SOX_SUCCESS || value != 0 || lsx_bit_read(reader, 8, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Unsupported xHE-AAC LATM frame length configuration");
    return SOX_EOF;
  }
  if (lsx_bit_read(reader, 1, &value) != SOX_SUCCESS || value != 0) {
    lsx_fail_errno(ft, SOX_EHDR, "xHE-AAC LATM otherData is unsupported");
    return SOX_EOF;
  }
  if (lsx_bit_read(reader, 1, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated xHE-AAC StreamMuxConfig");
    return SOX_EOF;
  }
  /* crcCheckPresent; the checksum itself is skipped rather than verified,
   * since the decoder will reject a corrupt access unit anyway. */
  if (value && lsx_bit_read(reader, 8, NULL) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated xHE-AAC StreamMuxConfig CRC");
    return SOX_EOF;
  }

  *config_changed = was_configured &&
      (p->config_bits != asc_bits || p->config_size != config_size || memcmp(p->config, config, config_size));
  memcpy(p->config, config, config_size);
  p->config_size = config_size;
  p->config_bits = asc_bits;
  p->configured = sox_true;
  return SOX_SUCCESS;
}

/* Unpack one AudioMuxElement, already read into p->loas_frame, into the raw
 * access unit the decoder wants.  destination must have room for
 * LSX_LOAS_MAX_FRAME_SIZE bytes; *destination_size is what was written.
 *
 * The leading useSameStreamMux bit says whether this frame repeats the
 * previous configuration or carries a new one, so a stream that never sends
 * one before its first payload cannot be decoded at all.  The payload length
 * that follows is coded as a chain of bytes, each below 255 ending it. */
static int parse_audio_mux_element(
    sox_format_t * ft,
    priv_t * p,
    uint8_t * destination,
    size_t * destination_size,
    sox_bool * config_changed,
    size_t frame_size)
{
  lsx_bit_reader_t reader = {
    p->loas_frame + LSX_LOAS_HEADER_SIZE,
    (frame_size - LSX_LOAS_HEADER_SIZE) * 8,
    0
  };
  uint32_t value;
  size_t payload_size = 0;

  *config_changed = sox_false;
  if (lsx_bit_read(&reader, 1, &value) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated xHE-AAC AudioMuxElement");
    return SOX_EOF;
  }
  if (!value) {
    if (parse_stream_mux_config(ft, p, &reader, config_changed) != SOX_SUCCESS)
      return SOX_EOF;
  }
  else if (!p->configured) {
    lsx_fail_errno(ft, SOX_EHDR, "xHE-AAC LATM stream starts without a StreamMuxConfig");
    return SOX_EOF;
  }

  /* The subtraction rather than an addition keeps the bound check itself
   * from overflowing, so an absurd chain of 255s fails instead of wrapping. */
  do {
    if (lsx_bit_read(&reader, 8, &value) != SOX_SUCCESS || payload_size > LSX_LOAS_MAX_FRAME_SIZE - value) {
      lsx_fail_errno(ft, SOX_EHDR, "Invalid xHE-AAC LATM payload length");
      return SOX_EOF;
    }
    payload_size += value;
  } while (value == 255);

  if (payload_size == 0 || copy_bits(&reader, destination, payload_size * 8) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated or empty xHE-AAC access unit");
    return SOX_EOF;
  }
  *destination_size = payload_size;
  return SOX_SUCCESS;
}

/* Read the next LOAS frame and unpack its access unit.  Returns 1 on
 * success, 0 at end of stream, or SOX_EOF on error.  clean_eof is passed
 * through to the LOAS reader: it says whether running out of input exactly at
 * a frame boundary is normal here, which it is for every caller in this file. */
static int read_loas_access_unit(
    sox_format_t * ft,
    priv_t * p,
    uint8_t * destination,
    size_t * destination_size,
    sox_bool * config_changed,
    sox_bool clean_eof)
{
  size_t frame_size;
  int result = lsx_loas_read_packet(ft, p->loas_frame, sizeof(p->loas_frame), &frame_size, clean_eof, "xHE-AAC");

  if (result != 1)
    return result;
  return parse_audio_mux_element(ft, p, destination,
      destination_size, config_changed, frame_size) == SOX_SUCCESS ? 1 :
      SOX_EOF;
}

/* Give the decoder the AudioSpecificConfig before it is opened.  Nothing in a
 * raw USAC access unit says how to decode it, so the config has to come from
 * the LATM layer, which means reading the first frame here; it is kept in
 * p->pending_packet and handed out by the first read_usac_packet.
 *
 * The extradata buffer becomes the codec context's property and is freed with
 * it, so it must come from FFmpeg's allocator and be padded as libavcodec
 * requires.  The version test is at run time, not compile time, because the
 * USAC decoder appeared in FFmpeg 8.0 while everything else here builds
 * against 61; failing with a clear message beats decoding garbage. */
static int prepare_usac_decoder(sox_format_t * ft, AVCodecContext * context)
{
  priv_t * p = (priv_t *)ft->priv;
  sox_bool config_changed;
  int result;

  if ((avcodec_version() >> 16) < 62) {
    lsx_fail_errno(ft, SOX_EFMT, "xHE-AAC decoding requires FFmpeg 8.0 or later");
    return SOX_EOF;
  }
  result = read_loas_access_unit(ft, p, p->pending_packet, &p->pending_size, &config_changed, sox_true);
  if (result != 1) {
    if (result == 0)
      lsx_fail_errno(ft, SOX_EHDR, "xHE-AAC stream contains no audio");
    return SOX_EOF;
  }

  context->extradata = av_mallocz(p->config_size + AV_INPUT_BUFFER_PADDING_SIZE);
  if (context->extradata == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate xHE-AAC decoder configuration");
    return SOX_EOF;
  }
  memcpy(context->extradata, p->config, p->config_size);
  context->extradata_size = (int)p->config_size;
  lsx_warn("`%s': xHE-AAC loudness and dynamic-range metadata, if " "present, will be ignored", ft->filename);
  return SOX_SUCCESS;
}

/* Supply the next access unit as an AVPacket, taking the framing off LOAS
 * rather than letting FFmpeg's parser do it.  The first call hands back the
 * frame prepare_usac_decoder already consumed.
 *
 * A config that changed mid-stream travels with the packet as NEW_EXTRADATA
 * side data, which is how libavcodec is told to reconfigure at exactly that
 * packet; putting it on the context instead would apply it at the wrong
 * point.  Ownership of the packet passes to the caller. */
static int read_usac_packet(sox_format_t * ft, AVPacket * packet)
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
    result = read_loas_access_unit(ft, p, access_unit, &access_unit_size, &config_changed, sox_true);
    if (result != 1)
      return result;
  }

  result = av_new_packet(packet, (int)access_unit_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate xHE-AAC compressed audio packet");
    return SOX_EOF;
  }
  memcpy(packet->data, access_unit, access_unit_size);
  if (!config_changed)
    return 1;

  new_config = av_packet_new_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, p->config_size);
  if (new_config == NULL) {
    av_packet_unref(packet);
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to attach updated xHE-AAC decoder configuration");
    return SOX_EOF;
  }
  memcpy(new_config, p->config, p->config_size);
  return 1;
}

/* Decode only: FFmpeg has no USAC encoder, so max_encode_channels is 0 and
 * the handler exposes no write path at all.  The codec is shared with plain
 * AAC, hence required_decode_profile, which is what stops an AAC-LC file from
 * being decoded as though it were xHE-AAC.  Unspecified layouts are accepted
 * up to 8 channels because USAC states its own channel order in the config,
 * so decoder order is the right order even when FFmpeg names no speakers. */
static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_USAC,
  "xHE-AAC",
  8,                            /* max_decode_channels */
  sox_true,                     /* accept_unspecified_decode_layout */
  0,                            /* max_encode_channels */
  24,                           /* precision */
  0,                            /* default_bit_rate */
  0,                            /* minimum_bit_rate */
  0,                            /* maximum_bit_rate */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_AAC_USAC,          /* required_decode_profile */
  prepare_usac_decoder,         /* prepare_decoder */
  NULL,                         /* prepare_encoder */
  read_usac_packet,             /* packet_reader */
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

#ifdef HAVE_FFMPEG_FORMATS

/* MP4 stores the USAC AudioSpecificConfig and access units in the container,
 * so this path is the container counterpart of the LOAS/LATM handler above.
 * It deliberately has its own format name: `m4a' remains ALAC, preserving the
 * existing read/write contract and making the codec choice explicit. */
typedef struct {
  lsx_ffmpeg_codec_t * codec;
  lsx_ffmpeg_container_t * container;
} usac_m4a_priv_t;

static int prepare_usac_m4a_decoder(sox_format_t * ft, AVCodecContext * context)
{
  usac_m4a_priv_t * p = (usac_m4a_priv_t *)ft->priv;

  if ((avcodec_version() >> 16) < 62) {
    lsx_fail_errno(ft, SOX_EFMT, "xHE-AAC decoding requires FFmpeg 8.0 or later");
    return SOX_EOF;
  }
  return lsx_ffmpeg_container_startread(
      ft, &p->container, "mov", AV_CODEC_ID_AAC, context);
}

static int read_usac_m4a_packet(sox_format_t * ft, AVPacket * packet)
{
  usac_m4a_priv_t * p = (usac_m4a_priv_t *)ft->priv;

  return lsx_ffmpeg_container_read_packet(ft, p->container, packet);
}

static lsx_ffmpeg_codec_definition_t const usac_m4a_definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_USAC,
  "xHE-AAC",
  8,                            /* max_decode_channels */
  sox_true,                     /* accept_unspecified_decode_layout */
  0,                            /* max_encode_channels */
  24,                           /* precision */
  0, 0, 0,                     /* bit-rate fields: decode only */
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_AAC_USAC,
  prepare_usac_m4a_decoder,
  NULL,
  read_usac_m4a_packet,
  NULL,
  sox_false,
  0, 0, 0,
  NULL
};

static int startread_usac_m4a(sox_format_t * ft)
{
  usac_m4a_priv_t * p = (usac_m4a_priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_startread(ft, &p->codec, &usac_m4a_definition);

  if (result != SOX_SUCCESS)
    lsx_ffmpeg_container_stopread(&p->container);
  else
    lsx_warn("`%s': xHE-AAC loudness and dynamic-range metadata, if "
        "present, will be ignored", ft->filename);
  return result;
}

static size_t read_samples_usac_m4a(
    sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  usac_m4a_priv_t * p = (usac_m4a_priv_t *)ft->priv;

  return lsx_ffmpeg_codec_read(ft, p->codec, samples, length);
}

static int stopread_usac_m4a(sox_format_t * ft)
{
  usac_m4a_priv_t * p = (usac_m4a_priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_stopread(&p->codec);

  lsx_ffmpeg_container_stopread(&p->container);
  return result;
}

LSX_FORMAT_HANDLER(usac_m4a)
{
  static char const * const names[] = {
    "xhe-m4a", "usac-m4a", NULL
  };
  static unsigned const encodings[] = {SOX_ENCODING_USAC, 0, 0};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "xHE-AAC/USAC in an M4A/MP4 container (decode only)",
    names,
    SOX_FILE_CODEC_OPTIONS,
    startread_usac_m4a,
    read_samples_usac_m4a,
    stopread_usac_m4a,
    NULL, NULL, NULL, NULL,
    encodings,
    NULL,
    sizeof(usac_m4a_priv_t)
  };

  return &handler;
}

#endif

#endif
