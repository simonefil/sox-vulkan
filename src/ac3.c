/* ATSC A/52 AC-3 elementary stream format.
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

#ifdef HAVE_FFMPEG_CODECS

typedef struct {
  lsx_ffmpeg_codec_t * codec;
} priv_t;

/* AC-3 needs nothing beyond the adapter's defaults: its elementary stream is
 * self-synchronising, so FFmpeg's own bitstream parser finds the frames. */
static lsx_ffmpeg_stream_definition_t const definition = {&lsx_ffmpeg_ac3_definition, NULL, NULL, NULL};

static int startread(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_startread(ft, &p->codec, &definition);
}

static size_t read_samples(sox_format_t * ft, sox_sample_t * samples, size_t length)
{
  priv_t * p = (priv_t *)ft->priv;

  return lsx_ffmpeg_codec_read(ft, p->codec, samples, length);
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

LSX_FORMAT_HANDLER(ac3)
{
  static char const * const names[] = {"ac3", NULL};
  static unsigned const encodings[] = {SOX_ENCODING_AC3, 0, 0};
  static sox_rate_t const rates[] = {32000, 44100, 48000, 0};
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "ATSC A/52 AC-3 lossy audio compression",
    names,
    SOX_FILE_CODEC_OPTIONS | SOX_FILE_CHANNEL_LAYOUT,
    startread,
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

#endif
