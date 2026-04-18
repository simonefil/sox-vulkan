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

typedef struct lsx_fir_vulkan lsx_fir_vulkan_t;

lsx_fir_vulkan_t *lsx_fir_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    size_t taps, uint32_t channels);
lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows,
    size_t taps, uint32_t channels);
int lsx_fir_vulkan_fuse_reference_coefficients(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_sets,
    size_t const *tap_counts, size_t set_count,
    double **result_highs, double **result_lows,
    size_t *result_count);
void lsx_fir_vulkan_destroy(lsx_fir_vulkan_t *context);
size_t lsx_fir_vulkan_block_frames(void);
size_t lsx_fir_vulkan_block_frames_for(
    lsx_vulkan_context_t const *context);
size_t lsx_fir_vulkan_prepared_stride(
    lsx_fir_vulkan_t const *context);
lsx_vulkan_buffer_t *lsx_fir_vulkan_prepared_input_buffer(lsx_fir_vulkan_t *context);
int lsx_fir_vulkan_process(
    lsx_fir_vulkan_t *context, double const *input,
    double const **output);
int lsx_fir_vulkan_process_resident(
    lsx_fir_vulkan_t *context, double const *input,
    sox_rate_t rate, uint64_t frame_offset,
    lsx_vulkan_resident_state_t state,
    lsx_vulkan_resident_buffer_t *resident);
int lsx_fir_vulkan_process_prepared_resident(lsx_fir_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);
int lsx_fir_vulkan_flush_resident(lsx_fir_vulkan_t *context);

#endif
