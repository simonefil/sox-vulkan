/* Vulkan FIR and DSD modulation backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_SDM_VULKAN_H
#define LSX_SDM_VULKAN_H

#include "sox.h"
#include "vulkan_engine.h"

typedef struct lsx_sdm_vulkan lsx_sdm_vulkan_t;

lsx_sdm_vulkan_t *lsx_sdm_vulkan_create(
    lsx_vulkan_context_t *vulkan, unsigned input_rate,
    unsigned output_rate, unsigned channels);
lsx_sdm_vulkan_t *lsx_sdm_vulkan_create_resident(
    lsx_vulkan_context_t *vulkan, unsigned output_rate,
    unsigned channels, size_t producer_block_frames);
void lsx_sdm_vulkan_destroy(lsx_sdm_vulkan_t *context);
size_t lsx_sdm_vulkan_input_capacity(
    lsx_sdm_vulkan_t const *context);
size_t lsx_sdm_vulkan_lookahead(void);

/*
 * Process channel-interleaved PCM frames.  available_frames can include the
 * future frames required by the centred FIR, while frames is the portion that
 * advances the stream.  The returned DSD words are channel-major: each plane
 * contains bytes_per_channel valid bytes and begins channel_stride bytes after
 * the previous plane.  The mapped pointer remains valid until the next call or
 * until the context is destroyed.
 */
int lsx_sdm_vulkan_process(
    lsx_sdm_vulkan_t *context, float const *input,
    size_t frames, size_t available_frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);
int lsx_sdm_vulkan_process_resident(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);
int lsx_sdm_vulkan_process_specialized_resident(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);
sox_bool lsx_sdm_vulkan_specialized_resident_active(
    lsx_sdm_vulkan_t const *context);
uint64_t lsx_sdm_vulkan_specialized_resident_clips(
    lsx_sdm_vulkan_t *context);
int lsx_sdm_vulkan_drain_resident(
    lsx_sdm_vulkan_t *context, sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);
int lsx_sdm_vulkan_resident_output(lsx_sdm_vulkan_t *context, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident);

#endif
