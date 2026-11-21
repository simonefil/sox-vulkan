/* (c) Simone Filippini <info@simonefilippini.it> 2026
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

/* Shared DTS and DTS-HD elementary stream format implementation. */

#include "sox_i.h"
#include "dts-common.h"
#include "ffmpeg-codec.h"

#ifdef HAVE_FFMPEG_CODECS

/* The dts and dtshd handlers share everything but one check.  A DTS-HD file
 * is a DTS core with extension substreams appended, so the same decoder reads
 * both and the profile it reports is what tells them apart. */
typedef struct {
  lsx_ffmpeg_codec_t * codec;
  sox_bool spatial_metadata_warning_shown;
} priv_t;

/* Encoding is offered by the dts handler alone: FFmpeg's encoder only ever
 * writes a core stream. */
static lsx_ffmpeg_stream_definition_t const definition = {&lsx_ffmpeg_dts_definition, NULL, NULL, NULL};

/* The profiles that carry an extension substream, and so count as DTS-HD.
 * DTS Express is included as a low-rate substream profile. */
static sox_bool is_dtshd_profile(int profile)
{
  switch (profile) {
    case AV_PROFILE_DTS_HD_HRA:
    case AV_PROFILE_DTS_HD_MA:
    case AV_PROFILE_DTS_HD_MA_X:
    case AV_PROFILE_DTS_HD_MA_X_IMAX:
    case AV_PROFILE_DTS_EXPRESS:
      return sox_true;
    default:
      return sox_false;
  }
}

/* Name a profile for the "is X rather than DTS-HD" message, so a rejected
 * file says what it actually is. */
static char const * profile_name(int profile)
{
  switch (profile) {
    case AV_PROFILE_DTS:
      return "DTS core";
    case AV_PROFILE_DTS_ES:
      return "DTS-ES";
    case AV_PROFILE_DTS_96_24:
      return "DTS 96/24";
    case AV_PROFILE_DTS_HD_HRA:
      return "DTS-HD High Resolution Audio";
    case AV_PROFILE_DTS_HD_MA:
      return "DTS-HD Master Audio";
    case AV_PROFILE_DTS_HD_MA_X:
      return "DTS-HD Master Audio with DTS:X";
    case AV_PROFILE_DTS_HD_MA_X_IMAX:
      return "DTS-HD Master Audio with DTS:X IMAX";
    case AV_PROFILE_DTS_EXPRESS:
      return "DTS Express";
    default:
      return "unknown DTS";
  }
}

/* Warn once that a DTS:X file is being decoded as its channel bed, the object
 * metadata being dropped because SoX cannot carry it.  Called from startread
 * and from every read because the profile is only known once the decoder has
 * seen the substream, which for some files is not on the first frame. */
static void warn_spatial_metadata(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context = lsx_ffmpeg_codec_context(p->codec);
  char const * name;

  if (p->spatial_metadata_warning_shown || context == NULL)
    return;
  if (context->profile == AV_PROFILE_DTS_HD_MA_X)
    name = "DTS:X";
  else if (context->profile == AV_PROFILE_DTS_HD_MA_X_IMAX)
    name = "DTS:X IMAX";
  else
    return;
  lsx_warn("`%s': %s detected; spatial metadata will be ignored and "
      "only the channel-based audio presentation will be decoded",
      ft->filename, name);
  p->spatial_metadata_warning_shown = sox_true;
}

/* Open a DTS stream for reading, optionally insisting that it be DTS-HD.
 * A file rejected for not being DTS-HD has already been opened, so the codec
 * state is released before failing. */
static int startread_common(sox_format_t * ft, sox_bool require_dtshd)
{
  priv_t * p = (priv_t *)ft->priv;
  AVCodecContext const * context;
  int result;

  p->spatial_metadata_warning_shown = sox_false;
  result = lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);
  if (result != SOX_SUCCESS)
    return result;
  context = lsx_ffmpeg_codec_context(p->codec);
  if (require_dtshd && (context == NULL || !is_dtshd_profile(context->profile))) {
    char const * name = context == NULL ? "unknown DTS" : profile_name(context->profile);

    lsx_ffmpeg_codec_stopread(&p->codec);
    lsx_fail_errno(ft, SOX_EFMT, "The input is %s rather than DTS-HD", name);
    return SOX_EOF;
  }
  warn_spatial_metadata(ft);
  return SOX_SUCCESS;
}

static int startread_dts(sox_format_t * ft)
{
  return startread_common(ft, sox_false);
}

static int startread_dtshd(sox_format_t * ft)
{
  return startread_common(ft, sox_true);
}

static size_t read_samples(sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;
  size_t result = lsx_ffmpeg_codec_read(ft, p->codec, samples, length);

  warn_spatial_metadata(ft);
  return result;
}

static int stopread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopread(&p->codec);
}

static int startwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startwrite(ft, &p->codec, &definition);
}

static size_t write_samples(sox_format_t * ft, sox_sample_t const * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_write(ft, p->codec, samples, length);
}

static int stopwrite(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_stopwrite(ft, &p->codec);
}

sox_format_handler_t const * lsx_dts_format_handler(void)
{
  static char const * const names[] = {"dts", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_DTS, 0, 0};
  static sox_rate_t const rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "DTS Coherent Acoustics elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread_dts,
    read_samples,
    stopread,
    startwrite,
    write_samples,
    stopwrite,
    NULL,
    encodings,
    rates,
    sizeof(priv_t)
  };

  return &handler;
}

sox_format_handler_t const * lsx_dtshd_format_handler(void)
{
  static char const * const names[] = {"dtshd", NULL};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "DTS-HD elementary stream",
    names,
    SOX_FILE_CODEC_OPTIONS,
    startread_dtshd,
    read_samples,
    stopread,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    sizeof(priv_t)
  };

  return &handler;
}

#endif
