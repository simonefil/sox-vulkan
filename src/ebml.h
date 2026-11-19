/* libSoX EBML, the binary markup Matroska and WebM are written in.
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

#ifndef SOX_EBML_H
#define SOX_EBML_H

#include "sox_i.h"

/* Element IDs, kept with their length marker as they appear in the file. */
#define EBML_ID_HEADER              0x1A45DFA3
#define EBML_ID_VERSION             0x4286
#define EBML_ID_READ_VERSION        0x42F7
#define EBML_ID_MAX_ID_LENGTH       0x42F2
#define EBML_ID_MAX_SIZE_LENGTH     0x42F3
#define EBML_ID_DOCTYPE             0x4282
#define EBML_ID_DOCTYPE_VERSION     0x4287
#define EBML_ID_DOCTYPE_READ_VERSION 0x4285
#define EBML_ID_VOID                0xEC

#define MKV_ID_SEGMENT              0x18538067
#define MKV_ID_SEEKHEAD             0x114D9B74
#define MKV_ID_SEEK                 0x4DBB
#define MKV_ID_SEEK_ID              0x53AB
#define MKV_ID_SEEK_POSITION        0x53AC
#define MKV_ID_INFO                 0x1549A966
#define MKV_ID_TIMESTAMP_SCALE      0x2AD7B1
#define MKV_ID_DURATION             0x4489
#define MKV_ID_MUXING_APP           0x4D80
#define MKV_ID_WRITING_APP          0x5741
#define MKV_ID_TRACKS               0x1654AE6B
#define MKV_ID_TRACK_ENTRY          0xAE
#define MKV_ID_TRACK_NUMBER         0xD7
#define MKV_ID_TRACK_UID            0x73C5
#define MKV_ID_TRACK_TYPE           0x83
#define MKV_ID_FLAG_LACING          0x9C
#define MKV_ID_NAME                 0x536E
#define MKV_ID_LANGUAGE             0x22B59C
#define MKV_ID_CODEC_ID             0x86
#define MKV_ID_CODEC_PRIVATE        0x63A2
#define MKV_ID_CODEC_DELAY          0x56AA
#define MKV_ID_CONTENT_ENCODINGS    0x6D80
#define MKV_ID_AUDIO                0xE1
#define MKV_ID_SAMPLING_FREQUENCY   0xB5
#define MKV_ID_CHANNELS             0x9F
#define MKV_ID_BIT_DEPTH            0x6264
#define MKV_ID_CLUSTER              0x1F43B675
#define MKV_ID_TIMESTAMP            0xE7
#define MKV_ID_SIMPLE_BLOCK         0xA3
#define MKV_ID_BLOCK_GROUP          0xA0
#define MKV_ID_BLOCK                0xA1
#define MKV_ID_DISCARD_PADDING      0x75A2
#define MKV_ID_CUES                 0x1C53BB6B
#define MKV_ID_CUE_POINT            0xBB
#define MKV_ID_CUE_TIME             0xB3
#define MKV_ID_CUE_TRACK_POSITIONS  0xB7
#define MKV_ID_CUE_TRACK            0xF7
#define MKV_ID_CUE_CLUSTER_POSITION 0xF1

#define MKV_TRACK_TYPE_AUDIO 2

/* The size of an element whose end is found only by what follows it. */
#define EBML_UNKNOWN_SIZE ((sox_uint64_t)-1)

/* Reading, straight from the file and never seeking backwards, so that a
 * pipe reads as well as a file.  lsx_ebml_read_id returns SOX_EOF without an
 * error at a clean end of file; every other failure sets ft->sox_errno. */
int lsx_ebml_read_id(sox_format_t * ft, sox_uint32_t * id);
int lsx_ebml_read_size(sox_format_t * ft, sox_uint64_t * size);
int lsx_ebml_read_uint(sox_format_t * ft, sox_uint64_t size, sox_uint64_t * value);
int lsx_ebml_read_sint(sox_format_t * ft, sox_uint64_t size, sox_int64_t * value);
int lsx_ebml_read_float(sox_format_t * ft, sox_uint64_t size, double * value);
int lsx_ebml_read_binary(sox_format_t * ft, sox_uint64_t size, size_t limit,
    unsigned char * * data);
int lsx_ebml_skip(sox_format_t * ft, sox_uint64_t size);
size_t lsx_ebml_parse_vint(unsigned char const * data, size_t len, sox_uint64_t * value);

/* Writing, into a growing buffer that is then written out whole. */
typedef struct {
  unsigned char * data;
  size_t size, alloc;
} lsx_ebml_buffer_t;

void lsx_ebml_put_bytes(lsx_ebml_buffer_t * b, void const * data, size_t size);
void lsx_ebml_put_id(lsx_ebml_buffer_t * b, sox_uint32_t id);
void lsx_ebml_put_size(lsx_ebml_buffer_t * b, sox_uint64_t size, unsigned width);
void lsx_ebml_put_uint(lsx_ebml_buffer_t * b, sox_uint32_t id, sox_uint64_t value);
void lsx_ebml_put_sint(lsx_ebml_buffer_t * b, sox_uint32_t id, sox_int64_t value);
void lsx_ebml_put_float(lsx_ebml_buffer_t * b, sox_uint32_t id, double value);
void lsx_ebml_put_string(lsx_ebml_buffer_t * b, sox_uint32_t id, char const * value);
void lsx_ebml_put_binary(lsx_ebml_buffer_t * b, sox_uint32_t id, void const * data, size_t size);
size_t lsx_ebml_open_master(lsx_ebml_buffer_t * b, sox_uint32_t id);
void lsx_ebml_close_master(lsx_ebml_buffer_t * b, size_t start);
void lsx_ebml_put_void(lsx_ebml_buffer_t * b, size_t total);

char const * lsx_matroska_detect(unsigned char const * data, size_t len);

#endif
