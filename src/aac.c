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

/* The ADTS frame length field is 13 bits wide, header included. */
#define ADTS_MAX_FRAME_SIZE 0x1fff

/* Room for the largest programme config element worth carrying.  A PCE is
 * variable length -- it grows with the element count and its trailing comment
 * -- but the layouts this handler writes need well under this, and the bound
 * keeps it in the private state rather than on the heap. */
#define AAC_MAX_PCE_SIZE 40

typedef struct {
  lsx_ffmpeg_codec_t * codec;

  /* Write side.  ADTS repeats a summary of the configuration in every frame
   * header, so the fields are taken apart once from the encoder's
   * AudioSpecificConfig, on the first packet, and reused from then on. */
  sox_bool adts_configured;
  uint32_t object_type;
  uint32_t rate_index;
  uint32_t channel_configuration;

  /* A layout that channel_configuration cannot express is described instead
   * by a PCE, which ADTS has no field for and which therefore travels in
   * front of the audio payload.  It is emitted once, in the first frame, so
   * a decoder joining later reads the channel_configuration 0 header and
   * looks for the PCE where the standard puts it. */
  uint8_t pce[AAC_MAX_PCE_SIZE];
  size_t pce_size;
  sox_bool pce_pending;
} priv_t;

/* Read exactly size bytes, looping because lsx_readbuf may return fewer.
 * Returns 1 on success, 0 when nothing was left and the caller allows the
 * stream to end there, SOX_EOF otherwise. */
static int read_exact(sox_format_t * ft, uint8_t * data, size_t size, sox_bool clean_eof)
{
  size_t done = 0;

  while (done < size) {
    size_t count = lsx_readbuf(ft, data + done, size - done);

    if (count == 0) {
      if (done == 0 && clean_eof)
        return 0;
      lsx_fail_errno(ft, SOX_EHDR, "Truncated AAC ADTS frame or metadata tag");
      return SOX_EOF;
    }
    done += count;
  }
  return 1;
}

/* Skip size bytes by reading and dropping them.  Seeking is not an option:
 * these streams are commonly read from a pipe. */
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

/* Supply the next ADTS frame, header included, as an AVPacket, skipping the
 * metadata tags these files are routinely wrapped in.  Returns 1 on success,
 * 0 at end of stream, SOX_EOF on error.
 *
 * The framing is done here rather than by FFmpeg's ADTS parser so that those
 * tags can be recognised: the parser would resynchronise past them, and a tag
 * body can hold anything, including bytes that look like a frame header.  The
 * loop is what lets any number of tags precede or separate frames.
 *
 * Nothing here resynchronises.  Once past the tags, a byte that is not a
 * frame header is a corrupt file, and saying so beats hunting for the next
 * plausible sync word. */
static int read_adts_packet(sox_format_t * ft, AVPacket * packet)
{
  /* Big enough for either an ADTS header or an ID3v2 one. */
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
      /* An ID3v2 size is four synchsafe bytes: seven bits each, so that no
       * byte of it can be mistaken for a sync word.  The checks above are
       * what that costs -- version in range, and the high bit clear in each
       * size byte -- and they also stop a stray "ID3" in the audio from
       * swallowing the rest of the file. */
      payload_size = ((size_t)header[6] << 21) + ((size_t)header[7] << 14) + ((size_t)header[8] << 7) + header[9];
      /* A v2.4 tag may be followed by a 10-byte footer, which the size does
       * not include. */
      if (header[3] == 4 && (header[5] & 0x10))
        payload_size += 10;
      if (discard_bytes(ft, payload_size) != SOX_SUCCESS)
        return SOX_EOF;
      continue;
    }
    /* An ID3v1 tag is a fixed 128 bytes, three of them already read. */
    if (!memcmp(header, "TAG", 3)) {
      if (discard_bytes(ft, 125) != SOX_SUCCESS)
        return SOX_EOF;
      continue;
    }

    result = read_exact(ft, header + 3, AV_AAC_ADTS_HEADER_SIZE - 3, sox_false);
    if (result != 1)
      return result;
    /* The 13-bit frame length spans three bytes and covers the header too.
     * The low bit of byte 1 is the "no CRC" flag: when it is clear, two more
     * bytes of checksum follow the header and are part of the frame, which
     * is the smallest a frame can then be. */
    frame_size = ((header[3] & 3) << 11) | (header[4] << 3) | (header[5] >> 5);
    header_size = (header[1] & 1) ? AV_AAC_ADTS_HEADER_SIZE : AV_AAC_ADTS_HEADER_SIZE + 2;
    /* Sync word, then the layer bits which must be zero; av_adts_header_parse
     * has the last word on whether the rest of the header is coherent. */
    if (header[0] != 0xff || (header[1] & 0xf6) != 0xf0 ||
        frame_size < header_size ||
        av_adts_header_parse(header, &samples, &frames) < 0) {
      lsx_fail_errno(ft, SOX_EHDR, "Invalid AAC ADTS frame header");
      return SOX_EOF;
    }

    result = av_new_packet(packet, (int)frame_size);
    if (result < 0) {
      lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate AAC compressed audio packet");
      return SOX_EOF;
    }
    memcpy(packet->data, header, AV_AAC_ADTS_HEADER_SIZE);
    result = read_exact(ft, packet->data + AV_AAC_ADTS_HEADER_SIZE, frame_size - AV_AAC_ADTS_HEADER_SIZE, sox_false);
    if (result != 1) {
      av_packet_unref(packet);
      return result;
    }
    return 1;
  }
}

