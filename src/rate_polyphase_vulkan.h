/* FP64 Vulkan polyphase stage for the SoX rate planner.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_RATE_POLYPHASE_VULKAN_H
#define LSX_RATE_POLYPHASE_VULKAN_H

#include "vulkan_engine.h"

typedef struct lsx_rate_polyphase_vulkan lsx_rate_polyphase_vulkan_t;

lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels);
void lsx_rate_polyphase_vulkan_destroy(lsx_rate_polyphase_vulkan_t *context);
size_t lsx_rate_polyphase_vulkan_block_frames(void);
uint32_t lsx_rate_polyphase_vulkan_taps(lsx_rate_polyphase_vulkan_t const *context);
int lsx_rate_polyphase_vulkan_process(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, double const **output, size_t *output_frames, size_t *consumed_frames);

#endif
