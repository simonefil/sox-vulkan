/* libavformat adapter for the SoX handlers whose codec is wrapped in a real
 * container rather than in a self-framing bitstream.
 *
 * It exists to supply the two hooks lsx_ffmpeg_codec_definition_t leaves
 * open, packet_reader and packet_writer, so the codec adapter can stay
 * unaware of containers.  All I/O goes through a custom AVIOContext bound to
 * SoX's own file handle, so libavformat never opens the file itself and the
 * usual SoX plumbing -- pipes, stdin/stdout, byte counting -- keeps working.
 * Only ALAC in M4A uses it at present.
 *
 * This whole layer is compiled only when HAVE_FFMPEG_FORMATS is defined.
 */

#ifndef LSX_FFMPEG_CONTAINER_H
#define LSX_FFMPEG_CONTAINER_H

#include "sox.h"

#include <libavcodec/avcodec.h>

/* Opaque per-file container state, created and destroyed by the calls below.
 * A given state is used for reading or for writing, never both. */
typedef struct lsx_ffmpeg_container_t lsx_ffmpeg_container_t;

/* Open the named libavformat demuxer on ft, find the audio stream carrying
 * codec_id, and copy that stream's parameters -- including any extradata the
 * decoder needs -- into codec_context, which must be allocated but not yet
 * open.  Sets ft->signal.length from the stream duration when it can.  On
 * success *state holds the new state; on failure it is left untouched and
 * nothing needs freeing. */
int lsx_ffmpeg_container_startread(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    enum AVCodecID codec_id,
    AVCodecContext * codec_context);

/* Read the next packet of the selected stream, skipping packets of any other.
 * Returns 1 with packet filled and owned by the caller, 0 at end of file, or
 * SOX_EOF on error.  Matches the lsx_ffmpeg_codec_packet_reader_t contract. */
int lsx_ffmpeg_container_read_packet(sox_format_t * ft, lsx_ffmpeg_container_t * state, AVPacket * packet);

/* Release the read-side state and set *state to NULL.  Safe on a NULL state. */
void lsx_ffmpeg_container_stopread(lsx_ffmpeg_container_t ** state);

/* Create the named muxer on ft with one audio stream described by
 * codec_context, which must already be open, and write the header.  Requires
 * a seekable output.  Ownership rules match the read side. */
int lsx_ffmpeg_container_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_container_t ** state,
    char const * format_name,
    AVCodecContext const * codec_context);

/* Mux one encoded packet, rescaling its timestamps from the codec's time base
 * to the stream's.  The packet stays owned by the caller.  Matches the
 * lsx_ffmpeg_codec_packet_writer_t contract. */
int lsx_ffmpeg_container_write_packet(
    sox_format_t * ft,
    lsx_ffmpeg_container_t * state,
    AVCodecContext const * codec_context,
    AVPacket const * packet);

/* Write the trailer, then release the state and set *state to NULL.  The
 * trailer is what makes the file playable, so this must run even on the error
 * paths; the state is destroyed either way. */
int lsx_ffmpeg_container_stopwrite(sox_format_t * ft, lsx_ffmpeg_container_t ** state);

#endif
