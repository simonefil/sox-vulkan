/* MP3 support for SoX
 *
 * Uses libmp3lame for MP3 encoding
 * and libtwolame for MP2 encoding.
 * Reading goes through the MPEG audio container and the MP3 codec.
 *
 * Written by Fabrizio Gennari <fabrizio.ge@tiscali.it>
 */

#include "sox_i.h"
#include "container.h"
#include <string.h>

#if defined(HAVE_LAME_LAME_H) || defined(HAVE_LAME_H) || defined(DL_LAME)
#define HAVE_LAME 1
#endif

#if defined(HAVE_TWOLAME_H) || defined(DL_TWOLAME)
  #define HAVE_TWOLAME 1
#endif

#if defined(HAVE_MAD_H) || defined(HAVE_LAME) || defined(HAVE_TWOLAME)

#if defined(HAVE_LAME_LAME_H)
#include <lame/lame.h>
#elif defined(HAVE_LAME_H)
#include <lame.h>
#elif defined(DL_LAME)
typedef struct lame_global_struct lame_global_flags;
typedef enum {
  vbr_off=0,
  vbr_default=4
} vbr_mode;
#endif

#ifdef HAVE_TWOLAME_H
  #include <twolame.h>
#endif

#ifndef HAVE_LIBLTDL
  #undef DL_LAME
#endif

#define MAXFRAMESIZE 2880
#define ID3PADDING 128

/* LAME takes float values as input. */
#define MP3_LAME_PRECISION 24

static const char* const lame_library_names[] =
{
#ifdef DL_LAME
  "libmp3lame",
  "libmp3lame-0",
  "lame-enc",
  "cygmp3lame-0",
#endif
  NULL
};

#ifdef DL_LAME

  /* Expected to be present in all builds of LAME. */
  #define LAME_FUNC           LSX_DLENTRY_DYNAMIC

  /* id3tag support is an optional component of LAME. Use if available. */
  #define LAME_FUNC_ID3       LSX_DLENTRY_STUB

#else /* DL_LAME */

  /* Expected to be present in all builds of LAME. */
  #define LAME_FUNC           LSX_DLENTRY_STATIC

  /* id3tag support is an optional component of LAME. Use if available. */
  #ifdef HAVE_LAME_ID3TAG
    #define LAME_FUNC_ID3     LSX_DLENTRY_STATIC
  #else
    #define LAME_FUNC_ID3     LSX_DLENTRY_STUB
  #endif

#endif /* DL_LAME */

#define LAME_FUNC_ENTRIES(f,x) \
  LAME_FUNC(f,x, lame_global_flags*, lame_init, (void)) \
  LAME_FUNC(f,x, int, lame_set_errorf, (lame_global_flags *, void (*)(const char *, va_list))) \
  LAME_FUNC(f,x, int, lame_set_debugf, (lame_global_flags *, void (*)(const char *, va_list))) \
  LAME_FUNC(f,x, int, lame_set_msgf, (lame_global_flags *, void (*)(const char *, va_list))) \
  LAME_FUNC(f,x, int, lame_set_num_samples, (lame_global_flags *, unsigned long)) \
  LAME_FUNC(f,x, int, lame_get_num_channels, (const lame_global_flags *)) \
  LAME_FUNC(f,x, int, lame_set_num_channels, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_set_in_samplerate, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_set_out_samplerate, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_set_bWriteVbrTag, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_set_brate, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_set_quality, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, vbr_mode, lame_get_VBR, (const lame_global_flags *)) \
  LAME_FUNC(f,x, int, lame_set_VBR, (lame_global_flags *, vbr_mode)) \
  LAME_FUNC(f,x, int, lame_set_VBR_q, (lame_global_flags *, int)) \
  LAME_FUNC(f,x, int, lame_init_params, (lame_global_flags *)) \
  LAME_FUNC(f,x, int, lame_encode_buffer_float, (lame_global_flags *, const float[], const float[], const int, unsigned char *, const int)) \
  LAME_FUNC(f,x, int, lame_encode_flush, (lame_global_flags *, unsigned char *, int)) \
  LAME_FUNC(f,x, int, lame_close, (lame_global_flags *)) \
  LAME_FUNC(f,x, size_t, lame_get_lametag_frame, (const lame_global_flags *, unsigned char*, size_t)) \
  LAME_FUNC_ID3(f,x, void, id3tag_init, (lame_global_flags *)) \
  LAME_FUNC_ID3(f,x, void, id3tag_set_title, (lame_global_flags *, const char* title)) \
  LAME_FUNC_ID3(f,x, void, id3tag_set_artist, (lame_global_flags *, const char* artist)) \
  LAME_FUNC_ID3(f,x, void, id3tag_set_album, (lame_global_flags *, const char* album)) \
  LAME_FUNC_ID3(f,x, void, id3tag_set_year, (lame_global_flags *, const char* year)) \
  LAME_FUNC_ID3(f,x, void, id3tag_set_comment, (lame_global_flags *, const char* comment)) \
  LAME_FUNC_ID3(f,x, int, id3tag_set_track, (lame_global_flags *, const char* track)) \
  LAME_FUNC_ID3(f,x, int, id3tag_set_genre, (lame_global_flags *, const char* genre)) \
  LAME_FUNC_ID3(f,x, size_t, id3tag_set_pad, (lame_global_flags *, size_t)) \
  LAME_FUNC_ID3(f,x, size_t, lame_get_id3v2_tag, (lame_global_flags *, unsigned char*, size_t)) \
  LAME_FUNC_ID3(f,x, int, id3tag_set_textinfo_utf16, (lame_global_flags *, const char *, const unsigned short *)) \
  LAME_FUNC_ID3(f,x, int, id3tag_set_comment_utf16, (lame_global_flags *, const char *, const unsigned short *, const unsigned short *))

