/* Common libavcodec adapter for SoX format handlers.
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

#ifndef LSX_FFMPEG_CODEC_H
#define LSX_FFMPEG_CODEC_H

#include "sox.h"

#include <libavcodec/avcodec.h>

#include <stddef.h>
#include <stdint.h>

/* Bit cursors over a buffer the caller owns, most significant bit first.
 * The handlers around these codecs all have to take apart or assemble a
 * transport header FFmpeg does not deal with itself -- ADTS, LOAS/LATM, an
 * AudioSpecificConfig -- and they all read it in the same bit order, so the
 * plumbing lives here once instead of in each of them.
 *
 * data is not owned; size_bits is its length in bits, not bytes; position is
 * the cursor, also in bits, and only ever advances.  A writer expects its
 * buffer to start zeroed: lsx_bit_write sets the one bits and leaves the zero
 * bits untouched. */
typedef struct {
  uint8_t const * data;
  size_t size_bits;
  size_t position;
} lsx_bit_reader_t;

typedef struct {
  uint8_t * data;
  size_t size_bits;
  size_t position;
} lsx_bit_writer_t;

/* Read count bits (at most 32) into *value, which may be NULL to skip them.
 * Returns SOX_EOF without moving the cursor if the field does not fit. */
int lsx_bit_read(lsx_bit_reader_t * reader, unsigned count, uint32_t * value);

/* Write the low count bits (at most 32) of value.  Returns SOX_EOF without
 * moving the cursor if the field does not fit. */
int lsx_bit_write(lsx_bit_writer_t * writer, unsigned count, uint32_t value);

/* Opaque per-file codec state.  Owned by the adapter, created by one of the
 * start functions below and destroyed by the matching stop function, which
 * also clears the caller's pointer. */
typedef struct lsx_ffmpeg_codec_t lsx_ffmpeg_codec_t;

/* Hooks a codec or a format handler may supply.  All of them are optional; a
 * NULL hook means "use the adapter's default behaviour".  None of them take
 * ownership of what they are handed, and the pointers stay valid only for the
 * duration of the call.  Unless noted, they return SOX_SUCCESS or SOX_EOF and
 * are expected to have reported their own failure through lsx_fail_errno. */

/* Called once before the decoder is opened, on a context that has been
 * allocated but not yet configured.  Its job is to hand the decoder anything
 * that cannot be recovered from the bitstream itself -- typically extradata
 * built from the stream's own configuration record. */
typedef int (*lsx_ffmpeg_codec_decoder_preparer_t)(sox_format_t * ft, AVCodecContext * context);

/* Called once after rate, layout, sample format and bit rate have been set on
 * the context but before it is opened, for codec-specific knobs.  Returns a
 * negative AVERROR rather than SOX_EOF, since the adapter turns the value
 * into a message itself. */
typedef int (*lsx_ffmpeg_codec_encoder_preparer_t)(AVCodecContext * context);

/* Replaces the adapter's own read-and-parse loop for handlers whose framing
 * comes from the file rather than from an FFmpeg bitstream parser.  Fills
 * packet with one frame's payload and returns 1, returns 0 at end of stream,
 * or SOX_EOF on error.  When it is supplied no AVCodecParserContext is
 * created at all.  Ownership of the packet payload passes to the adapter,
 * which unrefs it after submitting it. */
typedef int (*lsx_ffmpeg_codec_packet_reader_t)(sox_format_t * ft, AVPacket * packet);

/* Replaces the plain "write the packet payload" step on the encode side, for
 * handlers that have to wrap each packet in a transport header.  The packet
 * is still owned by the adapter and is unreffed after the call. */
typedef int (*lsx_ffmpeg_codec_packet_writer_t)(
    sox_format_t * ft,
    AVCodecContext const * context,
    AVPacket const * packet);

/* Where an encoder opened for a container delivers its packets, with the
 * opaque pointer it was opened with.  Same contract as a packet writer. */
