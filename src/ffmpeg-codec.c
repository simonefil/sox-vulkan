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

#include "sox_i.h"
#include "ffmpeg-codec.h"

#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int lsx_bit_read(lsx_bit_reader_t * reader, unsigned count, uint32_t * value)
{
  uint32_t result = 0;
  unsigned i;

  if (count > 32 || reader->position > reader->size_bits || count > reader->size_bits - reader->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = reader->position++;

    result = (result << 1) | ((reader->data[position / 8] >> (7 - position % 8)) & 1);
  }
  if (value != NULL)
    *value = result;
  return SOX_SUCCESS;
}

int lsx_bit_write(lsx_bit_writer_t * writer, unsigned count, uint32_t value)
{
  unsigned i;

  if (count > 32 || writer->position > writer->size_bits || count > writer->size_bits - writer->position)
    return SOX_EOF;
  for (i = 0; i < count; ++i) {
    size_t position = writer->position++;
    uint32_t bit = (value >> (count - i - 1)) & 1;

    if (bit)
      writer->data[position / 8] |= (uint8_t)(1U << (7 - position % 8));
  }
  return SOX_SUCCESS;
}

/* How much compressed input the parser is fed at a time.  FFmpeg requires the
 * padding past the end so that its bitstream readers may over-read safely. */
#define INPUT_BUFFER_SIZE 16384

/* One open codec.  A given state is used for reading or for writing, never
 * both, so the two groups of fields below are mutually exclusive in practice;
 * they share a struct because everything above them is common. */
struct lsx_ffmpeg_codec_t {
  lsx_ffmpeg_codec_definition_t const * definition; /* Not owned; caller-provided static data. */
  AVCodec const * codec;        /* Not owned; libavcodec's own registry entry. */
  AVCodecContext * context;
  AVCodecParserContext * parser; /* NULL when the definition reads packets itself. */
  AVPacket * packet;            /* Reused; unreffed after each use, freed once. */
  AVFrame * frame;              /* Reused; on the encode side it owns its buffers. */

  /* Read side: compressed bytes on their way into the parser.  input_offset
   * is how much of input_size the parser has consumed.  The three eof flags
   * are the stages of the drain -- file exhausted, parser drained, decoder
   * drained -- and only ever move forward. */
  uint8_t input[INPUT_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
  size_t input_offset;
  size_t input_size;
  sox_bool input_eof;
  sox_bool parser_eof;
  sox_bool parser_zero_progress; /* Set on a no-progress parse; a second one is fatal. */
  sox_bool decoder_flushed;      /* A NULL packet has been sent. */
  sox_bool decoder_eof;

  /* Read side: one decoded frame converted to interleaved sox samples.
   * decoded_offset..decoded_size is what the caller has not taken yet.
   * decoded_channels and decoded_rate are latched from the first frame and
   * every later frame must match them. */
  sox_sample_t * decoded;
  size_t decoded_capacity;      /* In samples, not bytes; grows, never shrinks. */
  size_t decoded_offset;
  size_t decoded_size;
  unsigned decoded_channels;
  int decoded_rate;
  sox_bool ignored_metadata_warning_shown; /* Both warnings are once per file. */
  sox_bool layout_warning_shown;

  /* Write side: interleaved sox samples accumulating towards one full codec
   * frame.  pending_capacity is frame_samples * channels, and the buffer is
   * handed to the encoder and reset every time it fills. */
  sox_sample_t * pending;
  size_t pending_capacity;
  size_t pending_size;
  int frame_samples;            /* Samples per channel per codec frame. */
  int64_t next_pts;             /* In samples; the time base is 1/rate. */
  sox_bool write_failed;        /* Latched, so a failed write is reported once. */
};

/* Report an FFmpeg failure as a SoX error, turning the AVERROR into text.
 * Always returns SOX_EOF, so callers can `return fail_av(...)'. */
static int fail_av(sox_format_t * ft, int sox_error, char const * operation, int av_error)
{
  char message[AV_ERROR_MAX_STRING_SIZE];

  if (av_strerror(av_error, message, sizeof(message)) < 0)
    strcpy(message, "unknown FFmpeg error");
  lsx_fail_errno(ft, sox_error, "%s: %s", operation, message);
  return SOX_EOF;
}

/* Options SoX derives from the signal and encoding it was given.  Letting
 * --ffmpeg-opts set them too would silently disagree with what SoX has told
 * the rest of the chain, so they are refused rather than overridden. */
static sox_bool is_reserved_codec_option(char const * key)
{
  static char const * const reserved[] = {
    "b", "ab", "bit_rate",
    "ar", "sample_rate",
    "ac", "channels", "channel_layout", "ch_layout", "channel_order",
    "downmix",
    "sample_fmt", "request_sample_fmt", "time_base",
    "compression_level",
    NULL
  };
  size_t i;

  for (i = 0; reserved[i] != NULL; ++i)
    if (!strcmp(key, reserved[i]))
      return sox_true;
  return sox_false;
}

/* Open state->context, applying --ffmpeg-opts on the way in.  avcodec_open2
 * consumes the options it recognises and leaves the rest in the dictionary,
 * so anything still there afterwards is a user typo worth failing on rather
 * than ignoring silently. */
static int open_codec(sox_format_t * ft, lsx_ffmpeg_codec_t * state, sox_bool encoding)
{
  AVDictionary * options = NULL;
  AVDictionaryEntry const * entry;
  int result;

  if (ft->codec_options != NULL) {
    result = av_dict_parse_string(&options, ft->codec_options, "=", ":", 0);
    if (result < 0) {
      av_dict_free(&options);
      return fail_av(ft, SOX_EINVAL, "Unable to parse --ffmpeg-opts (expected key=value:key=value)", result);
    }

    entry = NULL;
    while ((entry = av_dict_iterate(options, entry)) != NULL)
      if (is_reserved_codec_option(entry->key)) {
        lsx_fail_errno(ft, SOX_EINVAL,
            "FFmpeg option `%s' is controlled by SoX and cannot be passed "
            "through --ffmpeg-opts", entry->key);
        av_dict_free(&options);
        return SOX_EOF;
      }
  }

  result = avcodec_open2(state->context, state->codec, options != NULL ? &options : NULL);
  if (result < 0) {
    av_dict_free(&options);
    return fail_av(ft, SOX_EFMT, encoding ? "Unable to open FFmpeg encoder" : "Unable to open FFmpeg decoder", result);
  }

  entry = av_dict_iterate(options, NULL);
  if (entry != NULL) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "FFmpeg option `%s=%s' is unknown or unsupported by the %s %s",
        entry->key, entry->value, state->definition->name,
        encoding ? "encoder" : "decoder");
    av_dict_free(&options);
    return SOX_EOF;
  }

  av_dict_free(&options);
  return SOX_SUCCESS;
}