static const char* const twolame_library_names[] =
{
#ifdef DL_TWOLAME
  "libtwolame",
  "libtwolame-0",
#endif
  NULL
};

#ifdef DL_TWOLAME
  #define TWOLAME_FUNC LSX_DLENTRY_DYNAMIC
#else
  #define TWOLAME_FUNC LSX_DLENTRY_STATIC
#endif

#define TWOLAME_FUNC_ENTRIES(f,x) \
  TWOLAME_FUNC(f,x, twolame_options*, twolame_init, (void)) \
  TWOLAME_FUNC(f,x, int, twolame_get_num_channels, (twolame_options*)) \
  TWOLAME_FUNC(f,x, int, twolame_set_num_channels, (twolame_options*, int)) \
  TWOLAME_FUNC(f,x, int, twolame_set_in_samplerate, (twolame_options *, int)) \
  TWOLAME_FUNC(f,x, int, twolame_set_out_samplerate, (twolame_options *, int)) \
  TWOLAME_FUNC(f,x, int, twolame_set_brate, (twolame_options *, int)) \
  TWOLAME_FUNC(f,x, int, twolame_init_params, (twolame_options *)) \
  TWOLAME_FUNC(f,x, int, twolame_encode_buffer_float32_interleaved, (twolame_options *, const float [], int, unsigned char *, int)) \
  TWOLAME_FUNC(f,x, int, twolame_encode_flush, (twolame_options *, unsigned char *, int)) \
  TWOLAME_FUNC(f,x, void, twolame_close, (twolame_options **))

#if defined(HAVE_LAME) || defined(HAVE_TWOLAME)

/* Private data */
typedef struct mp3_priv_t {
  unsigned char *mp3_buffer;
  size_t mp3_buffer_size;
  float *pcm_buffer;
  size_t pcm_buffer_size;
  char mp2;

#ifdef HAVE_LAME
  lame_global_flags *gfp;
  uint64_t num_samples;
  int vbr_tag;
  LSX_DLENTRIES_TO_PTRS(LAME_FUNC_ENTRIES, lame_dl);
#endif

#ifdef HAVE_TWOLAME
  twolame_options *opt;
  LSX_DLENTRIES_TO_PTRS(TWOLAME_FUNC_ENTRIES, twolame_dl);
#endif
} priv_t;

/* Reading keeps the container's state in the same private data. */
#define PRIV_SIZE max(sizeof(priv_t), sizeof(lsx_container_t))

#else
#define PRIV_SIZE sizeof(lsx_container_t)
#endif

#if defined HAVE_MAD_H && (defined STATIC_MP3 || !defined HAVE_LIBLTDL)
static int startread(sox_format_t * ft)
{
  return lsx_container_startread(ft, &lsx_mpeg_audio_ops);
}
#define sox_mp3read lsx_container_read
#define stopread lsx_container_stopread
#define sox_mp3seek lsx_container_seek
#else
static int startread(sox_format_t * ft)
{
  lsx_fail_errno(ft,SOX_EOF,"SoX was compiled without MP3 decoding support");
  return SOX_EOF;
}
#define sox_mp3read NULL
#define stopread NULL
#define sox_mp3seek NULL
#endif

#ifdef HAVE_LAME

/* Adapters for lame message callbacks: */

static void errorf(const char* fmt, va_list va)
{
  sox_globals.subsystem=__FILE__;
  if (sox_globals.output_message_handler)
    (*sox_globals.output_message_handler)(1,sox_globals.subsystem,fmt,va);
  return;
}

