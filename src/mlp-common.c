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

typedef struct {
  lsx_ffmpeg_codec_t * codec;
  int stream_type;
  sox_bool starts_with_major_sync;
  unsigned packets_written;
} priv_t;

static int prepare_encoder(AVCodecContext * context)
{
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  return 0;
}

static size_t read_exact(
    sox_format_t * ft,
    uint8_t * data,
    size_t length)
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

static int read_packet(sox_format_t * ft, AVPacket * packet)
{
  priv_t * p = (priv_t *)ft->priv;
  uint8_t header[4];
  size_t header_size = read_exact(ft, header, sizeof(header));
  size_t frame_size;
  int result;

  if (header_size == 0)
    return 0;
  if (header_size != sizeof(header)) {
    lsx_fail_errno(ft, SOX_EHDR, "Truncated %s access unit header",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
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

  frame_size =
      ((((size_t)header[0] << 8) | header[1]) & 0xfff) * 2;
  if (frame_size < sizeof(header)) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid %s access unit length",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  result = av_new_packet(packet, (int)frame_size);
  if (result < 0) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate compressed audio packet");
    return SOX_EOF;
  }
  memcpy(packet->data, header, sizeof(header));
  if (read_exact(ft, packet->data + sizeof(header),
        frame_size - sizeof(header)) != frame_size - sizeof(header)) {
    av_packet_unref(packet);
    lsx_fail_errno(ft, SOX_EHDR, "Truncated %s access unit",
        p->stream_type == 0xba ? "Dolby TrueHD" : "MLP");
    return SOX_EOF;
  }
  if (frame_size >= 8 &&
      !memcmp(packet->data + 4, "\xf8\x72\x6f", 3) &&
      packet->data[7] != p->stream_type) {
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

static int write_packet(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet)
{
  priv_t * p = (priv_t *)ft->priv;

  (void)context;
  if (p->packets_written == 0 &&
      (size_t)packet->size >= 8 &&
      !memcmp(packet->data + 4, "\xf8\x72\x6f", 3) &&
      packet->data[7] == p->stream_type)
    p->starts_with_major_sync = sox_true;
  ++p->packets_written;
  return lsx_writebuf(ft, packet->data, (size_t)packet->size) ==
      (size_t)packet->size ? SOX_SUCCESS : SOX_EOF;
}

static lsx_ffmpeg_codec_definition_t const mlp_definition = {
  AV_CODEC_ID_MLP,
  SOX_ENCODING_MLP,
  "MLP",
  6,
  sox_false,
  6,
  0,
  0,
  0,
  0,
  AV_PROFILE_UNKNOWN,
  NULL,
  AV_PROFILE_UNKNOWN,
  NULL,
  prepare_encoder,
  read_packet,
  write_packet,
  sox_false,
  0,
  0,
  0,
  NULL
};

static lsx_ffmpeg_codec_definition_t const truehd_definition = {
  AV_CODEC_ID_TRUEHD,
  SOX_ENCODING_TRUEHD,
  "Dolby TrueHD",
  8,
  sox_false,
  6,
  0,
  0,
  0,
  0,
  AV_PROFILE_TRUEHD_ATMOS,
  "Dolby TrueHD with Dolby Atmos",
  AV_PROFILE_UNKNOWN,
  NULL,
  prepare_encoder,
  read_packet,
  write_packet,
  sox_false,
  0,
  0,
  0,
  NULL
};

static int startread(
    sox_format_t * ft,
    lsx_ffmpeg_codec_definition_t const * definition)
{
  priv_t * p = (priv_t *)ft->priv;

  p->stream_type =
      definition->codec_id == AV_CODEC_ID_TRUEHD ? 0xba : 0xbb;
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

static int startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_definition_t const * definition)
{
  priv_t * p = (priv_t *)ft->priv;

  if (ft->encoding.bits_per_sample != 16 &&
      ft->encoding.bits_per_sample != 24) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoding supports 16-bit or 24-bit PCM", definition->name);
    return SOX_EOF;
  }
  if (ft->encoding.compression != HUGE_VAL) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "%s is lossless and does not accept a bitrate or compression "
        "level with -C", definition->name);
    return SOX_EOF;
  }
  p->stream_type =
      definition->codec_id == AV_CODEC_ID_TRUEHD ? 0xba : 0xbb;
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
  int result = lsx_ffmpeg_codec_stopwrite(ft, &p->codec);

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