/* Free everything the state owns and clear the caller's pointer.  Tolerates a
 * partly built state, so the failure paths in allocate_state and the two
 * start functions can all unwind through it. */
static void destroy_state(lsx_ffmpeg_codec_t ** state)
{
  lsx_ffmpeg_codec_t * p;

  if (state == NULL || *state == NULL)
    return;
  p = *state;
  if (p->parser != NULL)
    av_parser_close(p->parser);
  av_packet_free(&p->packet);
  av_frame_free(&p->frame);
  avcodec_free_context(&p->context);
  free(p->decoded);
  free(p->pending);
  free(p);
  *state = NULL;
}

/* Look the codec up and allocate the context, packet and frame every path
 * needs.  On failure nothing is left allocated and *state is untouched. */
static int allocate_state(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition,
    sox_bool encoding)
{
  lsx_ffmpeg_codec_t * p = lsx_calloc(1, sizeof(*p));

  p->definition = definition;
  p->codec = encoding ? avcodec_find_encoder(definition->codec_id) : avcodec_find_decoder(definition->codec_id);
  if (p->codec == NULL) {
    lsx_fail_errno(ft, SOX_EFMT, "FFmpeg %s for %s is unavailable", encoding ? "encoder" : "decoder", definition->name);
    destroy_state(&p);
    return SOX_EOF;
  }

  p->context = avcodec_alloc_context3(p->codec);
  p->packet = av_packet_alloc();
  p->frame = av_frame_alloc();
  if (p->context == NULL || p->packet == NULL || p->frame == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Unable to allocate FFmpeg codec state");
    destroy_state(&p);
    return SOX_EOF;
  }

  *state = p;
  return SOX_SUCCESS;
}

/* The layout SoX assumes for a bare channel count, matching the order the
 * rest of SoX uses for multichannel files.  Follows the libavutil convention:
 * 0 on success, negative AVERROR otherwise.  The native layouts it returns
 * hold no allocation, so they need no av_channel_layout_uninit. */
static int canonical_layout(unsigned channels, AVChannelLayout * layout)
{
  switch (channels) {
    case 1:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
      return 0;
    case 2:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
      return 0;
    case 3:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_SURROUND;
      return 0;
    case 4:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD;
      return 0;
    case 5:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0_BACK;
      return 0;
    case 6:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK;
      return 0;
    case 7:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_6POINT1;
      return 0;
    case 8:
      *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1;
      return 0;
    default:
      return AVERROR(EINVAL);
  }
}

/* canonical_layout unless the definition overrides it. */
static int select_layout(lsx_ffmpeg_codec_definition_t const * definition, unsigned channels, AVChannelLayout * layout)
{
  return definition->select_layout != NULL ?
      definition->select_layout(channels, layout) :
      canonical_layout(channels, layout);
}

/* Fallback list of layouts for codecs that do not publish a layout list of
 * their own.  FFmpeg's native AAC encoder is the only such codec here: it
 * accepts everything its PCE writer can describe but advertises nothing, so
 * without this table --help-format would print an empty list and any layout
 * check would have to pass everything through. */
static char const * const aac_layout_names[] = {
  "mono",
  "stereo",
  "2.1",
  "3.0",
  "3.0(back)",
  "3.1",
  "quad",
  "quad(side)",
  "4.0",
  "4.1",
  "5.0",
  "5.0(side)",
  "5.1",
  "5.1(side)",
  "6.0",
  "6.0(front)",
  "hexagonal",
  "6.1",
  "6.1(back)",
  "6.1(front)",
  "7.0",
  "7.0(front)",
  "7.1",
  "7.1(wide)",
  "7.1(wide-side)",
  "octagonal",
  NULL
};

static char const * const * codec_layout_names(enum AVCodecID codec_id)
{
  return codec_id == AV_CODEC_ID_AAC ? aac_layout_names : NULL;
}

/* Map a SoX format name, including its aliases, to the codec and channel
 * limit its handler encodes with.  Kept separate from the handler tables so
 * that --help-format can answer without opening a file.  Returns 0 for a
 * format with no FFmpeg encoder behind it. */
static int format_encoder(char const * format_name, enum AVCodecID * codec_id, unsigned * max_channels)
{
  if (!strcmp(format_name, "aac") ||
      !strcmp(format_name, "adts") ||
      !strcmp(format_name, "latm") ||
      !strcmp(format_name, "loas")) {
    *codec_id = AV_CODEC_ID_AAC;
    *max_channels = 8;
    return 1;
  }
  if (!strcmp(format_name, "ac3")) {
    *codec_id = AV_CODEC_ID_AC3;
    *max_channels = 6;
    return 1;
  }
  if (!strcmp(format_name, "eac3") || !strcmp(format_name, "ec3")) {
    *codec_id = AV_CODEC_ID_EAC3;
    *max_channels = 6;
    return 1;
  }
  if (!strcmp(format_name, "dts")) {
    *codec_id = AV_CODEC_ID_DTS;
    *max_channels = 6;
    return 1;
  }
  if (!strcmp(format_name, "mlp")) {
    *codec_id = AV_CODEC_ID_MLP;
    *max_channels = 6;
    return 1;
  }
  if (!strcmp(format_name, "truehd") || !strcmp(format_name, "thd")) {
    *codec_id = AV_CODEC_ID_TRUEHD;
    *max_channels = 6;
    return 1;
  }
  if (!strcmp(format_name, "m4a")) {
    *codec_id = AV_CODEC_ID_ALAC;
    *max_channels = 8;
    return 1;
  }
  return 0;
}

/* Print the speaker names in the order the codec expects the channels, which
 * is what a caller needs in order to line its own channels up. */
static void print_channel_order(AVChannelLayout const * layout)
{
  int i;

  for (i = 0; i < layout->nb_channels; ++i) {
    enum AVChannel channel = av_channel_layout_channel_from_index(layout, (unsigned)i);
    char name[16];

    if (av_channel_name(name, sizeof(name), channel) < 0)
      strcpy(name, "?");
    printf("%s%s", i ? " " : "", name);
  }
}

/* One --help-format line: the layout name to pass to --channel-layout, then
 * the channel order it implies. */
