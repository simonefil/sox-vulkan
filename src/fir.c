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

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#if HAVE_VULKAN
#include "vulkan_engine.h"
#endif

typedef struct {
  unsigned           first;
  unsigned           last;
} channel_range_t;

typedef struct {
  char               * filename;
  channel_range_t    * ranges;
  size_t             range_count;
  double             * taps;
  int                tap_count;
  int                pre_peak;
} channel_map_entry_t;

typedef struct {
  channel_map_entry_t * entries;
  size_t             entry_count;
  double             ** taps;
  int                * original_tap_counts;
  int                * original_pre_peaks;
  unsigned           channels;
  int                tap_count;
  int                post_peak;
  sox_bool           ready;
} channel_filter_bank_t;

typedef struct {
  dft_filter_priv_t  base;
  char const         * filename;
  double             * h;
  int                n;
  channel_filter_bank_t * channel_bank;
} priv_t;

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
  if (count == 1u) {
    result = lsx_malloc(sizeof(*result));
    result[0] = first[0] * second[0];
    *result_count = 1;
    return result;
  }
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

#if HAVE_VULKAN
#define FIR_FAST_FUSION_MAX_TAPS 4194304u

static double const *vulkan_channel_source(
    dft_filter_priv_t const *base, uint32_t channel)
{
  return base->vulkan_channels ?
      base->vulkan_channels[channel].source_taps :
      base->vulkan_source_taps;
}

