/* Effect: fir filter from coefs   Copyright (c) 2009 robs@users.sourceforge.net
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
#include "dft_filter.h"

#include <limits.h>

#if HAVE_VULKAN
#include "vulkan_engine.h"
#endif

typedef struct {
  dft_filter_priv_t  base;
  char const         * filename;
  double             * h;
  int                n;
} priv_t;

#if HAVE_VULKAN
#define FIR_FAST_FUSION_MAX_TAPS 4194304u

static double *convolve_fir(
    double const *first, int first_count,
    double const *second, int second_count,
    int *result_count)
{
  size_t count =
      (size_t)first_count + (size_t)second_count - 1u;
  size_t dft_length = 1u;
  double *first_spectrum;
  double *second_spectrum;
  double *result;
  size_t index;
  double scale;

  if (count > INT_MAX)
    return NULL;
  while (dft_length < count) {
    if (dft_length > (size_t)INT_MAX / 2u)
      return NULL;
    dft_length *= 2u;
  }
  first_spectrum = lsx_calloc(
      dft_length, sizeof(*first_spectrum));
  second_spectrum = lsx_calloc(
      dft_length, sizeof(*second_spectrum));
  memcpy(
      first_spectrum, first,
      (size_t)first_count * sizeof(*first));
  memcpy(
      second_spectrum, second,
      (size_t)second_count * sizeof(*second));
  lsx_safe_rdft((int)dft_length, 1, first_spectrum);
  lsx_safe_rdft((int)dft_length, 1, second_spectrum);
  first_spectrum[0] *= second_spectrum[0];
  first_spectrum[1] *= second_spectrum[1];
  for (index = 2u; index < dft_length; index += 2u) {
    double real =
        first_spectrum[index] * second_spectrum[index] -
        first_spectrum[index + 1u] *
        second_spectrum[index + 1u];
    double imaginary =
        first_spectrum[index] *
        second_spectrum[index + 1u] +
        first_spectrum[index + 1u] *
        second_spectrum[index];

    first_spectrum[index] = real;
    first_spectrum[index + 1u] = imaginary;
  }
  lsx_safe_rdft((int)dft_length, -1, first_spectrum);
  result = lsx_malloc(count * sizeof(*result));
  scale = 2.0 / (double)dft_length;
  for (index = 0; index < count; ++index)
    result[index] = first_spectrum[index] * scale;
  free(second_spectrum);
  free(first_spectrum);
  *result_count = (int)count;
  return result;
}

int lsx_fir_vulkan_try_fuse(
    sox_effect_t *first, sox_effect_t const *second)
{
  priv_t *first_private;
  priv_t const *second_private;
  dft_filter_priv_t *first_base;
  dft_filter_priv_t const *second_base;
  double *combined;
  int first_count;
  int second_count;
  int combined_count;
  int combined_post_peak;

  if (!first || !second ||
      sox_globals.vulkan_profile == sox_vulkan_profile_none ||
      strcmp(first->handler.name, "fir") ||
      strcmp(second->handler.name, "fir") ||
      first->in_signal.rate != second->in_signal.rate ||
      first->in_signal.channels != second->in_signal.channels)
    return 0;
  first_private = (priv_t *)first->priv;
  second_private = (priv_t const *)second->priv;
  first_base = &first_private->base;
  second_base = &second_private->base;
  /* Fusion is not an optimisation that trades accuracy for speed: applying
   * the convolved filter once is the exact result, while chaining N stages
   * discards each stage's warm-up before the next one can use it and
   * truncates each stage's tail independently.  Measured against the fused
   * filter applied as a single fir in FP64: the fused route matches to the
   * 32-bit quantisation floor everywhere, the chained route deviates by
   * -113 dBFS over the first 1000 samples and -45 dBFS over the last 300.
   * It used to be enabled only for reference, and for fast on devices
   * without shaderFloat64 -- which is why the same --vulkan-fast command
   * behaved differently on an RTX 3080 and on an M5 Pro, and why accurate
   * scored below fast on Apple Silicon: the two were not running the same
   * computation. */
  if (getenv("SOX_VULKAN_DISABLE_FIR_FUSION"))
    return 0;
  if (!first_base->vulkan_source_taps ||
      !second_base->vulkan_source_taps)
    return 0;
  first_count = first_base->vulkan_source_num_taps;
  second_count = second_base->vulkan_source_num_taps;
  if (sox_globals.vulkan_profile == sox_vulkan_profile_reference) {
    uint32_t source_count =
        first_base->vulkan_fusion_source_count;

    if (!source_count) {
      first_base->vulkan_fusion_sources[0] = lsx_memdup(
          first_base->vulkan_source_taps,
          (size_t)first_count * sizeof(double));
      first_base->vulkan_fusion_source_taps[0] =
          (size_t)first_count;
      source_count = 1u;
    }
    if (source_count >= 8u)
      return 0;
    first_base->vulkan_fusion_sources[source_count] =
        lsx_memdup(
            second_base->vulkan_source_taps,
            (size_t)second_count * sizeof(double));
    first_base->vulkan_fusion_source_taps[source_count] =
        (size_t)second_count;
    first_base->vulkan_fusion_source_count = source_count + 1u;
    combined_count = first_count + second_count - 1;
    combined_post_peak =
        first_base->vulkan_source_post_peak +
        second_base->vulkan_source_post_peak;
    first_base->vulkan_source_num_taps = combined_count;
    first_base->vulkan_source_post_peak = combined_post_peak;
    first_base->vulkan_fusion_pending = sox_true;
    lsx_report(
        "Vulkan REFERENCE spectral fusion queued: "
        "%u filters, %d effective taps",
        source_count + 1u, combined_count);
    return 1;
  }
  if ((size_t)first_count + (size_t)second_count - 1u >
      FIR_FAST_FUSION_MAX_TAPS)
    return 0;
  combined = convolve_fir(
      first_base->vulkan_source_taps, first_count,
      second_base->vulkan_source_taps, second_count,
      &combined_count);
  if (!combined) {
    lsx_fail("Vulkan FAST FIR fusion exceeds supported length");
    return -1;
  }
  combined_post_peak =
      first_base->vulkan_source_post_peak +
      second_base->vulkan_source_post_peak;
  free(first_base->vulkan_source_taps);
  first_base->vulkan_source_taps = combined;
  first_base->vulkan_source_num_taps = combined_count;
  first_base->vulkan_source_post_peak = combined_post_peak;
  first_base->vulkan_fusion_pending = sox_true;
  lsx_report(
      "Vulkan FIR fusion: %d + %d taps -> %d taps, "
      "post-peak %d",
      first_count, second_count,
      combined_count, combined_post_peak);
  return 1;
}
#endif

