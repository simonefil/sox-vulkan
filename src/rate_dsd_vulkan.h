/* Fused packed-DSD to PCM stage for the SoX rate planner, on Vulkan.
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

#ifndef LSX_RATE_DSD_VULKAN_H
#define LSX_RATE_DSD_VULKAN_H

#include "vulkan_engine.h"

/*
 * The rate planner's fused DSD stage: one direct-form low-pass that takes the
 * file's own packed bits and decimates all the way to PCM in a single filter,
 * in place of the cascade of half-band stages the CPU path uses.
 *
 * Two things make the fused form the right one here and the wrong one on the
 * host.  A direct-form stage computes only the samples it keeps, so its cost
 * scales with the *output* rate: decimating by 2^k at once is a fraction of
 * the work of k successive halvings, whereas on the CPU the cascade wins
 * because each of its stages is short enough to stay in cache.  And the input
 * is bits: a DSD frame only ever picks the sign of a coefficient, so the
 * multiply disappears and there is nothing to gain from expanding a bit into
 * a sample, on the host or on the bus.
 *
 * Words are the unit of input throughout, thirty-two DSD frames each, bit
 * zero the earliest, laid out channel major.  That is the convention the
 * packed readers in dsf.c and dsdiff.c produce and the packed writers
 * consume, unchanged in either direction.
 */

typedef struct lsx_rate_dsd_vulkan lsx_rate_dsd_vulkan_t;

/* Build a fused stage.
 *
 * coefficients is the whole response, one value per tap, and is copied.
 * decimation is the ratio the stage resamples by, one output frame per that
 * many DSD frames.  channels is the DSD channel count.  The response's
 * amplitude convention is the caller's: whatever a DSD one becomes is folded
 * into the coefficients, so the shader adds and subtracts them unscaled.
 *
 * preload_frames is the response's own delay, the frames of silence that must
 * precede the stream so that output frame zero lines up with input frame
 * zero.  It is stated in frames but supplied in words, which is what
 * preload_words below rounds it up to; the remainder is carried inside the
 * stage, so the alignment stays exact to the frame. */
lsx_rate_dsd_vulkan_t *lsx_rate_dsd_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps,
    uint32_t decimation, uint32_t channels, uint32_t preload_frames);

/* Destroy a stage; safe on NULL and on a partly built one. */
void lsx_rate_dsd_vulkan_destroy(lsx_rate_dsd_vulkan_t *context);

/* Most output frames produced per call, and the response length, which is the
 * window a caller must have available beyond the frames it wants consumed. */
size_t lsx_rate_dsd_vulkan_block_frames(void);
uint32_t lsx_rate_dsd_vulkan_taps(lsx_rate_dsd_vulkan_t const *context);

/* Input words per channel the stage can take in one call, which is the window
 * of the largest block it will produce.  A caller sizes its own staging from
 * this rather than deriving it from taps and decimation itself. */
size_t lsx_rate_dsd_vulkan_window_words(lsx_rate_dsd_vulkan_t const *context);

/* Words of silence the caller must place in front of the stream, which is
 * create's preload_frames rounded up to a whole word. */
uint32_t lsx_rate_dsd_vulkan_preload_words(lsx_rate_dsd_vulkan_t const *context);

/* Decimate packed words and wait.
 *
 * words has one pointer per channel, each to available_words consecutive
 * words of that channel; the runs need not be adjacent, which is what lets a
 * caller keep one queue per channel.  flush says the stream has ended, so
 * that outputs whose window runs past the last word are produced against
 * silence rather than held back.
 *
 * *output points at *output_frames interleaved PCM frames in a buffer the
 * context owns and overwrites next call.  *consumed_words is how many whole
 * words per channel the caller may now drop: fewer than were supplied, since
 * the response's window has to remain for the next call, and the sub-word
 * remainder is kept inside the stage. */
int lsx_rate_dsd_vulkan_process(
    lsx_rate_dsd_vulkan_t *context, uint32_t const *const *words,
    size_t available_words, sox_bool flush, double const **output,
    size_t *output_frames, size_t *consumed_words);

/* The same, publishing the result as a resident buffer instead of waiting.
 * normalize scales the output to +/-1 rather than SoX's sample range. */
int lsx_rate_dsd_vulkan_process_resident(
    lsx_rate_dsd_vulkan_t *context, uint32_t const *const *words,
    size_t available_words, sox_bool flush, size_t *output_frames,
    size_t *consumed_words, sox_rate_t rate,
    lsx_vulkan_resident_state_t state, sox_bool normalize,
    lsx_vulkan_resident_buffer_t *resident);

#endif
