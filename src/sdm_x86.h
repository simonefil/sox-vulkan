/* Sigma-Delta modulator
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

#ifndef SOX_SDM_X86_H
#define SOX_SDM_X86_H

#ifdef __AVX__

#include <immintrin.h>

#define SDM_FILTER_AVX(s0, s1, src, x) do {                               \
    __m256d tx, t0, t1, t2;                                               \
    __m256d p0, p1;                                                       \
    __m256d sx;                                                           \
                                                                          \
    sx = _mm256_set1_pd(x);                                               \
    s0 = _mm256_load_pd(src);                                             \
    s1 = _mm256_load_pd(src + 4);                                         \
                                                                          \
    p0 = _mm256_permute_pd(s0, 5);                                        \
    p1 = _mm256_permute_pd(s1, 5);                                        \
                                                                          \
    tx = _mm256_blend_pd(p0, _mm256_permute2f128_pd(sx, p0, 0x21), 0x5);  \
    t0 = _mm256_blend_pd(p1, _mm256_permute2f128_pd(p0, p1, 0x21), 0x5);  \
    t1 = _mm256_permute2f128_pd(tx, t0, 0x21);                            \
    t2 = _mm256_blend_pd(p1, _mm256_permute2f128_pd(p1, p1, 0x21), 0xa);  \
                                                                          \
    s0 = _mm256_add_pd(s0, tx);                                           \
    s1 = _mm256_add_pd(s1, t0);                                           \
                                                                          \
    s0 = _mm256_sub_pd(s0, _mm256_mul_pd(t1, _mm256_load_pd(g)));         \
    s1 = _mm256_sub_pd(s1, _mm256_mul_pd(t2, _mm256_load_pd(g + 4)));     \
  } while (0)

#define sdm_filter_calc sdm_filter_calc_avx
static double sdm_filter_calc_avx(const double *src, double *dst,
                                  const sdm_filter_t *f,
                                  double x, double y)
{
  const double *a = f->a;
  const double *g = f->g;
  __m256d s0, s1;
  __m256d v0, v1;
  __m128d v;

  SDM_FILTER_AVX(s0, s1, src, x - y);

  _mm256_store_pd(dst,     s0);
  _mm256_store_pd(dst + 4, s1);

  v0 = _mm256_mul_pd(s0, _mm256_load_pd(a));
  v1 = _mm256_mul_pd(s1, _mm256_load_pd(a + 4));
  v0 = _mm256_add_pd(v0, v1);

  v = _mm_add_pd(_mm256_castpd256_pd128(v0), _mm256_extractf128_pd(v0, 1));
  v = _mm_add_pd(v, _mm_permute_pd(v, 1));

  return x + _mm_cvtsd_f64(v);
}

#define sdm_filter_calc2 sdm_filter_calc2_avx
static void sdm_filter_calc2_avx(sdm_state_t *src, sdm_state_t *dst,
                                 const sdm_filter_t *f, double x)
{
  const double *a = f->a;
  const double *g = f->g;
  __m256d s0, s1;
  __m256d t0, t1;
  __m256d v0, v1, v2;
  __m128d r0, r1;
  __m256d a0;

  SDM_FILTER_AVX(s0, s1, src->state, x);

  t1 = _mm256_set_pd(0.0, 0.0, 0.0, 1.0);
  t0 = _mm256_sub_pd(s0, t1);
  s0 = _mm256_add_pd(s0, t1);

  _mm256_store_pd(dst[0].state,     s0);
  _mm256_store_pd(dst[0].state + 4, s1);

  _mm256_store_pd(dst[1].state,     t0);
  _mm256_store_pd(dst[1].state + 4, s1);

  a0 = _mm256_load_pd(a);
  v0 = _mm256_mul_pd(s0, a0);
  v1 = _mm256_mul_pd(t0, a0);
  v2 = _mm256_mul_pd(s1, _mm256_load_pd(a + 4));

  v0 = _mm256_add_pd(v0, v2);
  v1 = _mm256_add_pd(v1, v2);

  r0 = _mm_add_pd(_mm256_castpd256_pd128(v0), _mm256_extractf128_pd(v0, 1));
  r1 = _mm_add_pd(_mm256_castpd256_pd128(v1), _mm256_extractf128_pd(v1, 1));

  r0 = _mm_add_pd(r0, _mm_permute_pd(r1, 1));
  r0 = _mm_add_pd(r0, _mm_set1_pd(x));
  r0 = _mm_mul_pd(r0, r0);

  r0 = _mm_add_pd(r0, _mm_load1_pd(&src->cost));

  _mm_storel_pd(&dst[0].cost, r0);
  _mm_storeh_pd(&dst[1].cost, r0);
}

#elif defined __SSE2__ || defined _M_X64 || \
  (defined _M_IX86_FP && _M_IX86_FP >= 2)

#include <emmintrin.h>

#define SWAPHL(x) \
  _mm_castsi128_pd(_mm_shuffle_epi32(_mm_castpd_si128(x), 0x4e))

#define SDM_FILTER_SSE2(s0, s1, s2, s3, src, x) do {            \
    __m128d tx, t0, t1, t2, t3;                                 \
    __m128d sx;                                                 \
                                                                \
    sx = _mm_set1_pd(x);                                        \
    s0 = _mm_load_pd(src);                                      \
    s1 = _mm_load_pd(src + 2);                                  \
    s2 = _mm_load_pd(src + 4);                                  \
    s3 = _mm_load_pd(src + 6);                                  \
                                                                \
    tx = _mm_shuffle_pd(sx, s0, 1);                             \
    t0 = _mm_shuffle_pd(s0, s1, 1);                             \
    t1 = _mm_shuffle_pd(s1, s2, 1);                             \
    t2 = _mm_shuffle_pd(s2, s3, 1);                             \
    t3 = _mm_shuffle_pd(s3, s3, 1);                             \
                                                                \
    s0 = _mm_add_pd(s0, tx);                                    \
    s1 = _mm_add_pd(s1, t0);                                    \
    s2 = _mm_add_pd(s2, t1);                                    \
    s3 = _mm_add_pd(s3, t2);                                    \
                                                                \
    s0 = _mm_sub_pd(s0, _mm_mul_pd(t0, _mm_load_pd(g)));        \
    s1 = _mm_sub_pd(s1, _mm_mul_pd(t1, _mm_load_pd(g + 2)));    \
    s2 = _mm_sub_pd(s2, _mm_mul_pd(t2, _mm_load_pd(g + 4)));    \
    s3 = _mm_sub_pd(s3, _mm_mul_pd(t3, _mm_load_pd(g + 6)));    \
  } while (0)

#define sdm_filter_calc sdm_filter_calc_sse2
static double sdm_filter_calc_sse2(const double *src, double *dst,
                                   const sdm_filter_t *f,
                                   double x, double y)
{
  const double *a = f->a;
  const double *g = f->g;
  __m128d s0, s1, s2, s3;
  __m128d v0, v1;

  SDM_FILTER_SSE2(s0, s1, s2, s3, src, x - y);

  _mm_store_pd(dst,     s0);
  _mm_store_pd(dst + 2, s1);
  _mm_store_pd(dst + 4, s2);
  _mm_store_pd(dst + 6, s3);

  v0 = _mm_mul_pd(s0, _mm_load_pd(a));
  v1 = _mm_mul_pd(s1, _mm_load_pd(a + 2));
  v0 = _mm_add_pd(v0, _mm_mul_pd(s2, _mm_load_pd(a + 4)));
  v1 = _mm_add_pd(v1, _mm_mul_pd(s3, _mm_load_pd(a + 6)));
  v0 = _mm_add_pd(v0, v1);
  v0 = _mm_add_pd(v0, SWAPHL(v0));

  return x + _mm_cvtsd_f64(v0);
}

#define sdm_filter_calc2 sdm_filter_calc2_sse2
static void sdm_filter_calc2_sse2(sdm_state_t *src, sdm_state_t *dst,
                                  const sdm_filter_t *f, double x)
{
  const double *a = f->a;
  const double *g = f->g;
  __m128d s0, s1, s2, s3;
  __m128d v0, v1, v2, v3;
  __m128d t0, t1;
  __m128d a0;

  SDM_FILTER_SSE2(s0, s1, s2, s3, src->state, x);

  t1 = _mm_set_sd(1.0);
  t0 = _mm_sub_pd(s0, t1);
  s0 = _mm_add_pd(s0, t1);

  _mm_store_pd(dst[0].state,     s0);
  _mm_store_pd(dst[0].state + 2, s1);
  _mm_store_pd(dst[0].state + 4, s2);
  _mm_store_pd(dst[0].state + 6, s3);

  _mm_store_pd(dst[1].state,     t0);
  _mm_store_pd(dst[1].state + 2, s1);
  _mm_store_pd(dst[1].state + 4, s2);
  _mm_store_pd(dst[1].state + 6, s3);

  a0 = _mm_load_pd(a);
  v0 = _mm_mul_pd(s0, a0);
  t0 = _mm_mul_pd(t0, a0);
  v1 = _mm_mul_pd(s1, _mm_load_pd(a + 2));
  v2 = _mm_mul_pd(s2, _mm_load_pd(a + 4));
  v3 = _mm_mul_pd(s3, _mm_load_pd(a + 6));

  v1 = _mm_add_pd(v1, v2);
  v1 = _mm_add_pd(v1, v3);

  v0 = _mm_add_pd(v0, v1);
  t0 = _mm_add_pd(t0, v1);

  v0 = _mm_add_pd(v0, SWAPHL(t0));
  v0 = _mm_add_pd(v0, _mm_set1_pd(x));

  v0 = _mm_mul_pd(v0, v0);
  v0 = _mm_add_pd(v0, _mm_load1_pd(&src->cost));

  _mm_storel_pd(&dst[0].cost, v0);
  _mm_storeh_pd(&dst[1].cost, v0);
}

#endif

#endif