static int create(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t             * p = (priv_t *)effp->priv;
  dft_filter_priv_t  * b = &p->base;
  double             d;
  char               c;

  b->filter_ptr = &b->filter;
  --argc, ++argv;
  if (!argc)
    p->filename = "-"; /* default to stdin */
  else if (argc == 1)
    p->filename = argv[0], --argc;
  else for (; argc && sscanf(*argv, "%lf%c", &d, &c) == 1; --argc, ++argv) {
    p->n++;
    p->h = lsx_realloc(p->h, p->n * sizeof(*p->h));
    p->h[p->n - 1] = d;
  }
  return argc? lsx_usage(effp) : SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
  priv_t        * p = (priv_t *)effp->priv;
  dft_filter_t  * f = p->base.filter_ptr;
  double        d;
  char          c;
  int           i;

  if (!f->num_taps) {
    if (!p->n && p->filename) {
      FILE * file = lsx_open_input_file(effp, p->filename, sox_true);
      if (!file)
        return SOX_EOF;
      while ((i = fscanf(file, " #%*[^\n]%c", &c)) >= 0) {
        if (i >= 1) continue; /* found and skipped a comment */
        if ((i = fscanf(file, "%lf", &d)) > 0) {
          /* found a coefficient value */
          p->n++;
          p->h = lsx_realloc(p->h, p->n * sizeof(*p->h));
          p->h[p->n - 1] = d;
        } else break; /* either EOF, or something went wrong
                         (read or syntax error) */
      }
      if (!feof(file)) {
        lsx_fail("error reading coefficient file");
        if (file != stdin) fclose(file);
        return SOX_EOF;
      }
      if (file != stdin) fclose(file);
    }
    lsx_report("%i coefficients", p->n);
    if (!p->n)
      return SOX_EFF_NULL;
    if (effp->global_info->plot != sox_plot_off) {
      char title[100];
      sprintf(title, "SoX effect: fir (%d coefficients)", p->n);
      lsx_plot_fir(p->h, p->n, effp->in_signal.rate,
          effp->global_info->plot, title, -30., 30.);
      free(p->h);
      return SOX_EOF;
    }
    lsx_set_dft_filter(f, p->h, p->n, p->n >> 1);
  }
  return lsx_dft_filter_effect_fn()->start(effp);
}

sox_effect_handler_t const * lsx_fir_effect_fn(void)
{
  static sox_effect_handler_t handler;
  handler = *lsx_dft_filter_effect_fn();
  handler.name = "fir";
  handler.usage = "[coef-file|coefs]";
  handler.getopts = create;
  handler.start = start;
  handler.priv_size = sizeof(priv_t);
  return &handler;
}
