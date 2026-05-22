/* Double-double primitives shared by every reference-profile shader.
 *
 * These were duplicated per shader and drifted: one copy guarded its
 * compensation terms with precise, another relied on fma(), and the guarded
 * copy still leaked because precise does not propagate backwards into the out
 * parameters of a callee.  Every step here is an error-free transformation
 * whose compensation term is algebraically zero, so a compiler free to
 * reassociate folds it away and silently reduces each pair to its high word.
 * Two rules keep that from happening again, and both are checkable on the
 * generated SPIR-V:
 *
 *   - every floating-point result lands in a precise local before it is used,
 *     including values passed as arguments to another function;
 *   - no primitive takes an out parameter, because those cannot be protected.
 *
 * The check is that every OpFAdd, OpFSub and OpFMul in the compiled shader
 * carries NoContraction; only OpFNegate may lack it, being exact.
 *
 * The including shader must already have requested float64.
 */

#ifndef DOUBLE_DOUBLE_GLSL
#define DOUBLE_DOUBLE_GLSL

struct DoubleDoubleComplex {
  dvec2 real_part;
  dvec2 imaginary_part;
};

/* Knuth: the error term is exactly representable, for any two operands. */
dvec2 two_sum(double first, double second)
{
  precise double sum = first + second;
  precise double recovered = sum - first;
  precise double error = (first - (sum - recovered)) + (second - recovered);

  return dvec2(sum, error);
}

dvec2 normalize_dd(double high, double low)
{
  return two_sum(high, low);
}

/* The accurate addition rather than the sloppy one: the low words get their
 * own error-free sum instead of being folded in with a bare chain of adds.
 * Reference exists to spend time on exactly this kind of second-order term,
 * and the partition accumulation adds up to sixteen of these in a row. */
dvec2 add_dd(dvec2 first, dvec2 second)
{
  const dvec2 high = two_sum(first.x, second.x);
  const dvec2 low = two_sum(first.y, second.y);
  precise double carry = high.y + low.x;
  const dvec2 folded = two_sum(high.x, carry);
  precise double remainder = folded.y + low.y;

  return two_sum(folded.x, remainder);
}

dvec2 subtract_dd(dvec2 first, dvec2 second)
{
  return add_dd(first, -second);
}

/* Dekker splitting rather than fma(): GLSL only guarantees that fma() is a
 * single rounding when its result reaches a precise variable, and an fma()
 * the driver does not actually fuse degenerates to a*b - a*b, which returns
 * exactly zero and takes the whole low word with it. */
dvec2 split_dd(double value)
{
  precise double scaled = value * 134217729.0;
  precise double shifted = scaled - value;
  precise double high = scaled - shifted;
  precise double low = value - high;

  return dvec2(high, low);
}

dvec2 two_product(double first, double second)
{
  precise double product = first * second;
  const dvec2 first_split = split_dd(first);
  const dvec2 second_split = split_dd(second);
  precise double error =
      ((first_split.x * second_split.x - product) +
      first_split.x * second_split.y +
      first_split.y * second_split.x) +
      first_split.y * second_split.y;

  return dvec2(product, error);
}

dvec2 multiply_dd(dvec2 first, dvec2 second)
{
  const dvec2 product = two_product(first.x, second.x);
  precise double error = product.y + (first.x * second.y + first.y * second.x) + first.y * second.y;

  return normalize_dd(product.x, error);
}

dvec2 multiply_dd_scalar(dvec2 value, double scale)
{
  const dvec2 product = two_product(value.x, scale);
  precise double error = product.y + value.y * scale;

  return normalize_dd(product.x, error);
}

DoubleDoubleComplex multiply_complex_dd(DoubleDoubleComplex first, DoubleDoubleComplex second)
{
  DoubleDoubleComplex result;

  result.real_part = subtract_dd(
      multiply_dd(first.real_part, second.real_part),
      multiply_dd(first.imaginary_part, second.imaginary_part));
  result.imaginary_part = add_dd(
      multiply_dd(first.real_part, second.imaginary_part),
      multiply_dd(first.imaginary_part, second.real_part));
  return result;
}

#endif
