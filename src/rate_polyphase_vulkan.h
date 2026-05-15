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

lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum);
void lsx_rate_polyphase_vulkan_destroy(lsx_rate_polyphase_vulkan_t *context);
size_t lsx_rate_polyphase_vulkan_block_frames(void);
uint32_t lsx_rate_polyphase_vulkan_taps(lsx_rate_polyphase_vulkan_t const *context);
int lsx_rate_polyphase_vulkan_process(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, double const **output, size_t *output_frames, size_t *consumed_frames);
int lsx_rate_polyphase_vulkan_process_resident(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_polyphase_vulkan_process_resident_normalized(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_polyphase_vulkan_process_resident_input(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_polyphase_vulkan_process_resident_input_normalized(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);

#endif