static void print_layout(AVChannelLayout const * layout)
{
  char description[128];

  if (av_channel_layout_describe(layout, description, sizeof(description)) < 0)
    strcpy(description, "unknown");
  printf("  %-18s ", description);
  print_channel_order(layout);
  putchar('\n');
}

void lsx_ffmpeg_codec_print_format_layouts(char const * format_name)
{
  enum AVCodecID codec_id;
  AVCodec const * codec;
  void const * configurations;
  AVChannelLayout const * layouts;
  char const * const * layout_names;
  unsigned max_channels;
  int count;
  int i;
  int result;

  if (!format_encoder(format_name, &codec_id, &max_channels))
    return;
  codec = avcodec_find_encoder(codec_id);
  if (codec == NULL)
    return;
  result = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configurations, &count);
  if (result < 0)
    return;

  puts("Output channel layouts (--channel-layout LAYOUT):");
  /* An encoder that publishes no layout list accepts anything it can
   * describe, so fall back to the table above; codec_layout_names returning
   * NULL means we have nothing useful to say and print nothing. */
  if (configurations == NULL || count <= 0) {
    layout_names = codec_layout_names(codec_id);
    if (layout_names == NULL)
      return;
    while (*layout_names != NULL) {
      AVChannelLayout layout = {0};

      if (av_channel_layout_from_string(
            &layout, *layout_names) >= 0 &&
          layout.nb_channels >= 1 &&
          (unsigned)layout.nb_channels <= max_channels)
        print_layout(&layout);
      av_channel_layout_uninit(&layout);
      ++layout_names;
    }
    return;
  }

  layouts = configurations;
  for (i = 0; i < count; ++i) {
    if (layouts[i].nb_channels < 1 || (unsigned)layouts[i].nb_channels > max_channels)
      continue;
    print_layout(&layouts[i]);
  }
}

/* Whether a layout the decoder reported is one this handler will pass on.  A
 * definition with its own selector accepts only the layout that selector
 * would have chosen for that channel count.  Otherwise anything with real
 * speaker positions is fine, and an unspecified layout is accepted up to 6
 * channels -- beyond that the channel order is too likely to be wrong to
 * guess at, unless the definition says the format's order is fixed anyway. */
static sox_bool is_supported_layout(AVChannelLayout const * layout, lsx_ffmpeg_codec_definition_t const * definition)
{
  AVChannelLayout expected = {0};

  if (!av_channel_layout_check(layout) || layout->nb_channels < 1)
    return sox_false;
  if (definition->select_layout != NULL)
    return select_layout(definition,
        (unsigned)layout->nb_channels, &expected) >= 0 &&
        av_channel_layout_compare(layout, &expected) == 0;
  if (layout->order == AV_CHANNEL_ORDER_UNSPEC)
    return layout->nb_channels >= 1 && (layout->nb_channels <= 6 || definition->accept_unspecified_decode_layout);
  return sox_true;
}

/* Warn once when the decoded layout is not the one SoX would assume for that
 * channel count.  SoX carries no channel map, so the samples are handed on in
 * decoder order and it is the user who has to account for the difference; the
 * canonical layouts pass silently because for those the two orders agree. */
static void warn_decoded_layout(sox_format_t * ft, lsx_ffmpeg_codec_t * state, AVChannelLayout const * layout)
{
  AVChannelLayout canonical = {0};
  char description[128];

  if (state->layout_warning_shown)
    return;
  if (layout->order == AV_CHANNEL_ORDER_UNSPEC) {
    lsx_warn("`%s': the %s decoder did not identify the %d-channel "
        "speaker layout; channel samples will be preserved in decoder "
        "order without remixing",
        ft->filename, state->definition->name, layout->nb_channels);
    state->layout_warning_shown = sox_true;
    return;
  }
  if (canonical_layout(
        (unsigned)layout->nb_channels, &canonical) < 0 ||
      av_channel_layout_compare(layout, &canonical) == 0)
    return;
  if (av_channel_layout_describe(layout, description, sizeof(description)) < 0)
    strcpy(description, "unknown");
  lsx_warn("`%s': decoding %s channel layout `%s' without remixing; "
      "channel samples remain in FFmpeg decoder order",
      ft->filename, state->definition->name, description);
  state->layout_warning_shown = sox_true;
}

/* Check a freshly decoded frame against what the handler supports and against
 * the first frame, then latch its rate and channel count.  SoX fixes both for
 * the whole file at startread time, so a mid-stream change cannot be honoured
 * and is refused instead of being silently misinterpreted. */
static int validate_decoded_frame(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  AVChannelLayout const * layout = &state->frame->ch_layout;
  /* Some decoders leave the rate on the context rather than on each frame. */
  int rate = state->frame->sample_rate ? state->frame->sample_rate : state->context->sample_rate;
  char description[128];

  if (state->definition->required_decode_profile != AV_PROFILE_UNKNOWN &&
      state->context->profile !=
          state->definition->required_decode_profile) {
    lsx_fail_errno(ft, SOX_EFMT, "The input is not %s audio", state->definition->name);
    return SOX_EOF;
  }
  if (layout->nb_channels == 0)
    layout = &state->context->ch_layout;
  if (!is_supported_layout(layout, state->definition) ||
      (unsigned)layout->nb_channels >
          state->definition->max_decode_channels) {
    if (av_channel_layout_describe(layout, description, sizeof(description)) < 0)
      strcpy(description, "unknown");
    lsx_fail_errno(ft, SOX_EFMT, "Unsupported %s channel layout: %s", state->definition->name, description);
    return SOX_EOF;
  }
  if (rate <= 0) {
    lsx_fail_errno(ft, SOX_EHDR, "Unable to determine %s sample rate", state->definition->name);
    return SOX_EOF;
  }
  if (state->decoded_channels &&
      (state->decoded_channels != (unsigned)layout->nb_channels ||
       state->decoded_rate != rate)) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s streams that change sample rate or channels are unsupported",
        state->definition->name);
    return SOX_EOF;
  }

  state->decoded_channels = (unsigned)layout->nb_channels;
  state->decoded_rate = rate;
  warn_decoded_layout(ft, state, layout);
  return SOX_SUCCESS;
}

/* Warn once for the formats that carry an object or spatial layer on top of a
 * channel bed.  FFmpeg decodes the bed only, which is a correct result but
 * not the whole file, so say so rather than let it pass for the full mix.
 * Deferred until after the first frame, since the profile is not reliably
 * known before then. */
