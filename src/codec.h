/* libSoX packet codecs: the codec half of a container format.
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

/* A container format (Matroska, Ogg, and in time MP4) only frames the
 * stream: it hands out packets and the parameters it declares for them.
 * What turns those packets into samples, and back, is a codec, chosen apart
 * from the container: from the file when reading, and from the type when
 * writing, as in `-t mka:flac'.
 *
 * A codec works on whole packets.  The decoder is pushed one packet at a
 * time and may answer with any number of samples, none included, since
 * codecs with lookahead lag behind their input; an empty push at the end of
 * the stream lets it give up what it still holds.  The encoder is pushed
 * samples and answers with any number of packets through write_packet, and
 * an empty push at the end flushes it.
 *
 * Sample counts are kept exact across the round trip by numbers the
 * container carries for the codec: the delay, priming frames at the start of
 * the stream that are not part of the signal; the skip on a packet, frames at
 * its start that precede the signal; and the discard on a packet, padding
 * frames at its end.  The container drops all three after decoding, and on
 * writing records what the encoder reports.  A packet's skip and discard are
 * taken off the samples that pushing that packet returned, which is exact for
 * a decoder that returns each packet's samples as it is pushed. */

#ifndef SOX_CODEC_H
#define SOX_CODEC_H

#include "sox_i.h"

/* The containers whose codec is chosen apart from the container. */
typedef enum {
  lsx_container_matroska,
  lsx_container_webm,
  lsx_container_ogg
} lsx_container_family_t;

/* Byte order a container identifier implies for the samples it carries. */
typedef enum {
  lsx_endian_any,
  lsx_endian_little,
  lsx_endian_big
} lsx_endian_t;

/* A stream as the container declares it, or as the encoder sets it up for
 * the container to record. */
typedef struct {
  sox_rate_t rate;
  unsigned channels;
  sox_encoding_t encoding;      /* sample layout, for codecs with several */
  unsigned bits_per_sample;
  sox_bool big_endian;
  unsigned char * private_data; /* codec private data, heap allocated */
  size_t private_size;
  uint64_t delay;               /* priming frames at the start of the stream */
} lsx_codec_params_t;

typedef struct {
  unsigned char const * data;
  size_t size;
  size_t frames;  /* frames the packet decodes to, padding included; 0 if unknown */
  size_t skip;    /* frames at the start of the packet that precede the signal */
  size_t discard; /* frames at the end of the packet that are padding */
} lsx_packet_t;

typedef struct lsx_codec lsx_codec_t;

typedef struct {
  char const * name;              /* as given after the colon of a type */
  unsigned const * write_formats; /* laid out as in sox_format_handler_t */
  sox_rate_t const * write_rates; /* likewise; NULL for any rate */
  size_t priv_size;

  /* Decoding: params come filled in from the container and are corrected
   * from the codec's own headers, and up to the end of the first packet from
   * the packets themselves.  decode is given NULL at the end.  packet_frames,
   * which may be NULL, gives the frames a packet decodes to, for a container
   * that places packets by its own count of frames; it is asked about every
   * packet in stream order, ahead of decoding it.  stop_decode may be NULL. */
  int (*start_decode)(lsx_codec_t * codec);
  int (*decode)(lsx_codec_t * codec, unsigned char const * data, size_t size);
  size_t (*packet_frames)(lsx_codec_t * codec, unsigned char const * data, size_t size);
  void (*stop_decode)(lsx_codec_t * codec);

  /* Encoding: params come filled in from the output format; start_encode
   * sets private_data and delay.  encode is given NULL at the end, and may
   * then rewrite private_data, keeping its size, with what it knows only
   * once the stream is complete. */
  int (*start_encode)(lsx_codec_t * codec);
  int (*encode)(lsx_codec_t * codec, sox_sample_t const * buf, size_t len);
  void (*stop_encode)(lsx_codec_t * codec);
} lsx_codec_handler_t;

struct lsx_codec {
  lsx_codec_handler_t const * handler;
  sox_format_t * ft; /* for errors, the clip count and the user's settings */
  lsx_codec_params_t params;
  void * priv;

  /* Decoded samples not yet handed out lie between samples_start and
   * samples_end; the decoder appends through lsx_codec_output. */
  sox_sample_t * samples;
  size_t samples_start, samples_end, samples_size;

  /* Where an encoder delivers its packets. */
  int (*write_packet)(lsx_codec_t * codec, lsx_packet_t const * packet);
};

/* How a container names a codec. */
typedef struct {
  lsx_container_family_t container;
  char const * id;         /* the container's own codec identifier */
  char const * codec;      /* codec name */
  sox_encoding_t encoding; /* sample layout the identifier implies */
  lsx_endian_t endian;
} lsx_codec_tag_t;

lsx_codec_handler_t const * lsx_codec_find(char const * name);
char const * lsx_codec_names(void);
char const * lsx_type_codec(char const * type);
char const * lsx_container_codec(char const * type);
char const * lsx_container_requested(char const * type);
sox_bool lsx_container_takes_codec(char const * type);
lsx_container_family_t lsx_container_family(char const * type);
lsx_codec_tag_t const * lsx_codec_tag_by_id(lsx_container_family_t container, char const * id);
lsx_codec_tag_t const * lsx_codec_tag_by_magic(lsx_container_family_t container,
    unsigned char const * data, size_t size);
lsx_codec_tag_t const * lsx_codec_tag_by_codec(lsx_container_family_t container,
    char const * codec, sox_encoding_t encoding, sox_bool big_endian);
sox_sample_t * lsx_codec_output(lsx_codec_t * codec, size_t len);

/* Row n serves n channels and holds, for each channel in Vorbis order, the
 * index of the same channel in SoX's, which is WAVE's. */
extern unsigned char const lsx_vorbis_channel_from_sox[9][8];

lsx_codec_handler_t const * lsx_pcm_codec_fn(void);
lsx_codec_handler_t const * lsx_flac_codec_fn(void);
lsx_codec_handler_t const * lsx_vorbis_codec_fn(void);
lsx_codec_handler_t const * lsx_opus_codec_fn(void);
lsx_codec_handler_t const * lsx_mp3_codec_fn(void);
lsx_codec_handler_t const * lsx_wavpack_codec_fn(void);

#endif
