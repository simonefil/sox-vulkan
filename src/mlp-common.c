/* Shared MLP and Dolby TrueHD elementary stream format implementation.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "ffmpeg-codec.h"
#include "mlp-common.h"

#ifdef HAVE_FFMPEG_CODECS

/* The mlp and truehd handlers share everything but a codec and a stream type
 * byte.  TrueHD is MLP extended, with the same access unit framing and the
 * same major sync header, and the byte at offset 7 of that header is what
 * says which of the two a file is -- 0xbb for MLP, 0xba for TrueHD.
 *
 * Neither format is self-describing enough for FFmpeg's parsers to be used
 * here: the framing is read and written by this file directly.
 */
typedef struct {
  lsx_ffmpeg_codec_t * codec;
  int stream_type;              /* 0xba TrueHD, 0xbb MLP; set before any I/O. */

  /* Write side.  A stream is only decodable standalone if its first access
   * unit carries a major sync, which the encoder emits on its own schedule,
   * so what actually happened is observed rather than assumed. */
  sox_bool starts_with_major_sync;
  unsigned packets_written;
} priv_t;

/* FFmpeg's MLP and TrueHD encoders are marked experimental and refuse to open
 * otherwise. */
static int prepare_encoder(AVCodecContext * context)
{
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  return 0;
}

/* Read up to length bytes, looping because lsx_readbuf may return fewer.
 * Returns how many were read; unlike its namesakes in the ADTS and LOAS
 * handlers it reports a short read rather than judging it, because the
 * callers here distinguish three cases -- nothing, some, all -- and attach
 * different meanings to each. */
static size_t read_exact(sox_format_t * ft, uint8_t * data, size_t length)
{
  size_t done = 0;

  while (done < length) {
    size_t count = lsx_readbuf(ft, data + done, length - done);

    if (count == 0)
      break;
    done += count;
  }
  return done;
}

/* Supply the next access unit as an AVPacket, header included.  Returns 1 on
 * success, 0 at end of stream, SOX_EOF on error.
 *
 * An access unit begins with a 4-byte header whose first 12 bits are its
 * length in 16-bit words, so the framing is read directly rather than through
 * an FFmpeg parser.  Two shapes are recognised and refused by name instead of
 * being misread: an AC-3 sync word, which means this is the interleaved
 * stream a Blu-ray carries rather than a bare elementary one, and a major
 * sync whose stream type belongs to the other of the two formats. */
