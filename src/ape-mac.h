/* libSoX Monkey's Audio frame decoder and encoder, from the Monkey's Audio
 * SDK.
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

/* The SDK's decompressor and compressor work on whole files; only their
 * frame decoder and encoder are used here, so that the file is framed by
 * SoX.  Samples are interleaved little-endian PCM as a WAVE file holds it,
 * 8-bit samples unsigned, floating-point ones IEEE single precision.  A frame is decoded from the 32-bit words of the
 * file that hold it, and the word after, skipping the bytes of its first
 * word that precede it; it is encoded into the bytes it occupies in a file
 * when it starts on a 32-bit boundary, padded with zeros to a whole number of
 * 32-bit words. */

#ifndef SOX_APE_MAC_H
#define SOX_APE_MAC_H

#include "sox.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MD5 (RFC 1321), kept by the files written.  The SDK is built without an
 * MD5 of its own, and its whole-file compressor and verifier, never called
 * by SoX, are given this one. */
typedef struct {
  sox_uint32_t state[4];
  sox_uint32_t length[2];         /* bytes added, low word first */
  unsigned char block[64];
} lsx_md5_t;

void lsx_md5_init(lsx_md5_t * md5);
void lsx_md5_add(lsx_md5_t * md5, void const * data, size_t size);
void lsx_md5_final(lsx_md5_t * md5, unsigned char digest[16]);

typedef struct lsx_ape_decoder lsx_ape_decoder_t;
typedef struct lsx_ape_encoder lsx_ape_encoder_t;

/* NULL if the decoder cannot be set up for the file: the version is the
 * file's, 3930 to 3990, the level the SDK's, 1000 to 5000, frame_blocks the
 * most blocks a frame holds, and floating_point nonzero for 32-bit
 * floating-point samples. */
lsx_ape_decoder_t * lsx_ape_decoder_create(unsigned version, int level, unsigned channels,
    unsigned bits_per_sample, unsigned rate, unsigned frame_blocks, int floating_point);

/* 0 with *pcm valid until the next call, or nonzero if the frame does not
 * decode or fails its CRC. */
int lsx_ape_decoder_decode(lsx_ape_decoder_t * decoder, unsigned char const * frame, size_t bytes,
    unsigned skip, unsigned blocks, unsigned char const * * pcm);

void lsx_ape_decoder_destroy(lsx_ape_decoder_t * decoder);


/* NULL if the encoder cannot be set up for the format; the level is the
 * SDK's, 1000 to 5000, frame_blocks the most blocks a frame holds, and
 * floating_point nonzero for 32-bit floating-point samples. */
lsx_ape_encoder_t * lsx_ape_encoder_create(unsigned channels, unsigned bits_per_sample,
    unsigned rate, int level, unsigned frame_blocks, int floating_point);

/* 0 with *frame valid until the next call, or nonzero on an error;
 * floating-point samples are transformed in place. */
int lsx_ape_encoder_encode(lsx_ape_encoder_t * encoder, void * pcm, size_t bytes,
    unsigned char const * * frame, size_t * frame_bytes);

void lsx_ape_encoder_destroy(lsx_ape_encoder_t * encoder);

#ifdef __cplusplus
}
#endif

#endif
