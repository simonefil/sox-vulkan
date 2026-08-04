/* Double-double stand-in for the quadmath FP128 type used by VkFFT.
 *
 * VkFFT reaches its double-double precision only when its host scalar type
 * carries more than the 53 bits of a double: it computes each twiddle once
 * with pfcos()/pfsin() and then merely splits the result into a hi/lo pair.
 * With a plain double that split recovers nothing, so the transform is
 * limited to FP64 twiddle accuracy no matter how precise the device
 * arithmetic is. Some compilers expose a native __float128 type without the
 * quadmath library and headers that VkFFT also needs, while MSVC has no
 * 128-bit floating type at all. This header supplies the same interface from
 * two doubles in both cases, which is what VkFFT ultimately stores anyway.
 *
 * Only the six entry points VkFFT actually uses are provided. This header is
 * placed on the include path of the double-double translation unit alone, so
 * no other VkFFT instantiation is affected.
 *
 * (c) Simone Filippini <info@simonefilippini.it> 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LSX_VULKAN_DD_QUADMATH_H
#define LSX_VULKAN_DD_QUADMATH_H

#ifndef __cplusplus
#error "the double-double quadmath shim requires C++"
#endif

#include <cmath>

namespace lsx_dd {

/* Dekker/Knuth error-free transformations. Each returns the rounded result
 * and leaves the exact residual in err, so that result + err == a op b. */

static inline double quick_two_sum(double a, double b, double &err)
{
  double s = a + b;

  err = b - (s - a);
  return s;
}

static inline double two_sum(double a, double b, double &err)
{
  double s = a + b;
  double bb = s - a;

  err = (a - (s - bb)) + (b - bb);
  return s;
}

static inline double two_prod(double a, double b, double &err)
{
  double p = a * b;

  err = std::fma(a, b, -p);
  return p;
}

}

struct lsx_dd_float {
  double hi;
  double lo;

  lsx_dd_float() = default;
  lsx_dd_float(double value) : hi(value), lo(0.0) {}
  lsx_dd_float(double high, double low) : hi(high), lo(low) {}

  /* Explicit on purpose: an implicit one would let a double-double
   * expression decay to double silently, and would make every mixed
   * arithmetic expression ambiguous against the builtin operators. */
  explicit operator double() const { return hi; }
  explicit operator long double() const
  {
    return (long double)hi + (long double)lo;
  }
  explicit operator float() const { return (float)hi; }
  explicit operator int() const { return (int)(hi + lo); }
  explicit operator long() const { return (long)(hi + lo); }
  explicit operator long long() const { return (long long)(hi + lo); }
  explicit operator unsigned long() const
  {
    return (unsigned long)(hi + lo);
  }
  explicit operator unsigned long long() const
  {
    return (unsigned long long)(hi + lo);
  }
};

static inline lsx_dd_float operator-(lsx_dd_float a)
{
  return lsx_dd_float(-a.hi, -a.lo);
}

static inline lsx_dd_float operator+(lsx_dd_float a, lsx_dd_float b)
{
  double s2, t2, t1;
  double s1 = lsx_dd::two_sum(a.hi, b.hi, s2);

  t1 = lsx_dd::two_sum(a.lo, b.lo, t2);
  s2 += t1;
  s1 = lsx_dd::quick_two_sum(s1, s2, s2);
  s2 += t2;
  s1 = lsx_dd::quick_two_sum(s1, s2, s2);
  return lsx_dd_float(s1, s2);
}

static inline lsx_dd_float operator-(lsx_dd_float a, lsx_dd_float b)
{
  return a + (-b);
}

static inline lsx_dd_float operator*(lsx_dd_float a, lsx_dd_float b)
{
  double p2;
  double p1 = lsx_dd::two_prod(a.hi, b.hi, p2);

  p2 += a.hi * b.lo + a.lo * b.hi;
  p1 = lsx_dd::quick_two_sum(p1, p2, p2);
  return lsx_dd_float(p1, p2);
}

