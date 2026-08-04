/* LOAS/LATM framing, shared by the two handlers that read it: latm for AAC
 * and usac for xHE-AAC.  Both formats put their access units in the same
 * transport, and the object type inside the configuration is what tells them
 * apart -- which is also what lets format detection route a file to the right
 * one of the two.
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

#ifndef LSX_LATM_COMMON_H
#define LSX_LATM_COMMON_H

#include "sox.h"
#include "ffmpeg-codec.h"

#include <stddef.h>
#include <stdint.h>

/* A LOAS AudioSyncStream header is a fixed 3 bytes: an 11-bit sync word and
 * a 13-bit payload length, whose width is what bounds a frame at 8191 bytes. */
#define LSX_LOAS_HEADER_SIZE 3
#define LSX_LOAS_MAX_FRAME_SIZE 0x1fff
#define LSX_LOAS_MAX_PACKET_SIZE \
  (LSX_LOAS_HEADER_SIZE + LSX_LOAS_MAX_FRAME_SIZE)

/* Read one whole LOAS frame, header included, into packet, which must have
 * room for LSX_LOAS_MAX_PACKET_SIZE bytes however small the frame turns out
 * to be.  *packet_size is what was written.  Returns 1 on success, 0 at end
 * of stream, or SOX_EOF on error; end of stream only counts as clean if
 * clean_eof is set and the file ended exactly on a frame boundary, so a
 * truncated frame is still an error.  codec_name only names the format in
 * messages.  Does not resynchronise: a bad sync word is a failure, since the
 * caller reached here having already identified the file. */
int lsx_loas_read_packet(
    sox_format_t * ft,
    uint8_t * packet,
    size_t capacity,
    size_t * packet_size,
    sox_bool clean_eof,
    char const * codec_name);

/* Read a LATM variable-length integer: a two-bit count of the bytes that
 * follow, then that many bytes, most significant first. */
int lsx_latm_read_value(lsx_bit_reader_t * reader, uint32_t * value);

/* Peek at the audioObjectType in the configuration of a LOAS frame, without
 * consuming anything and without requiring the whole frame to be present --
 * which is what makes it usable on the few bytes format detection has.
 * Returns 1 with *object_type set, 0 if the frame carries no configuration to
 * read, or SOX_EOF if it is malformed or too short to reach the field. */
int lsx_latm_config_object_type(uint8_t const * packet, size_t packet_size, uint32_t * object_type);

#endif
