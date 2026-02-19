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

typedef struct lsx_sdm_vulkan lsx_sdm_vulkan_t;

lsx_sdm_vulkan_t *lsx_sdm_vulkan_create(
    unsigned input_rate, unsigned output_rate, unsigned channels);
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

#endif
