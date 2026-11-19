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
   * until the next call.  close_read also follows a failed open_read. */
  int (*open_read)(sox_format_t * ft, void * priv, lsx_codec_params_t * params,
      char const * * codec_name);
  int (*read_packet)(sox_format_t * ft, void * priv, lsx_packet_t * packet);
  void (*close_read)(sox_format_t * ft, void * priv);

  /* Writing.  open_write follows the codec's start, so the private data is
   * known; close_write follows its flush, so the private data is final. */
  int (*open_write)(sox_format_t * ft, void * priv, lsx_codec_t const * codec);
  int (*write_packet)(sox_format_t * ft, void * priv, lsx_packet_t const * packet);
  int (*close_write)(sox_format_t * ft, void * priv, lsx_codec_t const * codec);
} lsx_container_ops_t;

typedef struct {
  lsx_container_ops_t const * ops;
  void * priv;
  lsx_codec_t codec;
  uint64_t skip; /* delay frames still to drop */
  sox_bool eof;
} lsx_container_t;

int lsx_container_startread(sox_format_t * ft, lsx_container_ops_t const * ops);
size_t lsx_container_read(sox_format_t * ft, sox_sample_t * buf, size_t len);
int lsx_container_stopread(sox_format_t * ft);
int lsx_container_startwrite(sox_format_t * ft, lsx_container_ops_t const * ops);
size_t lsx_container_write(sox_format_t * ft, sox_sample_t const * buf, size_t len);
int lsx_container_stopwrite(sox_format_t * ft);

#endif
