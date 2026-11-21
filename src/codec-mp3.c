/* This library is free software; you can redistribute it and/or modify it
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

/* libSoX packet codec: MP3, through libmad and LAME.
 *
 * A packet is one MPEG audio frame, header included; there is no private
 * data.  A frame may take part of its data from the frames before it, the bit
 * reservoir.  libmad keeps the reservoir only by looking at the next frame in
 * its buffer, which a packet decoded on its own lacks, so the reservoir is
 * kept here: the main data of the frames so far, after their headers and
 * side information, of which libmad is given the last bytes a frame can
 * reach back to before each frame.
 *
 * The decoder lags the signal by 529 frames, and LAME's encoder adds its own
 * delay ahead of that; together they are the codec delay a container carries,
 * and the padding LAME adds at the end is discard on the last packets.  A
 * stream whose first frame is a Xing or LAME information frame, as an MP3
 * file has, says the same in that frame: it is dropped, and the delay and
 * padding it gives are applied here when the container gives none.
 *
 * Decoding needs only libmad and encoding LAME, so that a build without
 * LAME still reads. */

#include "codec.h"

#if defined HAVE_MP3 && (defined STATIC_MP3 || !defined HAVE_LIBLTDL) && defined HAVE_MAD_H

#include <string.h>

#include <mad.h>
#ifdef HAVE_LAME
#include <math.h>
#if defined HAVE_LAME_LAME_H
#include <lame/lame.h>
#else
#include <lame.h>
#endif
#endif

#define DECODER_DELAY 529
#define HELD_FRAMES 2         /* encoding: enough to spread the padding over */
#define RESERVOIR_SIZE 511    /* the furthest back a frame's main data can begin */

typedef struct {
  /* Decoding */
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  unsigned char * input;
  size_t input_size;
  sox_bool started;
  sox_bool have_params;
  sox_uint64_t skip;    /* frames the information frame says to drop */
  sox_uint64_t end;     /* where its signal ends, if it gave one */
  sox_uint64_t position;
  unsigned char reservoir[RESERVOIR_SIZE];
  size_t reservoir_size;

#ifdef HAVE_LAME
  /* Encoding */
  lame_global_flags * gfp;
  float * left, * right;
  size_t pcm_size;
  unsigned char * output; /* LAME's output, not yet cut into frames */
  size_t output_size, output_alloc;
  sox_uint64_t input_frames, coded_frames;
#endif
} priv_t;

/* The length of the Layer III frame whose header data begins, or 0. */
static size_t frame_size(unsigned char const * data, size_t size, unsigned * frames)
{
  static unsigned const bitrates[2][15] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  static unsigned const rates[3] = {44100, 48000, 32000};
  unsigned version, bitrate, rate;

  if (size < 4 || data[0] != 0xff || (data[1] & 0xe0) != 0xe0 || ((data[1] >> 1) & 3) != 1)
    return 0;
  version = (data[1] >> 3) & 3; /* 3: MPEG 1, 2: MPEG 2, 0: MPEG 2.5 */
  bitrate = data[2] >> 4;
  if (version == 1 || !bitrate || bitrate == 15 || ((data[2] >> 2) & 3) == 3)
    return 0;
  bitrate = bitrates[version != 3][bitrate] * 1000;
  rate = rates[(data[2] >> 2) & 3] >> (version == 3? 0 : version == 2? 1 : 2);
  *frames = version == 3? 1152 : 576;
  return (version == 3? 144 : 72) * bitrate / rate + ((data[2] >> 1) & 1);
}

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  mad_stream_init(&p->stream);
  mad_frame_init(&p->frame);
  mad_synth_init(&p->synth);
  codec->params.encoding = SOX_ENCODING_MP3;
  codec->params.bits_per_sample = 0;
  codec->ft->signal.precision = 16;
  return SOX_SUCCESS;
}

/* Whether the frame is an information frame, and if its LAME extension gives
 * the delay and padding, the frames to drop and where the signal ends.  The
 * Xing or Info tag is looked for where the main data would begin were there
 * no CRC, which LAME leaves out of the count even when it sets the flag for
 * it, as FFmpeg does in reading. */
