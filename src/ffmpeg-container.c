#include "sox_i.h"
#include "ffmpeg-container.h"

#ifdef HAVE_FFMPEG_FORMATS

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AVIO_BUFFER_SIZE 32768

struct lsx_ffmpeg_container_t {
  sox_format_t * ft;
  AVFormatContext * format;
  AVIOContext * io;
  int stream_index;
  sox_bool writing;
  sox_bool header_written;
};

static int fail_av(
    sox_format_t * ft,
    int sox_error,
    char const * operation,
    int av_error)
{
  char message[AV_ERROR_MAX_STRING_SIZE];

  if (av_strerror(av_error, message, sizeof(message)) < 0)
    strcpy(message, "unknown FFmpeg error");
  lsx_fail_errno(ft, sox_error, "%s: %s", operation, message);
  return SOX_EOF;
}

static int read_packet(
    void * opaque,
    uint8_t * buffer,
    int size)
{
  lsx_ffmpeg_container_t * state = opaque;
  size_t count = lsx_readbuf(state->ft, buffer, (size_t)size);

  if (count == 0)
    return lsx_error(state->ft) ? AVERROR(EIO) : AVERROR_EOF;
  return (int)count;
}

static int write_packet(
    void * opaque,
    uint8_t const * buffer,
    int size)
{
  lsx_ffmpeg_container_t * state = opaque;
  size_t count = lsx_writebuf(
      state->ft, buffer, (size_t)size);

  return count == (size_t)size ? size : AVERROR(EIO);
}

static int64_t seek(
    void * opaque,
    int64_t offset,
    int whence)
{
  lsx_ffmpeg_container_t * state = opaque;
  sox_format_t * ft = state->ft;
  off_t position;

  if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) {
    sox_uint64_t length = lsx_filelength(ft);

    if (length == 0)
      return AVERROR(ENOSYS);
    return length <= INT64_MAX ?
        (int64_t)length : AVERROR(EOVERFLOW);
  }
  if (!ft->seekable)
    return AVERROR(ENOSYS);
  whence &= ~AVSEEK_FORCE;
  position = (off_t)offset;
  if ((int64_t)position != offset)
    return AVERROR(EOVERFLOW);
  if (lsx_seeki(ft, position, whence) != SOX_SUCCESS)
    return AVERROR(EIO);
  position = lsx_tell(ft);
  return position >= 0 ? position : AVERROR(EIO);
}

static int allocate_io(
    sox_format_t * ft,
    lsx_ffmpeg_container_t * state,
    sox_bool writing)
{
  uint8_t * buffer = av_malloc(AVIO_BUFFER_SIZE);

  if (buffer == NULL)
    return SOX_EOF;
  state->io = avio_alloc_context(buffer, AVIO_BUFFER_SIZE,
      writing, state, writing ? NULL : read_packet,
      writing ? write_packet : NULL, seek);
  if (state->io == NULL) {
    av_free(buffer);
    return SOX_EOF;
  }
  state->ft = ft;
  state->writing = writing;
  return SOX_SUCCESS;
}

static void destroy_state(
    lsx_ffmpeg_container_t ** state)
{
  lsx_ffmpeg_container_t * p;

  if (state == NULL || *state == NULL)
    return;
  p = *state;
  if (p->writing)
    avformat_free_context(p->format);
  else
    avformat_close_input(&p->format);
  if (p->io != NULL)
    av_freep(&p->io->buffer);
  avio_context_free(&p->io);
  free(p);
  *state = NULL;
}

int lsx_ffmpeg_container_startread(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    enum AVCodecID codec_id,
    AVCodecContext * codec_context)
{
  lsx_ffmpeg_container_t * p = lsx_calloc(1, sizeof(*p));
  AVInputFormat const * input_format =
      av_find_input_format(format_name);
  unsigned i;
  int result;

  if (input_format == NULL) {
    free(p);
    lsx_fail_errno(ft, SOX_EFMT,
        "FFmpeg input format `%s' is unavailable", format_name);
    return SOX_EOF;
  }
  if (allocate_io(ft, p, sox_false) != SOX_SUCCESS) {
    free(p);
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate FFmpeg container I/O");
    return SOX_EOF;
  }
  p->format = avformat_alloc_context();
  if (p->format == NULL) {
    destroy_state(&p);
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate FFmpeg input container");
    return SOX_EOF;
  }
  p->format->pb = p->io;
  p->format->flags |= AVFMT_FLAG_CUSTOM_IO;
  result = avformat_open_input(
      &p->format, NULL, input_format, NULL);
  if (result < 0) {
    fail_av(ft, SOX_EHDR,
        "Unable to open FFmpeg input container", result);
    destroy_state(&p);
    return SOX_EOF;
  }
  result = avformat_find_stream_info(p->format, NULL);
  if (result < 0) {
    fail_av(ft, SOX_EHDR,
        "Unable to read FFmpeg stream information", result);
    destroy_state(&p);
    return SOX_EOF;
  }

  p->stream_index = -1;
  for (i = 0; i < p->format->nb_streams; ++i)
    if (p->format->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO &&
        p->format->streams[i]->codecpar->codec_id == codec_id) {
      p->stream_index = (int)i;
      break;
    }
  if (p->stream_index < 0) {
    lsx_fail_errno(ft, SOX_EFMT,
        "M4A file does not contain the requested audio codec");
    destroy_state(&p);
    return SOX_EOF;
  }
  result = avcodec_parameters_to_context(codec_context,
      p->format->streams[p->stream_index]->codecpar);
  if (result < 0) {
    fail_av(ft, SOX_EHDR,
        "Unable to configure FFmpeg decoder from M4A", result);
    destroy_state(&p);
    return SOX_EOF;
  }
  if (ft->signal.length != SOX_IGNORE_LENGTH) {
    AVStream const * stream =
        p->format->streams[p->stream_index];

    if (stream->duration > 0 &&
        stream->duration != AV_NOPTS_VALUE &&
        codec_context->sample_rate > 0 &&
        codec_context->ch_layout.nb_channels > 0) {
      int64_t samples = av_rescale_q(
          stream->duration, stream->time_base,
          (AVRational){1, codec_context->sample_rate});
      unsigned channels =
          (unsigned)codec_context->ch_layout.nb_channels;

      if (samples > 0 &&
          (uint64_t)samples <= UINT64_MAX / channels)
        ft->signal.length =
            (sox_uint64_t)samples * channels;
    }
  }
  *state = p;
  return SOX_SUCCESS;
}

