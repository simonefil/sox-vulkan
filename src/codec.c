/* libSoX packet codecs: the registry, and what all codecs share.
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

#include "codec.h"

#include <string.h>

/* The codecs this build has.  A codec SoX implements itself and one backed
 * by FFmpeg are listed alike; only the handler differs. */
static lsx_codec_handler_t const * (* const codec_fns[])(void) = {
  lsx_pcm_codec_fn,
#if defined HAVE_FLAC && (defined STATIC_FLAC || !defined HAVE_LIBLTDL)
  lsx_flac_codec_fn,
#endif
};

/* Every identifier a container uses for a codec.  A codec the build lacks
 * keeps its identifiers here, so that a file holding it is reported as such
 * rather than as unknown. */
static lsx_codec_tag_t const codec_tags[] = {
  {lsx_container_matroska, "A_PCM/INT/LIT"   , "pcm" , SOX_ENCODING_SIGN2, lsx_endian_little},
  {lsx_container_matroska, "A_PCM/INT/BIG"   , "pcm" , SOX_ENCODING_SIGN2, lsx_endian_big},
  {lsx_container_matroska, "A_PCM/FLOAT/IEEE", "pcm" , SOX_ENCODING_FLOAT, lsx_endian_little},
  {lsx_container_matroska, "A_FLAC"          , "flac", SOX_ENCODING_FLAC , lsx_endian_any},
};

lsx_codec_handler_t const * lsx_codec_find(char const * name)
{
  size_t i;

  if (name)
    for (i = 0; i < array_length(codec_fns); ++i)
      if (!strcasecmp(codec_fns[i]()->name, name))
        return codec_fns[i]();
  return NULL;
}

/* The names of the codecs this build has, for messages. */
char const * lsx_codec_names(void)
{
  static char names[256];
  size_t i;

  if (!names[0])
    for (i = 0; i < array_length(codec_fns); ++i) {
      if (i)
        strcat(names, " ");
      strcat(names, codec_fns[i]()->name);
    }
  return names;
}

/* The codec a type names after its colon, as flac in mka:flac, or NULL. */
char const * lsx_type_codec(char const * type)
{
  char const * colon = type? strchr(type, ':') : NULL;

  return colon? colon + 1 : NULL;
}

lsx_codec_tag_t const * lsx_codec_tag_by_id(lsx_container_family_t container, char const * id)
{
  size_t i;

  for (i = 0; i < array_length(codec_tags); ++i)
    if (codec_tags[i].container == container && !strcmp(codec_tags[i].id, id))
      return &codec_tags[i];
  return NULL;
}

lsx_codec_tag_t const * lsx_codec_tag_by_codec(lsx_container_family_t container,
    char const * codec, sox_encoding_t encoding, sox_bool big_endian)
{
  size_t i;

  for (i = 0; i < array_length(codec_tags); ++i) {
    lsx_codec_tag_t const * tag = &codec_tags[i];

    if (tag->container == container && !strcasecmp(tag->codec, codec) &&
        tag->encoding == encoding &&
        (tag->endian == lsx_endian_any || (tag->endian == lsx_endian_big) == big_endian))
      return tag;
  }
  return NULL;
}

/* Room for len more decoded samples behind those not yet handed out. */
sox_sample_t * lsx_codec_output(lsx_codec_t * codec, size_t len)
{
  sox_sample_t * output;

  if (codec->samples_start == codec->samples_end)
    codec->samples_start = codec->samples_end = 0;
  if (codec->samples_end + len > codec->samples_size) {
    codec->samples_size = max(codec->samples_end + len, codec->samples_size * 2);
    codec->samples = lsx_realloc(codec->samples, codec->samples_size * sizeof(*codec->samples));
  }
  output = codec->samples + codec->samples_end;
  codec->samples_end += len;
  return output;
}
