/* FP64 Vulkan cubic stage for the SoX rate planner.
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

#ifndef LSX_RATE_CUBIC_VULKAN_H
#define LSX_RATE_CUBIC_VULKAN_H

#include "vulkan_engine.h"

/*
 * The rate planner's cubic interpolation stage, run on the GPU.
 *
 * Where the polyphase stages resample by an exact rational ratio, this one
 * handles an arbitrary ratio: it advances a fractional read position through
 * the input by a fixed step and fits a cubic through the samples around each
 * position.  The planner uses it for the fine adjustment a ratio of whole
 * numbers cannot express, the bulk of the rate change having been done by the
 * filtered stages before it.
 *
 * The position and the step are 32.32 fixed point, not floating point, so
 * that the phase accumulates exactly however long the stream runs: a double
 * would drift, and the drift would be audible as a slow pitch error.
 */

typedef struct lsx_rate_cubic_vulkan lsx_rate_cubic_vulkan_t;

/* Build a cubic stage.  step is the input positions advanced per output
 * frame, in 32.32 fixed point, so 1 << 32 means no rate change.  pre_post is
 * how many extra input frames the interpolator needs around its window, which
 * is what makes the caller's input longer than the frames it will consume. */
lsx_rate_cubic_vulkan_t *lsx_rate_cubic_vulkan_create(
    lsx_vulkan_context_t *vulkan, uint64_t step,
    uint32_t pre_post, uint32_t channels);

/* Destroy a stage; safe on NULL and on a partly built one. */
void lsx_rate_cubic_vulkan_destroy(lsx_rate_cubic_vulkan_t *context);

/* Most input frames processed per call; anything beyond is left for the next.
 * A maximum, unlike the FIR's exact block. */
size_t lsx_rate_cubic_vulkan_block_frames(void);

/* The stage's window margin, which a caller must have available beyond the
 * frames it wants consumed. */
uint32_t lsx_rate_cubic_vulkan_pre_post(lsx_rate_cubic_vulkan_t const *context);

/* Resample one batch and wait.  input holds input_frames interleaved frames,
 * of which more than pre_post must be present.  *output points at
 * *output_frames interleaved frames in a buffer the context owns and
 * overwrites next call, and *consumed_frames says how many input frames may
 * now be discarded -- fewer than were supplied, since the window margin has
 * to remain available for the next call. */
int lsx_rate_cubic_vulkan_process(
    lsx_rate_cubic_vulkan_t *context, double const *input,
    size_t input_frames, double const **output,
    size_t *output_frames, size_t *consumed_frames);

#endif