static void warn_ignored_metadata(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  if (!state->ignored_metadata_warning_shown &&
      state->definition->ignored_metadata_name != NULL &&
      state->context->profile ==
          state->definition->ignored_metadata_profile) {
    lsx_warn("`%s': %s detected; spatial metadata will be ignored and "
        "only the channel-based audio presentation will be decoded",
        ft->filename, state->definition->ignored_metadata_name);
    state->ignored_metadata_warning_shown = sox_true;
  }
}

/* Address of one sample in a decoded frame, hiding the planar/interleaved
 * split: planar formats keep each channel in its own extended_data plane,
 * packed formats put everything in plane 0.  channels is passed in because a
 * frame that reported no layout is described by the context instead. */
static void const * decoded_sample_address(AVFrame const * frame, int sample, int channel, unsigned channels)
{
  enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  int planar = av_sample_fmt_is_planar(format);
  int bytes = av_get_bytes_per_sample(format);
  size_t index = planar ? (size_t)sample :
      (size_t)sample * channels + channel;

  return frame->extended_data[planar ? channel : 0] + index * bytes;
}

/* Convert one decoded sample to SoX's 32-bit signed scale.  The integer
 * formats are exact shifts, so they cannot clip.  The float formats can:
 * they are scaled by 2^31 and rounded to nearest, and a value outside the
 * representable range is clamped and counted in ft->clips.  Full-scale
 * negative is deliberately not a clip -- -1.0 maps exactly onto INT32_MIN --
 * whereas +1.0 has no exact image and is clamped without counting, so that a
 * plain full-scale signal does not report clipping on every peak.
 * Unsupported formats are filtered out beforehand by supported_sample_format;
 * the default arm exists only to keep the switch total. */
static sox_sample_t decoded_sample_to_sox(
    sox_format_t * ft,
    AVFrame const * frame,
    int sample,
    int channel,
    unsigned channels)
{
  enum AVSampleFormat format = av_get_packed_sample_fmt((enum AVSampleFormat)frame->format);
  void const * source = decoded_sample_address(frame, sample, channel, channels);

  switch (format) {
    case AV_SAMPLE_FMT_U8:
      return (sox_sample_t)
          (((int32_t)*(uint8_t const *)source - 128) * INT32_C(16777216));
    case AV_SAMPLE_FMT_S16:
      return (sox_sample_t)
          ((int32_t)*(int16_t const *)source * INT32_C(65536));
    case AV_SAMPLE_FMT_S32:
      return (sox_sample_t)*(int32_t const *)source;
    case AV_SAMPLE_FMT_S64:
      return (sox_sample_t)(int32_t)
          ((uint64_t)*(int64_t const *)source >> 32);
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_DBL: {
      double value = format == AV_SAMPLE_FMT_FLT ? *(float const *)source : *(double const *)source;
      double scaled = value * 2147483648.;

      if (scaled <= -2147483648.5) {
        ++ft->clips;
        return INT32_MIN;
      }
      if (scaled >= 2147483647.5) {
        if (scaled > 2147483648.)
          ++ft->clips;
        return INT32_MAX;
      }
      return (sox_sample_t)(scaled < 0 ? scaled - .5 : scaled + .5);
    }
    default:
      return 0;
  }
}

/* The PCM formats decoded_sample_to_sox knows how to read, in either their
 * planar or their packed spelling. */
static sox_bool supported_sample_format(enum AVSampleFormat format)
{
  switch (av_get_packed_sample_fmt(format)) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_DBL:
      return sox_true;
    default:
      return sox_false;
  }
}

/* Interleave a decoded frame into the state's sox-sample buffer, replacing
 * whatever was there; the caller has already consumed it.  The buffer only
 * ever grows, so a stream of equal-sized frames allocates once. */
static int store_decoded_frame(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  size_t required;
  int sample;
  int channel;

  if (validate_decoded_frame(ft, state) != SOX_SUCCESS)
    return SOX_EOF;
  if (!supported_sample_format((enum AVSampleFormat)state->frame->format)) {
    lsx_fail_errno(ft, SOX_EFMT, "Unsupported FFmpeg PCM sample format: %s",
        av_get_sample_fmt_name((enum AVSampleFormat)state->frame->format));
    return SOX_EOF;
  }

  required = (size_t)state->frame->nb_samples * state->decoded_channels;
  if (state->decoded_capacity < required) {
    state->decoded = lsx_realloc(state->decoded, required * sizeof(*state->decoded));
    state->decoded_capacity = required;
  }

  for (sample = 0; sample < state->frame->nb_samples; ++sample)
    for (channel = 0; channel < (int)state->decoded_channels; ++channel)
      state->decoded[(size_t)sample * state->decoded_channels + channel] =
          decoded_sample_to_sox(ft, state->frame, sample, channel,
              state->decoded_channels);
  state->decoded_offset = 0;
  state->decoded_size = required;
  return SOX_SUCCESS;
}

/* Produce the next compressed packet, either from the handler's own framing
 * or by feeding file bytes through the FFmpeg bitstream parser.  Returns 1
 * with state->packet filled, 0 once both the file and the parser are drained,
 * or SOX_EOF on error.
 *
 * The loop iterates because a parse call can legitimately produce no packet:
 * the parser may be accumulating a frame, or the refill may have been empty.
 * Once the file is exhausted the parser is called once with a NULL buffer to
 * flush whatever it still holds. */
