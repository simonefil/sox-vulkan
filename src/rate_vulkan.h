/* VkFFT rate-stage backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_RATE_VULKAN_H
#define LSX_RATE_VULKAN_H

#include "fir_vulkan.h"

typedef struct lsx_rate_vulkan lsx_rate_vulkan_t;

lsx_rate_vulkan_t *lsx_rate_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, size_t taps, size_t post_peak, uint32_t up_factor, uint32_t down_factor, uint32_t channels);
void lsx_rate_vulkan_destroy(lsx_rate_vulkan_t *context);
size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context);
int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames);

#endif