/* Long division by quotient extraction.  q1 removes the FP64-sized leading
 * part, q2 removes the leading part of its double-double residual, and q3 is
 * the final correction retained by the pair.  VkFFT calls this only with
 * finite, non-zero sizing and angle constants; matching libquadmath's NaN or
 * divide-by-zero signalling is intentionally outside this narrow shim. */
static inline lsx_dd_float operator/(lsx_dd_float a, lsx_dd_float b)
{
  double q1 = a.hi / b.hi;
  lsx_dd_float remainder = a - b * lsx_dd_float(q1);
  double q2 = remainder.hi / b.hi;
  double q3;

  remainder = remainder - b * lsx_dd_float(q2);
  q3 = remainder.hi / b.hi;
  q1 = lsx_dd::quick_two_sum(q1, q2, q2);
  return lsx_dd_float(q1, q2) + lsx_dd_float(q3);
}

static inline lsx_dd_float &operator+=(lsx_dd_float &a, lsx_dd_float b)
{
  a = a + b;
  return a;
}

static inline lsx_dd_float &operator-=(lsx_dd_float &a, lsx_dd_float b)
{
  a = a - b;
  return a;
}

static inline lsx_dd_float &operator*=(lsx_dd_float &a, lsx_dd_float b)
{
  a = a * b;
  return a;
}

static inline lsx_dd_float &operator/=(lsx_dd_float &a, lsx_dd_float b)
{
  a = a / b;
  return a;
}

