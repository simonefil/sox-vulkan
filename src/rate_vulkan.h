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
lsx_rate_vulkan_t *lsx_rate_vulkan_create_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels);
lsx_rate_vulkan_t *lsx_rate_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels);
lsx_rate_vulkan_t *lsx_rate_vulkan_create_reference_dd_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_highs,
    double const *const *coefficient_lows, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels);
void lsx_rate_vulkan_destroy(lsx_rate_vulkan_t *context);
size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context);
int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames);
int lsx_rate_vulkan_process_resident(lsx_rate_vulkan_t *context, double const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);
int lsx_rate_vulkan_process_resident_input(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);
size_t lsx_rate_vulkan_resident_stream_room(lsx_rate_vulkan_t const *context);
int lsx_rate_vulkan_append_resident_stream(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input);
int lsx_rate_vulkan_append_resident_stream_quantized(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input);
int lsx_rate_vulkan_process_resident_stream(lsx_rate_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
int lsx_rate_vulkan_flush_resident(lsx_rate_vulkan_t *context);
sox_bool lsx_rate_vulkan_resident_stream_ready(lsx_rate_vulkan_t const *context);
int lsx_rate_vulkan_pad_resident_stream(lsx_rate_vulkan_t *context);
uint32_t lsx_rate_vulkan_resident_stream_clips(lsx_rate_vulkan_t *context);
uint32_t lsx_rate_vulkan_resident_stream_clips_completed(lsx_rate_vulkan_t *context);
uint32_t lsx_rate_vulkan_resident_batch_depth(lsx_rate_vulkan_t const *context);
lsx_vulkan_resident_format_t lsx_rate_vulkan_resident_format(
    lsx_rate_vulkan_t const *context);
lsx_vulkan_resident_topology_t lsx_rate_effect_resident_topology(
    sox_effect_t const *effp);
int lsx_rate_effect_flow_resident(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
int lsx_rate_effect_drain_resident(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);
sox_bool lsx_rate_effect_resident_supported(sox_effect_t const *effp);
sox_bool lsx_rate_effect_resident_input_supported(sox_effect_t const *effp);
sox_bool lsx_rate_effect_resident_transform_supported(sox_effect_t const *effp);
sox_bool lsx_rate_effect_resident_input_ready(sox_effect_t const *effp);
uint64_t lsx_rate_effect_external_input_clips(sox_effect_t *effp);

#endif