static sox_bool information_frame(priv_t * p, unsigned char const * data, size_t size)
{
  unsigned frames, pos, flags;
  sox_uint64_t count = 0;
  sox_bool mpeg1 = ((data[1] >> 3) & 3) == 3, mono = (data[3] >> 6) == 3;

  if (!frame_size(data, size, &frames))
    return sox_false;
  pos = 4 + (mpeg1? (mono? 17 : 32) : (mono? 9 : 17));
  if (pos + 8 > size || (memcmp(data + pos, "Xing", 4) && memcmp(data + pos, "Info", 4)))
    return sox_false;
  flags = data[pos + 7];
  pos += 8;
  if (flags & 1) {
    if (pos + 4 > size)
      return sox_true;
    count = (sox_uint64_t)data[pos] << 24 | data[pos + 1] << 16 | data[pos + 2] << 8 | data[pos + 3];
    pos += 4;
  }
  pos += (flags & 2? 4 : 0) + (flags & 4? 100 : 0) + (flags & 8? 4 : 0);
  if (count && pos + 24 <= size &&
      (!memcmp(data + pos, "LAME", 4) || !memcmp(data + pos, "Lavf", 4) || !memcmp(data + pos, "Lavc", 4))) {
    unsigned delay = (unsigned)data[pos + 21] << 4 | data[pos + 22] >> 4;
    unsigned padding = (unsigned)(data[pos + 22] & 15) << 8 | data[pos + 23];

    if (count * frames > delay + padding) {
      p->skip = delay + DECODER_DELAY;
      p->end = count * frames + DECODER_DELAY - padding;
    }
  }
  return sox_true;
}

/* Each frame of the packet in turn, as a frame of silence where it cannot be
 * decoded but says how long it is, so that the timeline holds.
 *
 * A free-format frame gives no bit rate, which libmad finds from where the
 * next frame begins, beyond the packet; the packet being the frame, the rate
 * is instead the least one for which libmad makes the frame as long. */
static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  sox_bool first = !p->started;

  if (!data)
    return SOX_SUCCESS;
  p->started = sox_true;
  if (first && information_frame(p, data, size)) {
    if (codec->params.delay)
      p->skip = p->end = 0;
    return SOX_SUCCESS;
  }
  if (size >= 4 && data[0] == 0xff && (data[1] & 0xe0) == 0xe0 && !(data[2] >> 4) && ((data[2] >> 2) & 3) != 3) {
    static unsigned long const rates[3] = {44100, 48000, 32000};
    unsigned version = (data[1] >> 3) & 3, layer = 4 - ((data[1] >> 1) & 3), pad = (data[2] >> 1) & 1;
    unsigned long rate = rates[(data[2] >> 2) & 3] >> (version == 3? 0 : version == 2? 1 : 2);
    unsigned long slots = layer == 1? 12 : layer == 3 && version != 3? 72 : 144;
    unsigned long units = (unsigned long)(layer == 1? size / 4 : size) - pad;

    p->stream.freerate = (units * rate + slots - 1) / slots;
  }
  if (p->input_size < size + MAD_BUFFER_GUARD) {
    p->input_size = size + MAD_BUFFER_GUARD;
    free(p->input);
    p->input = lsx_malloc(p->input_size);
  }
  memcpy(p->input, data, size);
  memset(p->input + size, 0, MAD_BUFFER_GUARD);
  mad_stream_buffer(&p->stream, p->input, size + MAD_BUFFER_GUARD);
  while (p->stream.next_frame < p->input + size) {
    sox_uint64_t start = p->position, stop;
    unsigned channels, channel;
    size_t from, to, i;
    sox_sample_t * out;
    unsigned char const * main_data;
    size_t main_size;

    if (p->stream.main_data) {
      memcpy(*p->stream.main_data, p->reservoir, p->reservoir_size);
      p->stream.md_len = (unsigned)p->reservoir_size;
    }
    if (mad_frame_decode(&p->frame, &p->stream)) {
      if (p->stream.error == MAD_ERROR_BUFLEN)
        break;
      if (!MAD_RECOVERABLE(p->stream.error)) {
        lsx_fail_errno(codec->ft, SOX_EFMT, "MP3: %s", mad_stream_errorstr(&p->stream));
        return SOX_EOF;
      }
      if (p->stream.error != MAD_ERROR_BADDATAPTR)
        continue;
      mad_frame_mute(&p->frame);
    }
    {
      struct mad_header const * header = &p->frame.header;
      sox_bool mono = MAD_NCHANNELS(header) == 1;
      size_t side = header->flags & MAD_FLAG_LSF_EXT? (mono? 9 : 17) : (mono? 17 : 32);

      main_data = p->stream.this_frame + 4 + (header->flags & MAD_FLAG_PROTECTION? 2 : 0) + side;
    }
    main_size = main_data < p->stream.next_frame? (size_t)(p->stream.next_frame - main_data) : 0;
    if (main_size >= RESERVOIR_SIZE)
      memcpy(p->reservoir, p->stream.next_frame - RESERVOIR_SIZE, p->reservoir_size = RESERVOIR_SIZE);
    else {
      size_t kept = min(p->reservoir_size, RESERVOIR_SIZE - main_size);

      memmove(p->reservoir, p->reservoir + p->reservoir_size - kept, kept);
      memcpy(p->reservoir + kept, main_data, main_size);
      p->reservoir_size = kept + main_size;
    }
    mad_synth_frame(&p->synth, &p->frame);
    channels = p->synth.pcm.channels;
    if (!p->have_params) {
      codec->params.rate = p->synth.pcm.samplerate;
      codec->params.channels = channels;
      p->have_params = sox_true;
    }
    else if (channels != codec->params.channels || p->synth.pcm.samplerate != codec->params.rate) {
      lsx_fail_errno(codec->ft, SOX_EFMT, "MP3 frame parameters differ from the stream's");
      return SOX_EOF;
    }
    p->position = stop = start + p->synth.pcm.length;
    from = (size_t)(min(max(p->skip, start), stop) - start);
    to = p->end? (size_t)(min(max(p->end, start), stop) - start) : p->synth.pcm.length;
    out = lsx_codec_output(codec, (max(to, from) - from) * channels);
    for (i = from; i < to; ++i)
      for (channel = 0; channel < channels; ++channel) {
        mad_fixed_t sample = p->synth.pcm.samples[channel][i];

        sample = sample < -MAD_F_ONE? -MAD_F_ONE : sample >= MAD_F_ONE? MAD_F_ONE - 1 : sample;
        *out++ = sample / (double)MAD_F_ONE;
      }
  }
  return SOX_SUCCESS;
}

