/* libSoX container formats: a demuxer or muxer bound to a packet codec.
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

#include "container.h"

#include <string.h>

static void release(lsx_container_t * p)
{
  free(p->codec.priv);
  free(p->codec.samples);
  free(p->codec.params.private_data);
  free(p->priv);
  memset(p, 0, sizeof(*p));
}

/* Comments the codec finds are the file's only for the stream that begins
 * it; a codec started on a later one, or again after a seek, keeps them to
 * itself. */
int lsx_codec_start_decode(lsx_codec_t * codec, sox_bool comments)
{
  sox_comments_t kept = codec->ft->oob.comments;
  int status;

  if (comments)
    return codec->handler->start_decode(codec);
  codec->ft->oob.comments = NULL;
  status = codec->handler->start_decode(codec);
  sox_delete_comments(&codec->ft->oob.comments);
  codec->ft->oob.comments = kept;
  return status;
}

/* The four bytes a file begins with after any ID3v2 tags, which taggers
 * also put in front of formats that have tags of their own, and where they
 * are.  The tags are read through rather than sought past, so that a pipe
 * will do. */
int lsx_container_read_magic(sox_format_t * ft, unsigned char magic[4], sox_uint64_t * offset)
{
  unsigned char h[6];

  *offset = 0;
  if (lsx_readbuf(ft, magic, 4) != 4)
    goto truncated;
  while (!memcmp(magic, "ID3", 3)) {
    size_t size;

    if (lsx_readbuf(ft, h, 6) != 6)
      goto truncated;
    if ((h[2] | h[3] | h[4] | h[5]) & 0x80) {
      lsx_fail_errno(ft, SOX_EHDR, "invalid ID3v2 tag");
      return SOX_EOF;
    }
    size = ((size_t)h[2] << 21) + ((size_t)h[3] << 14) + ((size_t)h[4] << 7) + h[5] +
        (magic[3] == 4 && (h[1] & 0x10)? 10 : 0);
    if (lsx_skipbytes(ft, size) != SOX_SUCCESS || lsx_readbuf(ft, magic, 4) != 4)
      goto truncated;
    *offset += 10 + size;
  }
  return SOX_SUCCESS;

truncated:
  lsx_fail_errno(ft, SOX_EHDR, "file truncated");
  return SOX_EOF;
}

/* The codec stopped, and started again on params, which it takes over: at
 * the start of a stream chained after the last, whose delay is then dropped
 * as at the start of the file, or after a seek, which counts it in. */
int lsx_container_restart(sox_format_t * ft, lsx_codec_params_t const * params, sox_bool stream_start)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;

  if (c->handler->stop_decode)
    c->handler->stop_decode(c);
  p->codec_started = sox_false;
  free(c->params.private_data);
  c->params = *params;
  memset(c->priv, 0, c->handler->priv_size);
  c->samples_start = c->samples_end = 0;
  p->stream_ended = p->eof = sox_false;
  if (lsx_codec_start_decode(c, sox_false) != SOX_SUCCESS) {
    p->eof = sox_true;
    return SOX_EOF;
  }
  p->codec_started = sox_true;
  p->skip = stream_start? c->params.delay : 0;
  return SOX_SUCCESS;
}

/* A stream chained after the first must go on as the first began: the
 * output's rate and channels are fixed. */