typedef int (*lsx_ffmpeg_codec_packet_sink_t)(
    void * opaque,
    AVCodecContext const * context,
    AVPacket const * packet);

/* Maps a channel count to the layout this codec expects for it, overriding
 * the adapter's canonical mono..7.1 table.  Follows the libavutil convention:
 * 0 on success with *layout initialised, a negative AVERROR otherwise.  The
 * layouts it produces must not need av_channel_layout_uninit. */
typedef int (*lsx_ffmpeg_codec_layout_selector_t)(unsigned channels, AVChannelLayout * layout);

/* Everything that distinguishes one codec from another, however its packets
 * are framed: an elementary stream handler and a container use the same
 * definition for the same codec.  Static const data; the adapter stores the
 * pointer, so it must outlive the file. */
typedef struct lsx_ffmpeg_codec_definition {
  enum AVCodecID codec_id;      /* Which libavcodec encoder/decoder to look up. */
  sox_encoding_t encoding;      /* Reported back through ft->encoding.encoding. */
  char const * name;            /* Human-readable codec name, used in messages. */

  /* Decode side. */
  unsigned max_decode_channels; /* Refuse streams wider than this. */
  sox_bool accept_unspecified_decode_layout; /* Allow >6 channels with no speaker layout. */

  /* Encode side. */
  unsigned max_encode_channels;

  /* Bits per sample the format carries.  Zero means "ask the decoder"; a
   * non-zero value is both the reported precision and the signal that this
   * is a lossy format, for which bits_per_sample is left at 0. */
  unsigned precision;

  /* Bit rate in bit/s.  A zero minimum or maximum disables that end of the
   * range check.  Ignored entirely when use_compression_level is set. */
  int64_t default_bit_rate;
  int64_t minimum_bit_rate;
  int64_t maximum_bit_rate;

  /* If the decoder reports this profile, warn once that the extra metadata
   * it carries is dropped.  ignored_metadata_name NULL disables the warning. */
  int ignored_metadata_profile;
  char const * ignored_metadata_name;

  /* Reject input whose profile is not this one, for definitions that sit on
   * a shared codec but only claim one of its profiles.  AV_PROFILE_UNKNOWN
   * accepts anything. */
  int required_decode_profile;

  /* The libavcodec major version the decoder first appeared in, checked at
   * run time; 0 for any the build accepts. */
  unsigned minimum_decoder_version;

  lsx_ffmpeg_codec_encoder_preparer_t prepare_encoder;

  /* Checks of the output format beyond what the encoder makes itself, run
   * before it is opened; NULL for none. */
  int (*check_encoder)(sox_format_t * ft, struct lsx_ffmpeg_codec_definition const * definition);

  /* The layout a channel count is encoded as when --channel-layout is not
   * given, where the codec's own differs from the canonical one and is not
   * the only one it decodes; NULL or a NULL answer for the canonical one. */
  char const * (*encode_layout)(unsigned channels);

  /* Says, once the encoder is open, in which order the channels went in,
   * where that is not the order SoX assumes; NULL for nothing to say. */
  void (*warn_encoded_layout)(sox_format_t * ft);

  /* Whether the depth the stream was mastered at, which the decoder reports,
   * is the bits per sample to report, as for DTS; 16 when it is none of 16,
   * 20 and 24. */
  sox_bool decoded_depth;

  /* When set, -C selects an integer compression level in the range below
   * instead of a bit rate in kbit/s. */
  sox_bool use_compression_level;
  int default_compression_level;
  int minimum_compression_level;
  int maximum_compression_level;

  lsx_ffmpeg_codec_layout_selector_t select_layout;
} lsx_ffmpeg_codec_definition_t;

/* The codecs both kinds of format share. */
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_aac_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_usac_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_alac_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_ac3_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_eac3_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_dts_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_mlp_definition;
extern lsx_ffmpeg_codec_definition_t const lsx_ffmpeg_truehd_definition;

