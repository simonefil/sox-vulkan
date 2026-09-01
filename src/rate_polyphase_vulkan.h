/* FP64 Vulkan polyphase stage for the SoX rate planner.
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

#ifndef LSX_RATE_POLYPHASE_VULKAN_H
#define LSX_RATE_POLYPHASE_VULKAN_H

#include "vulkan_engine.h"

/*
 * The rate planner's polyphase stage, run on the GPU.
 *
 * A polyphase filter resamples by a rational ratio without ever computing the
 * samples it would throw away: the prototype low-pass is split into
 * phase_count interleaved sub-filters, and each output frame is one
 * sub-filter applied to the input at one position.  Which sub-filter and
 * which position come from a phase counter that advances by phase_step and
 * wraps at phase_count, so the ratio is phase_count to phase_step and the
 * counter's wrap is what makes it exact over any length of stream.
 *
 * Compared with the FIR-based stage in rate_vulkan.c, this one does no
 * transform and keeps no history bank: it is the direct form, which wins for
 * the short responses the intermediate stages of a rate plan use, whereas the
 * transform wins for the long ones.
 */

typedef struct lsx_rate_polyphase_vulkan lsx_rate_polyphase_vulkan_t;

/* Build a polyphase stage.
 *
 * coefficients holds taps values per phase, phase_count phases in all, and is
 * copied.  phase_step and phase_start are the counter's increment and its
 * initial value.  resident_preload_frames is how much input the resident path
 * should hold back before producing, so a consumer's first block is complete.
 * symmetric_presum asks the shader to exploit a symmetric response by adding
 * the mirrored sample pairs before multiplying, which halves the multiplies;
 * it is only valid for a response that really is symmetric. */
lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum);

/* As above with the response given as double-double pairs.  The low halves
 * reach the device only under the reference profile, whose arithmetic is the
 * only one that can hold them; elsewhere this behaves as the call above. */
lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create_reference_dd(lsx_vulkan_context_t *vulkan, double const *coefficient_highs, double const *coefficient_lows, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum);

/* Destroy a stage; safe on NULL and on a partly built one. */
void lsx_rate_polyphase_vulkan_destroy(lsx_rate_polyphase_vulkan_t *context);

/* Most input frames processed per call, and the response length, which is the
 * window margin a caller must keep available beyond what it wants consumed. */
size_t lsx_rate_polyphase_vulkan_block_frames(void);
uint32_t lsx_rate_polyphase_vulkan_taps(lsx_rate_polyphase_vulkan_t const *context);

/* Resample host samples and wait.  *output points at *output_frames
 * interleaved frames in a buffer the context owns and overwrites next call;
 * *consumed_frames is fewer than were supplied, the window margin having to
 * remain for the next call. */
int lsx_rate_polyphase_vulkan_process(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, double const **output, size_t *output_frames, size_t *consumed_frames);

/* Resample host samples and publish the result as a resident buffer.  The
 * normalized form additionally takes whether to scale the output to +/-1
 * rather than SoX's sample range; the plain form never normalizes. */
int lsx_rate_polyphase_vulkan_process_resident(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_polyphase_vulkan_process_resident_normalized(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);

/* Resample a resident input and publish a resident result, so a stage between
 * two Vulkan effects touches host memory at neither end.  The stage keeps its
 * own device-side input buffer and appends into it, since it needs the window
 * margin from the previous call as well; *output is filled only when the
 * caller also wants the frames on the host. */
int lsx_rate_polyphase_vulkan_process_resident_input(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_polyphase_vulkan_process_resident_input_normalized(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);

#endif