int lsx_container_check_chained(sox_format_t * ft, lsx_codec_params_t const * first,
    lsx_codec_params_t const * chained)
{
  if (chained->rate != first->rate) {
    lsx_fail_errno(ft, SOX_EFMT, "chained streams with different sample rates are unsupported");
    return SOX_EOF;
  }
  if (chained->channels != first->channels) {
    lsx_fail_errno(ft, SOX_EFMT, "chained streams with different channel counts are unsupported");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* The stream chained after the one that has ended, if the container has one. */
static int next_stream(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  lsx_codec_params_t params, first = c->params;
  int status;

  memset(&params, 0, sizeof(params));
  if (!p->ops->next_stream || (status = p->ops->next_stream(ft, p->priv, c, &params)) == SOX_EOF) {
    p->eof = sox_true;
    return SOX_SUCCESS;
  }
  if (status != SOX_SUCCESS || lsx_container_restart(ft, &params, sox_true) != SOX_SUCCESS)
    return SOX_EOF;
  return lsx_container_check_chained(ft, &first, &c->params);
}

/* Decode one more packet into the codec's empty output, or drain the codec
 * at the end of a stream, and drop what the packet marks as preceding the
 * signal or as padding.  After a drain, the stream chained next, if any, is
 * taken up. */
static int decode_packet(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  size_t channels = c->params.channels;
  lsx_packet_t packet;
  int status;

  c->samples_start = c->samples_end = 0;
  if (p->stream_ended) {
    if (next_stream(ft) != SOX_SUCCESS) {
      p->eof = sox_true;
      return SOX_EOF;
    }
    if (p->eof)
      return SOX_SUCCESS;
  }
  status = p->ops->read_packet(ft, p->priv, c, &packet);
  if (status == SOX_EOF) {
    p->stream_ended = sox_true;
    status = c->handler->decode(c, NULL, 0);
  }
  else if (status == SOX_SUCCESS) {
    status = c->handler->decode(c, packet.data, packet.size);
    if (status == SOX_SUCCESS) {
      size_t frames = c->samples_end / channels;

      if (packet.duration && frames > packet.duration)
        packet.discard = max(packet.discard, frames - packet.duration);
      c->samples_end -= min(packet.discard * channels, c->samples_end);
      c->samples_start += min(packet.skip * channels, c->samples_end);
    }
  }
  if (status != SOX_SUCCESS) {
    p->eof = sox_true;
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* The codec comes from the file; a codec named in the type only has to
 * agree with it, and a format handler that reads through this layer without
 * being a container type, as mp3 does, names none.  The first packet is
 * decoded here, so that what a codec learns only from its packets is known
 * when the format is reported.  The length is the container's own count of
 * frames where it has an exact one, and is otherwise left unknown: a
 * duration is rounded, and a length that was off by a sample would cut the
 * signal. */
int lsx_container_startread(sox_format_t * ft, lsx_container_ops_t const * ops)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  char const * requested = ft->handler.flags & SOX_FILE_CONTAINER? lsx_container_requested(ft->filetype) : NULL;
  char const * codec_name;
  sox_uint64_t length = 0;

  p->ops = ops;
  p->priv = lsx_calloc(1, ops->priv_size);
  c->ft = ft;
  if (ops->open_read(ft, p->priv, &c->params, &codec_name) != SOX_SUCCESS)
    goto error;
  if (requested && strcasecmp(requested, codec_name)) {
    lsx_fail_errno(ft, SOX_EFMT, "the audio track holds codec `%s', not `%s'", codec_name, requested);
    goto error;
  }
  if (!(c->handler = lsx_codec_find(codec_name))) {
    lsx_fail_errno(ft, SOX_EFMT, "codec `%s' is not available in this build", codec_name);
    goto error;
  }
  c->priv = lsx_calloc(1, c->handler->priv_size);
  if (lsx_codec_start_decode(c, sox_true) != SOX_SUCCESS)
    goto error;
  p->codec_started = sox_true;
  if ((decode_packet(ft) != SOX_SUCCESS && ft->sox_errno) ||
      (ops->length && ops->length(ft, p->priv, c, &length) != SOX_SUCCESS)) {
    if (c->handler->stop_decode && p->codec_started)
      c->handler->stop_decode(c);
    goto error;
  }
  ft->signal.rate = c->params.rate;
  ft->signal.channels = c->params.channels;
  ft->signal.length = length? length * c->params.channels : SOX_UNSPEC;
  ft->encoding.encoding = c->params.encoding;
  ft->encoding.bits_per_sample = c->params.bits_per_sample;
  p->skip = c->params.delay;
  return SOX_SUCCESS;

error:
  ops->close_read(ft, p->priv);
  release(p);
  return SOX_EOF;
}

/* The delay is dropped as the samples are handed out, which leaves the
 * codec until the end of the first packet to settle it. */
size_t lsx_container_read(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  size_t done = 0;

  while (done < len) {
    size_t n = c->samples_end - c->samples_start;

    if (p->skip && n) {
      size_t drop = min(p->skip, n / c->params.channels);

      c->samples_start += drop * c->params.channels;
      p->skip -= drop;
      continue;
    }
    n = min(len - done, n);
    if (n) {
      memcpy(buf + done, c->samples + c->samples_start, n * sizeof(*buf));
      c->samples_start += n;
      done += n;
    }
    else if (p->eof || decode_packet(ft) != SOX_SUCCESS)
      break;
  }
  return done;
}

/* The container finds the place and restarts the codec there. */
int lsx_container_seek(sox_format_t * ft, sox_uint64_t offset)
{
  lsx_container_t * p = ft->priv;

  return p->ops->seek(ft, p->priv, &p->codec, offset / p->codec.params.channels);
}

int lsx_container_stopread(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;

  if (p->codec.handler->stop_decode && p->codec_started)
    p->codec.handler->stop_decode(&p->codec);
  p->ops->close_read(ft, p->priv);
  release(p);
  return SOX_SUCCESS;
}

static int write_packet(lsx_codec_t * codec, lsx_packet_t const * packet)
{
  lsx_container_t * p = codec->ft->priv;

  return p->ops->write_packet(codec->ft, p->priv, packet);
}

static sox_bool big_endian(sox_format_t const * ft)
{
  return (ft->encoding.reverse_bytes == sox_option_yes) != MACHINE_IS_BIGENDIAN;
}

/* The container's identifier for what is to be written, or NULL if it has
 * none.  8-bit integer PCM is unsigned under the signed identifiers. */
static lsx_codec_tag_t const * write_tag(sox_format_t const * ft)
{
  sox_encoding_t encoding = ft->encoding.encoding;

  if (encoding == SOX_ENCODING_UNSIGNED && ft->encoding.bits_per_sample == 8)
    encoding = SOX_ENCODING_SIGN2;
  return lsx_codec_tag_by_codec(lsx_container_family(ft->filetype),
      lsx_container_codec(ft->filetype), encoding, big_endian(ft));
}

/* Whether the container can hold the codec and the sample layout settled
 * for the output, and the encoder started, before the output file is opened,
 * so that a refused output does not leave behind or truncate a file.  The
 * type names the codec, or the container has a default; open_write in
 * formats.c has made sure of it.  MP3, having no precision to be chosen by,
 * is left unsettled, and is the codec's own encoding. */
int lsx_container_prepare_write(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;

  c->handler = lsx_codec_find(lsx_container_codec(ft->filetype));
  if (!ft->encoding.encoding)
    ft->encoding.encoding = c->handler->write_formats[0];
  if (!write_tag(ft)) {
    if (ft->encoding.encoding == SOX_ENCODING_SIGN2 || ft->encoding.encoding == SOX_ENCODING_UNSIGNED ||
        ft->encoding.encoding == SOX_ENCODING_FLOAT)
      lsx_fail_errno(ft, SOX_EFMT, "file type `%s' can't hold %s-endian %s as `%s'", ft->filetype,
          big_endian(ft)? "big" : "little", sox_encodings_info[ft->encoding.encoding].desc,
          lsx_container_codec(ft->filetype));
    else lsx_fail_errno(ft, SOX_EFMT, "file type `%s' can't hold codec `%s'", ft->filetype,
        lsx_container_codec(ft->filetype));
    return SOX_EOF;
  }
  c->ft = ft;
  c->priv = lsx_calloc(1, c->handler->priv_size);
  c->params.rate = ft->signal.rate;
  c->params.channels = ft->signal.channels;
  c->params.encoding = ft->encoding.encoding;
  c->params.bits_per_sample = ft->encoding.bits_per_sample;
  c->params.big_endian = big_endian(ft);
  c->write_packet = write_packet;
  if (c->handler->start_encode(c) != SOX_SUCCESS) {
    release(p);
    return SOX_EOF;
  }
  p->codec_started = sox_true;
  return SOX_SUCCESS;
}

/* What lsx_container_prepare_write started, when the output then fails to
 * open. */
void lsx_container_cancel_write(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;

  if (p->codec_started)
    p->codec.handler->stop_encode(&p->codec);
  release(p);
}

/* The encoder was started before the file was opened. */
int lsx_container_startwrite(sox_format_t * ft, lsx_container_ops_t const * ops)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;

  p->ops = ops;
  p->priv = lsx_calloc(1, ops->priv_size);
  if (ops->open_write(ft, p->priv, c, write_tag(ft)) != SOX_SUCCESS) {
    lsx_container_cancel_write(ft);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

size_t lsx_container_write(sox_format_t * ft, sox_sample_t const * buf, size_t len)
{
  lsx_codec_t * c = &((lsx_container_t *)ft->priv)->codec;

  return c->handler->encode(c, buf, len) == SOX_SUCCESS? len : 0;
}

/* The muxer is closed even when the flush failed, so that it releases what
 * it holds; the first failure is the one reported. */
int lsx_container_stopwrite(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  int flushed = c->handler->encode(c, NULL, 0);
  int closed = p->ops->close_write(ft, p->priv, c);

  c->handler->stop_encode(c);
  release(p);
  return flushed != SOX_SUCCESS? flushed : closed;
}
