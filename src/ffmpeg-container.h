#ifndef LSX_FFMPEG_CONTAINER_H
#define LSX_FFMPEG_CONTAINER_H

#include "sox.h"

#include <libavcodec/avcodec.h>

typedef struct lsx_ffmpeg_container_t lsx_ffmpeg_container_t;

int lsx_ffmpeg_container_startread(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    enum AVCodecID codec_id,
    AVCodecContext * codec_context);

int lsx_ffmpeg_container_read_packet(sox_format_t * ft, lsx_ffmpeg_container_t * state, AVPacket * packet);

void lsx_ffmpeg_container_stopread(lsx_ffmpeg_container_t ** state);

int lsx_ffmpeg_container_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    AVCodecContext const * codec_context);

int lsx_ffmpeg_container_write_packet(
    sox_format_t * ft,
    lsx_ffmpeg_container_t * state,
    AVCodecContext const * codec_context,
    AVPacket const * packet);

int lsx_ffmpeg_container_stopwrite(sox_format_t * ft, lsx_ffmpeg_container_t ** state);

#endif
