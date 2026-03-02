/* VkFFT rate-stage backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "rate_vulkan.h"

struct lsx_rate_vulkan {
  lsx_fir_vulkan_t *fir;
  double *stage_input;
  double *output;
  size_t input_frames;
  size_t skip_frames;
  uint32_t up_factor;
  uint32_t down_factor;
  uint32_t channels;
  uint32_t decimation_phase;
};

lsx_rate_vulkan_t *lsx_rate_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, size_t taps, size_t post_peak, uint32_t up_factor, uint32_t down_factor, uint32_t channels)
{
  lsx_rate_vulkan_t *context;
  size_t block_frames = lsx_fir_vulkan_block_frames();
  size_t output_capacity;

  if (!vulkan || !coefficients || !taps || post_peak >= taps || !up_factor || !down_factor || !channels || block_frames % up_factor) {
    lsx_fail("unsupported Vulkan rate stage");
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->input_frames = block_frames / up_factor;
  context->skip_frames = taps - 1u - post_peak / up_factor * up_factor;
  context->up_factor = up_factor;
  context->down_factor = down_factor;
  context->channels = channels;
  context->fir = lsx_fir_vulkan_create(vulkan, coefficients, taps, channels);
  if (!context->fir)
    goto error;
  context->stage_input = lsx_calloc(block_frames * channels, sizeof(*context->stage_input));
  output_capacity = (block_frames + down_factor - 1u) / down_factor;
  context->output = lsx_malloc(output_capacity * channels * sizeof(*context->output));
  lsx_report("Vulkan rate: %u/%u, %lu taps, %u channel%s", up_factor, down_factor, (unsigned long)taps, channels, channels == 1u ? "" : "s");
  return context;

error:
  lsx_rate_vulkan_destroy(context);
  return NULL;
}

void lsx_rate_vulkan_destroy(lsx_rate_vulkan_t *context)
{
  if (!context)
    return;
  lsx_fir_vulkan_destroy(context->fir);
  free(context->stage_input);
  free(context->output);
  free(context);
}

size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context)
{
  return context ? context->input_frames : 0;
}

int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames)
{
  size_t block_frames = lsx_fir_vulkan_block_frames();
  double const *filtered;
  size_t input_frame;
  size_t output_frame = 0;
  size_t channel;
  size_t frame;

  if (!context || !input || !output || !output_frames)
    return SOX_EOF;
  memset(context->stage_input, 0, block_frames * context->channels * sizeof(*context->stage_input));
  for (input_frame = 0; input_frame < context->input_frames; ++input_frame)
    for (channel = 0; channel < context->channels; ++channel)
      context->stage_input[(input_frame * context->up_factor) * context->channels + channel] = input[input_frame * context->channels + channel] * context->up_factor;
  if (lsx_fir_vulkan_process(context->fir, context->stage_input, &filtered) != SOX_SUCCESS)
    return SOX_EOF;
  for (frame = 0; frame < block_frames; ++frame) {
    if (context->skip_frames) {
      --context->skip_frames;
      continue;
    }
    if (!context->decimation_phase) {
      memcpy(context->output + output_frame * context->channels, filtered + frame * context->channels, context->channels * sizeof(*context->output));
      ++output_frame;
    }
    if (++context->decimation_phase == context->down_factor)
      context->decimation_phase = 0;
  }
  *output = context->output;
  *output_frames = output_frame;
  return SOX_SUCCESS;
}
