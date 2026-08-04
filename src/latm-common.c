/* LOAS/LATM framing implementation shared by the AAC and USAC handlers.
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
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "sox_i.h"
#include "ffmpeg-codec.h"
#include "latm-common.h"

#ifdef HAVE_FFMPEG_CODECS

/* Read exactly size bytes, looping because lsx_readbuf may return fewer.
 * Returns 1 on success, 0 only when nothing at all was left and the caller
 * said that is a clean end of stream, and SOX_EOF otherwise -- which is how a
 * file that stops in the middle of a frame is told from one that ends
 * between frames. */
static int read_exact(sox_format_t * ft, uint8_t * data, size_t size, sox_bool clean_eof, char const * codec_name)
{
  size_t done = 0;

  while (done < size) {
    size_t count = lsx_readbuf(ft, data + done, size - done);

    if (count == 0) {
      if (done == 0 && clean_eof)
        return 0;
      lsx_fail_errno(ft, SOX_EHDR, "Truncated %s LOAS frame", codec_name);
      return SOX_EOF;
    }
    done += count;
  }
  return 1;
}

int lsx_latm_read_value(lsx_bit_reader_t * reader, uint32_t * value)
{
  uint32_t bytes_for_value;
  uint32_t result = 0;
  uint32_t byte;
  unsigned i;

  if (lsx_bit_read(reader, 2, &bytes_for_value) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = 0; i <= bytes_for_value; ++i) {
    if (lsx_bit_read(reader, 8, &byte) != SOX_SUCCESS)
      return SOX_EOF;
    result = (result << 8) | byte;
  }
  *value = result;
  return SOX_SUCCESS;
}

int lsx_loas_read_packet(
    sox_format_t * ft,
    uint8_t * packet,
    size_t capacity,
    size_t * packet_size,
    sox_bool clean_eof,
    char const * codec_name)
{
  size_t frame_size;
  int result;

  if (capacity < LSX_LOAS_MAX_PACKET_SIZE) {
    lsx_fail_errno(ft, SOX_EINVAL, "Internal LOAS packet buffer is too small");
    return SOX_EOF;
  }
  /* Only the header may end the stream cleanly; once it has been accepted the
   * payload must be there in full, so the second read never forgives EOF. */
  result = read_exact(ft, packet, LSX_LOAS_HEADER_SIZE, clean_eof, codec_name);
  if (result != 1)
    return result;
  /* The 11-bit sync word 0x2b7, spread over the first byte and the top three
   * bits of the second. */
  if (packet[0] != 0x56 || (packet[1] & 0xe0) != 0xe0) {
    lsx_fail_errno(ft, SOX_EHDR, "Invalid %s LOAS sync word", codec_name);
    return SOX_EOF;
  }

  frame_size = ((size_t)(packet[1] & 0x1f) << 8) | packet[2];
  if (frame_size == 0 || read_exact(ft, packet + LSX_LOAS_HEADER_SIZE, frame_size, sox_false, codec_name) != 1)
    return SOX_EOF;
  *packet_size = LSX_LOAS_HEADER_SIZE + frame_size;
  return 1;
}

int lsx_latm_config_object_type(uint8_t const * packet, size_t packet_size, uint32_t * object_type)
{
  lsx_bit_reader_t reader;
  size_t declared_size;
  size_t available_size;
  uint32_t value;
  uint32_t audio_mux_version;
  uint32_t asc_bits = 0;
  size_t asc_start;

  if (packet_size < LSX_LOAS_HEADER_SIZE || packet[0] != 0x56 || (packet[1] & 0xe0) != 0xe0)
    return SOX_EOF;
  declared_size = ((size_t)(packet[1] & 0x1f) << 8) | packet[2];
  if (declared_size == 0)
    return SOX_EOF;
  /* Read within whichever is smaller, what the frame claims or what the
   * caller actually has: the object type sits near the front, so a partial
   * frame is usually enough, and the reader's bounds turn "not enough" into
   * a clean failure rather than an over-read. */
  available_size = min(declared_size, packet_size - LSX_LOAS_HEADER_SIZE);
  reader.data = packet + LSX_LOAS_HEADER_SIZE;
  reader.size_bits = available_size * 8;
  reader.position = 0;

  /* useSameStreamMux: this frame repeats the previous configuration and so
   * carries nothing to inspect.  Not an error -- just no answer here. */
  if (lsx_bit_read(&reader, 1, &value) != SOX_SUCCESS)
    return SOX_EOF;
  if (value)
    return 0;
  /* Both audioMuxVersions are handled, unlike the stricter parse in usac.c:
   * this runs during format detection, where being able to read the object
   * type of any well-formed stream is the whole point.  Version 1 inserts
   * audioMuxVersionA and taraBufferFullness, and states the config length
   * explicitly, which version 0 does not. */
  if (lsx_bit_read(&reader, 1, &audio_mux_version) != SOX_SUCCESS)
    return SOX_EOF;
  if (audio_mux_version) {
    if (lsx_bit_read(&reader, 1, &value) != SOX_SUCCESS || value != 0 ||
        lsx_latm_read_value(&reader, &value) != SOX_SUCCESS)
      return SOX_EOF;
  }
  /* allStreamsSameTimeFraming, numSubFrames, numProgram, numLayer.  Anything
   * other than a single synchronous program and layer puts the config
   * somewhere this shortcut does not follow, so it is reported as unreadable
   * rather than parsed further. */
  if (lsx_bit_read(&reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      lsx_bit_read(&reader, 6, &value) != SOX_SUCCESS || value != 0 ||
      lsx_bit_read(&reader, 4, &value) != SOX_SUCCESS || value != 0 ||
      lsx_bit_read(&reader, 3, &value) != SOX_SUCCESS || value != 0)
    return SOX_EOF;
  if (audio_mux_version && (lsx_latm_read_value(&reader, &asc_bits) != SOX_SUCCESS || asc_bits < 5))
    return SOX_EOF;

  asc_start = reader.position;
  if (lsx_bit_read(&reader, 5, object_type) != SOX_SUCCESS)
    return SOX_EOF;
  if (*object_type == 31) {
    if (lsx_bit_read(&reader, 6, &value) != SOX_SUCCESS)
      return SOX_EOF;
    *object_type = 32 + value;
  }
  /* When the config length was stated, check the object type actually fitted
   * inside it; a type read from past the end would be someone else's bits. */
  if (asc_bits && reader.position - asc_start > asc_bits)
    return SOX_EOF;
  return 1;
}

#endif