static void stop_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  mad_synth_finish(&p->synth);
  mad_frame_finish(&p->frame);
  mad_stream_finish(&p->stream);
  free(p->input);
}

#ifdef HAVE_LAME

static void lame_error(char const * format, va_list args)
{
  if (sox_globals.output_message_handler)
    sox_globals.output_message_handler(1, __FILE__, format, args);
}

static void lame_debug(char const * format, va_list args)
{
  if (sox_globals.output_message_handler)
    sox_globals.output_message_handler(4, __FILE__, format, args);
}

/* -C as the mp3 format takes it: a bit rate in kbit/s, or minus a VBR
 * quality from 0 to 9, with a fraction choosing the encoder's quality, so
 * that 128.2 is 128 kbit/s at quality 2 and -4.2 is VBR 4 at quality 2. */
static int set_compression(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  sox_format_t * ft = codec->ft;
  double whole = floor(fabs(ft->encoding.compression));
  double fraction = fabs(ft->encoding.compression) - whole;
  int quality = fraction == 0? -1 : (int)(fraction * 10 + .5);

  if (ft->encoding.compression < .5) {
    if (lame_get_VBR(p->gfp) == vbr_off)
      lame_set_VBR(p->gfp, vbr_default);
    if (lame_set_VBR_q(p->gfp, (int)whole) < 0) {
      lsx_fail_errno(ft, SOX_EOF, "lame_set_VBR_q(%d) failed (should be between 0 and 9)", (int)whole);
      return SOX_EOF;
    }
  }
  else if (lame_set_brate(p->gfp, (int)whole) < 0) {
    lsx_fail_errno(ft, SOX_EOF, "lame_set_brate(%d) failed", (int)whole);
    return SOX_EOF;
  }
  if (quality >= 0 && lame_set_quality(p->gfp, quality) < 0) {
    lsx_fail_errno(ft, SOX_EOF, "lame_set_quality(%d) failed", quality);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* No information frame is written: the container carries the delay and the
 * padding. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;

  if (!(p->gfp = lame_init())) {
    lsx_fail_errno(ft, SOX_EOF, "Initialization of LAME library failed");
    return SOX_EOF;
  }
  lame_set_errorf(p->gfp, lame_error);
  lame_set_debugf(p->gfp, lame_debug);
  lame_set_msgf(p->gfp, lame_debug);
  if (lame_set_num_channels(p->gfp, (int)params->channels) < 0 || params->channels > 2) {
    lsx_fail_errno(ft, SOX_EOF, "Unsupported number of channels");
    goto error;
  }
  lame_set_in_samplerate(p->gfp, (int)params->rate);
  lame_set_out_samplerate(p->gfp, (int)params->rate);
  lame_set_bWriteVbrTag(p->gfp, 0);
  if (ft->encoding.compression != HUGE_VAL && set_compression(codec) != SOX_SUCCESS)
    goto error;
  if (lame_init_params(p->gfp) < 0) {
    lsx_fail_errno(ft, SOX_EOF, "LAME initialization failed");
    goto error;
  }
  params->delay = (sox_uint64_t)lame_get_encoder_delay(p->gfp) + DECODER_DELAY;
  ft->signal.precision = 24;
  return SOX_SUCCESS;

error:
  lame_close(p->gfp);
  return SOX_EOF;
}

/* Hand on the frames LAME has finished, but for the last few until the end,
 * when the padding is known and is spread over them from the last back. */
static int write_frames(lsx_codec_t * codec, sox_bool end)
{
  priv_t * p = codec->priv;
  size_t pos, size, count = 0, i;
  sox_uint64_t later = 0, padding = 0;
  unsigned frames;

  for (pos = 0; (size = frame_size(p->output + pos, p->output_size - pos, &frames)) &&
      size <= p->output_size - pos; pos += size, ++count)
    later += frames;
  if (end) {
    if (pos != p->output_size) {
      lsx_fail_errno(codec->ft, SOX_EOF, "LAME produced data that is not a whole MP3 frame");
      return SOX_EOF;
    }
    padding = p->coded_frames + later - min(p->coded_frames + later, codec->params.delay + p->input_frames);
  }
  else count -= min(count, HELD_FRAMES);
  for (pos = 0, i = 0; i < count; ++i, pos += size) {
    lsx_packet_t packet;

    size = frame_size(p->output + pos, p->output_size - pos, &frames);
    later -= frames;
    packet.data = p->output + pos;
    packet.size = size;
    packet.frames = frames;
    packet.skip = 0;
    packet.discard = padding > later? (size_t)min(frames, padding - later) : 0;
    p->coded_frames += frames;
    if (codec->write_packet(codec, &packet) != SOX_SUCCESS)
      return SOX_EOF;
  }
  p->output_size -= pos;
  memmove(p->output, p->output + pos, p->output_size);
  return SOX_SUCCESS;
}

static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  unsigned channels = codec->params.channels;
  size_t frames = len / channels, i, room = frames + frames / 4 + 7200;
  int bytes;

  if (p->output_alloc < p->output_size + room) {
    p->output_alloc = p->output_size + room;
    p->output = lsx_realloc(p->output, p->output_alloc);
  }
  if (!buf)
    bytes = lame_encode_flush(p->gfp, p->output + p->output_size, (int)(p->output_alloc - p->output_size));
  else {
    if (p->pcm_size < frames) {
      p->pcm_size = frames;
      free(p->left);
      p->left = lsx_malloc(2 * frames * sizeof(*p->left));
      p->right = p->left + frames;
    }
    for (i = 0; i < frames; ++i) {
      p->left[i] = (float)(32768 * SOX_SAMPLE_TO_FLOAT_32BIT(buf[i * channels],));
      if (channels == 2)
        p->right[i] = (float)(32768 * SOX_SAMPLE_TO_FLOAT_32BIT(buf[i * channels + 1],));
    }
    bytes = lame_encode_buffer_float(p->gfp, p->left, channels == 2? p->right : NULL, (int)frames,
        p->output + p->output_size, (int)(p->output_alloc - p->output_size));
    p->input_frames += frames;
  }
  if (bytes < 0) {
    lsx_fail_errno(codec->ft, SOX_EOF, "Encoding failed");
    return SOX_EOF;
  }
  p->output_size += (size_t)bytes;
  return write_frames(codec, !buf);
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  lame_close(p->gfp);
  free(p->left);
  free(p->output);
}

#else
#define start_encode NULL
#define encode NULL
#define stop_encode NULL
#endif

lsx_codec_handler_t const * lsx_mp3_codec_fn(void)
{
  static unsigned const write_formats[] = {SOX_ENCODING_MP3, 0, 0};
  static sox_rate_t const write_rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0};
  static lsx_codec_handler_t const handler = {
    "mp3", write_formats, write_rates, sizeof(priv_t),
    start_decode, decode, NULL, stop_decode,
    start_encode, encode, stop_encode
  };
  return &handler;
}

#endif
