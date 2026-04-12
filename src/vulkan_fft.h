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

typedef struct lsx_vulkan_fft lsx_vulkan_fft_t;

lsx_vulkan_fft_t *lsx_vulkan_fft_create(
    lsx_vulkan_context_t *vulkan, lsx_vulkan_buffer_t *buffer,
    uint32_t length, uint32_t batches, sox_bool double_precision,
    sox_bool double_double_precision,
    sox_bool real_to_complex, sox_bool normalize_inverse,
    VkFence *fence);
void lsx_vulkan_fft_destroy(lsx_vulkan_fft_t *context);
int lsx_vulkan_fft_append(
    lsx_vulkan_fft_t *context, VkCommandBuffer command_buffer,
    sox_bool inverse);

#endif
