#include "sox_i.h"
#include "latm-common.h"

#ifdef HAVE_FFMPEG_CODECS

typedef struct {
  uint8_t const * data;
  size_t size_bits;
  size_t position;
} bit_reader_t;

static int read_exact(
    sox_format_t * ft,
    uint8_t * data,
    size_t size,
    sox_bool clean_eof,
    char const * codec_name)
{
  size_t done = 0;

  while (done < size) {
    size_t count = lsx_readbuf(ft, data + done, size - done);

    if (count == 0) {
      if (done == 0 && clean_eof)
        return 0;
      lsx_fail_errno(ft, SOX_EHDR,
          "Truncated %s LOAS frame", codec_name);
      return SOX_EOF;
    }
    done += count;
  }
  return 1;
}

static int read_bits(
    bit_reader_t * reader,
    unsigned count,
    uint32_t * value)
{
  uint32_t result = 0;
  unsigned i;

  if (count > 32 || reader->position > reader->size_bits ||
      count > reader->size_bits - reader->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = reader->position++;

    result = (result << 1) |
        ((reader->data[position / 8] >> (7 - position % 8)) & 1);
  }
  if (value != NULL)
    *value = result;
  return SOX_SUCCESS;
}

static int read_latm_value(
    bit_reader_t * reader,
    uint32_t * value)
{
  uint32_t bytes_for_value;
  uint32_t result = 0;
  uint32_t byte;
  unsigned i;

  if (read_bits(reader, 2, &bytes_for_value) != SOX_SUCCESS)
    return SOX_EOF;
  for (i = 0; i <= bytes_for_value; ++i) {
    if (read_bits(reader, 8, &byte) != SOX_SUCCESS)
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
    lsx_fail_errno(ft, SOX_EINVAL,
        "Internal LOAS packet buffer is too small");
    return SOX_EOF;
  }
  result = read_exact(ft, packet, LSX_LOAS_HEADER_SIZE,
      clean_eof, codec_name);
  if (result != 1)
    return result;
  if (packet[0] != 0x56 || (packet[1] & 0xe0) != 0xe0) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Invalid %s LOAS sync word", codec_name);
    return SOX_EOF;
  }

  frame_size = ((size_t)(packet[1] & 0x1f) << 8) | packet[2];
  if (frame_size == 0 ||
      read_exact(ft, packet + LSX_LOAS_HEADER_SIZE,
          frame_size, sox_false, codec_name) != 1)
    return SOX_EOF;
  *packet_size = LSX_LOAS_HEADER_SIZE + frame_size;
  return 1;
}

int lsx_latm_config_object_type(
    uint8_t const * packet,
    size_t packet_size,
    uint32_t * object_type)
{
  bit_reader_t reader;
  size_t declared_size;
  size_t available_size;
  uint32_t value;
  uint32_t audio_mux_version;
  uint32_t asc_bits = 0;
  size_t asc_start;

  if (packet_size < LSX_LOAS_HEADER_SIZE ||
      packet[0] != 0x56 || (packet[1] & 0xe0) != 0xe0)
    return SOX_EOF;
  declared_size = ((size_t)(packet[1] & 0x1f) << 8) | packet[2];
  if (declared_size == 0)
    return SOX_EOF;
  available_size = min(
      declared_size, packet_size - LSX_LOAS_HEADER_SIZE);
  reader.data = packet + LSX_LOAS_HEADER_SIZE;
  reader.size_bits = available_size * 8;
  reader.position = 0;

  if (read_bits(&reader, 1, &value) != SOX_SUCCESS)
    return SOX_EOF;
  if (value)
    return 0;
  if (read_bits(&reader, 1, &audio_mux_version) != SOX_SUCCESS)
    return SOX_EOF;
  if (audio_mux_version) {
    if (read_bits(&reader, 1, &value) != SOX_SUCCESS || value != 0 ||
        read_latm_value(&reader, &value) != SOX_SUCCESS)
      return SOX_EOF;
  }
  if (read_bits(&reader, 1, &value) != SOX_SUCCESS || value != 1 ||
      read_bits(&reader, 6, &value) != SOX_SUCCESS || value != 0 ||
      read_bits(&reader, 4, &value) != SOX_SUCCESS || value != 0 ||
      read_bits(&reader, 3, &value) != SOX_SUCCESS || value != 0)
    return SOX_EOF;
  if (audio_mux_version &&
      (read_latm_value(&reader, &asc_bits) != SOX_SUCCESS ||
       asc_bits < 5))
    return SOX_EOF;

  asc_start = reader.position;
  if (read_bits(&reader, 5, object_type) != SOX_SUCCESS)
    return SOX_EOF;
  if (*object_type == 31) {
    if (read_bits(&reader, 6, &value) != SOX_SUCCESS)
      return SOX_EOF;
    *object_type = 32 + value;
  }
  if (asc_bits && reader.position - asc_start > asc_bits)
    return SOX_EOF;
  return 1;
}

#endif