/* A codec as an elementary stream handler frames it in a file of its own. */
typedef struct {
  lsx_ffmpeg_codec_definition_t const * codec;
  lsx_ffmpeg_codec_decoder_preparer_t prepare_decoder;
  lsx_ffmpeg_codec_packet_reader_t packet_reader;
  lsx_ffmpeg_codec_packet_writer_t packet_writer;
} lsx_ffmpeg_stream_definition_t;

/* Open definition's decoder on ft and decode far enough to learn the stream's
 * rate, channel count and precision, which are written back into ft->signal
 * and ft->encoding.  On success *state holds the new codec state; on failure
 * it is left NULL and nothing needs freeing.  definition must outlive *state. */
int lsx_ffmpeg_codec_startread(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_stream_definition_t const * definition);

/* Fill samples with up to length interleaved sox samples, decoding as needed.
 * Returns the number written, which is short only at end of stream or on a
 * decode error already reported through ft. */
size_t lsx_ffmpeg_codec_read(sox_format_t * ft, lsx_ffmpeg_codec_t * state, sox_sample_t * samples, size_t length);

/* Release the state, of either side, and set *state to NULL.  Safe on a NULL
 * state. */
int lsx_ffmpeg_codec_stopread(lsx_ffmpeg_codec_t ** state);

/* Configure and open definition's encoder from ft->signal and ft->encoding,
 * validating rate, channel layout and bit rate or compression level against
 * what the encoder actually supports before committing.  Ownership rules
 * match lsx_ffmpeg_codec_startread. */
int lsx_ffmpeg_codec_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_stream_definition_t const * definition);

/* Buffer length interleaved sox samples and encode whole frames as they fill.
 * Returns the number consumed; a short return means encoding failed, and the
 * state latches that failure so later calls return 0 rather than retry. */
size_t lsx_ffmpeg_codec_write(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t const * samples,
    size_t length);

/* Encode whatever is still buffered, drain the encoder, then release the
 * state and set *state to NULL.  Must be called even after a write failure,
 * which it reports as SOX_EOF without attempting to flush. */
int lsx_ffmpeg_codec_stopwrite(sox_format_t * ft, lsx_ffmpeg_codec_t ** state);

/* Packet by packet, for a container that does its own framing.  The decoder
 * is opened with extradata from the container, which may be NULL.  A packet
 * is sent, NULL at the end of the stream, and then the decoded samples are
 * received until there are no more: 1 with *samples and *length (in samples,
 * all channels) valid until the next call, 0 when the decoder wants more
 * input or has been drained, SOX_EOF on an error.  The rate and channel count
 * are those of the frames received, and are fixed by the first one. */
int lsx_ffmpeg_codec_open_decoder(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition,
    uint8_t const * extradata,
    size_t extradata_size);
int lsx_ffmpeg_codec_send_packet(sox_format_t * ft, lsx_ffmpeg_codec_t * state, uint8_t const * data, size_t size);
int lsx_ffmpeg_codec_receive_samples(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t const ** samples,
    size_t * length);

/* An encoder whose packets go to sink rather than to the file, as
 * lsx_ffmpeg_codec_startwrite otherwise; lsx_ffmpeg_codec_write feeds it and
 * lsx_ffmpeg_codec_flush ends the stream.  Released by stopread. */
int lsx_ffmpeg_codec_open_encoder(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition,
    lsx_ffmpeg_codec_packet_sink_t sink,
    void * opaque);
int lsx_ffmpeg_codec_flush(sox_format_t * ft, lsx_ffmpeg_codec_t * state);

/* The open codec context, for handlers that need to read what the codec
 * negotiated (extradata, frame size).  Owned by state; valid until the
 * matching stop call.  NULL state yields NULL. */
AVCodecContext const * lsx_ffmpeg_codec_context(lsx_ffmpeg_codec_t const * state);

/* Print the channel layouts --help-format should advertise for the named SoX
 * format.  Does nothing for a format with no FFmpeg encoder behind it. */
void lsx_ffmpeg_codec_print_format_layouts(char const * format_name);

#endif