/* ADTS names the sample rate by its index in this fixed table rather than
 * carrying the rate itself.  Returns -1 for a rate the table has no slot
 * for, which is a rate ADTS simply cannot express. */
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

/* Move count bits from the AudioSpecificConfig to the Program Config Element
 * being assembled, reporting them as well when the caller needs the value to
 * decide what comes next. */
static int copy_bits(lsx_bit_reader_t * reader, lsx_bit_writer_t * writer, unsigned count, uint32_t * value)
{
  uint32_t copied;

  if (lsx_bit_read(reader, count, &copied) != SOX_SUCCESS || lsx_bit_write(writer, count, copied) != SOX_SUCCESS)
    return SOX_EOF;
  if (value != NULL)
    *value = copied;
  return SOX_SUCCESS;
}

/* Advance to the next byte boundary.  A PCE has one alignment point in it,
 * before the comment length, and the two cursors have to cross it together
 * even though they sit at different offsets: the reader is somewhere inside
 * the config, the writer three bits into a fresh element. */
static int align_reader(lsx_bit_reader_t * reader)
{
  size_t aligned = (reader->position + 7) & ~(size_t)7;

  if (aligned > reader->size_bits)
    return SOX_EOF;
  reader->position = aligned;
  return SOX_SUCCESS;
}

/* Pad to the next byte boundary with zeros.  Written out bit by bit rather
 * than by moving the cursor, so that the padding is bounds-checked like any
 * other field. */
static int align_writer(lsx_bit_writer_t * writer)
{
  while (writer->position & 7)
    if (lsx_bit_write(writer, 1, 0) != SOX_SUCCESS)
      return SOX_EOF;
  return SOX_SUCCESS;
}

/* Copy the programme config element out of an AudioSpecificConfig and into a
 * standalone syntactic element that can be written in front of an ADTS
 * payload.  reader must be positioned at the start of the PCE; on success it
 * is left just past it, destination holds the element and *destination_size
 * its length in bytes.
 *
 * The PCE is not simply memcpy'd, for two reasons.  It is variable length, so
 * where it ends can only be learned by parsing it: three counts of elements
 * whose descriptors are 5 bits and two whose descriptors are 4, then three
 * optional mixdown fields, then a comment whose length is stated inline.  And
 * it is relocated: inside the config it follows a byte-aligned 16-bit header,
 * whereas in a raw data block it follows the 3-bit element identifier written
 * below.  Its internal byte_alignment() therefore falls at a different bit
 * offset in each, which is why the two cursors are aligned separately rather
 * than the padding being copied along with everything else.
 *
 * Fields whose values do not matter here are moved through without being
 * interpreted; only the counts and flags that decide what follows are read
 * back.  Anything that overruns either buffer fails, and the caller reports
 * it as an invalid PCE. */
