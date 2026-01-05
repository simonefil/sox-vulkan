/* libSoX Opus-in-Ogg sound format handler
 * Copyright (C) 2013 John Stumpo <stump@jstump.com>
 *
 * Largely based on vorbis.c:
 * libSoX Ogg Vorbis sound format handler
 * Copyright 2001, Stan Seibert <indigo@aztec.asu.edu>
 *
 * Portions from oggenc, (c) Michael Smith <msmith@labyrinth.net.au>,
 * ogg123, (c) Kenneth Arnold <kcarnold@yahoo.com>, and
 * libvorbisfile (c) Xiphophorus Company
 *
 * May 9, 2001 - Stan Seibert (indigo@aztec.asu.edu)
 * Ogg Vorbis handler initially written.
 *
 * July 5, 1991 - Skeleton file
 * Copyright 1991 Lance Norskog And Sundry Contributors
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained.
 * Lance Norskog And Sundry Contributors are not responsible for
 * the consequences of using this software.
 */

#include "sox_i.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_OPUSFILE
#include <opusfile.h>
#endif

#ifdef HAVE_OPUSENC
#include <opusenc.h>
#endif

#define DEF_BUF_LEN 4096
#define OPUS_OUTPUT_RATE 48000

#define BUF_ERROR -1
#define BUF_EOF  0
#define BUF_DATA 1

#if defined(HAVE_OPUSFILE) || defined(HAVE_OPUSENC)
/*
 * SoX PCM follows the canonical WAVE channel order used by wav.c, while
 * Opus mapping family 1 uses Vorbis channel order.  Each row maps a Vorbis
 * channel index to the corresponding SoX channel index.
 */
