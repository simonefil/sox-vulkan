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
}

/* The codec comes from the file; a codec named in the type only has to
 * agree with it.  The length is left unknown: the container's duration is
 * rounded, and a length that was off by a sample would cut the signal. */
int lsx_container_startread(sox_format_t * ft, lsx_container_ops_t const * ops)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  char const * requested = lsx_type_codec(ft->filetype);
  char const * codec_name;

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
  if (c->handler->start_decode(c) != SOX_SUCCESS)
    goto error;
  ft->signal.rate = c->params.rate;
  ft->signal.channels = c->params.channels;
  ft->signal.length = SOX_UNSPEC;
  ft->encoding.encoding = c->params.encoding;
  ft->encoding.bits_per_sample = c->params.bits_per_sample;
  p->skip = c->params.delay;
  return SOX_SUCCESS;

error:
  ops->close_read(ft, p->priv);
  release(p);
  return SOX_EOF;
}

/* Decode one more packet into the codec's empty output, or drain the codec
 * at the end, and drop what is delay or padding. */
static int decode_packet(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  size_t channels = c->params.channels;
  lsx_packet_t packet;
  int status;

  c->samples_start = c->samples_end = 0;
  status = p->ops->read_packet(ft, p->priv, &packet);
  if (status == SOX_EOF) {
    p->eof = sox_true;
    status = c->handler->decode(c, NULL, 0);
  }
  else if (status == SOX_SUCCESS) {
    status = c->handler->decode(c, packet.data, packet.size);
    if (status == SOX_SUCCESS)
      c->samples_end -= min(packet.discard * channels, c->samples_end);
  }
  if (status != SOX_SUCCESS) {
    p->eof = sox_true;
    return SOX_EOF;
  }
  if (p->skip) {
    size_t drop = min(p->skip, (c->samples_end - c->samples_start) / channels);

    c->samples_start += drop * channels;
    p->skip -= drop;
  }
  return SOX_SUCCESS;
}

size_t lsx_container_read(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;
  size_t done = 0;

  while (done < len) {
    size_t n = min(len - done, c->samples_end - c->samples_start);

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

int lsx_container_stopread(sox_format_t * ft)
{
  lsx_container_t * p = ft->priv;

  if (p->codec.handler->stop_decode)
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

/* The type names the codec; open_write in formats.c has made sure of it. */
int lsx_container_startwrite(sox_format_t * ft, lsx_container_ops_t const * ops)
{
  lsx_container_t * p = ft->priv;
  lsx_codec_t * c = &p->codec;

  p->ops = ops;
  p->priv = lsx_calloc(1, ops->priv_size);
  c->ft = ft;
  c->handler = lsx_codec_find(lsx_type_codec(ft->filetype));
  c->priv = lsx_calloc(1, c->handler->priv_size);
  c->params.rate = ft->signal.rate;
  c->params.channels = ft->signal.channels;
  c->params.encoding = ft->encoding.encoding;
  c->params.bits_per_sample = ft->encoding.bits_per_sample;
  c->params.big_endian = (ft->encoding.reverse_bytes == sox_option_yes) != MACHINE_IS_BIGENDIAN;
  c->write_packet = write_packet;
  if (c->handler->start_encode(c) != SOX_SUCCESS) {
    release(p);
    return SOX_EOF;
  }
  if (ops->open_write(ft, p->priv, c) != SOX_SUCCESS) {
    c->handler->stop_encode(c);
    release(p);
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
