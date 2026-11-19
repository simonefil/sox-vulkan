/* libSoX container formats: a demuxer or muxer bound to a packet codec.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* A container handler supplies only the framing, as the operations below,
 * and leaves the format handler entry points to this layer: it picks the
 * codec, moves packets between the two, and trims the encoder delay and the
 * padding so that the samples handed out are exactly the signal. */

#ifndef SOX_CONTAINER_H
#define SOX_CONTAINER_H

#include "codec.h"

typedef struct {
  size_t priv_size;

  /* Reading.  open_read reads up to the first packet, fills in params as the
   * container declares them, and names the codec.  read_packet returns
   * SOX_SUCCESS with a packet, SOX_EOF at the end of the stream, and any
   * other value on an error, with ft->sox_errno set; the packet stays valid
   * until the next call.  next_stream, which may be NULL, follows the end of
   * a stream and reads up to the first packet of one chained after it,
   * filling in params, or returns SOX_EOF if there is none.  length, which
   * may be NULL, follows the start of the codec and gives the frames of
   * signal, or 0 if they are not known.  seek, which may be NULL, goes to a
   * frame of the signal and restarts the codec there, through
   * lsx_container_restart.  close_read also follows a failed open_read. */
  int (*open_read)(sox_format_t * ft, void * priv, lsx_codec_params_t * params,
      char const * * codec_name);
  int (*read_packet)(sox_format_t * ft, void * priv, lsx_codec_t * codec, lsx_packet_t * packet);
  int (*next_stream)(sox_format_t * ft, void * priv, lsx_codec_t const * codec,
      lsx_codec_params_t * params);
  int (*length)(sox_format_t * ft, void * priv, lsx_codec_t * codec, sox_uint64_t * frames);
  int (*seek)(sox_format_t * ft, void * priv, lsx_codec_t * codec, sox_uint64_t frame);
  void (*close_read)(sox_format_t * ft, void * priv);

  /* Writing.  open_write follows the codec's start, so the private data is
   * known, and is given the container's identifier for the codec; close_write
   * follows its flush, so the private data is final. */
  int (*open_write)(sox_format_t * ft, void * priv, lsx_codec_t const * codec,
      lsx_codec_tag_t const * tag);
  int (*write_packet)(sox_format_t * ft, void * priv, lsx_packet_t const * packet);
  int (*close_write)(sox_format_t * ft, void * priv, lsx_codec_t const * codec);
} lsx_container_ops_t;

typedef struct {
  lsx_container_ops_t const * ops;
  void * priv;
  lsx_codec_t codec;
  uint64_t skip; /* delay frames still to drop */
  sox_bool codec_started;
  sox_bool stream_ended; /* and the codec drained */
  sox_bool eof;
} lsx_container_t;

int lsx_codec_start_decode(lsx_codec_t * codec, sox_bool comments);
int lsx_container_check_chained(sox_format_t * ft, lsx_codec_params_t const * first,
    lsx_codec_params_t const * chained);
int lsx_container_restart(sox_format_t * ft, lsx_codec_params_t const * params, sox_bool stream_start);
int lsx_container_prepare_write(sox_format_t * ft);
void lsx_container_cancel_write(sox_format_t * ft);
int lsx_container_startread(sox_format_t * ft, lsx_container_ops_t const * ops);
size_t lsx_container_read(sox_format_t * ft, sox_sample_t * buf, size_t len);
int lsx_container_seek(sox_format_t * ft, sox_uint64_t offset);
int lsx_container_stopread(sox_format_t * ft);
int lsx_container_startwrite(sox_format_t * ft, lsx_container_ops_t const * ops);
size_t lsx_container_write(sox_format_t * ft, sox_sample_t const * buf, size_t len);
int lsx_container_stopwrite(sox_format_t * ft);

#endif