static void debugf(const char* fmt, va_list va)
{
  sox_globals.subsystem=__FILE__;
  if (sox_globals.output_message_handler)
    (*sox_globals.output_message_handler)(4,sox_globals.subsystem,fmt,va);
  return;
}

static void msgf(const char* fmt, va_list va)
{
  sox_globals.subsystem=__FILE__;
  if (sox_globals.output_message_handler)
    (*sox_globals.output_message_handler)(3,sox_globals.subsystem,fmt,va);
  return;
}

/* These functions are considered optional. If they aren't present in the
   library, the stub versions defined here will be used instead. */

UNUSED static void id3tag_init_stub(lame_global_flags * gfp UNUSED)
  { return; }
UNUSED static void id3tag_set_title_stub(lame_global_flags * gfp UNUSED, const char* title UNUSED)
  { return; }
UNUSED static void id3tag_set_artist_stub(lame_global_flags * gfp UNUSED, const char* artist UNUSED)
  { return; }
UNUSED static void id3tag_set_album_stub(lame_global_flags * gfp UNUSED, const char* album UNUSED)
  { return; }
UNUSED static void id3tag_set_year_stub(lame_global_flags * gfp UNUSED, const char* year UNUSED)
  { return; }
UNUSED static void id3tag_set_comment_stub(lame_global_flags * gfp UNUSED, const char* comment UNUSED)
  { return; }
UNUSED static void id3tag_set_track_stub(lame_global_flags * gfp UNUSED, const char* track UNUSED)
  { return; }
UNUSED static int id3tag_set_genre_stub(lame_global_flags * gfp UNUSED, const char* genre UNUSED)
  { return 0; }
UNUSED static size_t id3tag_set_pad_stub(lame_global_flags * gfp UNUSED, size_t n UNUSED)
  { return 0; }
UNUSED static size_t lame_get_id3v2_tag_stub(lame_global_flags * gfp UNUSED, unsigned char * buffer UNUSED, size_t size UNUSED)
  { return 0; }
UNUSED static int id3tag_set_textinfo_utf16_stub(lame_global_flags * gfp UNUSED, const char *id UNUSED, const unsigned short *text UNUSED)
  { return 0; }
UNUSED static int id3tag_set_comment_utf16_stub(lame_global_flags * gfp UNUSED, const char *lang UNUSED, const unsigned short *desc UNUSED, const unsigned short *text UNUSED)
  { return 0; }

static void write_comments(sox_format_t * ft)
{
  /* SoX comments are UTF-8, while ID3v1 only holds Latin-1 and LAME's
     char* setters store their argument as Latin-1 in ID3v2 too.  As LAME's
     own frontend does, each value is therefore given twice: converted to
     Latin-1 to the classic setter, which fills ID3v1, and converted to
     UTF-16 to the ID3v2 setter, which replaces the Latin-1 frame the first
     call added.  Discnumber has no ID3v1 field and only gets the second. */
  enum { TITLE, ARTIST, ALBUM, TRACK, YEAR, COMMENT, GENRE, DISC, TAG_COUNT };
  static const struct {
    const char *name;
    const char *frame_id;
  } tags[TAG_COUNT] = {
    [TITLE] = {"Title", "TIT2"},
    [ARTIST] = {"Artist", "TPE1"},
    [ALBUM] = {"Album", "TALB"},
    [TRACK] = {"Tracknumber", "TRCK"},
    [YEAR] = {"Year", "TYER"},
    [COMMENT] = {"Comment", "COMM"},
    [GENRE] = {"Genre", "TCON"},
    [DISC] = {"Discnumber", "TPOS"},
  };
  /* The comment's description is empty but, being UTF-16 like the text,
     still needs its byte order mark: LAME writes a NULL description as a
     bare terminator, which strict readers such as FFmpeg 5 reject. */
  static const unsigned short empty_desc[] = {0xfeff, 0};
  priv_t *p = (priv_t *) ft->priv;
  size_t i;

  p->id3tag_init(p->gfp);
  p->id3tag_set_pad(p->gfp, (size_t)ID3PADDING);

  for (i = 0; i < TAG_COUNT; ++i) {
    const unsigned char *in = (const unsigned char *)sox_find_comment(ft->oob.comments, tags[i].name);
    unsigned short *utf16;
    char *latin1;
    size_t n16 = 0, n1 = 0;

    /* An empty value is left out, as LAME's char* setters do, rather than
       written as an empty ID3v2 frame. */
    if (!in || !*in)
      continue;

    /* A UTF-8 sequence never yields more UTF-16 units than it has bytes;
       the extra two units hold the byte order mark LAME requires and the
       terminator. */
    utf16 = lsx_malloc((strlen((const char *)in) + 2) * sizeof(*utf16));
    latin1 = lsx_malloc(strlen((const char *)in) + 1);
    utf16[n16++] = 0xfeff;
    while (*in) {
      unsigned long c = *in;
      int more = c >= 0xf5 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
      int k;

      /* A byte that does not start a well-formed sequence is taken as a
         Latin-1 character, which keeps comments read from files that
         predate UTF-8 legible. */
      for (k = 1; k <= more && (in[k] & 0xc0) == 0x80; ++k);
      if (more == 0 || k <= more) {
        ++in;
      } else {
        c &= 0x3f >> more;
        for (k = 1; k <= more; ++k)
          c = (c << 6) | (in[k] & 0x3f);
        in += more + 1;
        if (c > 0x10ffff)
          c = 0xfffd;
      }
      if (c >= 0x10000) {
        utf16[n16++] = (unsigned short)(0xd800 + ((c - 0x10000) >> 10));
        utf16[n16++] = (unsigned short)(0xdc00 + (c & 0x3ff));
      } else {
        utf16[n16++] = (unsigned short)c;
      }
      latin1[n1++] = c < 0x100 ? (char)c : '?';
    }
    utf16[n16] = 0;
    latin1[n1] = 0;

    switch (i) {
      case TITLE: p->id3tag_set_title(p->gfp, latin1); break;
      case ARTIST: p->id3tag_set_artist(p->gfp, latin1); break;
      case ALBUM: p->id3tag_set_album(p->gfp, latin1); break;
      case TRACK: p->id3tag_set_track(p->gfp, latin1); break;
      case YEAR: p->id3tag_set_year(p->gfp, latin1); break;
      case COMMENT: p->id3tag_set_comment(p->gfp, latin1); break;
      case GENRE:
        if (p->id3tag_set_genre(p->gfp, latin1))
          lsx_warn("\"%s\" is not a recognized ID3v1 genre.", latin1);
        break;
    }
    if (i == COMMENT)
      p->id3tag_set_comment_utf16(p->gfp, NULL, empty_desc, utf16);
    else
      p->id3tag_set_textinfo_utf16(p->gfp, tags[i].frame_id, utf16);

    free(utf16);
    free(latin1);
  }
}