static int read_parsed_packet(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  if (state->definition->packet_reader != NULL)
    return state->definition->packet_reader(ft, state->packet);

  for (;;) {
    uint8_t * packet_data = NULL;
    int packet_size = 0;
    int consumed;
    int result;

    if (state->input_offset == state->input_size && !state->input_eof) {
      state->input_size = lsx_readbuf(ft, state->input, INPUT_BUFFER_SIZE);
      state->input_offset = 0;
      memset(state->input + state->input_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
      if (state->input_size == 0)
        state->input_eof = sox_true;
    }

    if (state->input_eof && state->input_offset == state->input_size) {
      if (state->parser_eof)
        return 0;
      consumed = av_parser_parse2(state->parser, state->context,
          &packet_data, &packet_size, NULL, 0,
          AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
      if (consumed < 0)
        return fail_av(ft, SOX_EHDR, "Unable to flush FFmpeg bitstream parser", consumed);
      if (packet_size == 0) {
        state->parser_eof = sox_true;
        return 0;
      }
    }
    else {
      size_t available = state->input_size - state->input_offset;

      consumed = av_parser_parse2(state->parser, state->context,
          &packet_data, &packet_size,
          state->input + state->input_offset, (int)available,
          AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
      if (consumed < 0)
        return fail_av(ft, SOX_EHDR, "Unable to parse compressed audio", consumed);
      state->input_offset += (size_t)consumed;
      /* A parse that consumes nothing and emits nothing is retried once and
       * then treated as fatal.  Without the guard the loop would call the
       * parser forever on the same bytes: the buffer is only refilled once
       * it has been fully consumed, so nothing about the next call would
       * differ.  Failing here turns that hang into an error. */
      if (consumed == 0 && packet_size == 0) {
        if (!state->parser_zero_progress) {
          state->parser_zero_progress = sox_true;
          continue;
        }
        lsx_fail_errno(ft, SOX_EHDR, "FFmpeg bitstream parser made no progress");
        return SOX_EOF;
      }
    }

    state->parser_zero_progress = sox_false;
    if (packet_size == 0)
      continue;
    result = av_new_packet(state->packet, packet_size);
    if (result < 0)
      return fail_av(ft, SOX_ENOMEM, "Unable to allocate compressed audio packet", result);
    memcpy(state->packet->data, packet_data, (size_t)packet_size);
    return 1;
  }
}

/* Run the libavcodec receive/send cycle until one frame comes out.  Returns 1
 * with the frame stored in state->decoded, 0 once the decoder is drained, or
 * SOX_EOF on error.
 *
 * The order is receive-then-send, as the API requires: the decoder is asked
 * for output first and only fed when it reports EAGAIN, and end of input is
 * signalled by sending a NULL packet exactly once.  A decoder that asks for
 * more input after that flush would leave the loop spinning, so it is treated
 * as an error rather than retried. */
static int decode_next_frame(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  for (;;) {
    int result = avcodec_receive_frame(state->context, state->frame);

    if (result == 0) {
      warn_ignored_metadata(ft, state);
      return store_decoded_frame(ft, state) == SOX_SUCCESS ? 1 : SOX_EOF;
    }
    if (result == AVERROR_EOF) {
      state->decoder_eof = sox_true;
      return 0;
    }
    if (result != AVERROR(EAGAIN))
      return fail_av(ft, SOX_EFMT, "Unable to decode audio frame", result);

    if (state->decoder_flushed) {
      lsx_fail_errno(ft, SOX_EFMT, "FFmpeg decoder requested input after end of stream");
      return SOX_EOF;
    }

    result = read_parsed_packet(ft, state);
    if (result == SOX_EOF)
      return SOX_EOF;
    if (result == 0) {
      result = avcodec_send_packet(state->context, NULL);
      state->decoder_flushed = sox_true;
    }
    else {
      result = avcodec_send_packet(state->context, state->packet);
      av_packet_unref(state->packet);
    }
    if (result < 0)
      return fail_av(ft, SOX_EFMT, "Unable to submit compressed audio packet", result);
  }
}

int lsx_ffmpeg_codec_startread(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition)
{
  lsx_ffmpeg_codec_t * p;
  int result;

  if (allocate_state(ft, state, definition, sox_false) != SOX_SUCCESS)
    return SOX_EOF;
  p = *state;
  if (definition->prepare_decoder != NULL && definition->prepare_decoder(ft, p->context) != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }
  if (definition->packet_reader == NULL) {
    p->parser = av_parser_init(definition->codec_id);
    if (p->parser == NULL) {
      lsx_fail_errno(ft, SOX_EFMT, "FFmpeg parser for %s is unavailable", definition->name);
      destroy_state(state);
      return SOX_EOF;
    }
  }

  result = open_codec(ft, p, sox_false);
  if (result != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }
  /* Decode one frame up front: for these formats the rate, channel count and
   * precision are only reliably known once the decoder has seen real data,
   * and SoX needs all three before the first read.  The frame is kept in the
   * state, so nothing is decoded twice. */
  result = decode_next_frame(ft, p);
  if (result <= 0) {
    if (result == 0)
      lsx_fail_errno(ft, SOX_EHDR, "%s stream contains no audio", definition->name);
    destroy_state(state);
    return SOX_EOF;
  }

  ft->signal.rate = (sox_rate_t)p->decoded_rate;
  ft->signal.channels = p->decoded_channels;
  ft->signal.precision = definition->precision ? definition->precision : (unsigned)p->context->bits_per_raw_sample;
  /* None of these streams carry a reliable sample count in their header, so
   * the length stays unknown rather than being guessed from the file size. */
  if (ft->signal.length == SOX_IGNORE_LENGTH)
    ft->signal.length = SOX_UNSPEC;
  ft->encoding.encoding = definition->encoding;
  /* A definition with a fixed precision is a lossy format: reporting bits per
   * sample for it would suggest a word size the bitstream does not have. */
  ft->encoding.bits_per_sample = definition->precision ? 0 : ft->signal.precision;
  return SOX_SUCCESS;
}

size_t lsx_ffmpeg_codec_read(sox_format_t * ft, lsx_ffmpeg_codec_t * state, sox_sample_t * samples, size_t length)
{
  size_t done = 0;

  while (done < length) {
    size_t available;
    size_t count;
    int result;

    if (state->decoded_offset == state->decoded_size) {
      if (state->decoder_eof)
        break;
      result = decode_next_frame(ft, state);
      if (result <= 0)
        break;
    }
    available = state->decoded_size - state->decoded_offset;
    count = min(available, length - done);
    memcpy(samples + done, state->decoded + state->decoded_offset, count * sizeof(*samples));
    state->decoded_offset += count;
    done += count;
  }
  return done;
}

int lsx_ffmpeg_codec_stopread(lsx_ffmpeg_codec_t ** state)
{
  destroy_state(state);
  return SOX_SUCCESS;
}

/* Reject a rate the encoder does not support, before anything is committed.
 * An encoder that publishes no rate list accepts any rate.  SoX will not
 * resample silently on the way out, so the message points at the rate effect
 * instead. */
static int codec_supports_rate(sox_format_t * ft, AVCodec const * codec, int rate)
{
  void const * configurations;
  int count;
  int i;
  int result = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configurations, &count);
  int const * rates = configurations;

  if (result < 0)
    return fail_av(ft, SOX_EFMT, "Unable to query FFmpeg sample rates", result);
  if (rates == NULL)
    return SOX_SUCCESS;
  for (i = 0; i < count; ++i)
    if (rates[i] == rate)
      return SOX_SUCCESS;
  lsx_fail_errno(ft, SOX_EFMT,
      "%s encoder does not support %d Hz; use the SoX rate effect explicitly",
      codec->name, rate);
  return SOX_EOF;
}

/* Reject a channel layout the encoder does not support.  Encoders that
 * publish no layout list are checked against the name table instead, and if
 * there is no table for them either the layout is accepted and the encoder is
 * left to complain at open time. */
static int codec_supports_layout(sox_format_t * ft, AVCodec const * codec, AVChannelLayout const * layout)
{
  void const * configurations;
  char const * const * layout_names;
  char description[128];
  int count;
  int i;
  int result = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configurations, &count);
  AVChannelLayout const * layouts = configurations;

  if (result < 0)
    return fail_av(ft, SOX_EFMT, "Unable to query FFmpeg channel layouts", result);
  if (layouts != NULL) {
    for (i = 0; i < count; ++i)
      if (av_channel_layout_compare(layout, &layouts[i]) == 0)
        return SOX_SUCCESS;
  }
  else {
    layout_names = codec_layout_names(codec->id);
    if (layout_names == NULL)
      return SOX_SUCCESS;
    while (*layout_names != NULL) {
      AVChannelLayout supported = {0};
      sox_bool matches =
          av_channel_layout_from_string(
              &supported, *layout_names) >= 0 &&
          av_channel_layout_compare(layout, &supported) == 0;

      av_channel_layout_uninit(&supported);
      if (matches)
        return SOX_SUCCESS;
      ++layout_names;
    }
  }
  if (av_channel_layout_describe(layout, description, sizeof(description)) < 0)
    strcpy(description, "unknown");
  lsx_fail_errno(ft, SOX_EFMT,
      "%s encoder does not support channel layout `%s' (%d channels)",
      codec->name, description, layout->nb_channels);
  return SOX_EOF;
}

/* Pick the PCM format to hand the encoder.  A requested precision wins if the
 * encoder offers an integer format that carries it exactly, since going
 * through float would round samples that did not need rounding.  Otherwise
 * the preference list below decides, ordered by how little it costs to
 * convert a sox sample into it and by how much of the sample survives.  An
 * encoder that publishes no format list gets planar float, which every such
 * encoder in FFmpeg accepts. */
static int choose_sample_format(
    sox_format_t * ft,
    AVCodec const * codec,
    unsigned precision,
    enum AVSampleFormat * selected)
{
  static enum AVSampleFormat const preferred[] = {
    AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
    AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P,
    AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P,
    AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
    AV_SAMPLE_FMT_S64, AV_SAMPLE_FMT_S64P,
    AV_SAMPLE_FMT_U8, AV_SAMPLE_FMT_U8P
  };
  void const * configurations;
  enum AVSampleFormat const * formats;
  int count;
  size_t preference;
  int i;
  int result = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configurations, &count);

  if (result < 0)
    return fail_av(ft, SOX_EFMT, "Unable to query FFmpeg sample formats", result);
  formats = configurations;
  if (formats == NULL) {
    *selected = AV_SAMPLE_FMT_FLTP;
    return SOX_SUCCESS;
  }
  if (precision && precision <= 16) {
    for (i = 0; i < count; ++i)
      if (formats[i] == AV_SAMPLE_FMT_S16P || formats[i] == AV_SAMPLE_FMT_S16) {
        *selected = formats[i];
        return SOX_SUCCESS;
      }
  }
  else if (precision > 16) {
    for (i = 0; i < count; ++i)
      if (formats[i] == AV_SAMPLE_FMT_S32P || formats[i] == AV_SAMPLE_FMT_S32) {
        *selected = formats[i];
        return SOX_SUCCESS;
      }
  }
  for (preference = 0; preference < array_length(preferred); ++preference)
    for (i = 0; i < count; ++i)
      if (formats[i] == preferred[preference]) {
        *selected = formats[i];
        return SOX_SUCCESS;
      }
  lsx_fail_errno(ft, SOX_EFMT, "%s encoder exposes no supported PCM sample format", codec->name);
  return SOX_EOF;
}