static int extract_pce(lsx_bit_reader_t * reader, uint8_t * destination, size_t * destination_size)
{
  lsx_bit_writer_t writer = {
    destination,
    AAC_MAX_PCE_SIZE * 8,
    0
  };
  /* Running totals of the descriptors that follow the counts, grouped by
   * width: front, side, back and coupling channels are 5 bits each, LFE and
   * associated data 4. */
  uint32_t five_bit_elements;
  uint32_t four_bit_elements;
  uint32_t count;
  uint32_t flag;
  uint32_t comment_size;

  /* Zeroed because lsx_bit_write only sets bits, so the alignment padding and
   * any unwritten tail have to start out clear. */
  memset(destination, 0, AAC_MAX_PCE_SIZE);
  /* Element identifier 5, ID_PCE, then the instance tag, object type and
   * sample rate index that a raw data block expects to find in front of it. */
  if (lsx_bit_write(&writer, 3, 5) != SOX_SUCCESS ||
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

  /* Mono, stereo and matrix mixdown: a present flag, and its index only when
   * the flag is set. */
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

  /* The descriptors themselves, which are just moved across; chunked only
   * because a single copy_bits carries at most 32 bits. */
  count = five_bit_elements * 5 + four_bit_elements * 4;
  while (count) {
    unsigned chunk = (unsigned)min(count, 16U);

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

  /* The element ends byte-aligned, the comment having started so, hence the
   * exact division. */
  *destination_size = writer.position / 8;
  return *destination_size ? SOX_SUCCESS : SOX_EOF;
}

/* Take the encoder's AudioSpecificConfig apart into the fields every ADTS
 * header repeats, and extract the PCE if there is one.  Run once, from the
 * first packet, because the config is only final after the encoder is open.
 *
 * ADTS carries strictly less than the config does, so anything the header has
 * no room for is refused rather than written as something else: only object
 * types 1 to 4 have a two-bit spelling, the rate must be one the index table
 * names and must agree with what the header will claim, and the 960-sample
 * frame length, core coder dependency and extension flags have nowhere to go
 * at all. */
static int configure_adts(sox_format_t * ft, AVCodecContext const * context, priv_t * p)
{
  lsx_bit_reader_t reader;
  uint32_t frame_length_flag;
  uint32_t depends_on_core_coder;
  uint32_t extension_flag;
  int expected_rate_index = sampling_frequency_index(context->sample_rate);

  if (context->extradata == NULL || context->extradata_size < 2) {
    lsx_fail_errno(ft, SOX_EHDR, "AAC encoder did not provide an AudioSpecificConfig");
    return SOX_EOF;
  }
  reader.data = context->extradata;
  reader.size_bits = (size_t)context->extradata_size * 8;
  reader.position = 0;
  if (lsx_bit_read(&reader, 5, &p->object_type) != SOX_SUCCESS ||
      lsx_bit_read(&reader, 4, &p->rate_index) != SOX_SUCCESS ||
      lsx_bit_read(&reader, 4, &p->channel_configuration) != SOX_SUCCESS ||
      lsx_bit_read(&reader, 1, &frame_length_flag) != SOX_SUCCESS ||
      lsx_bit_read(&reader, 1, &depends_on_core_coder) != SOX_SUCCESS ||
      lsx_bit_read(&reader, 1, &extension_flag) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid AAC AudioSpecificConfig from encoder");
    return SOX_EOF;
  }
  if (p->object_type < 1 || p->object_type > 4 ||
      expected_rate_index < 0 || p->rate_index > 12 ||
      p->rate_index != (uint32_t)expected_rate_index ||
      p->channel_configuration > 7 ||
      frame_length_flag || depends_on_core_coder || extension_flag) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC encoder configuration cannot be represented in ADTS");
    return SOX_EOF;
  }
  /* channel_configuration 0 means the layout is described by a PCE that
   * follows in the config. */
  if (!p->channel_configuration) {
    if (extract_pce(&reader, p->pce, &p->pce_size) != SOX_SUCCESS) {
      lsx_fail_errno(ft, SOX_EHDR, "Invalid Program Config Element from AAC encoder");
      return SOX_EOF;
    }
    p->pce_pending = sox_true;
  }
  /* prepare_aac_encoder asked for a PCE for exactly these channel counts, so
   * a numbered configuration here means the encoder ignored that and the
   * file would state a layout other than the one being written. */
  else if (context->ch_layout.nb_channels == 4 || context->ch_layout.nb_channels >= 7) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC encoder did not provide the required Program Config Element");
    return SOX_EOF;
  }
  p->adts_configured = sox_true;
  return SOX_SUCCESS;
}

