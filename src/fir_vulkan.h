/* Partitioned VkFFT FIR backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_FIR_VULKAN_H
#define LSX_FIR_VULKAN_H

#include "vulkan_engine.h"

/*
 * Uniformly partitioned frequency-domain convolution on the GPU.
 *
 * The response is cut into partitions of one block each, every partition is
 * transformed once at setup, and each incoming block is transformed once and
 * multiplied against all of them, the products accumulating into one spectrum
 * that is transformed back.  This is overlap-save: the transform is twice a
 * block long, its first half is the previous block and its second the current
 * one, and only the second half of the result is output, the first half being
 * where the circular wrap-around lands.  The cost per sample therefore grows
 * with the logarithm of the block size and only linearly with the number of
 * partitions, which is what makes a response of a million taps practical.
 *
 * The caller must feed exactly lsx_fir_vulkan_block_frames_for() frames per
 * call, interleaved.  There is no partial block: the block size is baked into
 * the transform, the partitioning and the recorded command buffers.  Latency
 * is one block, since a block cannot be transformed until it is complete.
 *
 * Four numerical strategies live behind the same interface, chosen from the
 * context's profile; -V3 reports which is in force.  What they have in common
 * is the structure above -- they differ in how the transform, the coefficient
 * spectra and the accumulation are carried out, and in what a resident output
 * buffer then contains.
 */

typedef struct lsx_fir_vulkan lsx_fir_vulkan_t;

/* Build a FIR context for a response applied to every channel alike.
 * coefficients holds taps values and is copied, so the caller may free it on
 * return.  Returns NULL on failure, having reported why. */
lsx_fir_vulkan_t *lsx_fir_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    size_t taps, uint32_t channels);

/* As above with a separate response per channel: coefficients holds channels
 * pointers of taps values each.  All the responses must be the same length,
 * since the partitioning and the transform size are shared. */
lsx_fir_vulkan_t *lsx_fir_vulkan_create_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients,
    size_t taps, uint32_t channels);

/* The reference-profile forms, whose coefficients arrive as unevaluated sums
 * of two doubles.  A response computed in double-double must be handed over
 * as one, or its extra precision is lost before the transform ever sees it --
 * which is exactly what the profile exists to avoid.  Both arrays hold taps
 * values and are copied. */
lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows,
    size_t taps, uint32_t channels);
lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_highs,
    double const *const *coefficient_lows,
    size_t taps, uint32_t channels);

/* Convolve several responses into the single one their cascade is equivalent
 * to, in double-double, on the device.  Cascading filters as separate stages
 * would round between them; fusing them means one convolution and one
 * rounding, which is what the reference profile requires.
 *
 * set_count responses of tap_counts[i] taps each yield one of
 * sum(tap_counts) - set_count + 1.  On success the caller owns *result_highs
 * and *result_lows and must free both.  Only available under the reference
 * profile with hardware double precision; returns SOX_EOF otherwise. */
int lsx_fir_vulkan_fuse_reference_coefficients(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_sets,
    size_t const *tap_counts, size_t set_count,
    double **result_highs, double **result_lows,
    size_t *result_count);

/* Destroy a context; safe on NULL and on a partly built one.  Waits for the
 * device, so any block still in flight has completed on return. */
void lsx_fir_vulkan_destroy(lsx_fir_vulkan_t *context);

/* Write this backend's share of the effect's diagnostics keys: precision,
 * strategy, and the shape it settled on.  A no-op unless --diagnostics is
 * on. */
void lsx_fir_vulkan_diagnostics(lsx_fir_vulkan_t const *context, sox_effect_t const *effp);

/* Frames per call.  The first assumes the default transform size and exists
 * for callers with no context yet; the second is the authority, the fast
 * profile using a larger transform and therefore a larger block. */
size_t lsx_fir_vulkan_block_frames(void);
size_t lsx_fir_vulkan_block_frames_for(lsx_vulkan_context_t const *context);

/* Distance in elements between one channel's samples and the next in the
 * prepared input buffer, for a producer writing into it directly. */
size_t lsx_fir_vulkan_prepared_stride(lsx_fir_vulkan_t const *context);

/* The buffer an upstream Vulkan effect may write its block into, so that a
 * block produced on the device is not copied back to the host merely to be
 * uploaded again.  Owned by the context; the caller writes the next block and
 * then calls lsx_fir_vulkan_process_prepared_resident rather than one of the
 * forms that take host samples. */
lsx_vulkan_buffer_t *lsx_fir_vulkan_prepared_input_buffer(lsx_fir_vulkan_t *context);

/* Process one block and wait for it.  input is one block of interleaved
 * frames; *output points at the same many interleaved frames, in a buffer the
 * context owns and overwrites on the next call.  This is the non-resident
 * path: it costs a round trip per block and is what runs when the next effect
 * cannot take a resident buffer. */
int lsx_fir_vulkan_process(lsx_fir_vulkan_t *context, double const *input, double const **output);

/* Process one block and publish the result as a resident buffer instead of
 * downloading it.  Nothing is submitted: the work is queued for the next
 * submission, so the host does not wait here and the consumer must respect
 * the barrier the resident description declares.  state marks where the block
 * falls in the stream, and frame_offset its position.
 *
 * The resident description is valid until this context produces the next
 * block; the buffer behind it is reused on a rotation of
 * LSX_VULKAN_RESIDENT_BATCH_DEPTH blocks, which is what bounds how far the
 * consumer may lag. */
int lsx_fir_vulkan_process_resident(
    lsx_fir_vulkan_t *context, double const *input,
    sox_rate_t rate, uint64_t frame_offset,
    lsx_vulkan_resident_state_t state,
    lsx_vulkan_resident_buffer_t *resident);

/* As above for a block already written into the prepared input buffer. */
int lsx_fir_vulkan_process_prepared_resident(lsx_fir_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);

/* Submit everything queued and wait.  Called at the end of a resident stream,
 * or whenever the host must see the device caught up; submits an empty
 * command buffer, the point being the batch it carries with it. */
int lsx_fir_vulkan_flush_resident(lsx_fir_vulkan_t *context);

#endif
