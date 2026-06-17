/* Shared VkFFT executor for Vulkan SoX effects.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_VULKAN_FFT_H
#define LSX_VULKAN_FFT_H

#include "vulkan_engine.h"

/* One VkFFT plan.  This wrapper exists so that the rest of the backend can
 * ask for a transform without knowing whether it will be served by VkFFT's
 * ordinary single or double precision path or by the separate double-double
 * instantiation in vulkan_fft_dd.cpp, and without meeting VkFFT's headers,
 * which cannot be included from more than one translation unit at a time. */
typedef struct lsx_vulkan_fft lsx_vulkan_fft_t;

/* Build a plan for length-point transforms over batches signals, operating
 * in place on buffer.
 *
 * The plan is bound to buffer for its lifetime: VkFFT records the buffer
 * handle and its size into the kernels it generates, so the buffer must
 * outlive the plan and must not be recreated under it.  fence likewise must
 * remain valid, being reused for VkFFT's own internal submissions during
 * initialisation; it is reset before use.
 *
 * double_precision and double_double_precision are exclusive, the latter
 * selecting the paired-double instantiation.  real_to_complex halves the
 * work and the storage for real input, and changes the spectrum layout the
 * caller must expect.  normalize_inverse asks VkFFT to divide by the
 * transform length on the inverse, so the caller does not have to.
 *
 * Kernel compilation dominates startup, so a plan is looked up in the shared
 * cache first and stored there when it had to be compiled.  Returns NULL on
 * failure, having reported why; nothing is left allocated. */
lsx_vulkan_fft_t *lsx_vulkan_fft_create(
    lsx_vulkan_context_t *vulkan, lsx_vulkan_buffer_t *buffer,
    uint32_t length, uint32_t batches, sox_bool double_precision,
    sox_bool double_double_precision,
    sox_bool real_to_complex, sox_bool normalize_inverse,
    VkFence *fence);

/* Destroy a plan.  Safe on NULL and on a partly built plan.  Does not wait
 * for the device: the caller must have done so, since the plan owns pipelines
 * that in-flight command buffers may still reference. */
void lsx_vulkan_fft_destroy(lsx_vulkan_fft_t *context);

/* Record a forward or inverse transform into command_buffer, which must be in
 * the recording state.  Nothing is submitted here, so the caller stays free
 * to batch the transform with the work around it -- which is the point: a
 * resident chain records its whole block, transforms included, into one
 * command buffer.  The caller is responsible for the barriers on either side. */
int lsx_vulkan_fft_append(lsx_vulkan_fft_t *context, VkCommandBuffer command_buffer, sox_bool inverse);

#endif
