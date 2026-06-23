/* Tap on the last conversion before samples leave libSoX as integers.
 *
 * The numerical quality probes compare a Vulkan profile against a reference
 * run, and to do that they need the samples as libSoX computed them, in
 * double precision, rather than after the 32-bit quantisation lsx_save_samples
 * performs.  Reading the effect chain's output would give the quantised
 * values, and every difference below the quantisation floor -- which is
 * precisely what the high-precision profiles are measured on -- would be
 * invisible.
 *
 * The hook is therefore installed inside lsx_save_samples, where both forms
 * exist.  It is test-only: nothing in a shipped run installs an observer, and
 * with none installed it costs one comparison per call.
 */

#ifndef SOX_VULKAN_QUALITY_PROBE_H
#define SOX_VULKAN_QUALITY_PROBE_H

#include <stddef.h>

/* Called with the samples about to be converted.  samples and count describe
 * a buffer owned by the caller and valid only for the duration of the call;
 * normalization_scale is what multiplies them into the +/-1 range, so an
 * observer need not know libSoX's sample range.  client_data is whatever was
 * registered alongside the observer. */
typedef void (*lsx_save_samples_observer_t)(
    double const *samples, size_t count, double normalization_scale,
    void *client_data);

/* Install or, with a NULL observer, remove the tap.  Process-wide and not
 * thread safe, which is adequate for its one caller: a probe sets it up
 * before running a chain and clears it afterwards. */
void lsx_set_save_samples_observer(lsx_save_samples_observer_t observer, void *client_data);

#endif