static int get_id3v2_tag_size(sox_format_t * ft)
{
  size_t bytes_read;
  int id3v2_size;
  unsigned char id3v2_header[10];

  if (lsx_seeki(ft, (off_t)0, SEEK_SET) != 0) {
    lsx_warn("cannot update id3 tag - failed to seek to beginning");
    return SOX_EOF;
  }

  /* read 10 bytes in case there's an ID3 version 2 header here */
  bytes_read = lsx_readbuf(ft, id3v2_header, sizeof(id3v2_header));
  if (bytes_read != sizeof(id3v2_header)) {
    lsx_warn("cannot update id3 tag - failed to read id3 header");
    return SOX_EOF;      /* not readable, maybe opened Write-Only */
  }

  /* does the stream begin with the ID3 version 2 file identifier? */
  if (!strncmp((char *) id3v2_header, "ID3", (size_t)3)) {
    /* the tag size (minus the 10-byte header) is encoded into four
     * bytes where the most significant bit is clear in each byte */
    id3v2_size = (((id3v2_header[6] & 0x7f) << 21)
                    | ((id3v2_header[7] & 0x7f) << 14)
                    | ((id3v2_header[8] & 0x7f) << 7)
                    | (id3v2_header[9] & 0x7f))
        + sizeof(id3v2_header);
  } else {
    /* no ID3 version 2 tag in this stream */
    id3v2_size = 0;
  }
  return id3v2_size;
}