static int promote_vulkan_channels(
    dft_filter_priv_t *base, uint32_t channels)
{
  dft_filter_vulkan_channel_t *channel_filters;
  size_t source_tap_count = base->vulkan_fusion_source_count ?
      base->vulkan_fusion_source_taps[0] :
      (size_t)base->vulkan_source_num_taps;
  uint32_t channel;
  uint32_t source;

  if (base->vulkan_channels)
    return base->vulkan_channel_count == channels ?
        SOX_SUCCESS : SOX_EOF;
  if (!base->vulkan_source_taps)
    return SOX_EOF;
  channel_filters = lsx_calloc(
      channels, sizeof(*channel_filters));
  for (channel = 0; channel < channels; ++channel) {
    channel_filters[channel].source_taps = lsx_memdup(
        base->vulkan_source_taps,
        source_tap_count * sizeof(double));
    if (base->vulkan_reference_low_taps)
      channel_filters[channel].reference_low_taps = lsx_memdup(
          base->vulkan_reference_low_taps,
          (size_t)base->vulkan_source_num_taps * sizeof(double));
    for (source = 0;
         source < base->vulkan_fusion_source_count; ++source) {
      channel_filters[channel].fusion_sources[source] = lsx_memdup(
          base->vulkan_fusion_sources[source],
          base->vulkan_fusion_source_taps[source] * sizeof(double));
      channel_filters[channel].fusion_source_taps[source] =
          base->vulkan_fusion_source_taps[source];
    }
  }
  free(base->vulkan_source_taps);
  free(base->vulkan_reference_low_taps);
  base->vulkan_source_taps = NULL;
  base->vulkan_reference_low_taps = NULL;
  for (source = 0;
       source < base->vulkan_fusion_source_count; ++source) {
    free(base->vulkan_fusion_sources[source]);
    base->vulkan_fusion_sources[source] = NULL;
    base->vulkan_fusion_source_taps[source] = 0;
  }
  base->vulkan_channels = channel_filters;
  base->vulkan_channel_count = channels;
  return SOX_SUCCESS;
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
  uint32_t channels;

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
  channels = (uint32_t)first->in_signal.channels;
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
  if ((!first_base->vulkan_source_taps &&
       !first_base->vulkan_channels) ||
      (!second_base->vulkan_source_taps &&
       !second_base->vulkan_channels) ||
      (first_base->vulkan_channels &&
       first_base->vulkan_channel_count != channels) ||
      (second_base->vulkan_channels &&
       second_base->vulkan_channel_count != channels))
    return 0;
  first_count = first_base->vulkan_source_num_taps;
  second_count = second_base->vulkan_source_num_taps;
  if (sox_globals.vulkan_profile == sox_vulkan_profile_reference) {
    uint32_t source_count =
        first_base->vulkan_fusion_source_count;
    sox_bool channel_fusion =
        first_base->vulkan_channels || second_base->vulkan_channels;

    if (channel_fusion &&
        promote_vulkan_channels(first_base, channels) != SOX_SUCCESS)
      return -1;
    if (!source_count && channel_fusion) {
      uint32_t channel;

      for (channel = 0; channel < channels; ++channel) {
        first_base->vulkan_channels[channel].fusion_sources[0] =
            lsx_memdup(
                first_base->vulkan_channels[channel].source_taps,
                (size_t)first_count * sizeof(double));
        first_base->vulkan_channels[channel].fusion_source_taps[0] =
            (size_t)first_count;
      }
      source_count = 1u;
    }
    else if (!source_count) {
      first_base->vulkan_fusion_sources[0] = lsx_memdup(
          first_base->vulkan_source_taps,
          (size_t)first_count * sizeof(double));
      first_base->vulkan_fusion_source_taps[0] =
          (size_t)first_count;
      source_count = 1u;
    }
    if (source_count >= 8u)
      return 0;
    if (channel_fusion) {
      uint32_t channel;

      for (channel = 0; channel < channels; ++channel) {
        first_base->vulkan_channels[channel].fusion_sources[source_count] =
            lsx_memdup(
                vulkan_channel_source(second_base, channel),
                (size_t)second_count * sizeof(double));
        first_base->vulkan_channels[channel].fusion_source_taps[source_count] =
            (size_t)second_count;
      }
    }
    else {
      first_base->vulkan_fusion_sources[source_count] =
          lsx_memdup(
              second_base->vulkan_source_taps,
              (size_t)second_count * sizeof(double));
      first_base->vulkan_fusion_source_taps[source_count] =
          (size_t)second_count;
    }
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
  if (first_base->vulkan_channels || second_base->vulkan_channels) {
    uint32_t channel;

    if (promote_vulkan_channels(first_base, channels) != SOX_SUCCESS)
      return -1;
    for (channel = 0; channel < channels; ++channel) {
      combined = convolve_fir(
          first_base->vulkan_channels[channel].source_taps,
          first_count,
          vulkan_channel_source(second_base, channel), second_count,
          &combined_count);
      if (!combined) {
        lsx_fail("Vulkan channel FIR fusion exceeds supported length");
        return -1;
      }
      free(first_base->vulkan_channels[channel].source_taps);
      first_base->vulkan_channels[channel].source_taps = combined;
    }
    /* Every channel convolves the same tap counts, so the fused length is
     * fixed by the two inputs and stays defined even for a zero-channel
     * signal, where the loop above never runs. */
    combined_count = first_count + second_count - 1;
    combined_post_peak =
        first_base->vulkan_source_post_peak +
        second_base->vulkan_source_post_peak;
    first_base->vulkan_source_num_taps = combined_count;
    first_base->vulkan_source_post_peak = combined_post_peak;
    first_base->vulkan_fusion_pending = sox_true;
    lsx_report(
        "Vulkan FIR channel fusion: %d + %d taps -> %d taps, "
        "post-peak %d",
        first_count, second_count,
        combined_count, combined_post_peak);
    return 1;
  }
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

static int channel_is_selected(
    channel_map_entry_t const *entry, unsigned channel)
{
  size_t index;

  for (index = 0; index < entry->range_count; ++index)
    if (channel >= entry->ranges[index].first &&
        channel <= entry->ranges[index].last)
      return 1;
  return 0;
}

static void append_channel_range(
    channel_map_entry_t *entry, unsigned first, unsigned last)
{
  entry->ranges = lsx_realloc(
      entry->ranges,
      (entry->range_count + 1u) * sizeof(*entry->ranges));
  entry->ranges[entry->range_count].first = first;
  entry->ranges[entry->range_count].last = last;
  ++entry->range_count;
}

static int parse_channel_number(
    char const **text, char const *end, unsigned *value)
{
  char *number_end;
  unsigned long parsed;

  if (*text == end || !isdigit((unsigned char)**text))
    return SOX_EOF;
  errno = 0;
  parsed = strtoul(*text, &number_end, 10);
  if (errno || number_end == *text || number_end > end ||
      !parsed || parsed > UINT_MAX)
    return SOX_EOF;
  *text = number_end;
  *value = (unsigned)parsed;
  return SOX_SUCCESS;
}

static int parse_channel_selector(
    channel_map_entry_t *entry,
    char const *selector, size_t length)
{
  char const *text = selector;
  char const *end = selector + length;

  while (text < end) {
    unsigned first;
    unsigned last;

    if (parse_channel_number(&text, end, &first) != SOX_SUCCESS)
      return SOX_EOF;
    last = first;
    if (text < end && *text == '-') {
      ++text;
      if (parse_channel_number(&text, end, &last) != SOX_SUCCESS)
        return SOX_EOF;
    }
    if (last < first) {
      unsigned swap = first;

      first = last;
      last = swap;
    }
    append_channel_range(entry, first, last);
    if (text == end)
      break;
    if (*text++ != ',' || text == end)
      return SOX_EOF;
  }
  return entry->range_count ? SOX_SUCCESS : SOX_EOF;
}

static int parse_channel_map(
    sox_effect_t *effp, int argc, char **argv,
    channel_filter_bank_t **result)
{
  channel_filter_bank_t *bank = lsx_calloc(1, sizeof(*bank));
  int stdin_entries = 0;
  int index;

  bank->entries = lsx_calloc(
      (size_t)argc, sizeof(*bank->entries));
  bank->entry_count = (size_t)argc;
  for (index = 0; index < argc; ++index) {
    channel_map_entry_t *entry = &bank->entries[index];
    char const *equals = strchr(argv[index], '=');

    if (!equals || equals == argv[index] || !equals[1] ||
        parse_channel_selector(
            entry, argv[index], (size_t)(equals - argv[index])) !=
            SOX_SUCCESS) {
      size_t cleanup;

      for (cleanup = 0; cleanup <= (size_t)index; ++cleanup) {
        free(bank->entries[cleanup].filename);
        free(bank->entries[cleanup].ranges);
      }
      free(bank->entries);
      free(bank);
      return lsx_usage(effp);
    }
    entry->filename = lsx_strdup(equals + 1);
    if (!strcmp(entry->filename, "-") && ++stdin_entries > 1) {
      size_t cleanup;

      lsx_fail("stdin can be used by only one fir channel mapping");
      for (cleanup = 0; cleanup <= (size_t)index; ++cleanup) {
        free(bank->entries[cleanup].filename);
        free(bank->entries[cleanup].ranges);
      }
      free(bank->entries);
      free(bank);
      return SOX_EOF;
    }
  }
  *result = bank;
  return SOX_SUCCESS;
}

static int read_coefficients(
    sox_effect_t *effp, char const *filename,
    double **taps, int *tap_count)
{
  FILE *file = lsx_open_input_file(effp, filename, sox_true);
  double value;
  char character;
  int converted;

  if (!file)
    return SOX_EOF;
  while ((converted = fscanf(file, " #%*[^\n]%c", &character)) >= 0) {
    if (converted >= 1)
      continue;
    if (fscanf(file, "%lf", &value) > 0) {
      if (*tap_count == INT_MAX) {
        lsx_fail("too many coefficients in `%s'", filename);
        if (file != stdin)
          fclose(file);
        return SOX_EOF;
      }
      ++*tap_count;
      *taps = lsx_realloc(
          *taps, (size_t)*tap_count * sizeof(**taps));
      (*taps)[*tap_count - 1] = value;
    }
    else
      break;
  }
  if (!feof(file)) {
    lsx_fail("error reading coefficient file `%s'", filename);
    if (file != stdin)
      fclose(file);
    return SOX_EOF;
  }
  if (file != stdin)
    fclose(file);
  if (!*tap_count) {
    lsx_fail("coefficient file `%s' is empty", filename);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int prepare_channel_bank(
    sox_effect_t *effp, channel_filter_bank_t *bank)
{
  unsigned channels = effp->in_signal.channels;
  double **unnormalized;
  int *tap_counts;
  int *pre_peaks;
  int common_pre_peak = 0;
  int common_post_peak = 0;
  int common_tap_count;
  size_t entry_index;
  unsigned channel;

  for (entry_index = 0; entry_index < bank->entry_count; ++entry_index) {
    channel_map_entry_t *entry = &bank->entries[entry_index];
    size_t range_index;

    for (range_index = 0;
         range_index < entry->range_count; ++range_index) {
      if (entry->ranges[range_index].last > channels) {
        lsx_fail(
            "fir channel %u does not exist in %u-channel input",
            entry->ranges[range_index].last, channels);
        return SOX_EOF;
      }
    }
    if (read_coefficients(
        effp, entry->filename,
        &entry->taps, &entry->tap_count) != SOX_SUCCESS)
      return SOX_EOF;
    entry->pre_peak =
        entry->tap_count - 1 - (entry->tap_count >> 1);
    lsx_report(
        "fir mapping `%s': %d coefficients, pre-peak %d",
        entry->filename, entry->tap_count, entry->pre_peak);
  }

  unnormalized = lsx_calloc(channels, sizeof(*unnormalized));
  tap_counts = lsx_calloc(channels, sizeof(*tap_counts));
  pre_peaks = lsx_calloc(channels, sizeof(*pre_peaks));
  for (channel = 0; channel < channels; ++channel) {
    unsigned applied = 0;

    for (entry_index = 0;
         entry_index < bank->entry_count; ++entry_index) {
      channel_map_entry_t const *entry = &bank->entries[entry_index];

      if (channel_is_selected(entry, channel + 1u)) {
        double *combined;
        int combined_count;

        lsx_report(
            "fir channel %u applies `%s'",
            channel + 1u, entry->filename);
        if (!applied) {
          unnormalized[channel] = lsx_memdup(
              entry->taps,
              (size_t)entry->tap_count * sizeof(*entry->taps));
          tap_counts[channel] = entry->tap_count;
        }
        else {
          combined = convolve_fir(
              unnormalized[channel], tap_counts[channel],
              entry->taps, entry->tap_count,
              &combined_count);
          if (!combined) {
            lsx_fail(
                "combined fir for channel %u exceeds supported length",
                channel + 1u);
            goto error;
          }
          free(unnormalized[channel]);
          unnormalized[channel] = combined;
          tap_counts[channel] = combined_count;
        }
        if (pre_peaks[channel] > INT_MAX - entry->pre_peak) {
          lsx_fail("fir pre-peak for channel %u is too large", channel + 1u);
          goto error;
        }
        pre_peaks[channel] += entry->pre_peak;
        ++applied;
      }
    }
    if (!applied) {
      unnormalized[channel] = lsx_calloc(1, sizeof(**unnormalized));
      unnormalized[channel][0] = 1.;
      tap_counts[channel] = 1;
      lsx_warn(
          "fir channel %u is not mapped; passing it through",
          channel + 1u);
    }
    common_pre_peak = max(common_pre_peak, pre_peaks[channel]);
    common_post_peak = max(
        common_post_peak,
        tap_counts[channel] - 1 - pre_peaks[channel]);
  }

  if (common_pre_peak == INT_MAX ||
      common_post_peak > INT_MAX - common_pre_peak - 1) {
    lsx_fail("normalized fir is too large");
    goto error;
  }
  common_tap_count = common_pre_peak + common_post_peak + 1;
  bank->taps = lsx_calloc(channels, sizeof(*bank->taps));
  bank->original_tap_counts = tap_counts;
  bank->original_pre_peaks = pre_peaks;
  bank->channels = channels;
  bank->tap_count = common_tap_count;
  bank->post_peak = common_post_peak;
  for (channel = 0; channel < channels; ++channel) {
    int leading = common_pre_peak - pre_peaks[channel];

    bank->taps[channel] = lsx_calloc(
        (size_t)common_tap_count, sizeof(**bank->taps));
    memcpy(
        bank->taps[channel] + leading,
        unnormalized[channel],
        (size_t)tap_counts[channel] * sizeof(**bank->taps));
    free(unnormalized[channel]);
    lsx_report(
        "fir channel %u resolved: %d taps, pre-peak %d; "
        "normalized to %d taps, pre-peak %d",
        channel + 1u, tap_counts[channel], pre_peaks[channel],
        common_tap_count, common_pre_peak);
  }
  free(unnormalized);
  for (entry_index = 0; entry_index < bank->entry_count; ++entry_index) {
    free(bank->entries[entry_index].taps);
    bank->entries[entry_index].taps = NULL;
  }
  bank->ready = sox_true;
  return SOX_SUCCESS;

error:
  for (channel = 0; channel < channels; ++channel)
    free(unnormalized[channel]);
  free(pre_peaks);
  free(tap_counts);
  free(unnormalized);
  return SOX_EOF;
}

static int kill(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  channel_filter_bank_t *bank = p->channel_bank;
  size_t entry_index;
  unsigned channel;

  if (!bank)
    return SOX_SUCCESS;
#if HAVE_VULKAN
  lsx_clear_dft_filter_vulkan_channels(&p->base);
#endif
  for (entry_index = 0; entry_index < bank->entry_count; ++entry_index) {
    free(bank->entries[entry_index].filename);
    free(bank->entries[entry_index].ranges);
    free(bank->entries[entry_index].taps);
  }
  for (channel = 0; channel < bank->channels; ++channel)
    free(bank->taps[channel]);
  free(bank->original_pre_peaks);
  free(bank->original_tap_counts);
  free(bank->taps);
  free(bank->entries);
  free(bank);
  p->channel_bank = NULL;
  return SOX_SUCCESS;
}

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
  else if (strchr(argv[0], '=') &&
      isdigit((unsigned char)argv[0][0]))
    return parse_channel_map(effp, argc, argv, &p->channel_bank);
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

  if (p->channel_bank) {
    channel_filter_bank_t *bank = p->channel_bank;
    double *flow_taps;

    if (!bank->ready && (effp->flow ||
        prepare_channel_bank(effp, bank) != SOX_SUCCESS))
      return SOX_EOF;
    if (effp->global_info->plot != sox_plot_off) {
      char title[120];

      if (effp->flow)
        return SOX_EOF;
      sprintf(
          title, "SoX effect: fir channel 1 (%d normalized coefficients)",
          bank->tap_count);
      lsx_report("fir --plot displays resolved channel 1");
      lsx_plot_fir(
          bank->taps[0], bank->tap_count,
          effp->in_signal.rate, effp->global_info->plot,
          title, -30., 30.);
      return SOX_EOF;
    }
    if (effp->flow >= bank->channels)
      return SOX_EOF;
    p->base.filter_ptr = &p->base.filter;
    f = p->base.filter_ptr;
    flow_taps = lsx_memdup(
        bank->taps[effp->flow],
        (size_t)bank->tap_count * sizeof(*flow_taps));
    lsx_set_dft_filter(
        f, flow_taps, bank->tap_count, bank->post_peak);
#if HAVE_VULKAN
    if (!effp->flow &&
        sox_globals.vulkan_profile != sox_vulkan_profile_none &&
        lsx_set_dft_filter_vulkan_channels(
            &p->base, (double const *const *)bank->taps,
            bank->tap_count, bank->post_peak,
            bank->channels) != SOX_SUCCESS) {
      free(f->taps);
      memset(f, 0, sizeof(*f));
      return SOX_EOF;
    }
#endif
    return lsx_dft_filter_effect_fn()->start(effp);
  }

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
  handler.usage =
      "[coef-file|coefs|channel-list=coef-file ...]";
  handler.getopts = create;
  handler.start = start;
  handler.kill = kill;
  handler.priv_size = sizeof(priv_t);
  return &handler;
}
