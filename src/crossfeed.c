/* libSoX effect: headphone crossfeed after Bauer's stereophonic-to-binaural
 * network, as designed in libbs2b by Boris Mikhaylov.
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
 *
 * The filter design and the per-sample processing are ported from libbs2b
 * (src/bs2b.c, init() and cross_feed_d()), which carries this notice:
 *
 * Copyright (c) 2005 Boris Mikhaylov
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Each output channel is its own channel through a first-order high-boost
 * shelf plus the opposite channel through a first-order low-pass, the sum
 * scaled so that the bass, which both paths carry, does not grow.  FCUT is
 * the low-pass corner and FEED how far below the direct signal the crossfed
 * one sits at DC.  The coefficients come from the actual sample rate: unlike
 * libbs2b, which falls back to 44.1 kHz outside 2 to 384 kHz, any rate is
 * designed for. */

#include "sox_i.h"
#include <math.h>

typedef struct {
  double fcut, feed;            /* Low-pass corner (Hz), crossfeed level (dB) */
  double a0_lo, b1_lo;          /* Low-pass IIR coefficients */
  double a0_hi, a1_hi, b1_hi;   /* High-boost IIR coefficients */
  double gain;                  /* Overall gain against overloading */
  double asis[2], lo[2], hi[2]; /* Filter state, per channel */
} priv_t;

static int getopts(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;

  --argc, ++argv;
  if (argc != 2)
    return lsx_usage(effp);
  do {
    NUMERIC_PARAMETER(fcut, 300, 2000)
    NUMERIC_PARAMETER(feed, 1, 15)
  } while (0);
  return argc ? lsx_usage(effp) : SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  double GB_lo, GB_hi, G_lo, G_hi, Fc_lo, Fc_hi, x;

  if (effp->in_signal.packing) {
    lsx_fail("works only with PCM audio, not packed DSD");
    return SOX_EOF;
  }
  if (effp->in_signal.channels != 2) {
    lsx_fail("works only with stereo audio");
    return SOX_EOF;
  }

  Fc_lo = p->fcut;
  GB_lo = p->feed * -5.0 / 6.0 - 3.0;
  GB_hi = p->feed / 6.0 - 3.0;

  G_lo  = pow(10, GB_lo / 20.0);
  G_hi  = 1.0 - pow(10, GB_hi / 20.0);
  Fc_hi = Fc_lo * pow(2.0, (GB_lo - 20.0 * log10(G_hi)) / 12.0);

  x = exp(-2.0 * M_PI * Fc_lo / effp->in_signal.rate);
  p->b1_lo = x;
  p->a0_lo = G_lo * (1.0 - x);

  x = exp(-2.0 * M_PI * Fc_hi / effp->in_signal.rate);
  p->b1_hi = x;
  p->a0_hi = 1.0 - G_hi * (1.0 - x);
  p->a1_hi = -x;

  p->gain = 1.0 / (1.0 - G_hi + G_lo);

  p->asis[0] = p->asis[1] = p->lo[0] = p->lo[1] = p->hi[0] = p->hi[1] = 0;
  return SOX_SUCCESS;
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t len = min(*isamp, *osamp) / 2;

  *isamp = *osamp = len * 2;
  while (len--) {
    double l = ibuf[0], r = ibuf[1], out_l, out_r;

    p->lo[0] = p->a0_lo * l + p->b1_lo * p->lo[0];
    p->lo[1] = p->a0_lo * r + p->b1_lo * p->lo[1];

    p->hi[0] = p->a0_hi * l + p->a1_hi * p->asis[0] + p->b1_hi * p->hi[0];
    p->hi[1] = p->a0_hi * r + p->a1_hi * p->asis[1] + p->b1_hi * p->hi[1];
    p->asis[0] = l;
    p->asis[1] = r;

    out_l = p->hi[0] + p->lo[1];
    out_r = p->hi[1] + p->lo[0];
    out_l *= p->gain;
    out_r *= p->gain;
    obuf[0] = out_l;
    obuf[1] = out_r;
    ibuf += 2;
    obuf += 2;
  }
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_crossfeed_effect_fn(void)
{
  static sox_effect_handler_t handler = {"crossfeed", "FCUT FEED"
    "\n  FCUT  low-pass corner of the crossfed signal, 300 to 2000 Hz"
    "\n  FEED  crossfeed level below the direct signal, 1 to 15 dB",
    SOX_EFF_MCHAN, getopts, start, flow, NULL, NULL, NULL, sizeof(priv_t)};
  return &handler;
}