static void rewrite_id3v2_tag(sox_format_t * ft, size_t id3v2_size, uint64_t num_samples)
{
  priv_t *p = (priv_t *)ft->priv;
  size_t new_size;
  unsigned char * buffer;

  if (LSX_DLFUNC_IS_STUB(p, lame_get_id3v2_tag))
  {
    if (p->num_samples)
      lsx_warn("cannot update track length info - tag update not supported with this version of LAME. Track length will be incorrect.");
    else
      lsx_report("cannot update track length info - tag update not supported with this version of LAME. Track length will be unspecified.");
    return;
  }

  buffer = lsx_malloc(id3v2_size);
  if (!buffer)
  {
    lsx_warn("cannot update track length info - failed to allocate buffer");
    return;
  }

  if (num_samples > ULONG_MAX)
  {
    lsx_warn("cannot accurately update track length info - file is too long");
    num_samples = 0;
  }
  p->lame_set_num_samples(p->gfp, (unsigned long)num_samples);
  lsx_debug("updated MP3 TLEN to %lu samples", (unsigned long)num_samples);

  new_size = p->lame_get_id3v2_tag(p->gfp, buffer, id3v2_size);

  if (new_size != id3v2_size && new_size-ID3PADDING <= id3v2_size) {
    p->id3tag_set_pad(p->gfp, ID3PADDING + id3v2_size - new_size);
    new_size = p->lame_get_id3v2_tag(p->gfp, buffer, id3v2_size);
  }

  if (new_size != id3v2_size) {
    if (LSX_DLFUNC_IS_STUB(p, id3tag_set_pad))
    {
      if (p->num_samples)
        lsx_warn("cannot update track length info - tag size adjustment not supported with this version of LAME. Track length will be invalid.");
      else
        lsx_report("cannot update track length info - tag size adjustment not supported with this version of LAME. Track length will be unspecified.");
    }
    else
      lsx_warn("cannot update track length info - failed to adjust tag size");
  } else {
    lsx_seeki(ft, (off_t)0, SEEK_SET);
    /* Overwrite the Id3v2 tag (this time TLEN should be accurate) */
    if (lsx_writebuf(ft, buffer, id3v2_size) != 1) {
      lsx_debug("Rewrote Id3v2 tag (%" PRIuPTR " bytes)", id3v2_size);
    }
  }

  free(buffer);
}

static void rewrite_tags(sox_format_t * ft, uint64_t num_samples)
{
  priv_t *p = (priv_t *)ft->priv;

  off_t file_size;
  size_t id3v2_size;

  if (lsx_seeki(ft, (off_t)0, SEEK_END)) {
    lsx_warn("cannot update tags - seek to end failed");
    return;
  }

  /* Get file size */
  file_size = lsx_tell(ft);

  if (file_size == 0) {
    lsx_warn("cannot update tags - file size is 0");
    return;
  }

  id3v2_size = get_id3v2_tag_size(ft);
  if (id3v2_size > 0 && num_samples != p->num_samples) {
    rewrite_id3v2_tag(ft, id3v2_size, num_samples);
  }

  if (p->vbr_tag) {
    size_t lametag_size;
    uint8_t buffer[MAXFRAMESIZE];

    if (lsx_seeki(ft, (off_t)id3v2_size, SEEK_SET)) {
      lsx_warn("cannot write VBR tag - seek to tag block failed");
      return;
    }

    lametag_size = p->lame_get_lametag_frame(p->gfp, buffer, sizeof(buffer));
    if (lametag_size > sizeof(buffer)) {
      lsx_warn("cannot write VBR tag - VBR tag too large for buffer");
      return;
    }

    if (lametag_size < 1) {
      return;
    }

    if (lsx_writebuf(ft, buffer, lametag_size) != lametag_size) {
      lsx_warn("cannot write VBR tag - VBR tag write failed");
    } else {
      lsx_debug("rewrote VBR tag (%" PRIuPTR " bytes)", lametag_size);
    }
  }
}

#endif /* HAVE_LAME */

#if defined(HAVE_LAME) || defined(HAVE_TWOLAME)

#define LAME_BUFFER_SIZE(num_samples) (((num_samples) + 3) / 4 * 5 + 7200)

