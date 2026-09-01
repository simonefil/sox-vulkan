/* libSoX filter design in double-double precision.
 * All public functions & data are prefixed with lsx_ .
 *
 * Copyright (c) 2026 Simone Filippini
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

#include "quadmath.h"
#include "rate_design_dd.h"

/* pi as an unevaluated pair: the fp64 value, and the residual that fp64
 * drops.  Written as a sum so that the pair is normalised by two_sum rather
 * than by hand. */
static lsx_dd_float const dd_pi =
    lsx_dd_float(3.141592653589793116e+00) +
    lsx_dd_float(1.224646799147353207e-16);

/* The series of lsx_bessel_I_0, term for term, stopped by the same test: the
 * sum has stopped moving.  In fp64 that happens after some twenty terms; at
 * beta near forty and 106 bits of room it takes about ninety, so the bound
 * is generous rather than tight. */
static lsx_dd_float bessel_I_0_dd(lsx_dd_float x)
{
  lsx_dd_float term(1.0), sum(1.0), x2 = x / lsx_dd_float(2.0);
  int i = 1;

  for (; i < 1000; ++i) {
    lsx_dd_float y = x2 / lsx_dd_float((double)i);
    lsx_dd_float next;

    term = term * y * y;
    next = sum + term;
    if (next.hi == sum.hi && next.lo == sum.lo)
      break;
    sum = next;
  }
  return sum;
}

extern "C" void lsx_make_lpf_dd(int num_taps, double Fc, double beta,
    double rho, double scale, sox_bool dc_norm, double * high, double * low)
{
  int i, m = num_taps - 1;
  lsx_dd_float sum(0.0);
  lsx_dd_float const dd_Fc(Fc), dd_beta(beta), dd_scale(scale);
  lsx_dd_float const half_m = lsx_dd_float((double)m) / lsx_dd_float(2.0);
  lsx_dd_float const mult = dd_scale / bessel_I_0_dd(dd_beta);
  /* .5 * m + rho is exact in fp64 for every m and every rho the designer
   * uses (.5, .63, .75), but the reciprocal is not, so the division is left
   * for later rather than folded into a multiplier as the fp64 code does. */
  lsx_dd_float const half_width = half_m + lsx_dd_float(rho);

  for (i = 0; i <= m / 2; ++i) {
    lsx_dd_float z = lsx_dd_float((double)i) - half_m;
    lsx_dd_float x = z * dd_pi;
    lsx_dd_float y = z / half_width;
    lsx_dd_float w = bessel_I_0_dd(dd_beta *
        sqrtq(lsx_dd_float(1.0) - y * y));
    lsx_dd_float h = z.hi == 0.0 && z.lo == 0.0 ?
        dd_Fc : sinq(dd_Fc * x) / x;

    h = h * w * mult;
    sum += h;
    high[i] = h.hi;
    if (low)
      low[i] = h.lo;
    if (m - i != i) {
      sum += h;
      high[m - i] = h.hi;
      if (low)
        low[m - i] = h.lo;
    }
  }
  if (dc_norm) {
    lsx_dd_float const norm = dd_scale / sum;

    for (i = 0; i < num_taps; ++i) {
      lsx_dd_float h = lsx_dd_float(high[i], low ? low[i] : 0.0) * norm;

      high[i] = h.hi;
      if (low)
        low[i] = h.lo;
    }
  }
}