/* Turn -C into whatever quality knob this codec actually has.  For the
 * lossless codecs that is an integer compression level, passed through as
 * given; for the lossy ones it is a bit rate in kbit/s, so the value is
 * scaled to bit/s and range-checked.  -C absent is HUGE_VAL, which selects
 * the definition's default. */
static int set_encoder_bit_rate(sox_format_t * ft, lsx_ffmpeg_codec_t * state)
{
  int64_t bit_rate = state->definition->default_bit_rate;

  if (state->definition->use_compression_level) {
    double requested = ft->encoding.compression;
    int level = state->definition->default_compression_level;

    if (requested != HUGE_VAL) {
      if (!isfinite(requested) ||
          requested != (int)requested ||
          requested < state->definition->minimum_compression_level ||
          requested > state->definition->maximum_compression_level) {
        lsx_fail_errno(ft, SOX_EINVAL,
            "%s compression level must be an integer from %d to %d",
            state->definition->name,
            state->definition->minimum_compression_level,
            state->definition->maximum_compression_level);
        return SOX_EOF;
      }
      level = (int)requested;
    }
    state->context->compression_level = level;
    state->context->bit_rate = 0;
    return SOX_SUCCESS;
  }
  if (ft->encoding.compression != HUGE_VAL) {
    double requested = ft->encoding.compression * 1000.;

    if (!isfinite(requested) || requested < 1 || requested > (double)INT64_MAX) {
      lsx_fail_errno(ft, SOX_EINVAL, "Invalid %s bitrate", state->definition->name);
      return SOX_EOF;
    }
    bit_rate = (int64_t)(requested + .5);
  }
  if ((state->definition->minimum_bit_rate &&
       bit_rate < state->definition->minimum_bit_rate) ||
      (state->definition->maximum_bit_rate &&
       bit_rate > state->definition->maximum_bit_rate)) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "%s bitrate must be between %.0f and %.0f kbit/s",
        state->definition->name,
        state->definition->minimum_bit_rate / 1000.,
        state->definition->maximum_bit_rate / 1000.);
    return SOX_EOF;
  }
  state->context->bit_rate = bit_rate;
  return SOX_SUCCESS;
}

/* Address of one sample in the encoder's input frame; the write-side twin of
 * decoded_sample_address.  Here the channel count is the frame's own, since
 * the frame was built by this file. */