static int startwrite(sox_format_t * ft)
{
  priv_t *p = (priv_t *) ft->priv;
  int openlibrary_result;
  int fail = 0;

  if (ft->encoding.encoding != SOX_ENCODING_MP3) {
    if(ft->encoding.encoding != SOX_ENCODING_UNKNOWN)
      lsx_report("Encoding forced to MP2/MP3");
    ft->encoding.encoding = SOX_ENCODING_MP3;
  }

  if(strchr(ft->filetype, '2'))
      p->mp2 = 1;

  if (p->mp2) {
#ifdef HAVE_TWOLAME
    LSX_DLLIBRARY_OPEN(
        p,
        twolame_dl,
        TWOLAME_FUNC_ENTRIES,
        "Twolame encoder library",
        twolame_library_names,
        openlibrary_result);
#else
    lsx_fail_errno(ft,SOX_EOF,"SoX was compiled without MP2 encoding support");
    return SOX_EOF;
#endif
  } else {
#ifdef HAVE_LAME
    LSX_DLLIBRARY_OPEN(
        p,
        lame_dl,
        LAME_FUNC_ENTRIES,
        "LAME encoder library",
        lame_library_names,
        openlibrary_result);
#else
    lsx_fail_errno(ft,SOX_EOF,"SoX was compiled without MP3 encoding support");
    return SOX_EOF;
#endif
  }
  if (openlibrary_result)
    return SOX_EOF;

  p->mp3_buffer_size = LAME_BUFFER_SIZE(sox_globals.bufsiz / max(ft->signal.channels, 1));
  p->mp3_buffer = lsx_malloc(p->mp3_buffer_size);

  p->pcm_buffer_size = sox_globals.bufsiz * sizeof(float);
  p->pcm_buffer = lsx_malloc(p->pcm_buffer_size);

  if (p->mp2) {
#ifdef HAVE_TWOLAME
    p->opt = p->twolame_init();

    if (p->opt == NULL){
      lsx_fail_errno(ft,SOX_EOF,"Initialization of Twolame library failed");
      return(SOX_EOF);
    }
#endif
  } else {
#ifdef HAVE_LAME
    p->gfp = p->lame_init();

    if (p->gfp == NULL){
      lsx_fail_errno(ft,SOX_EOF,"Initialization of LAME library failed");
      return(SOX_EOF);
    }

    /* First set message callbacks so we don't miss any messages: */
    p->lame_set_errorf(p->gfp,errorf);
    p->lame_set_debugf(p->gfp,debugf);
    p->lame_set_msgf  (p->gfp,msgf);

    p->num_samples = ft->signal.length == SOX_IGNORE_LENGTH ? 0 : ft->signal.length / max(ft->signal.channels, 1);
    p->lame_set_num_samples(p->gfp, p->num_samples > ULONG_MAX ? 0 : (unsigned long)p->num_samples);
#endif
  }

  ft->signal.precision = MP3_LAME_PRECISION;

  if (ft->signal.channels != SOX_ENCODING_UNKNOWN) {
    if (p->mp2) {
#ifdef HAVE_TWOLAME
      fail = (p->twolame_set_num_channels(p->opt,(int)ft->signal.channels) != 0);
#endif
    } else {
#ifdef HAVE_LAME
      fail = (p->lame_set_num_channels(p->gfp,(int)ft->signal.channels) < 0);
#endif
    }
    if (fail) {
      lsx_fail_errno(ft,SOX_EOF,"Unsupported number of channels");
      return(SOX_EOF);
    }
  }
  else {
    if (p->mp2) {
#ifdef HAVE_TWOLAME
      ft->signal.channels = p->twolame_get_num_channels(p->opt); /* Twolame default */
#endif
    } else {
#ifdef HAVE_LAME
      ft->signal.channels = p->lame_get_num_channels(p->gfp); /* LAME default */
#endif
    }
  }

  if (p->mp2) {
#ifdef HAVE_TWOLAME
    p->twolame_set_in_samplerate(p->opt,(int)ft->signal.rate);
    p->twolame_set_out_samplerate(p->opt,(int)ft->signal.rate);
#endif
  } else {
#ifdef HAVE_LAME
    p->lame_set_in_samplerate(p->gfp,(int)ft->signal.rate);
    p->lame_set_out_samplerate(p->gfp,(int)ft->signal.rate);
#endif
  }

  if (!p->mp2) {
#ifdef HAVE_LAME
    if (!LSX_DLFUNC_IS_STUB(p, id3tag_init))
      write_comments(ft);
#endif
  }

  /* The primary parameter to the LAME encoder is the bit rate. If the
   * value of encoding.compression is a positive integer, it's taken as
   * the bitrate in kbps (that is if you specify 128, it use 128 kbps).
   *
   * The second most important parameter is probably "quality" (really
   * performance), which allows balancing encoding speed vs. quality.
   * In LAME, 0 specifies highest quality but is very slow, while
   * 9 selects poor quality, but is fast. (5 is the default and 2 is
   * recommended as a good trade-off for high quality encodes.)
   *
   * Because encoding.compression is a float, the fractional part is used
   * to select quality. 128.2 selects 128 kbps encoding with a quality
   * of 2. There is one problem with this approach. We need 128 to specify
   * 128 kbps encoding with default quality, so .0 means use default. Instead
   * of .0 you have to use .01 to specify the highest quality (128.01).
   *
   * LAME uses bitrate to specify a constant bitrate, but higher quality
   * can be achieved using Variable Bit Rate (VBR). VBR quality (really
   * size) is selected using a number from 0 to 9. Use a value of 0 for high
   * quality, larger files, and 9 for smaller files of lower quality. 4 is
   * the default.
   *
   * In order to squeeze the selection of VBR into the encoding.compression
   * float we use negative numbers to select VRR. -4.2 would select default
   * VBR encoding (size) with high quality (speed). One special case is 0,
   * which is a valid VBR encoding parameter but not a valid bitrate.
   * Compression value of 0 is always treated as a high quality vbr, as a
   * result both -0.2 and 0.2 are treated as highest quality VBR (size) and
   * high quality (speed).
   *
   * Note: It would have been nice to simply use low values, 0-9, to trigger
   * VBR mode, but 8 kbps is a valid bit rate, so negative values were
   * used instead.
  */

  lsx_debug("-C option is %f", ft->encoding.compression);

  if (ft->encoding.compression == HUGE_VAL) {
    /* Do nothing, use defaults: */
    lsx_report("using %s encoding defaults", p->mp2? "MP2" : "MP3");
  } else {
    double abs_compression = fabs(ft->encoding.compression);
    double floor_compression = floor(abs_compression);
    double fraction_compression = abs_compression - floor_compression;
    int bitrate_q = (int)floor_compression;
    int encoder_q =
        fraction_compression == 0.0
        ? -1
        : (int)(fraction_compression * 10.0 + 0.5);

    if (ft->encoding.compression < 0.5) {
      if (p->mp2) {
        lsx_fail_errno(ft,SOX_EOF,"Variable bitrate encoding not supported for MP2 audio");
        return(SOX_EOF);
      }
#ifdef HAVE_LAME
      if (p->lame_get_VBR(p->gfp) == vbr_off)
        p->lame_set_VBR(p->gfp, vbr_default);

      if (ft->seekable) {
        p->vbr_tag = 1;
      } else {
        lsx_warn("unable to write VBR tag because we can't seek");
      }

      if (p->lame_set_VBR_q(p->gfp, bitrate_q) < 0)
      {
        lsx_fail_errno(ft, SOX_EOF,
          "lame_set_VBR_q(%d) failed (should be between 0 and 9)",
          bitrate_q);
        return(SOX_EOF);
      }
      lsx_report("lame_set_VBR_q(%d)", bitrate_q);
#endif
    } else {
      if (p->mp2) {
#ifdef HAVE_TWOLAME
        fail = (p->twolame_set_brate(p->opt, bitrate_q) != 0);
#endif
      } else {
#ifdef HAVE_LAME
        fail = (p->lame_set_brate(p->gfp, bitrate_q) < 0);
#endif
      }
      if (fail) {
        lsx_fail_errno(ft, SOX_EOF,
          "%slame_set_brate(%d) failed", p->mp2? "two" : "", bitrate_q);
        return(SOX_EOF);
      }
      lsx_report("(two)lame_set_brate(%d)", bitrate_q);
    }

    /* Set Quality */

    if (encoder_q < 0 || p->mp2) {
      /* use default quality value */
      lsx_report("using %s default quality", p->mp2? "MP2" : "MP3");
    } else {
#ifdef HAVE_LAME
      if (p->lame_set_quality(p->gfp, encoder_q) < 0) {
        lsx_fail_errno(ft, SOX_EOF,
          "lame_set_quality(%d) failed", encoder_q);
        return(SOX_EOF);
      }
      lsx_report("lame_set_quality(%d)", encoder_q);
#endif
    }
  }

  if (!p->mp2) {
#ifdef HAVE_LAME
    p->lame_set_bWriteVbrTag(p->gfp, p->vbr_tag);
#endif
  }

  if (p->mp2) {
#ifdef HAVE_TWOLAME
    fail = (p->twolame_init_params(p->opt) != 0);
#endif
  } else {
#ifdef HAVE_LAME
    fail = (p->lame_init_params(p->gfp) < 0);
#endif
  }
  if (fail) {
    lsx_fail_errno(ft,SOX_EOF,"%s initialization failed", p->mp2? "Twolame" : "LAME");
    return(SOX_EOF);
  }

  return(SOX_SUCCESS);
}