/* Force a programme config element for the layouts the numbered channel
 * configurations cannot describe, so the file states its own channel order
 * instead of leaning on a default that means something else. */
static int prepare_aac_encoder(AVCodecContext * context)
{
  /* Quad and 6.1 require a PCE.  Force one for 7.1 as well because ADTS
   * channel_configuration 7 describes the different 7.1(wide) layout. */
  if (context->ch_layout.nb_channels == 4 || context->ch_layout.nb_channels >= 7)
    return av_opt_set_int(context->priv_data, "aac_pce", 1, 0);
  return 0;
}

/* Write one encoded frame with an ADTS header in front, and the PCE too if
 * this is the first frame.  The header is assembled by hand rather than
 * through FFmpeg's ADTS bitstream filter, because that filter has no way to
 * insert the PCE, which has to sit between the header and the payload. */
static int write_adts_packet(sox_format_t * ft, AVCodecContext const * context, AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint8_t header[AV_AAC_ADTS_HEADER_SIZE];
  size_t frame_size;

  if (packet->size == 0)
    return SOX_SUCCESS;
  if (!p->adts_configured && configure_adts(ft, context, p) != SOX_SUCCESS)
    return SOX_EOF;
  frame_size = sizeof(header) + (size_t)packet->size + (p->pce_pending ? p->pce_size : 0);
  if (frame_size > ADTS_MAX_FRAME_SIZE) {
    lsx_fail_errno(ft, SOX_EFMT, "AAC frame is too large for the 13-bit ADTS frame length");
    return SOX_EOF;
  }

  /* Sync word, then MPEG-4, layer 0 and no CRC in 0xf1.  The object type is
   * stored biased by one, and the sample rate index, private bit and channel
   * configuration straddle bytes 2 and 3, as does the 13-bit frame length
   * across bytes 3 to 5.  The trailing 0x1f and 0xfc set the buffer
   * fullness to its "variable rate" value of all ones and declare one raw
   * data block in this frame. */
  header[0] = 0xff;
  header[1] = 0xf1;
  header[2] = (uint8_t)((((p->object_type - 1) & 3) << 6) | (p->rate_index << 2) | (p->channel_configuration >> 2));
  header[3] = (uint8_t)(((p->channel_configuration & 3) << 6) | (frame_size >> 11));
  header[4] = (uint8_t)(frame_size >> 3);
  header[5] = (uint8_t)(((frame_size & 7) << 5) | 0x1f);
  header[6] = 0xfc;

  if (lsx_writebuf(ft, header, sizeof(header)) != sizeof(header))
    return SOX_EOF;
  if (p->pce_pending && lsx_writebuf(ft, p->pce, p->pce_size) != p->pce_size)
    return SOX_EOF;
  if (lsx_writebuf(ft, packet->data, (size_t)packet->size) != (size_t)packet->size)
    return SOX_EOF;
  p->pce_pending = sox_false;
  return SOX_SUCCESS;
}

/* One definition serves both directions: the same libavcodec AAC codec reads
 * and writes, with this file supplying the ADTS framing on each side. */
static lsx_ffmpeg_codec_definition_t const definition = {
  AV_CODEC_ID_AAC,
  SOX_ENCODING_AAC,
  "AAC",
  8,                            /* max_decode_channels */
  /* FFmpeg preserves canonical PCE sample order but reports 6.1 and 7.1
   * frames with AV_CHANNEL_ORDER_UNSPEC. */
  sox_true,                     /* accept_unspecified_decode_layout */
  8,                            /* max_encode_channels */
  24,                           /* precision */
  128000,                       /* default_bit_rate */
  /* No fixed range: what is usable depends on the rate and channel count, so
   * the encoder is left to reject what it cannot do. */
  0,                            /* minimum_bit_rate */
  0,                            /* maximum_bit_rate */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  NULL,                         /* prepare_decoder */
  prepare_aac_encoder,          /* prepare_encoder */
  read_adts_packet,             /* packet_reader */
  write_adts_packet,            /* packet_writer */
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
