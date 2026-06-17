/* Double-double VkFFT instantiation, implemented in vulkan_fft_dd.cpp.
 *
 * Kept behind a plain C interface so that the C++ translation unit holding
 * the wider host scalar type stays isolated from the rest of SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_VULKAN_FFT_DD_H
#define LSX_VULKAN_FFT_DD_H

#include "vulkan_fft_cache.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a double-double plan, returning an opaque handle or NULL on failure,
 * in which case *result_code holds VkFFT's own result value -- reported by
 * number rather than mapped, since the SoX logging functions are on the C
 * side.  The handles are passed by address because that is how VkFFT takes
 * them; every one of them, and the buffer, must outlive the plan.
 *
 * The caller is expected to have initialised glslang already, this file
 * asserting to VkFFT that it has.
 *
 * key is built by the caller so that both VkFFT instantiations agree on what
 * a plan depends on; it may be null to force a compile. */
void *lsx_vulkan_fft_dd_create(
    VkDevice *device, VkPhysicalDevice *physical_device,
    VkQueue *queue, VkCommandPool *command_pool,
    VkBuffer *buffer, uint64_t buffer_size,
    uint32_t length, uint32_t batches,
    int real_to_complex, int normalize_inverse,
    VkFence *fence, lsx_vulkan_fft_cache_key_t const *key,
    int *result_code);

/* Destroy a plan; safe on NULL.  As on the C side, the caller must already
 * have waited for the device. */
void lsx_vulkan_fft_dd_destroy(void *handle);

/* Record a transform into command_buffer.  Returns a VkFFTResult value as an
 * int, zero being success. */
int lsx_vulkan_fft_dd_append(void *handle, VkCommandBuffer command_buffer, int inverse);

#ifdef __cplusplus
}
#endif

#endif