#define MP3_SAMPLE_TO_FLOAT(d) ((float)(32768*SOX_SAMPLE_TO_FLOAT_32BIT(d,)))

static size_t sox_mp3write(sox_format_t * ft, const sox_sample_t *buf, size_t samp)
{
    priv_t *p = (priv_t *)ft->priv;
    size_t new_buffer_size;
    float *buffer_l, *buffer_r = NULL;
    int nsamples = samp/ft->signal.channels;
    int i,j;
    int written = 0;
    SOX_SAMPLE_LOCALS;

    new_buffer_size = samp * sizeof(float);
    if (p->pcm_buffer_size < new_buffer_size) {
      float *new_buffer = lsx_realloc(p->pcm_buffer, new_buffer_size);
      if (!new_buffer) {
        lsx_fail_errno(ft, SOX_ENOMEM, "Out of memory");
        return 0;
      }
      p->pcm_buffer_size = new_buffer_size;
      p->pcm_buffer = new_buffer;
    }

    buffer_l = p->pcm_buffer;

    if (p->mp2)
    {
        size_t s;
        for(s = 0; s < samp; s++)
            buffer_l[s] = SOX_SAMPLE_TO_FLOAT_32BIT(buf[s],);
    }
    else
    {
        if (ft->signal.channels == 2)
        {
            /* lame doesn't support interleaved samples for floats so we must break
             * them out into seperate buffers.
             */
            buffer_r = p->pcm_buffer + nsamples;
            j=0;
            for (i = 0; i < nsamples; i++)
            {
                buffer_l[i] = MP3_SAMPLE_TO_FLOAT(buf[j++]);
                buffer_r[i] = MP3_SAMPLE_TO_FLOAT(buf[j++]);
            }
        }
        else
        {
            j=0;
            for (i = 0; i < nsamples; i++) {
                buffer_l[i] = MP3_SAMPLE_TO_FLOAT(buf[j++]);
            }
        }
    }

    new_buffer_size = LAME_BUFFER_SIZE(nsamples);
    if (p->mp3_buffer_size < new_buffer_size) {
      unsigned char *new_buffer = lsx_realloc(p->mp3_buffer, new_buffer_size);
      if (!new_buffer) {
        lsx_fail_errno(ft, SOX_ENOMEM, "Out of memory");
        return 0;
      }
      p->mp3_buffer_size = new_buffer_size;
      p->mp3_buffer = new_buffer;
    }

    if(p->mp2) {
#ifdef HAVE_TWOLAME
        written = p->twolame_encode_buffer_float32_interleaved(p->opt, buffer_l,
                  nsamples, p->mp3_buffer, (int)p->mp3_buffer_size);
#endif
    } else {
#ifdef HAVE_LAME
        written = p->lame_encode_buffer_float(p->gfp, buffer_l, buffer_r,
                  nsamples, p->mp3_buffer, (int)p->mp3_buffer_size);
#endif
    }
    if (written < 0) {
        lsx_fail_errno(ft,SOX_EOF,"Encoding failed");
        return 0;
    }

    if (lsx_writebuf(ft, p->mp3_buffer, (size_t)written) < (size_t)written)
    {
        lsx_fail_errno(ft,SOX_EOF,"File write failed");
        return 0;
    }

    return samp;
}