static void * encoded_sample_address(AVFrame * frame, int sample, int channel)
{
  enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  int planar = av_sample_fmt_is_planar(format);
  int bytes = av_get_bytes_per_sample(format);
  size_t index = planar ? (size_t)sample :
      (size_t)sample * frame->ch_layout.nb_channels + channel;

  return frame->extended_data[planar ? channel : 0] + index * bytes;
}

/* Store one sox sample into the encoder's input frame in its own PCM format.
 * The narrowing integer cases round to nearest away from zero and clamp, the
 * arithmetic being done in int64 so that adding the rounding offset cannot
 * itself overflow.  No clip is counted: the target format was chosen to carry
 * the requested precision, so any loss here was asked for.  The float cases
 * divide by 2^31, which is exact.  Formats outside this set are never
 * selected by choose_sample_format. */
static void sox_sample_to_encoded(AVFrame * frame, int sample, int channel, sox_sample_t value)
{
  enum AVSampleFormat format = av_get_packed_sample_fmt((enum AVSampleFormat)frame->format);
  void * destination = encoded_sample_address(frame, sample, channel);

  switch (format) {
    case AV_SAMPLE_FMT_U8: {
      int64_t rounded = value;

      rounded += rounded < 0 ? -INT64_C(8388608) : INT64_C(8388608);
      rounded /= INT64_C(16777216);
      if (rounded < -128)
        rounded = -128;
      else if (rounded > 127)
        rounded = 127;
      *(uint8_t *)destination = (uint8_t)(rounded + 128);
      break;
    }
    case AV_SAMPLE_FMT_S16: {
      int64_t rounded = value;

      rounded += rounded < 0 ? -INT64_C(32768) : INT64_C(32768);
      rounded /= INT64_C(65536);
      if (rounded < INT16_MIN)
        rounded = INT16_MIN;
      else if (rounded > INT16_MAX)
        rounded = INT16_MAX;
      *(int16_t *)destination = (int16_t)rounded;
      break;
    }
    case AV_SAMPLE_FMT_S32:
      *(int32_t *)destination = (int32_t)value;
      break;
    case AV_SAMPLE_FMT_S64:
      *(int64_t *)destination =
          (int64_t)value * INT64_C(4294967296);
      break;
    case AV_SAMPLE_FMT_FLT:
      *(float *)destination = (float)(value * (1. / 2147483648.));
      break;
    case AV_SAMPLE_FMT_DBL:
      *(double *)destination = value * (1. / 2147483648.);
      break;
    default:
      break;
  }
}

/* Drain every packet the encoder currently has and write it out, through the
 * definition's packet writer if it has one.  EAGAIN means the encoder simply
 * wants more input, which is the normal way out mid-stream; during the final
 * flush it must not happen, since a flushed encoder is required to answer
 * with packets until AVERROR_EOF, so there it is an error.  The packet is
 * unreffed on every path, including the failing one. */
static int write_available_packets(sox_format_t * ft, lsx_ffmpeg_codec_t * state, sox_bool flushing)
{
  for (;;) {
    int result = avcodec_receive_packet(state->context, state->packet);

    if (result == AVERROR(EAGAIN))
      return flushing ? SOX_EOF : SOX_SUCCESS;
    if (result == AVERROR_EOF)
      return SOX_SUCCESS;
    if (result < 0)
      return fail_av(ft, SOX_EFMT, "Unable to receive encoded audio packet", result);
    if (state->definition->packet_writer != NULL)
      result = state->definition->packet_writer(ft, state->context, state->packet);
    else
      result = lsx_writebuf(ft, state->packet->data,
          (size_t)state->packet->size) == (size_t)state->packet->size ?
          SOX_SUCCESS : SOX_EOF;
    if (result != SOX_SUCCESS) {
      av_packet_unref(state->packet);
      return SOX_EOF;
    }
    av_packet_unref(state->packet);
  }
}

/* Convert the buffered sox samples into the encoder's frame and submit it.
 * samples_per_channel is normally the codec's frame size and is smaller only
 * for a final partial frame, which is why it is a parameter rather than read
 * from the state.
 *
 * The frame is reused across calls, so it has to be made writable again each
 * time -- the encoder may still hold a reference to the buffers it was given
 * last time.  Presentation timestamps are counted in samples, matching the
 * 1/rate time base set at startwrite. */
static int encode_pending_frame(sox_format_t * ft, lsx_ffmpeg_codec_t * state, int samples_per_channel)
{
  int channel;
  int sample;
  int result;

  state->frame->nb_samples = samples_per_channel;
  result = av_frame_make_writable(state->frame);
  if (result < 0)
    return fail_av(ft, SOX_ENOMEM, "Unable to prepare FFmpeg audio frame", result);

  for (sample = 0; sample < samples_per_channel; ++sample)
    for (channel = 0; channel < state->context->ch_layout.nb_channels; ++channel)
      sox_sample_to_encoded(state->frame, sample, channel,
          state->pending[(size_t)sample *
              state->context->ch_layout.nb_channels + channel]);
  state->frame->pts = state->next_pts;
  state->next_pts += samples_per_channel;

  result = avcodec_send_frame(state->context, state->frame);
  if (result < 0)
    return fail_av(ft, SOX_EFMT, "Unable to submit PCM frame to FFmpeg encoder", result);
  return write_available_packets(ft, state, sox_false);
}

