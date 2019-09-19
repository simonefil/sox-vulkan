/* DSD over PCM
 * Copyright (c) 2015 Mans Rullgard <mans@mansr.com>
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

#include "sox_i.h"

typedef struct dop {
  sox_sample_t *buf;
  unsigned marker;
  unsigned pos;
} dop_t;

#define DOP_MARKER 0x05

static int dop_start(sox_effect_t *eff)
{
  dop_t *p = eff->priv;

  if (eff->in_signal.precision != 1) {
    lsx_fail("1-bit input required");
    return SOX_EOF;
  }

  if (eff->in_signal.rate != 16 * eff->out_signal.rate) {
    lsx_fail("incorrect output rate, should be %.1fk",
             eff->in_signal.rate / 16 / 1000);
    return SOX_EOF;
  }

  eff->out_signal.precision = 24;

  p->buf = lsx_calloc(eff->out_signal.channels, sizeof(*p->buf));
  p->marker = DOP_MARKER;

  return SOX_SUCCESS;
}

static unsigned dop_load_bits(const sox_sample_t *ibuf, unsigned step,
                              unsigned pos, unsigned num)
{
  unsigned shift = 23 - pos;
  unsigned buf = 0;

  while (num--) {
    unsigned bit = *ibuf > 0 ? 1 : 0;
    buf |= bit << shift;
    ibuf += step;
    shift--;
  }

  return buf;
}

static int dop_flow(sox_effect_t *eff, const sox_sample_t *ibuf,
                    sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
  dop_t *p = eff->priv;
  unsigned channels = eff->in_signal.channels;
  const sox_sample_t *in = ibuf;
  sox_sample_t *out = obuf;
  size_t ilen = *isamp / channels;
  size_t olen = *osamp / channels;
  unsigned i;

  if (p->pos) {
    size_t n = min(16 - p->pos, ilen);
    for (i = 0; i < channels; i++)
        p->buf[i] |= dop_load_bits(in + i, channels, p->pos, n);
    in += n * channels;
    ilen -= n;
    p->pos += n;
    if (p->pos == 16) {
      for (i = 0; i < channels; i++) {
        *out++ = p->buf[i] | p->marker << 24;
        p->buf[i] = 0;
      }
      olen--;
      p->marker ^= 0xff;
      p->pos = 0;
    }
  }

  while (olen && ilen >= 16) {
    for (i = 0; i < channels; i++)
      *out++ = dop_load_bits(in + i, channels, 0, 16) | p->marker << 24;
    olen--;
    in += 16 * channels;
    ilen -= 16;
    p->marker ^= 0xff;
  }

  if (olen && ilen < 16) {
    size_t n = min(16 - p->pos, ilen);
    for (i = 0; i < channels; i++)
      p->buf[i] |= dop_load_bits(in + i, channels, p->pos, n);
    in += n * channels;
    ilen -= n;
    p->pos += n;
    if (p->pos == 16) {
      for (i = 0; i < channels; i++) {
        *out++ = p->buf[i] | p->marker << 24;
        p->buf[i] = 0;
      }
      olen--;
      p->marker ^= 0xff;
      p->pos = 0;
    }
  }

  *isamp = in - ibuf;
  *osamp = out - obuf;

  return SOX_SUCCESS;
}

static int dop_drain(sox_effect_t *eff, sox_sample_t *obuf, size_t *osamp)
{
  dop_t *p = eff->priv;
  unsigned i;

  if (p->pos) {
    unsigned silence = (0xffff00 >> p->pos) & 0x696900;
    for (i = 0; i < eff->in_signal.channels; i++)
      *obuf++ = p->buf[i] | p->marker << 24 | silence;
    p->pos = 0;
    *osamp = i;
  } else {
    *osamp = 0;
  }

  return SOX_SUCCESS;
}

static int dop_stop(sox_effect_t *eff)
{
  dop_t *p = eff->priv;
  free(p->buf);
  return SOX_SUCCESS;
}

const sox_effect_handler_t *lsx_dop_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "dop", NULL,
    SOX_EFF_MCHAN | SOX_EFF_PREC | SOX_EFF_RATE,
    NULL, dop_start, dop_flow, dop_drain, dop_stop, NULL,
    sizeof(dop_t),
  };
  return &handler;
}