static int stopwrite(sox_format_t * ft)
{
  priv_t *p = (priv_t *) ft->priv;
  uint64_t num_samples = ft->olength == SOX_IGNORE_LENGTH ? 0 : ft->olength / max(ft->signal.channels, 1);
  int written = 0;

  if (p->mp2) {
#ifdef HAVE_TWOLAME
    written = p->twolame_encode_flush(p->opt, p->mp3_buffer, (int)p->mp3_buffer_size);
#endif
  } else {
#ifdef HAVE_LAME
    written = p->lame_encode_flush(p->gfp, p->mp3_buffer, (int)p->mp3_buffer_size);
#endif
  }
  if (written < 0)
    lsx_fail_errno(ft, SOX_EOF, "Encoding failed");
  else if (lsx_writebuf(ft, p->mp3_buffer, (size_t)written) < (size_t)written)
    lsx_fail_errno(ft, SOX_EOF, "File write failed");
  else if (!p->mp2) {
#ifdef HAVE_LAME
    if (ft->seekable && (num_samples != p->num_samples || p->vbr_tag))
      rewrite_tags(ft, num_samples);
#endif
  }

  free(p->mp3_buffer);
  free(p->pcm_buffer);

  if(p->mp2) {
#ifdef HAVE_TWOLAME
    p->twolame_close(&p->opt);
    LSX_DLLIBRARY_CLOSE(p, twolame_dl);
#endif
  } else {
#ifdef HAVE_LAME
    p->lame_close(p->gfp);
    LSX_DLLIBRARY_CLOSE(p, lame_dl);
#endif
  }
  return SOX_SUCCESS;
}

#else /* !(HAVE_LAME || HAVE_TWOLAME) */
static int startwrite(sox_format_t * ft UNUSED)
{
  lsx_fail_errno(ft,SOX_EOF,"SoX was compiled with neither MP2 nor MP3 encoding support");
  return SOX_EOF;
}
#define sox_mp3write NULL
#define stopwrite NULL
#endif /* HAVE_LAME || HAVE_TWOLAME */

LSX_FORMAT_HANDLER(mp3)
{
  static char const * const names[] = {"mp3", "mp2", "audio/mpeg", NULL};
  static unsigned const write_encodings[] = {
    SOX_ENCODING_MP3, 0, 0};
  static sox_rate_t const write_rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "MPEG Layer 2/3 lossy audio compression", names, 0,
    startread, sox_mp3read, stopread,
    startwrite, sox_mp3write, stopwrite,
    sox_mp3seek, write_encodings, write_rates, PRIV_SIZE
  };
  return &handler;
}
#endif /* defined(HAVE_MAD_H) || defined(HAVE_LAME) || defined(HAVE_TWOLAME) */
