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

#ifndef LSX_RATE_DESIGN_DD_H
#define LSX_RATE_DESIGN_DD_H

#include "soxconfig.h"
#include "sox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The windowed-sinc of lsx_make_lpf, computed in double-double.  Writes the
 * unevaluated pair: high[i] is what fp64 would have held and low[i] the
 * residual, so high[i] + low[i] carries about 106 bits.  Both arrays hold
 * num_taps entries; low may be NULL, in which case only the high halves are
 * written and the result is the correctly rounded fp64 design.
 *
 * The arguments mean what they mean in lsx_make_lpf, and the two agree to
 * within one fp64 ulp: this is the same filter, not a different one. */
void lsx_make_lpf_dd(int num_taps, double Fc, double beta, double rho,
    double scale, sox_bool dc_norm, double * high, double * low);

#ifdef __cplusplus
}
#endif

#endif