int lsx_ffmpeg_codec_startwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition)
{
  lsx_ffmpeg_codec_t * p;
  AVChannelLayout layout = {0};
  enum AVSampleFormat sample_format;
  double rate = ft->signal.rate;
  unsigned precision = ft->encoding.bits_per_sample ? ft->encoding.bits_per_sample : definition->precision;
  int result;

  /* Everything the encoder cannot change later is settled before the state is
   * allocated or the context is opened, so that a rejected request leaves
   * nothing behind.  The layout is the one exception: it may hold an
   * allocation, so every exit from here on has to uninit it. */
  if (ft->signal.channels < 1 || ft->signal.channels > definition->max_encode_channels) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoding supports layouts with 1 to %u channels",
        definition->name, definition->max_encode_channels);
    return SOX_EOF;
  }
  if (ft->channel_layout != NULL) {
    result = av_channel_layout_from_string(&layout, ft->channel_layout);
    if (result < 0 || !av_channel_layout_check(&layout)) {
      av_channel_layout_uninit(&layout);
      lsx_fail_errno(ft, SOX_EINVAL,
          "Unknown or invalid %s channel layout `%s'; "
          "use --help-format %s to list supported layouts",
          definition->name, ft->channel_layout, ft->filetype);
      return SOX_EOF;
    }
    if ((unsigned)layout.nb_channels != ft->signal.channels) {
      lsx_fail_errno(ft, SOX_EINVAL,
          "%s channel layout `%s' has %d channels, "
          "but the output signal has %u",
          definition->name, ft->channel_layout,
          layout.nb_channels, ft->signal.channels);
      av_channel_layout_uninit(&layout);
      return SOX_EOF;
    }
  }
  else if (select_layout(definition, ft->signal.channels, &layout) < 0) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoding has no default layout for %u channels; "
        "specify --channel-layout explicitly",
        definition->name, ft->signal.channels);
    return SOX_EOF;
  }
  if (rate < 1 || rate > INT_MAX || rate != (int)rate) {
    lsx_fail_errno(ft, SOX_EFMT, "%s encoding requires an integer sample rate", definition->name);
    av_channel_layout_uninit(&layout);
    return SOX_EOF;
  }
  if (allocate_state(ft, state, definition, sox_true) != SOX_SUCCESS) {
    av_channel_layout_uninit(&layout);
    return SOX_EOF;
  }
  p = *state;

  if (codec_supports_rate(ft, p->codec, (int)rate) != SOX_SUCCESS ||
      codec_supports_layout(ft, p->codec, &layout) != SOX_SUCCESS ||
      choose_sample_format(
          ft, p->codec, precision, &sample_format) != SOX_SUCCESS ||
      set_encoder_bit_rate(ft, p) != SOX_SUCCESS) {
    av_channel_layout_uninit(&layout);
    destroy_state(state);
    return SOX_EOF;
  }

  p->context->sample_rate = (int)rate;
  p->context->sample_fmt = sample_format;
  p->context->bits_per_raw_sample = (int)precision;
  p->context->time_base = (AVRational){1, (int)rate};
  result = av_channel_layout_copy(&p->context->ch_layout, &layout);
  av_channel_layout_uninit(&layout);
  if (result < 0) {
    fail_av(ft, SOX_ENOMEM, "Unable to configure FFmpeg channel layout", result);
    destroy_state(state);
    return SOX_EOF;
  }
  if (definition->prepare_encoder != NULL) {
    result = definition->prepare_encoder(p->context);
    if (result < 0) {
      fail_av(ft, SOX_EFMT, "Unable to prepare FFmpeg encoder", result);
      destroy_state(state);
      return SOX_EOF;
    }
  }

  result = open_codec(ft, p, sox_true);
  if (result != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }
  /* The frame size is only known once the encoder is open, and the whole
   * write path is built around buffering exactly that many samples, so an
   * encoder that does not fix one cannot be driven by this adapter. */
  if (p->context->frame_size <= 0) {
    lsx_fail_errno(ft, SOX_EFMT, "%s encoder did not provide a fixed audio frame size", definition->name);
    destroy_state(state);
    return SOX_EOF;
  }

  p->frame_samples = p->context->frame_size;
  p->pending_capacity = (size_t)p->frame_samples * p->context->ch_layout.nb_channels;
  p->pending = lsx_calloc(p->pending_capacity, sizeof(*p->pending));
  p->frame->format = p->context->sample_fmt;
  p->frame->sample_rate = p->context->sample_rate;
  p->frame->nb_samples = p->frame_samples;
  result = av_channel_layout_copy(&p->frame->ch_layout, &p->context->ch_layout);
  if (result >= 0)
    result = av_frame_get_buffer(p->frame, 0);
  if (result < 0) {
    fail_av(ft, SOX_ENOMEM, "Unable to allocate FFmpeg PCM frame", result);
    destroy_state(state);
    return SOX_EOF;
  }

  ft->encoding.encoding = definition->encoding;
  ft->encoding.bits_per_sample = definition->precision ? 0 : precision;
  ft->signal.precision = precision;
  return SOX_SUCCESS;
}

size_t lsx_ffmpeg_codec_write(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t const * samples,
    size_t length)
{
  size_t done = 0;

  if (state->write_failed)
    return 0;
  while (done < length) {
    size_t available = state->pending_capacity - state->pending_size;
    size_t count = min(available, length - done);

    memcpy(state->pending + state->pending_size, samples + done, count * sizeof(*samples));
    state->pending_size += count;
    done += count;
    if (state->pending_size == state->pending_capacity) {
      if (encode_pending_frame(ft, state, state->frame_samples) != SOX_SUCCESS) {
        /* The samples just copied in were not encoded, so they are not
         * reported as written; the failure is latched and stopwrite will
         * neither flush nor try again. */
        state->write_failed = sox_true;
        return done - count;
      }
      state->pending_size = 0;
    }
  }
  return done;
}

int lsx_ffmpeg_codec_stopwrite(sox_format_t * ft, lsx_ffmpeg_codec_t ** state)
{
  lsx_ffmpeg_codec_t * p;
  int result = SOX_SUCCESS;

  if (state == NULL || *state == NULL)
    return SOX_SUCCESS;
  p = *state;
  /* The state is destroyed on every path below, failure included, so that a
   * caller which ignores the return value still does not leak. */
  if (p->write_failed)
    result = SOX_EOF;
  else if (p->pending_size) {
    size_t channels = (size_t)p->context->ch_layout.nb_channels;
    int samples_per_channel;

    if (p->pending_size % channels) {
      lsx_fail_errno(ft, SOX_EINVAL, "%s encoder received an incomplete interleaved sample frame", p->definition->name);
      result = SOX_EOF;
    }
    else {
      samples_per_channel = (int)(p->pending_size / channels);
      /* An encoder that cannot take a short last frame is given a full one
       * padded with silence, which lengthens the file by up to one frame.
       * There is no alternative: the codecs here have no way to record a
       * partial final frame. */
      if (!(p->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
        memset(p->pending + p->pending_size, 0, (p->pending_capacity - p->pending_size) * sizeof(*p->pending));
        samples_per_channel = p->frame_samples;
      }
      result = encode_pending_frame(ft, p, samples_per_channel);
    }
  }
  if (result == SOX_SUCCESS) {
    int av_result = avcodec_send_frame(p->context, NULL);

    if (av_result < 0)
      result = fail_av(ft, SOX_EFMT, "Unable to flush FFmpeg encoder", av_result);
    else
      result = write_available_packets(ft, p, sox_true);
  }
  destroy_state(state);
  return result;
}

AVCodecContext const * lsx_ffmpeg_codec_context(lsx_ffmpeg_codec_t const * state)
{
  return state != NULL ? state->context : NULL;
}