static unsigned char const vorbis_channel_from_sox[9][8] = {
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
#endif

typedef struct {
#ifdef HAVE_OPUSFILE
  /* Decoding data */
  OggOpusFile *of;
  char *buf;
  size_t buf_len;
  size_t start;
  size_t end;     /* Unsent data samples in buf[start] through buf[end-1] */
  int current_section;
  int eof;
#endif
#ifdef HAVE_OPUSENC
  /* Encoding data */
  OggOpusEnc *encoder;
  float *write_buf;
  size_t write_buf_len;
#endif
} priv_t;

#ifdef HAVE_OPUSFILE
/******** Callback functions used in op_open_callbacks ************/

static int callback_read(void* ft_data, unsigned char* ptr, int nbytes)
{
  sox_format_t* ft = (sox_format_t*)ft_data;
  return lsx_readbuf(ft, ptr, (size_t)nbytes);
}

static int callback_seek(void* ft_data, opus_int64 off, int whence)
{
  sox_format_t* ft = (sox_format_t*)ft_data;
  int ret = ft->seekable ? lsx_seeki(ft, (off_t)off, whence) : -1;

  if (ret == EBADF)
    ret = -1;
  return ret;
}
#endif

static int callback_close(void* ft_data UNUSED)
{
  /* Do nothing so sox can close the file for us */
  return 0;
}

#ifdef HAVE_OPUSFILE
static opus_int64 callback_tell(void* ft_data)
{
  sox_format_t* ft = (sox_format_t*)ft_data;
  return lsx_tell(ft);
}

/********************* End callbacks *****************************/

static int validate_opus_layout(sox_format_t * ft, const OpusHead * head,
    unsigned expected_channels)
{
  if (head == NULL) {
    lsx_fail_errno(ft, SOX_EHDR, "Opus stream is missing its ID header");
    return SOX_EOF;
  }
  if (head->channel_count < 1 || head->channel_count > 8) {
    lsx_fail_errno(ft, SOX_EFMT,
        "Opus decoding supports standard layouts with 1 to 8 channels");
    return SOX_EOF;
  }
  if (head->mapping_family != 0 && head->mapping_family != 1) {
    lsx_fail_errno(ft, SOX_EFMT,
        "Unsupported Opus channel mapping family %d", head->mapping_family);
    return SOX_EOF;
  }
  if (head->mapping_family == 0 && head->channel_count > 2) {
    lsx_fail_errno(ft, SOX_EHDR,
        "Opus mapping family 0 is invalid for %d channels",
        head->channel_count);
    return SOX_EOF;
  }
  if (expected_channels &&
      (unsigned)head->channel_count != expected_channels) {
    lsx_fail_errno(ft, SOX_EFMT,
        "Chained Opus streams with different channel counts are unsupported");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static void reorder_vorbis_to_sox(opus_int16 * pcm, int frames,
    unsigned channels)
{
  int frame;

  if (channels <= 2)
    return;
  for (frame = 0; frame < frames; ++frame) {
    opus_int16 source[8];
    unsigned vorbis_channel;

    memcpy(source, pcm + (size_t)frame * channels,
        channels * sizeof(*source));
    for (vorbis_channel = 0; vorbis_channel < channels; ++vorbis_channel)
      pcm[(size_t)frame * channels +
          vorbis_channel_from_sox[channels][vorbis_channel]] =
          source[vorbis_channel];
  }
}

/*
 * Do anything required before you start reading samples.
 * Read file header.
 *      Find out sampling rate,
 *      size and encoding of samples,
 *      mono/stereo/quad.
 */
static int startread(sox_format_t * ft)
{
  priv_t * vb = (priv_t *) ft->priv;
  const OpusHead *oh;
  const OpusTags *ot;
  int i;

  OpusFileCallbacks callbacks = {
    callback_read,
    callback_seek,
    callback_tell,
    callback_close
  };

  /* Init the decoder */
  vb->of = op_open_callbacks(ft, &callbacks, NULL, (size_t) 0, NULL);
  if (vb->of == NULL) {
    lsx_fail_errno(ft, SOX_EHDR, "Input not an Ogg Opus audio stream");
    return (SOX_EOF);
  }

  /* Get info about the Opus stream */
  oh = op_head(vb->of, 0);
  if (validate_opus_layout(ft, oh, 0) != SOX_SUCCESS) {
    op_free(vb->of);
    vb->of = NULL;
    return SOX_EOF;
  }
  if (ft->seekable) {
    int links = op_link_count(vb->of);

    for (i = 1; i < links; ++i) {
      if (validate_opus_layout(ft, op_head(vb->of, i),
            (unsigned)oh->channel_count) != SOX_SUCCESS) {
        op_free(vb->of);
        vb->of = NULL;
        return SOX_EOF;
      }
    }
  }
  ot = op_tags(vb->of, -1);

  /* Record audio info */
  ft->signal.rate = 48000;  /* libopusfile always uses 48 kHz */
  ft->encoding.encoding = SOX_ENCODING_OPUS;
  ft->signal.channels = (unsigned)oh->channel_count;

  /* op_pcm_total doesn't work on non-seekable files so
   * skip that step in that case.  Also, it reports
   * "frame"-ish results so we must * channels.
   */
  if (ft->seekable)
    ft->signal.length = op_pcm_total(vb->of, -1) * ft->signal.channels;

  /* Record comments */
  for (i = 0; i < ot->comments; i++)
    sox_append_comment(&ft->oob.comments, ot->user_comments[i]);

  /* Setup buffer */
  vb->buf_len = DEF_BUF_LEN;
  vb->buf_len -= vb->buf_len % (ft->signal.channels*2); /* 2 bytes per sample */
  vb->buf = lsx_calloc(vb->buf_len, sizeof(char));
  vb->start = vb->end = 0;

  /* Fill in other info */
  vb->eof = 0;
  vb->current_section = -1;

  return (SOX_SUCCESS);
}


/* Refill the buffer with samples.  Returns BUF_EOF if the end of the
 * Opus data was reached while the buffer was being filled,
 * BUF_ERROR is something bad happens, and BUF_DATA otherwise */
static int refill_buffer(sox_format_t * ft)
{
  priv_t * vb = (priv_t *) ft->priv;
  int num_read;

  if (vb->start == vb->end)     /* Samples all played */
    vb->start = vb->end = 0;

  while (vb->end < vb->buf_len) {
    int previous_section = vb->current_section;
    opus_int16 *pcm = (opus_int16 *)(vb->buf + vb->end);

    num_read = op_read(vb->of, (opus_int16*) (vb->buf + vb->end),
        (int) ((vb->buf_len - vb->end) / sizeof(opus_int16)),
        &vb->current_section);
    if (num_read == 0)
      return (BUF_EOF);
    else if (num_read == OP_HOLE)
      lsx_warn("Warning: hole in stream; probably harmless");
    else if (num_read < 0)
      return (BUF_ERROR);
    else {
      if (vb->current_section != previous_section &&
          validate_opus_layout(ft, op_head(vb->of, vb->current_section),
            ft->signal.channels) != SOX_SUCCESS)
        return BUF_ERROR;
      reorder_vorbis_to_sox(pcm, num_read, ft->signal.channels);
      vb->end += num_read * sizeof(opus_int16) * ft->signal.channels;
    }
  }
  return (BUF_DATA);
}


/*
 * Read up to len samples from file.
 * Convert to signed longs.
 * Place in buf[].
 * Return number of samples read.
 */

static size_t read_samples(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  priv_t * vb = (priv_t *) ft->priv;
  size_t i;
  int ret;


  for (i = 0; i < len; i++) {
    if (vb->start == vb->end) {
      if (vb->eof)
        break;
      ret = refill_buffer(ft);
      if (ret == BUF_EOF || ret == BUF_ERROR) {
        vb->eof = 1;
        if (vb->end == 0)
          break;
      }
    }

    *(buf + i) = SOX_SIGNED_16BIT_TO_SAMPLE(
        ((opus_int16 *)vb->buf)[vb->start / sizeof(opus_int16)], ft->clips);
    vb->start += 2;
  }
  return i;
}

/*
 * Do anything required when you stop reading samples.
 * Don't close input file!
 */
static int stopread(sox_format_t * ft)
{
  priv_t * vb = (priv_t *) ft->priv;

  free(vb->buf);
  op_free(vb->of);

  return (SOX_SUCCESS);
}

static int seek(sox_format_t * ft, uint64_t offset)
{
  priv_t * vb = (priv_t *) ft->priv;

  return op_pcm_seek(vb->of, (opus_int64)(offset / ft->signal.channels))? SOX_EOF:SOX_SUCCESS;
}
#endif

#ifdef HAVE_OPUSENC
static int callback_write(void * ft_data, const unsigned char * ptr, opus_int32 len)
{
  sox_format_t * ft = (sox_format_t *)ft_data;
  size_t bytes;

  if (len < 0)
    return 1;
  bytes = (size_t)len;
  return lsx_writebuf(ft, ptr, bytes) == bytes ? 0 : 1;
}

static int add_comments(sox_format_t * ft, OggOpusComments * comments)
{
  int i;

  for (i = 0; ft->oob.comments && ft->oob.comments[i]; ++i) {
    int result = strchr(ft->oob.comments[i], '=') ?
        ope_comments_add_string(comments, ft->oob.comments[i]) :
        ope_comments_add(comments, "Comment", ft->oob.comments[i]);
    if (result != OPE_OK) {
      lsx_fail_errno(ft, SOX_EINVAL, "Failed to add Opus comment: %s",
          ope_strerror(result));
      return SOX_EOF;
    }
  }
  return SOX_SUCCESS;
}

static int startwrite(sox_format_t * ft)
{
  static const OpusEncCallbacks callbacks = {
    callback_write,
    callback_close
  };
  priv_t * vb = (priv_t *)ft->priv;
  OggOpusComments * comments;
  int mapping_family;
  int result = OPE_OK;

  if (ft->signal.channels < 1 || ft->signal.channels > 8) {
    lsx_fail_errno(ft, SOX_EFMT,
        "Opus encoding supports standard layouts with 1 to 8 channels");
    return SOX_EOF;
  }
  if (ft->signal.rate != OPUS_OUTPUT_RATE) {
    lsx_fail_errno(ft, SOX_EFMT,
        "Opus encoding requires a 48000 Hz SoX pipeline");
    return SOX_EOF;
  }

  ft->encoding.encoding = SOX_ENCODING_OPUS;
  ft->signal.precision = 24;

  comments = ope_comments_create();
  if (comments == NULL) {
    lsx_fail_errno(ft, SOX_ENOMEM, "Failed to create Opus comments");
    return SOX_EOF;
  }
  if (add_comments(ft, comments) != SOX_SUCCESS) {
    ope_comments_destroy(comments);
    return SOX_EOF;
  }

  mapping_family = ft->signal.channels <= 2 ? 0 : 1;
  vb->encoder = ope_encoder_create_callbacks(&callbacks, ft, comments,
      OPUS_OUTPUT_RATE, (int)ft->signal.channels, mapping_family, &result);
  ope_comments_destroy(comments);
  if (vb->encoder == NULL) {
    lsx_fail_errno(ft, SOX_EFMT, "Failed to initialize Opus encoder: %s",
        ope_strerror(result));
    return SOX_EOF;
  }

  if (ft->encoding.compression != HUGE_VAL) {
    double bitrate = ft->encoding.compression;
    double maximum = 256. * ft->signal.channels;
    opus_int32 bitrate_bps;

    if (bitrate < 6 || bitrate > maximum) {
      lsx_fail_errno(ft, SOX_EINVAL,
          "Opus bitrate must be between 6 and %.0f kbit/s for %u channel%s",
          maximum, ft->signal.channels, ft->signal.channels == 1 ? "" : "s");
      ope_encoder_destroy(vb->encoder);
      vb->encoder = NULL;
      return SOX_EOF;
    }
    bitrate_bps = (opus_int32)(bitrate * 1000 + .5);
    result = ope_encoder_ctl(vb->encoder, OPUS_SET_BITRATE(bitrate_bps));
    if (result != OPE_OK) {
      lsx_fail_errno(ft, SOX_EINVAL, "Failed to set Opus bitrate: %s",
          ope_strerror(result));
      ope_encoder_destroy(vb->encoder);
      vb->encoder = NULL;
      return SOX_EOF;
    }
  }

  result = ope_encoder_flush_header(vb->encoder);
  if (result != OPE_OK) {
    lsx_fail_errno(ft, SOX_EOF, "Failed to write Opus header: %s",
        ope_strerror(result));
    ope_encoder_destroy(vb->encoder);
    vb->encoder = NULL;
    return SOX_EOF;
  }

  return SOX_SUCCESS;
}

static size_t write_samples(sox_format_t * ft, const sox_sample_t * buf,
    size_t len)
{
  priv_t * vb = (priv_t *)ft->priv;
  size_t i;
  int result;
  SOX_SAMPLE_LOCALS;

  if (len % ft->signal.channels) {
    lsx_fail_errno(ft, SOX_EINVAL,
        "Opus encoder received an incomplete interleaved sample frame");
    return 0;
  }

  if (vb->write_buf_len < len) {
    vb->write_buf = lsx_realloc(vb->write_buf, len * sizeof(*vb->write_buf));
    vb->write_buf_len = len;
  }
  for (i = 0; i < len / ft->signal.channels; ++i) {
    unsigned vorbis_channel;

    for (vorbis_channel = 0; vorbis_channel < ft->signal.channels;
        ++vorbis_channel) {
      size_t output = i * ft->signal.channels + vorbis_channel;
      size_t input = i * ft->signal.channels +
          vorbis_channel_from_sox[ft->signal.channels][vorbis_channel];

      vb->write_buf[output] =
          SOX_SAMPLE_TO_FLOAT_32BIT(buf[input], ft->clips);
    }
  }

  result = ope_encoder_write_float(vb->encoder, vb->write_buf,
      (int)(len / ft->signal.channels));
  if (result != OPE_OK) {
    lsx_fail_errno(ft, SOX_EOF, "Opus encoding failed: %s",
        ope_strerror(result));
    return 0;
  }
  return len;
}

static int stopwrite(sox_format_t * ft)
{
  priv_t * vb = (priv_t *)ft->priv;
  int result = ope_encoder_drain(vb->encoder);

  if (result != OPE_OK)
    lsx_fail_errno(ft, SOX_EOF, "Failed to finalize Opus stream: %s",
        ope_strerror(result));
  ope_encoder_destroy(vb->encoder);
  free(vb->write_buf);
  return result == OPE_OK ? SOX_SUCCESS : SOX_EOF;
}
#endif

#ifdef HAVE_OPUSFILE
#define OPUS_STARTREAD startread
#define OPUS_READ read_samples
#define OPUS_STOPREAD stopread
#define OPUS_SEEK seek
#else
#define OPUS_STARTREAD NULL
#define OPUS_READ NULL
#define OPUS_STOPREAD NULL
#define OPUS_SEEK NULL
#endif

#ifdef HAVE_OPUSENC
#define OPUS_STARTWRITE startwrite
#define OPUS_WRITE write_samples
#define OPUS_STOPWRITE stopwrite
#else
#define OPUS_STARTWRITE NULL
#define OPUS_WRITE NULL
#define OPUS_STOPWRITE NULL
#endif

LSX_FORMAT_HANDLER(opus)
{
  static const char *const names[] = {"opus", NULL};
#ifdef HAVE_OPUSENC
  static const unsigned encodings[] = {SOX_ENCODING_OPUS, 0, 0};
  static const sox_rate_t write_rates[] = {OPUS_OUTPUT_RATE, 0};
#endif
  static sox_format_handler_t handler = {SOX_LIB_VERSION_CODE,
    "Xiph.org's Opus lossy compression", names, 0,
    OPUS_STARTREAD, OPUS_READ, OPUS_STOPREAD,
    OPUS_STARTWRITE, OPUS_WRITE, OPUS_STOPWRITE,
    OPUS_SEEK,
#ifdef HAVE_OPUSENC
    encodings, write_rates,
#else
    NULL, NULL,
#endif
    sizeof(priv_t)
  };
  return &handler;
}
