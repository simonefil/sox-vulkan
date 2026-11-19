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

#include <assert.h>
#include <string.h>

/* The codecs this build has.  A codec SoX implements itself and one backed
 * by FFmpeg are listed alike; only the handler differs. */
static lsx_codec_handler_t const * (* const codec_fns[])(void) = {
  lsx_pcm_codec_fn,
#if defined HAVE_FLAC && (defined STATIC_FLAC || !defined HAVE_LIBLTDL)
  lsx_flac_codec_fn,
#endif
#if defined HAVE_OGG_VORBIS && (defined STATIC_OGG_VORBIS || !defined HAVE_LIBLTDL)
  lsx_vorbis_codec_fn,
#endif
#if defined HAVE_OPUS && (defined STATIC_OPUS || !defined HAVE_LIBLTDL)
  lsx_opus_codec_fn,
#endif
#if defined HAVE_MP3 && defined STATIC_MP3 && defined HAVE_MAD_H && defined HAVE_LAME
  lsx_mp3_codec_fn,
#endif
#if defined HAVE_WAVPACK && (defined STATIC_WAVPACK || !defined HAVE_LIBLTDL)
  lsx_wavpack_codec_fn,
#endif
};

/* Every identifier a container uses for a codec.  A codec the build lacks
 * keeps its identifiers here, so that a file holding it is reported as such
 * rather than as unknown.  Ogg names a codec by the bytes its first packet
 * begins with. */
static lsx_codec_tag_t const codec_tags[] = {
  {lsx_container_matroska, "A_PCM/INT/LIT"   , "pcm"    , SOX_ENCODING_SIGN2   , lsx_endian_little},
  {lsx_container_matroska, "A_PCM/INT/BIG"   , "pcm"    , SOX_ENCODING_SIGN2   , lsx_endian_big},
  {lsx_container_matroska, "A_PCM/FLOAT/IEEE", "pcm"    , SOX_ENCODING_FLOAT   , lsx_endian_little},
  {lsx_container_matroska, "A_FLAC"          , "flac"   , SOX_ENCODING_FLAC    , lsx_endian_any},
  {lsx_container_matroska, "A_VORBIS"        , "vorbis" , SOX_ENCODING_VORBIS  , lsx_endian_any},
  {lsx_container_matroska, "A_OPUS"          , "opus"   , SOX_ENCODING_OPUS    , lsx_endian_any},
  {lsx_container_matroska, "A_MPEG/L3"       , "mp3"    , SOX_ENCODING_MP3     , lsx_endian_any},
  {lsx_container_matroska, "A_WAVPACK4"      , "wavpack", SOX_ENCODING_WAVPACK , lsx_endian_any},
  {lsx_container_matroska, "A_WAVPACK4"      , "wavpack", SOX_ENCODING_WAVPACKF, lsx_endian_any},
  {lsx_container_webm    , "A_VORBIS"        , "vorbis" , SOX_ENCODING_VORBIS  , lsx_endian_any},
  {lsx_container_webm    , "A_OPUS"          , "opus"   , SOX_ENCODING_OPUS    , lsx_endian_any},
  {lsx_container_ogg     , "\x01vorbis"      , "vorbis" , SOX_ENCODING_VORBIS  , lsx_endian_any},
  {lsx_container_ogg     , "OpusHead"        , "opus"   , SOX_ENCODING_OPUS    , lsx_endian_any},
  {lsx_container_ogg     , "\x7f" "FLAC"     , "flac"   , SOX_ENCODING_FLAC    , lsx_endian_any},
};

/* The container each type is, and the codec it writes when its type names
 * none: Ogg has one, Vorbis, which is what `-t ogg' always meant.  The vorbis
 * and opus types are Ogg with the codec fixed: they take no other, and read
 * only that one. */
static struct {
  char const * type;
  lsx_container_family_t container;
  char const * codec;
  sox_bool fixed;
} const container_types[] = {
  {"mka"   , lsx_container_matroska, NULL    , sox_false},
  {"mkv"   , lsx_container_matroska, NULL    , sox_false},
  {"webm"  , lsx_container_webm    , NULL    , sox_false},
  {"ogg"   , lsx_container_ogg     , "vorbis", sox_false},
  {"vorbis", lsx_container_ogg     , "vorbis", sox_true},
  {"opus"  , lsx_container_ogg     , "opus"  , sox_true},
};

/* SoX orders channels as WAVE does, Vorbis as its specification, section
 * 4.3.9, does, and Opus mapping family 1 as Vorbis. */
unsigned char const lsx_vorbis_channel_from_sox[9][8] = {
  {0},
  {0},
  {0, 1},
  {0, 2, 1},
  {0, 1, 2, 3},
  {0, 2, 1, 3, 4},
  {0, 2, 1, 4, 5, 3},
  {0, 2, 1, 5, 6, 4, 3},
  {0, 2, 1, 6, 7, 4, 5, 3}
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

/* The entry of a type that names a container handler, as in mka or ogg:opus. */
static size_t container_type(char const * type)
{
  size_t i, len = strcspn(type, ":");

  for (i = 0; strlen(container_types[i].type) != len ||
      strncasecmp(container_types[i].type, type, len); ++i)
    assert(i + 1 < array_length(container_types));
  return i;
}

/* The codec a container type writes: the one it names, or its default. */
char const * lsx_container_codec(char const * type)
{
  char const * codec = lsx_type_codec(type);

  return codec? codec : container_types[container_type(type)].codec;
}

/* The codec a container type reads: the one it names, or the one it is
 * fixed to; NULL for any. */
char const * lsx_container_requested(char const * type)
{
  size_t i = container_type(type);

  return container_types[i].fixed? container_types[i].codec : lsx_type_codec(type);
}

/* Whether a codec may be named after the container type's colon. */
sox_bool lsx_container_takes_codec(char const * type)
{
  return !container_types[container_type(type)].fixed;
}

lsx_container_family_t lsx_container_family(char const * type)
{
  return container_types[container_type(type)].container;
}

lsx_codec_tag_t const * lsx_codec_tag_by_id(lsx_container_family_t container, char const * id)
{
  size_t i;

  for (i = 0; i < array_length(codec_tags); ++i)
    if (codec_tags[i].container == container && !strcmp(codec_tags[i].id, id))
      return &codec_tags[i];
  return NULL;
}

/* The tag whose identifier the start of a stream holds. */
lsx_codec_tag_t const * lsx_codec_tag_by_magic(lsx_container_family_t container,
    unsigned char const * data, size_t size)
{
  size_t i;

  for (i = 0; i < array_length(codec_tags); ++i) {
    size_t len = strlen(codec_tags[i].id);

    if (codec_tags[i].container == container && size >= len && !memcmp(data, codec_tags[i].id, len))
      return &codec_tags[i];
  }
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
