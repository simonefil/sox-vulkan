/* Common libavcodec adapter for SoX format handlers.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
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

#define INPUT_BUFFER_SIZE 16384

struct lsx_ffmpeg_codec_t {
  lsx_ffmpeg_codec_definition_t const * definition;
  AVCodec const * codec;
  AVCodecContext * context;
  AVCodecParserContext * parser;
  AVPacket * packet;
  AVFrame * frame;

  uint8_t input[INPUT_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
  size_t input_offset;
  size_t input_size;
  sox_bool input_eof;
  sox_bool parser_eof;
  sox_bool decoder_flushed;
  sox_bool decoder_eof;

  sox_sample_t * decoded;
  size_t decoded_capacity;
  size_t decoded_offset;
  size_t decoded_size;
  unsigned decoded_channels;
  int decoded_rate;
  sox_bool ignored_metadata_warning_shown;

  sox_sample_t * pending;
  size_t pending_capacity;
  size_t pending_size;
  int frame_samples;
  int64_t next_pts;
  sox_bool write_failed;
};

static int fail_av(
    sox_format_t * ft,
    int sox_error,
    char const * operation,
    int av_error)
{
  char message[AV_ERROR_MAX_STRING_SIZE];

  if (av_strerror(av_error, message, sizeof(message)) < 0)
    strcpy(message, "unknown FFmpeg error");
  lsx_fail_errno(ft, sox_error, "%s: %s", operation, message);
  return SOX_EOF;
}

static sox_bool is_reserved_codec_option(char const * key)
{
  static char const * const reserved[] = {
    "b", "ab", "bit_rate",
    "ar", "sample_rate",
    "ac", "channels", "channel_layout", "ch_layout", "downmix",
    "sample_fmt", "request_sample_fmt", "time_base",
    NULL
  };
  size_t i;

  for (i = 0; reserved[i] != NULL; ++i)
    if (!strcmp(key, reserved[i]))
      return sox_true;
  return sox_false;
}

static int open_codec(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_bool encoding)
{
  AVDictionary * options = NULL;
  AVDictionaryEntry const * entry;
  int result;

  if (ft->codec_options != NULL) {
    result = av_dict_parse_string(&options, ft->codec_options, "=", ":", 0);
    if (result < 0) {
      av_dict_free(&options);
      return fail_av(ft, SOX_EINVAL,
          "Unable to parse --ffmpeg-opts (expected key=value:key=value)",
          result);
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

  result = avcodec_open2(state->context, state->codec,
      options != NULL ? &options : NULL);
  if (result < 0) {
    av_dict_free(&options);
    return fail_av(ft, SOX_EFMT,
        encoding ? "Unable to open FFmpeg encoder" :
            "Unable to open FFmpeg decoder",
        result);
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

static int allocate_state(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state,
    lsx_ffmpeg_codec_definition_t const * definition,
    sox_bool encoding)
{
  lsx_ffmpeg_codec_t * p = lsx_calloc(1, sizeof(*p));

  p->definition = definition;
  p->codec = encoding ?
      avcodec_find_encoder(definition->codec_id) :
      avcodec_find_decoder(definition->codec_id);
  if (p->codec == NULL) {
    lsx_fail_errno(ft, SOX_EFMT, "FFmpeg %s for %s is unavailable",
        encoding ? "encoder" : "decoder", definition->name);
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

static sox_bool is_supported_layout(
    AVChannelLayout const * layout,
    lsx_ffmpeg_codec_definition_t const * definition)
{
  uint64_t mask;

  if (!av_channel_layout_check(layout))
    return sox_false;
  if (layout->order == AV_CHANNEL_ORDER_UNSPEC)
    return layout->nb_channels >= 1 &&
        (layout->nb_channels <= 6 ||
         definition->accept_unspecified_decode_layout);
  if (layout->order != AV_CHANNEL_ORDER_NATIVE)
    return sox_false;

  mask = layout->u.mask;
  switch (layout->nb_channels) {
    case 1:
      return mask == AV_CH_LAYOUT_MONO;
    case 2:
      return mask == AV_CH_LAYOUT_STEREO;
    case 3:
      return mask == AV_CH_LAYOUT_SURROUND;
    case 4:
      return mask == AV_CH_LAYOUT_QUAD || mask == AV_CH_LAYOUT_2_2;
    case 5:
      return mask == AV_CH_LAYOUT_5POINT0 ||
          mask == AV_CH_LAYOUT_5POINT0_BACK;
    case 6:
      return mask == AV_CH_LAYOUT_5POINT1 ||
          mask == AV_CH_LAYOUT_5POINT1_BACK;
    case 7:
      return mask == AV_CH_LAYOUT_6POINT1 ||
          mask == AV_CH_LAYOUT_6POINT1_BACK;
    case 8:
      return mask == AV_CH_LAYOUT_7POINT1;
    default:
      return sox_false;
  }
}

static int validate_decoded_frame(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
{
  AVChannelLayout const * layout = &state->frame->ch_layout;
  int rate = state->frame->sample_rate ?
      state->frame->sample_rate : state->context->sample_rate;
  char description[128];

  if (layout->nb_channels == 0)
    layout = &state->context->ch_layout;
  if (!is_supported_layout(layout, state->definition) ||
      (unsigned)layout->nb_channels >
          state->definition->max_decode_channels) {
    if (av_channel_layout_describe(layout, description,
          sizeof(description)) < 0)
      strcpy(description, "unknown");
    lsx_fail_errno(ft, SOX_EFMT, "Unsupported %s channel layout: %s",
        state->definition->name, description);
    return SOX_EOF;
  }
  if (rate <= 0) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Unable to determine %s sample rate", state->definition->name);
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
  return SOX_SUCCESS;
}

static void warn_ignored_metadata(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
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

static void const * decoded_sample_address(
    AVFrame const * frame,
    int sample,
    int channel,
    unsigned channels)
{
  enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  int planar = av_sample_fmt_is_planar(format);
  int bytes = av_get_bytes_per_sample(format);
  size_t index = planar ? (size_t)sample :
      (size_t)sample * channels + channel;

  return frame->extended_data[planar ? channel : 0] + index * bytes;
}

static sox_sample_t decoded_sample_to_sox(
    sox_format_t * ft,
    AVFrame const * frame,
    int sample,
    int channel,
    unsigned channels)
{
  enum AVSampleFormat format =
      av_get_packed_sample_fmt((enum AVSampleFormat)frame->format);
  void const * source =
      decoded_sample_address(frame, sample, channel, channels);

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
      double value = format == AV_SAMPLE_FMT_FLT ?
          *(float const *)source : *(double const *)source;
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

static int store_decoded_frame(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
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
    state->decoded = lsx_realloc(
        state->decoded, required * sizeof(*state->decoded));
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

static int read_parsed_packet(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
{
  if (state->definition->packet_reader != NULL)
    return state->definition->packet_reader(ft, state->packet);

  for (;;) {
    uint8_t * packet_data = NULL;
    int packet_size = 0;
    int consumed;
    int result;

    if (state->input_offset == state->input_size && !state->input_eof) {
      state->input_size = lsx_readbuf(
          ft, state->input, INPUT_BUFFER_SIZE);
      state->input_offset = 0;
      memset(state->input + state->input_size, 0,
          AV_INPUT_BUFFER_PADDING_SIZE);
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
        return fail_av(ft, SOX_EHDR,
            "Unable to flush FFmpeg bitstream parser", consumed);
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
        return fail_av(ft, SOX_EHDR,
            "Unable to parse compressed audio", consumed);
      state->input_offset += (size_t)consumed;
      if (consumed == 0 && packet_size == 0) {
        lsx_fail_errno(ft, SOX_EHDR,
            "FFmpeg bitstream parser made no progress");
        return SOX_EOF;
      }
    }

    if (packet_size == 0)
      continue;
    result = av_new_packet(state->packet, packet_size);
    if (result < 0)
      return fail_av(ft, SOX_ENOMEM,
          "Unable to allocate compressed audio packet", result);
    memcpy(state->packet->data, packet_data, (size_t)packet_size);
    return 1;
  }
}

static int decode_next_frame(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
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
      lsx_fail_errno(ft, SOX_EFMT,
          "FFmpeg decoder requested input after end of stream");
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
      return fail_av(ft, SOX_EFMT,
          "Unable to submit compressed audio packet", result);
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
  if (definition->packet_reader == NULL) {
    p->parser = av_parser_init(definition->codec_id);
    if (p->parser == NULL) {
      lsx_fail_errno(ft, SOX_EFMT,
          "FFmpeg parser for %s is unavailable", definition->name);
      destroy_state(state);
      return SOX_EOF;
    }
  }

  result = open_codec(ft, p, sox_false);
  if (result != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }
  result = decode_next_frame(ft, p);
  if (result <= 0) {
    if (result == 0)
      lsx_fail_errno(ft, SOX_EHDR, "%s stream contains no audio",
          definition->name);
    destroy_state(state);
    return SOX_EOF;
  }

  ft->signal.rate = (sox_rate_t)p->decoded_rate;
  ft->signal.channels = p->decoded_channels;
  ft->signal.precision = definition->precision;
  ft->signal.length = SOX_UNSPEC;
  ft->encoding.encoding = definition->encoding;
  ft->encoding.bits_per_sample = 0;
  return SOX_SUCCESS;
}

size_t lsx_ffmpeg_codec_read(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_sample_t * samples,
    size_t length)
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
    memcpy(samples + done, state->decoded + state->decoded_offset,
        count * sizeof(*samples));
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

static int codec_supports_rate(
    sox_format_t * ft,
    AVCodec const * codec,
    int rate)
{
  void const * configurations;
  int count;
  int i;
  int result = avcodec_get_supported_config(NULL, codec,
      AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configurations, &count);
  int const * rates = configurations;

  if (result < 0)
    return fail_av(ft, SOX_EFMT,
        "Unable to query FFmpeg sample rates", result);
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

static int codec_supports_layout(
    sox_format_t * ft,
    AVCodec const * codec,
    AVChannelLayout const * layout)
{
  void const * configurations;
  int count;
  int i;
  int result = avcodec_get_supported_config(NULL, codec,
      AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configurations, &count);
  AVChannelLayout const * layouts = configurations;

  if (result < 0)
    return fail_av(ft, SOX_EFMT,
        "Unable to query FFmpeg channel layouts", result);
  if (layouts == NULL)
    return SOX_SUCCESS;
  for (i = 0; i < count; ++i)
    if (av_channel_layout_compare(layout, &layouts[i]) == 0)
      return SOX_SUCCESS;
  lsx_fail_errno(ft, SOX_EFMT,
      "%s encoder does not support the canonical %d-channel SoX layout",
      codec->name, layout->nb_channels);
  return SOX_EOF;
}

static int choose_sample_format(
    sox_format_t * ft,
    AVCodec const * codec,
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
  int result = avcodec_get_supported_config(NULL, codec,
      AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configurations, &count);

  if (result < 0)
    return fail_av(ft, SOX_EFMT,
        "Unable to query FFmpeg sample formats", result);
  formats = configurations;
  if (formats == NULL) {
    *selected = AV_SAMPLE_FMT_FLTP;
    return SOX_SUCCESS;
  }
  for (preference = 0; preference < array_length(preferred); ++preference)
    for (i = 0; i < count; ++i)
      if (formats[i] == preferred[preference]) {
        *selected = formats[i];
        return SOX_SUCCESS;
      }
  lsx_fail_errno(ft, SOX_EFMT,
      "%s encoder exposes no supported PCM sample format", codec->name);
  return SOX_EOF;
}

static int set_encoder_bit_rate(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state)
{
  int64_t bit_rate = state->definition->default_bit_rate;

  if (ft->encoding.compression != HUGE_VAL) {
    double requested = ft->encoding.compression * 1000.;

    if (!isfinite(requested) || requested < 1 ||
        requested > (double)INT64_MAX) {
      lsx_fail_errno(ft, SOX_EINVAL, "Invalid %s bitrate",
          state->definition->name);
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

static void * encoded_sample_address(
    AVFrame * frame,
    int sample,
    int channel)
{
  enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  int planar = av_sample_fmt_is_planar(format);
  int bytes = av_get_bytes_per_sample(format);
  size_t index = planar ? (size_t)sample :
      (size_t)sample * frame->ch_layout.nb_channels + channel;

  return frame->extended_data[planar ? channel : 0] + index * bytes;
}

static void sox_sample_to_encoded(
    AVFrame * frame,
    int sample,
    int channel,
    sox_sample_t value)
{
  enum AVSampleFormat format =
      av_get_packed_sample_fmt((enum AVSampleFormat)frame->format);
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

static int write_available_packets(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    sox_bool flushing)
{
  for (;;) {
    int result = avcodec_receive_packet(state->context, state->packet);

    if (result == AVERROR(EAGAIN))
      return flushing ? SOX_EOF : SOX_SUCCESS;
    if (result == AVERROR_EOF)
      return SOX_SUCCESS;
    if (result < 0)
      return fail_av(ft, SOX_EFMT,
          "Unable to receive encoded audio packet", result);
    if (state->definition->packet_writer != NULL)
      result = state->definition->packet_writer(
          ft, state->context, state->packet);
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

static int encode_pending_frame(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t * state,
    int samples_per_channel)
{
  int channel;
  int sample;
  int result;

  state->frame->nb_samples = samples_per_channel;
  result = av_frame_make_writable(state->frame);
  if (result < 0)
    return fail_av(ft, SOX_ENOMEM,
        "Unable to prepare FFmpeg audio frame", result);

  for (sample = 0; sample < samples_per_channel; ++sample)
    for (channel = 0; channel < state->context->ch_layout.nb_channels;
        ++channel)
      sox_sample_to_encoded(state->frame, sample, channel,
          state->pending[(size_t)sample *
              state->context->ch_layout.nb_channels + channel]);
  state->frame->pts = state->next_pts;
  state->next_pts += samples_per_channel;

  result = avcodec_send_frame(state->context, state->frame);
  if (result < 0)
    return fail_av(ft, SOX_EFMT,
        "Unable to submit PCM frame to FFmpeg encoder", result);
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
  int result;

  if (ft->signal.channels < 1 ||
      ft->signal.channels > definition->max_encode_channels ||
      canonical_layout(ft->signal.channels, &layout) < 0) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoding supports standard layouts with 1 to %u channels",
        definition->name, definition->max_encode_channels);
    return SOX_EOF;
  }
  if (rate < 1 || rate > INT_MAX || rate != (int)rate) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoding requires an integer sample rate", definition->name);
    return SOX_EOF;
  }
  if (allocate_state(ft, state, definition, sox_true) != SOX_SUCCESS)
    return SOX_EOF;
  p = *state;

  if (codec_supports_rate(ft, p->codec, (int)rate) != SOX_SUCCESS ||
      codec_supports_layout(ft, p->codec, &layout) != SOX_SUCCESS ||
      choose_sample_format(ft, p->codec, &sample_format) != SOX_SUCCESS ||
      set_encoder_bit_rate(ft, p) != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }

  p->context->sample_rate = (int)rate;
  p->context->sample_fmt = sample_format;
  p->context->time_base = (AVRational){1, (int)rate};
  result = av_channel_layout_copy(&p->context->ch_layout, &layout);
  if (result < 0) {
    fail_av(ft, SOX_ENOMEM,
        "Unable to configure FFmpeg channel layout", result);
    destroy_state(state);
    return SOX_EOF;
  }
  if (definition->prepare_encoder != NULL) {
    result = definition->prepare_encoder(p->context);
    if (result < 0) {
      fail_av(ft, SOX_EFMT,
          "Unable to prepare FFmpeg encoder", result);
      destroy_state(state);
      return SOX_EOF;
    }
  }

  result = open_codec(ft, p, sox_true);
  if (result != SOX_SUCCESS) {
    destroy_state(state);
    return SOX_EOF;
  }
  if (p->context->frame_size <= 0) {
    lsx_fail_errno(ft, SOX_EFMT,
        "%s encoder did not provide a fixed audio frame size",
        definition->name);
    destroy_state(state);
    return SOX_EOF;
  }

  p->frame_samples = p->context->frame_size;
  p->pending_capacity =
      (size_t)p->frame_samples * p->context->ch_layout.nb_channels;
  p->pending = lsx_calloc(p->pending_capacity, sizeof(*p->pending));
  p->frame->format = p->context->sample_fmt;
  p->frame->sample_rate = p->context->sample_rate;
  p->frame->nb_samples = p->frame_samples;
  result = av_channel_layout_copy(
      &p->frame->ch_layout, &p->context->ch_layout);
  if (result >= 0)
    result = av_frame_get_buffer(p->frame, 0);
  if (result < 0) {
    fail_av(ft, SOX_ENOMEM, "Unable to allocate FFmpeg PCM frame", result);
    destroy_state(state);
    return SOX_EOF;
  }

  ft->encoding.encoding = definition->encoding;
  ft->encoding.bits_per_sample = 0;
  ft->signal.precision = definition->precision;
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

    memcpy(state->pending + state->pending_size, samples + done,
        count * sizeof(*samples));
    state->pending_size += count;
    done += count;
    if (state->pending_size == state->pending_capacity) {
      if (encode_pending_frame(ft, state, state->frame_samples) !=
          SOX_SUCCESS) {
        state->write_failed = sox_true;
        return done - count;
      }
      state->pending_size = 0;
    }
  }
  return done;
}

int lsx_ffmpeg_codec_stopwrite(
    sox_format_t * ft,
    lsx_ffmpeg_codec_t ** state)
{
  lsx_ffmpeg_codec_t * p;
  int result = SOX_SUCCESS;

  if (state == NULL || *state == NULL)
    return SOX_SUCCESS;
  p = *state;
  if (p->write_failed)
    result = SOX_EOF;
  else if (p->pending_size) {
    size_t channels = (size_t)p->context->ch_layout.nb_channels;
    int samples_per_channel;

    if (p->pending_size % channels) {
      lsx_fail_errno(ft, SOX_EINVAL,
          "%s encoder received an incomplete interleaved sample frame",
          p->definition->name);
      result = SOX_EOF;
    }
    else {
      samples_per_channel = (int)(p->pending_size / channels);
      if (!(p->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
        memset(p->pending + p->pending_size, 0,
            (p->pending_capacity - p->pending_size) * sizeof(*p->pending));
        samples_per_channel = p->frame_samples;
      }
      result = encode_pending_frame(ft, p, samples_per_channel);
    }
  }
  if (result == SOX_SUCCESS) {
    int av_result = avcodec_send_frame(p->context, NULL);

    if (av_result < 0)
      result = fail_av(ft, SOX_EFMT,
          "Unable to flush FFmpeg encoder", av_result);
    else
      result = write_available_packets(ft, p, sox_true);
  }
  destroy_state(state);
  return result;
}
