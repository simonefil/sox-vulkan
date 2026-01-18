#ifndef LSX_LATM_COMMON_H
#define LSX_LATM_COMMON_H

#include "sox.h"

#include <stddef.h>
#include <stdint.h>

#define LSX_LOAS_HEADER_SIZE 3
#define LSX_LOAS_MAX_FRAME_SIZE 0x1fff
#define LSX_LOAS_MAX_PACKET_SIZE \
  (LSX_LOAS_HEADER_SIZE + LSX_LOAS_MAX_FRAME_SIZE)

int lsx_loas_read_packet(
    sox_format_t * ft,
    uint8_t * packet,
    size_t capacity,
    size_t * packet_size,
    sox_bool clean_eof,
    char const * codec_name);

int lsx_latm_config_object_type(
    uint8_t const * packet,
    size_t packet_size,
    uint32_t * object_type);

#endif