static int read_packet(sox_format_t * ft, AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint8_t header[4];
  size_t header_size = read_exact(ft, header, sizeof(header));
  size_t frame_size;
  int result;

  /* Nothing at all is a clean end of stream; a partial header is not. */
  if (header_size == 0)
    return 0;
  if (header_size != sizeof(header)) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated %s access unit header", p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  if (header[0] == 0x0b && header[1] == 0x77) {
    lsx_fail_errno(ft, SOX_EFMT,
        "An interleaved AC-3 core is unsupported in a %s elementary "
        "stream; extract the separate TrueHD stream from the Blu-ray "
        "container first",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }

  /* The 12-bit length counts 16-bit words and includes the header itself,
   * so anything shorter than the header cannot be a real access unit. */
  frame_size = ((((size_t)header[0] << 8) | header[1]) & 0xfff) * 2;
  if (frame_size < sizeof(header)) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid %s access unit length", p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  result = av_new_packet(packet, (int)frame_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate compressed audio packet");
    return SOX_EOF;
  }
  memcpy(packet->data, header, sizeof(header));
  if (read_exact(ft, packet->data + sizeof(header), frame_size - sizeof(header)) != frame_size - sizeof(header)) {
    av_packet_unref(packet);
    lsx_fail_errno(ft, SOX_EHDR, "Truncated %s access unit", p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  /* A major sync follows the access unit header: signature f8 72 6f, then
   * the stream type.  Only present on some units, hence the length test. */
  if (frame_size >= 8 && !memcmp(packet->data + 4, "\xf8\x72\x6f", 3) && packet->data[7] != p->stream_type) {
    int input_stream_type = packet->data[7];

    av_packet_unref(packet);
    lsx_fail_errno(ft, SOX_EFMT,
        "The input is %s rather than %s",
        input_stream_type == 0xba ? "Dolby TrueHD" : "MLP",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  return 1;
}

/* Write one access unit unchanged: the encoder already produces the framing,
 * so nothing is added here.  The one thing observed on the way past is
 * whether the very first unit carries a major sync of the right stream type,
 * which is what stopwrite needs in order to tell whether the file it wrote
 * can be decoded on its own. */
static int write_packet(sox_format_t * ft, AVCodecContext const * context, AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;

  (void)context;
  if (p->packets_written == 0 &&
      (size_t)packet->size >= 8 &&
      !memcmp(packet->data + 4, "\xf8\x72\x6f", 3) &&
      packet->data[7] == p->stream_type)
    p->starts_with_major_sync = sox_true;
  ++p->packets_written;
  return lsx_writebuf(ft, packet->data, (size_t)packet->size) == (size_t)packet->size ? SOX_SUCCESS : SOX_EOF;
}

/* Both are lossless, so precision is 0 and the real bit depth is reported,
 * and there is no bit rate or compression level to set.  MLP is a DVD-Audio
 * format and stops at 5.1; TrueHD decodes up to 8 channels but FFmpeg's
 * encoder writes at most 5.1.  An Atmos stream is decoded as its channel bed
 * with the object metadata warned about and dropped. */

static lsx_ffmpeg_codec_definition_t const mlp_definition = {
  AV_CODEC_ID_MLP,
  SOX_ENCODING_MLP,
  "MLP",
  6,                            /* max_decode_channels */
  sox_false,                    /* accept_unspecified_decode_layout */
  6,                            /* max_encode_channels */
  0,                            /* precision */
  0,                            /* default_bit_rate */
  0,                            /* minimum_bit_rate */
  0,                            /* maximum_bit_rate */
  AV_PROFILE_UNKNOWN,           /* ignored_metadata_profile */
  NULL,                         /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  NULL,                         /* prepare_decoder */
  prepare_encoder,              /* prepare_encoder */
  read_packet,                  /* packet_reader */
  write_packet,                 /* packet_writer */
  sox_false,                    /* use_compression_level */
  0,                            /* default_compression_level */
  0,                            /* minimum_compression_level */
  0,                            /* maximum_compression_level */
  NULL                          /* select_layout */
};

static lsx_ffmpeg_codec_definition_t const truehd_definition = {
  AV_CODEC_ID_TRUEHD,
  SOX_ENCODING_TRUEHD,
  "Dolby TrueHD",
  8,                            /* max_decode_channels */
  sox_false,                    /* accept_unspecified_decode_layout */
  6,                            /* max_encode_channels */
  0,                            /* precision */
  0,                            /* default_bit_rate */
  0,                            /* minimum_bit_rate */
  0,                            /* maximum_bit_rate */
  AV_PROFILE_TRUEHD_ATMOS,      /* ignored_metadata_profile */
  "Dolby TrueHD with Dolby Atmos", /* ignored_metadata_name */
  AV_PROFILE_UNKNOWN,           /* required_decode_profile */
  NULL,                         /* prepare_decoder */
  prepare_encoder,              /* prepare_encoder */
  read_packet,                  /* packet_reader */
  write_packet,                 /* packet_writer */
  sox_false,                    /* use_compression_level */
  0,                            /* default_compression_level */
  0,                            /* minimum_compression_level */
  0,                            /* maximum_compression_level */
  NULL                          /* select_layout */
};

/* The stream type is set before the adapter is entered, since read_packet
 * runs during startread and needs it to police the major sync. */
static int startread(sox_format_t * ft, lsx_ffmpeg_codec_definition_t const * definition)
{
  priv_t * p = (priv_t *)ft->priv;

  p->stream_type = definition->codec_id == AV_CODEC_ID_TRUEHD ? 0xba : 0xbb;
  return lsx_ffmpeg_codec_startread(ft, &p->codec, definition);
}

static int startread_mlp(sox_format_t * ft)
{
  return startread(ft, &mlp_definition);
}

static int startread_truehd(sox_format_t * ft)
{
  return startread(ft, &truehd_definition);
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

static int startwrite(sox_format_t * ft, lsx_ffmpeg_codec_definition_t const * definition)
{
  priv_t * p = (priv_t *)ft->priv;

  /* FFmpeg's encoders implement only these two depths. */
  if (ft->encoding.bits_per_sample != 16 && ft->encoding.bits_per_sample != 24) {
    lsx_fail_errno(ft, SOX_EFMT, "%s encoding supports 16-bit or 24-bit PCM", definition->name);
    return SOX_EOF;
  }
  /* -C is rejected rather than ignored: for a lossless format it would mean
   * nothing, and silently dropping it would look like it had taken effect. */
  if (ft->encoding.compression != HUGE_VAL) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "%s is lossless and does not accept a bitrate or compression "
        "level with -C", definition->name);
    return SOX_EOF;
  }
  p->stream_type = definition->codec_id == AV_CODEC_ID_TRUEHD ? 0xba : 0xbb;
  p->starts_with_major_sync = sox_false;
  p->packets_written = 0;
  return lsx_ffmpeg_codec_startwrite(ft, &p->codec, definition);
}

static int startwrite_mlp(sox_format_t * ft)
{
  return startwrite(ft, &mlp_definition);
}

static int startwrite_truehd(sox_format_t * ft)
{
  return startwrite(ft, &truehd_definition);
}

static size_t write_samples(sox_format_t * ft, sox_sample_t const * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_write(ft, p->codec, samples, length);
}

static int stopwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  int result = lsx_ffmpeg_codec_stopwrite(ft, &p->codec);

  /* The encoder places a major sync at the start of each header interval, so
   * input shorter than one interval yields a file whose first access unit
   * has none and which therefore cannot be decoded on its own.  Nothing can
   * be repaired at this point -- the packets are written -- so this reports
   * a failure and says what would avoid it, rather than leaving a file that
   * looks fine and does not play.  Both a warning and an error are emitted:
   * the warning describes the file that exists, the error is what SoX's exit
   * status reflects. */
  if (result == SOX_SUCCESS && !p->starts_with_major_sync) {
    lsx_warn("`%s': %s input is too short for the FFmpeg encoder to "
        "start with a major sync header; the output is not a standalone "
        "decodable elementary stream",
        ft->filename, p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    lsx_fail_errno(ft, SOX_EFMT,
        "%s input is too short for the FFmpeg encoder to emit a major "
        "sync header; provide at least one complete encoder header interval",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  return result;
}

sox_format_handler_t const * lsx_mlp_format_handler(void)
{
  static char const * const names[] = {"mlp", NULL};
  static unsigned const encodings[] = {
    SOX_ENCODING_MLP, 16, 24, 0, 0
  };
  static sox_rate_t const rates[] = {
    44100, 48000, 88200, 96000, 176400, 192000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "Meridian Lossless Packing elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread_mlp,
    read_samples,
    stopread,
    startwrite_mlp,
    write_samples,
    stopwrite,
    NULL,
    encodings,
    rates,
    sizeof(priv_t)
  };

  return &handler;
}

sox_format_handler_t const * lsx_truehd_format_handler(void)
{
  static char const * const names[] = {"truehd", "thd", NULL};
  static unsigned const encodings[] = {
    SOX_ENCODING_TRUEHD, 16, 24, 0, 0
  };
  static sox_rate_t const rates[] = {
    44100, 48000, 88200, 96000, 176400, 192000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "Dolby TrueHD elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread_truehd,
    read_samples,
    stopread,
    startwrite_truehd,
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
