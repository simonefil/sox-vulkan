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

/*
 * The modulator does not resample.  It consumes a stream already at the DSD
 * rate, from the host or from a resident producer, and emits packed DSD.
 *
 * A sigma-delta modulator is serial by construction -- each output bit
 * depends on the error left by the one before it -- so what makes a GPU
 * implementation possible is that both of its parts are associative in
 * disguise: the noise-shaping accumulators are prefix sums, and the one-bit
 * reducer is a finite-state machine whose per-block transitions compose.
 * Both are exact integer computations, so the result is bit for bit what a
 * serial modulator would have produced, not an approximation of it.
 */

/* Whether a rate is one of the DSD rates this backend implements. */
sox_bool lsx_sdm_vulkan_dsd_rate_supported(unsigned rate);
/*
 * batch_frames is the largest block the producer will hand over.  The
 * backend reserves one partial FSM block beyond it to preserve continuity
 * between slices; zero asks for the default host-side batch.
 */
lsx_sdm_vulkan_t *lsx_sdm_vulkan_create(
    lsx_vulkan_context_t *vulkan, unsigned rate, unsigned channels,
    size_t batch_frames);
/* Destroy a modulator; safe on NULL and on a partly built one. */
void lsx_sdm_vulkan_destroy(lsx_sdm_vulkan_t *context);

/* Most frames the modulator will accept before it must be run. */
size_t lsx_sdm_vulkan_input_capacity(lsx_sdm_vulkan_t const *context);

/*
 * Process channel-interleaved normalized PCM frames at the DSD rate.  The
 * returned DSD words are channel-major: each plane contains
 * bytes_per_channel valid bytes and begins channel_stride bytes after the
 * previous plane.  The mapped pointer remains valid until the next call or
 * until the context is destroyed.
 */
int lsx_sdm_vulkan_process(
    lsx_sdm_vulkan_t *context, float const *input,
    size_t frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);

/*
 * Consume one slice from a resident producer.  Any of the four resident
 * formats is accepted in either domain, so the profile picked for the chain
 * reaches the modulator unchanged.  A NULL input drains what is buffered
 * once the final slice has been seen.
 */
int lsx_sdm_vulkan_consume_resident(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);
/* Whether a further drain is owed: the stream has ended but frames are still
 * buffered, so the caller must call again with a NULL input. */
sox_bool lsx_sdm_vulkan_resident_active(lsx_sdm_vulkan_t const *context);

/* Samples the ingest had to clamp, counted on the device.  Meaningful only
 * once the work that wrote them has completed. */
uint64_t lsx_sdm_vulkan_resident_clips(lsx_sdm_vulkan_t const *context);

#endif
