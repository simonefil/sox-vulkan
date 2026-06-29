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

/*
 * One rational resampling stage of the rate effect, run on the GPU.
 *
 * A stage is the classic three steps: insert up_factor - 1 zeros between
 * input samples, filter the result to remove the images that creates, then
 * keep every down_factor'th sample.  The filtering is done by the partitioned
 * FIR backend, so this file is the interpolation, the decimation and the
 * bookkeeping around them -- which is most of the difficulty, since the stage
 * has to account for the filter's latency exactly and keep the decimation
 * phase continuous across blocks.
 *
 * Latency is handled by discarding taps - 1 - post_peak output frames at the
 * start of the stream: that is where the filter's peak lands, so dropping
 * them aligns the output with the input.  The decimation phase is kept in the
 * context and carried from one block to the next, since a block boundary is
 * not in general a multiple of down_factor.
 *
 * Beyond the plain block-at-a-time entry point there is a resident path,
 * where the stage takes its input from a resident buffer and publishes its
 * output as one; and on top of that a resident stream, which accumulates
 * output frames on the device across several blocks so that a consumer
 * wanting a fixed block size is not forced to a round trip per stage.
 */

typedef struct lsx_rate_vulkan lsx_rate_vulkan_t;

/* Build a stage from a prototype low-pass response.
 *
 * taps and post_peak describe the response: post_peak is how many taps follow
 * its peak, which is what fixes the latency to compensate.  up_factor and
 * down_factor are the resampling ratio, already in lowest terms.
 *
 * The coefficients are copied.  Returns NULL on failure, having reported why;
 * a ratio whose interpolated block does not divide evenly is rejected here
 * rather than silently rounded. */
lsx_rate_vulkan_t *lsx_rate_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, size_t taps, size_t post_peak, uint32_t up_factor, uint32_t down_factor, uint32_t channels);

/* As above with one response per channel, and the reference-profile forms
 * whose coefficients arrive as pairs of doubles.  The distinctions match
 * those in fir_vulkan.h, which these forward to. */
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
/* Destroy a stage; safe on NULL and on a partly built one.  Waits for the
 * device, so anything still queued has finished on return. */
void lsx_rate_vulkan_destroy(lsx_rate_vulkan_t *context);

/* Input frames per call: the FIR block size divided by up_factor, since
 * interpolation is what fills the block.  Exact, not a maximum -- every entry
 * point taking host samples expects precisely this many. */
size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context);

/* Process one block and wait.  *output points at *output_frames interleaved
 * frames in a buffer the context owns and overwrites next call.  The count
 * varies from block to block: the decimation phase carries across boundaries,
 * and the first blocks are shortened by the latency being discarded. */
int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames);

/* Process one block of host samples and publish the result as a resident
 * buffer.  normalize asks for output scaled to +/-1 rather than in SoX's
 * sample range, for a consumer that wants it that way; the resident domain
 * field records which was produced. */
int lsx_rate_vulkan_process_resident(lsx_rate_vulkan_t *context, double const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident);

/*
 * The resident stream.  A producer whose block size does not match this
 * stage's appends into a device-side buffer, and the stage consumes from it
 * once a whole block has accumulated -- so the two need not agree on a block
 * size and nothing goes back to the host to be re-blocked.
 */

/* Frames that may still be appended before the stream is full. */
size_t lsx_rate_vulkan_resident_stream_room(lsx_rate_vulkan_t const *context);

/* Append a resident buffer to the stream.  The caller must have checked there
 * is room; the input is read on the device, so it must remain valid until the
 * next submission.  The quantized form rounds each sample to SoX's integer
 * sample grid on the way in, for a producer whose output is nominally
 * integral and must match what the CPU path would have carried. */
int lsx_rate_vulkan_append_resident_stream(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input);
int lsx_rate_vulkan_append_resident_stream_quantized(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input);

/* Consume one block from the stream and publish the result.  *produced is
 * false, with success returned, when the stream does not yet hold a whole
 * block: that is the normal way to be told to append more. */
int lsx_rate_vulkan_process_resident_stream(lsx_rate_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);

/* Submit everything queued and wait; the stage's end-of-stream barrier. */
int lsx_rate_vulkan_flush_resident(lsx_rate_vulkan_t *context);

/* Whether a whole block has accumulated, so that a call to
 * process_resident_stream would produce output. */
sox_bool lsx_rate_vulkan_resident_stream_ready(lsx_rate_vulkan_t const *context);

/* Zero-fill the rest of the current block at end of stream, so the tail is
 * flushed through the filter instead of being left in the buffer. */
int lsx_rate_vulkan_pad_resident_stream(lsx_rate_vulkan_t *context);

/* Clips counted by the quantizing append, which is where a value outside
 * SoX's sample range is clamped.  Counted on the device, so the first form
 * returns what has been read back so far and the second waits for the
 * outstanding blocks first -- which is why the two exist. */
uint32_t lsx_rate_vulkan_resident_stream_clips(lsx_rate_vulkan_t *context);
uint32_t lsx_rate_vulkan_resident_stream_clips_completed(lsx_rate_vulkan_t *context);

/* How many blocks this stage may have in flight, and the element format its
 * resident output carries; both are what a consumer needs to size itself. */
uint32_t lsx_rate_vulkan_resident_batch_depth(lsx_rate_vulkan_t const *context);
lsx_vulkan_resident_format_t lsx_rate_vulkan_resident_format(lsx_rate_vulkan_t const *context);

#endif