static inline bool operator<(lsx_dd_float a, lsx_dd_float b)
{
  return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

static inline bool operator>(lsx_dd_float a, lsx_dd_float b)
{
  return b < a;
}

static inline bool operator<=(lsx_dd_float a, lsx_dd_float b)
{
  return !(b < a);
}

static inline bool operator>=(lsx_dd_float a, lsx_dd_float b)
{
  return !(a < b);
}

static inline bool operator==(lsx_dd_float a, lsx_dd_float b)
{
  return a.hi == b.hi && a.lo == b.lo;
}

static inline bool operator!=(lsx_dd_float a, lsx_dd_float b)
{
  return !(a == b);
}


#define __float128 lsx_dd_float

/* These two return double rather than the pair. VkFFT applies them only to
 * ratios of small sizing integers, whose rounded result is itself a small
 * integer, and it assigns that result straight into double variables. The
 * pair still decides the rounding: when the high word lands exactly on an
 * integer, the low word is what says which side of it the true value lies. */
static inline double floorq(lsx_dd_float a)
{
  double integral = std::floor(a.hi);

  return integral == a.hi ? integral + std::floor(a.lo) : integral;
}

static inline double ceilq(lsx_dd_float a)
{
  double integral = std::ceil(a.hi);

  return integral == a.hi ? integral + std::ceil(a.lo) : integral;
}

/* One Newton correction around the hardware square root.  If x is the FP64
 * approximation, (a - x*x)/(2*x) restores the residual carried by the low
 * word without iterating in ordinary precision.  VkFFT requests square roots
 * only of non-negative transform constants; zero and any invalid negative
 * input therefore collapse to zero instead of emulating libquadmath errno,
 * exceptions or NaN payloads that its caller never observes. */
static inline lsx_dd_float sqrtq(lsx_dd_float a)
{
  double inverse;
  double approximate;
  lsx_dd_float scaled;
  lsx_dd_float difference;

  if (a.hi <= 0.0)
    return lsx_dd_float(0.0);
  inverse = 1.0 / std::sqrt(a.hi);
  approximate = a.hi * inverse;
  scaled = lsx_dd_float(approximate);
  difference = a - scaled * scaled;
  return scaled + lsx_dd_float(difference.hi * inverse * 0.5);
}

/* Parses a decimal literal into a double-double. VkFFT uses this only for
 * compile-time constants such as pi, so a straightforward accumulation is
 * enough: the residual error stays far below the 2^-106 the pair can hold. */
static inline lsx_dd_float strtoflt128(char const *text, char **end)
{
  lsx_dd_float value(0.0);
  lsx_dd_float ten(10.0);
  int negative = 0;
  int fraction_digits = 0;
  int seen_point = 0;
  char const *cursor = text;

  while (*cursor == ' ' || *cursor == '\t')
    ++cursor;
  if (*cursor == '+' || *cursor == '-') {
    negative = *cursor == '-';
    ++cursor;
  }
  for (; *cursor; ++cursor) {
    if (*cursor == '.' && !seen_point) {
      seen_point = 1;
      continue;
    }
    if (*cursor < '0' || *cursor > '9')
      break;
    value = value * ten + lsx_dd_float((double)(*cursor - '0'));
    if (seen_point)
      ++fraction_digits;
  }
  while (fraction_digits-- > 0)
    value = value / ten;
  if (end)
    *end = (char *)cursor;
  return negative ? -value : value;
}

namespace lsx_dd {

/* pi/2 split across three doubles so that argument reduction stays exact
 * well beyond the precision the pair itself carries. */
static inline lsx_dd_float half_pi_reduce(
    lsx_dd_float value, long long &quadrant)
{
  static double const half_pi_0 = 1.5707963267948966192e+00;
  static double const half_pi_1 = 6.1232339957367658860e-17;
  static double const half_pi_2 = -1.4973849048591698329e-33;
  double rounded = std::nearbyint((value.hi + value.lo) /
      (half_pi_0 + half_pi_1));
  lsx_dd_float count(rounded);
  lsx_dd_float reduced = value;

  reduced = reduced - count * lsx_dd_float(half_pi_0);
  reduced = reduced - count * lsx_dd_float(half_pi_1);
  reduced = reduced - count * lsx_dd_float(half_pi_2);
  quadrant = (long long)rounded;
  return reduced;
}

/* Taylor series on the reduced argument, which never leaves [-pi/4, pi/4].
 * The loop stops once a term can no longer move the double-double sum. */
static inline void sin_cos_reduced(
    lsx_dd_float value, lsx_dd_float &sine, lsx_dd_float &cosine)
{
  lsx_dd_float square = value * value;
  lsx_dd_float sine_term = value;
  lsx_dd_float cosine_term(1.0);
  int index;

  sine = value;
  cosine = lsx_dd_float(1.0);
  for (index = 1; index <= 20; ++index) {
    double odd = (double)(2 * index) * (double)(2 * index + 1);
    double even = (double)(2 * index - 1) * (double)(2 * index);

    cosine_term = -cosine_term * square / lsx_dd_float(even);
    cosine += cosine_term;
    sine_term = -sine_term * square / lsx_dd_float(odd);
    sine += sine_term;
    if (std::fabs(sine_term.hi) < 1e-40 &&
        std::fabs(cosine_term.hi) < 1e-40)
      break;
  }
}

}

static inline lsx_dd_float sinq(lsx_dd_float value)
{
  long long quadrant;
  lsx_dd_float reduced = lsx_dd::half_pi_reduce(value, quadrant);
  lsx_dd_float sine;
  lsx_dd_float cosine;

  lsx_dd::sin_cos_reduced(reduced, sine, cosine);
  switch (((quadrant % 4) + 4) % 4) {
    case 0: return sine;
    case 1: return cosine;
    case 2: return -sine;
    default: return -cosine;
  }
}

static inline lsx_dd_float cosq(lsx_dd_float value)
{
  long long quadrant;
  lsx_dd_float reduced = lsx_dd::half_pi_reduce(value, quadrant);
  lsx_dd_float sine;
  lsx_dd_float cosine;

  lsx_dd::sin_cos_reduced(reduced, sine, cosine);
  switch (((quadrant % 4) + 4) % 4) {
    case 0: return cosine;
    case 1: return -sine;
    case 2: return -cosine;
    default: return sine;
  }
}

#endif