int lsx_ffmpeg_container_read_packet(
    sox_format_t * ft,
    lsx_ffmpeg_container_t * state,
    AVPacket * packet)
{
  for (;;) {
    int result = av_read_frame(state->format, packet);

    if (result == AVERROR_EOF)
      return 0;
    if (result < 0)
      return fail_av(ft, SOX_EHDR,
          "Unable to read compressed packet from M4A", result);
    if (packet->stream_index == state->stream_index)
      return 1;
    av_packet_unref(packet);
  }
}

void lsx_ffmpeg_container_stopread(
    lsx_ffmpeg_container_t ** state)
{
  destroy_state(state);
}

int lsx_ffmpeg_container_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    AVCodecContext const * codec_context)
{
  lsx_ffmpeg_container_t * p = lsx_calloc(1, sizeof(*p));
  AVStream * stream;
  AVDictionary * options = NULL;
  int result;

  p->writing = sox_true;
  if (!ft->seekable) {
    free(p);
    lsx_fail_errno(ft, SOX_EFMT,
        "M4A output requires a seekable file");
    return SOX_EOF;
  }
  result = avformat_alloc_output_context2(
      &p->format, NULL, format_name, ft->filename);
  if (result < 0 || p->format == NULL) {
    if (result < 0)
      fail_av(ft, SOX_EFMT,
          "Unable to allocate FFmpeg output container", result);
    else
      lsx_fail_errno(ft, SOX_ENOMEM,
          "Unable to allocate FFmpeg output container");
    destroy_state(&p);
    return SOX_EOF;
  }
  if (allocate_io(ft, p, sox_true) != SOX_SUCCESS) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate FFmpeg container I/O");
    destroy_state(&p);
    return SOX_EOF;
  }
  p->format->pb = p->io;
  p->format->flags |= AVFMT_FLAG_CUSTOM_IO;
  stream = avformat_new_stream(p->format, NULL);
  if (stream == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to allocate M4A audio stream");
    destroy_state(&p);
    return SOX_EOF;
  }
  result = avcodec_parameters_from_context(
      stream->codecpar, codec_context);
  if (result < 0) {
    fail_av(ft, SOX_EFMT,
        "Unable to configure M4A audio stream", result);
    destroy_state(&p);
    return SOX_EOF;
  }
  stream->codecpar->codec_tag = 0;
  stream->time_base = codec_context->time_base;
  p->stream_index = stream->index;
  av_dict_set(&options, "movflags", "faststart", 0);
  result = avformat_write_header(p->format, &options);
  av_dict_free(&options);
  if (result < 0) {
    fail_av(ft, SOX_EFMT,
        "Unable to write M4A header", result);
    destroy_state(&p);
    return SOX_EOF;
  }
  p->header_written = sox_true;
  *state = p;
  return SOX_SUCCESS;
}

int lsx_ffmpeg_container_write_packet(
    sox_format_t * ft,
    lsx_ffmpeg_container_t * state,
    AVCodecContext const * codec_context,
    AVPacket const * packet)
{
  AVPacket * copy = av_packet_clone(packet);
  AVStream * stream =
      state->format->streams[state->stream_index];
  int result;

  if (copy == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM,
        "Unable to copy compressed packet for M4A");
    return SOX_EOF;
  }
  av_packet_rescale_ts(
      copy, codec_context->time_base, stream->time_base);
  copy->stream_index = state->stream_index;
  result = av_interleaved_write_frame(state->format, copy);
  av_packet_free(&copy);
  return result < 0 ? fail_av(ft, SOX_EFMT,
      "Unable to write compressed packet to M4A", result) : SOX_SUCCESS;
}

int lsx_ffmpeg_container_stopwrite(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state)
{
  lsx_ffmpeg_container_t * p;
  int result = SOX_SUCCESS;

  if (state == NULL || *state == NULL)
    return SOX_SUCCESS;
  p = *state;
  if (p->header_written) {
    int av_result = av_write_trailer(p->format);

    if (av_result < 0)
      result = fail_av(ft, SOX_EFMT,
          "Unable to finalize M4A container", av_result);
  }
  destroy_state(state);
  return result;
}

#endif
