/* VkFFT rate-stage backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "rate_vulkan.h"

#include "rate_select_f64_spv.inc"
#include "rate_select_f32_spv.inc"
#include "rate_select_strict_f32_spv.inc"
#include "rate_select_reference_dd_spv.inc"
#include "rate_prepare_f64_spv.inc"
#include "rate_prepare_f32_spv.inc"
#include "rate_prepare_strict_f32_spv.inc"
#include "rate_prepare_reference_dd_spv.inc"
#include "rate_stream_append_f64_spv.inc"
#include "rate_stream_append_f32_spv.inc"
#include "rate_stream_append_strict_f32_spv.inc"
#include "rate_stream_append_reference_dd_spv.inc"

/* Three small helper shaders do the work either side of the FIR, so that a
 * resident block never leaves the device merely to be rearranged:
 *
 *  - prepare interpolates, writing each input frame into every up_factor'th
 *    slot of the FIR's input and zeroing the rest;
 *  - select decimates, copying every down_factor'th filtered frame out;
 *  - stream append accumulates frames from a producer whose block size does
 *    not match this stage's.
 *
 * Each has its own descriptor layout, pipeline and push constant block; the
 * binding counts and layouts below must match what the shaders declare, which
 * is what the assertions check.
 */
#define RATE_SELECT_BINDINGS 2u
#define RATE_SELECT_LOCAL_SIZE 128u
#define RATE_PREPARE_BINDINGS 3u
#define RATE_STREAM_APPEND_BINDINGS 3u

/* input_step is the decimation factor and first_input_frame the phase to
 * start from, which together carry the phase across block boundaries. */
typedef struct {
  uint32_t output_frames;
  uint32_t first_input_frame;
  uint32_t input_step;
  uint32_t input_channel_stride;
  uint32_t channels;
  uint32_t normalize;
  uint32_t padding[2];
} select_parameters_t;

lsx_static_assert(sizeof(select_parameters_t) == 32, vulkan_rate_select_push_layout);

typedef struct {
  uint32_t block_frames;
  uint32_t input_frames;
  uint32_t input_frame_stride;
  uint32_t input_channel_stride;
  uint32_t prepared_channel_stride;
  uint32_t up_factor;
  uint32_t channels;
  uint32_t padding;
} prepare_parameters_t;

lsx_static_assert(sizeof(prepare_parameters_t) == 32, vulkan_rate_prepare_push_layout);

typedef struct {
  uint32_t input_base_element;
  uint32_t output_first_frame;
  uint32_t frames;
  uint32_t input_frame_stride;
  uint32_t input_channel_stride;
  uint32_t channels;
  uint32_t quantize_sox_sample;
  uint32_t clip_index;
} stream_append_parameters_t;

lsx_static_assert(sizeof(stream_append_parameters_t) == 32, vulkan_rate_stream_append_push_layout);

struct lsx_rate_vulkan {
  lsx_vulkan_context_t *vulkan;  /* Not owned. */
  lsx_fir_vulkan_t *fir;         /* Owned; does the filtering between the two steps. */

  /* Decimation ("select") resources.  Everything indexed by bank exists once
   * per block in flight, so preparing the next block does not disturb one the
   * device has not finished. */
  lsx_vulkan_buffer_t resident_output;
  VkDescriptorSetLayout resident_descriptor_layout;
  VkDescriptorPool resident_descriptor_pool;
  VkDescriptorSet resident_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout resident_pipeline_layout;
  VkPipeline resident_pipeline;
  VkCommandBuffer resident_command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkFence resident_fence;
  lsx_vulkan_buffer_t resident_previous;

  /* Interpolation ("prepare") resources, created lazily: a stage only needs
   * them once it is fed a resident input. */
  VkDescriptorSetLayout prepare_descriptor_layout;
  VkDescriptorPool prepare_descriptor_pool;
  VkDescriptorSet prepare_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout prepare_pipeline_layout;
  VkPipeline prepare_pipeline;
  VkCommandBuffer prepare_command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  sox_bool prepare_initialized;

  /* Resident stream resources.  Two buffers, so one can be filled while the
   * other is being consumed; resident_stream_index says which is current. */
  lsx_vulkan_buffer_t resident_stream[2];
  lsx_vulkan_buffer_t stream_append_clips; /* Device-side clip counters. */
  VkCommandBuffer resident_stream_commands[LSX_VULKAN_RESIDENT_BATCH_DEPTH * 2u];
  VkDescriptorSetLayout stream_append_descriptor_layout;
  VkDescriptorPool stream_append_descriptor_pool;
  VkDescriptorSet stream_append_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout stream_append_pipeline_layout;
  VkPipeline stream_append_pipeline;
  size_t resident_stream_capacity;
  size_t resident_stream_occupancy; /* Frames appended but not yet consumed. */
  uint32_t resident_stream_index;
  /* Independent rotations over the per-block resources.  They advance at
   * different times -- an append is not a block -- so each has its own. */
  uint32_t resident_select_bank_index;
  uint32_t resident_prepare_bank_index;
  uint32_t resident_stream_command_index;
  uint32_t resident_stream_descriptor_index;
  uint32_t resident_stream_clip_pending_mask; /* Counters written but not yet read. */

  double *stage_input;           /* Host-side interpolated block for the plain path. */
  double *output;                /* Host-side decimated result of _process. */
  size_t input_frames;           /* Exactly what a caller must supply per block. */
  size_t output_capacity;

  /* Output frames still to be discarded for the filter's latency.  Counts
   * down to zero over the first blocks and stays there. */
  size_t skip_frames;

  uint32_t up_factor;
  uint32_t down_factor;
  uint32_t channels;
  /* Position in the decimation cycle, carried across blocks: a block boundary
   * is not in general a multiple of down_factor, so resetting it per block
   * would shift the output phase every time. */
  uint32_t decimation_phase;

  sox_bool double_precision;
  sox_bool strict_fp32;
  sox_bool reference_dd;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static size_t resident_sample_size(lsx_rate_vulkan_t const *context)
{
  return context->reference_dd ?
      2u * sizeof(double) :
      context->strict_fp32 ?
      2u * sizeof(float) :
      context->double_precision ? sizeof(double) : sizeof(float);
}

static lsx_vulkan_resident_format_t resident_format(lsx_rate_vulkan_t const *context)
{
  return context->reference_dd ?
      lsx_vulkan_resident_format_f64x2 :
      context->strict_fp32 ?
      lsx_vulkan_resident_format_f32x2 :
      context->double_precision ?
      lsx_vulkan_resident_format_f64 :
      lsx_vulkan_resident_format_f32;
}

lsx_vulkan_resident_format_t lsx_rate_vulkan_resident_format(lsx_rate_vulkan_t const *context)
{
  return resident_format(context);
}

/* Each resident kernel is compiled once per numerical family, and the select,
 * prepare and stream-append pipelines all choose among the families with the
 * same rule.  Naming the rule once is what keeps the three in step: three
 * copies of the test order would only have to agree by inspection. */
typedef enum {
  resident_kernel_reference_dd,
  resident_kernel_f64,
  resident_kernel_strict_f32,
  resident_kernel_f32,
  resident_kernel_count
} resident_kernel_t;

typedef struct {
  uint32_t const *spirv;
  size_t size;
} resident_kernel_blob_t;

/* Blob and size are taken from one name, so they cannot describe different
 * modules. */
#define RESIDENT_KERNEL(name) {name, sizeof(name)}

/* Only the first test is order-sensitive: reference_dd is set as
 * double_precision && profile == reference, so it has to be recognised before
 * the plain FP64 family.  strict_fp32 is set only when double_precision is
 * false, so the families below cannot overlap.
 *
 * Four kernels for five profiles: accurate has none of its own and falls to
 * the FP32 one alongside fast.  Unlike the polyphase, cubic and FIR partition
 * families, select, prepare and stream-append only move samples, with at most
 * the single multiply that normalisation costs, so there is no accumulation
 * for an accurate variant to order differently. */
static resident_kernel_t resident_kernel(lsx_rate_vulkan_t const *context)
{
  if (context->reference_dd)
    return resident_kernel_reference_dd;
  if (context->double_precision)
    return resident_kernel_f64;
  if (context->strict_fp32)
    return resident_kernel_strict_f32;
  return resident_kernel_f32;
}

/* One entry per resident_kernel_t, in enum order. */
static resident_kernel_blob_t const select_kernels[resident_kernel_count] = {
  RESIDENT_KERNEL(rate_select_reference_dd_spv),
  RESIDENT_KERNEL(rate_select_f64_spv),
  RESIDENT_KERNEL(rate_select_strict_f32_spv),
  RESIDENT_KERNEL(rate_select_f32_spv)
};

static resident_kernel_blob_t const prepare_kernels[resident_kernel_count] = {
  RESIDENT_KERNEL(rate_prepare_reference_dd_spv),
  RESIDENT_KERNEL(rate_prepare_f64_spv),
  RESIDENT_KERNEL(rate_prepare_strict_f32_spv),
  RESIDENT_KERNEL(rate_prepare_f32_spv)
};

static resident_kernel_blob_t const stream_append_kernels[resident_kernel_count] = {
  RESIDENT_KERNEL(rate_stream_append_reference_dd_spv),
  RESIDENT_KERNEL(rate_stream_append_f64_spv),
  RESIDENT_KERNEL(rate_stream_append_strict_f32_spv),
  RESIDENT_KERNEL(rate_stream_append_f32_spv)
};

static int create_resident_output(lsx_rate_vulkan_t *context)
{
  resident_kernel_blob_t const *kernel = &select_kernels[resident_kernel(context)];
  VkDescriptorSetLayoutBinding bindings[RATE_SELECT_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(select_parameters_t)
  };
  VkPipelineLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_SELECT_BINDINGS
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
  };
  VkDescriptorSetAllocateInfo set_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorSetLayout layouts[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkCommandBufferAllocateInfo command_info = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
  };
  VkFenceCreateInfo fence_info = {
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };
  uint32_t index;

  if (lsx_vulkan_buffer_create(
      context->vulkan, &context->resident_output,
      (VkDeviceSize)context->output_capacity *
      context->channels * resident_sample_size(context),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  memset(bindings, 0, sizeof(bindings));
  for (index = 0; index < RATE_SELECT_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_SELECT_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      context->vulkan->device, &descriptor_info, NULL,
      &context->resident_descriptor_layout),
      "vkCreateDescriptorSetLayout rate select") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->resident_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->resident_pipeline_layout),
      "vkCreatePipelineLayout rate select") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(context->vulkan, kernel->spirv, kernel->size, context->resident_pipeline_layout, &context->resident_pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_size.descriptorCount *= LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.maxSets = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(
      context->vulkan->device, &pool_info, NULL,
      &context->resident_descriptor_pool),
      "vkCreateDescriptorPool rate select") != SOX_SUCCESS)
    return SOX_EOF;
  set_info.descriptorPool = context->resident_descriptor_pool;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    layouts[index] = context->resident_descriptor_layout;
  set_info.descriptorSetCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  set_info.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(
      context->vulkan->device, &set_info,
      context->resident_descriptor_sets),
      "vkAllocateDescriptorSets rate select") != SOX_SUCCESS)
    return SOX_EOF;
  command_info.commandPool = context->vulkan->command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &command_info,
      context->resident_command_buffers),
      "vkAllocateCommandBuffers rate select") != SOX_SUCCESS ||
      vk_result(vkCreateFence(
      context->vulkan->device, &fence_info, NULL,
      &context->resident_fence),
      "vkCreateFence rate select") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int create_resident_prepare(lsx_rate_vulkan_t *context)
{
  resident_kernel_blob_t const *kernel = &prepare_kernels[resident_kernel(context)];
  VkDescriptorSetLayoutBinding bindings[RATE_PREPARE_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(prepare_parameters_t)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_PREPARE_BINDINGS};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  VkDescriptorSetAllocateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  VkDescriptorSetLayout layouts[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkCommandBufferAllocateInfo command_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  VkDeviceSize previous_size =
      (VkDeviceSize)lsx_fir_vulkan_block_frames_for(
      context->vulkan) *
      context->channels * resident_sample_size(context);
  uint32_t index;

  if (lsx_vulkan_buffer_create(context->vulkan, &context->resident_previous, previous_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  memset(bindings, 0, sizeof(bindings));
  for (index = 0; index < RATE_PREPARE_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_PREPARE_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(context->vulkan->device, &descriptor_info, NULL, &context->prepare_descriptor_layout), "vkCreateDescriptorSetLayout rate prepare") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->prepare_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(context->vulkan->device, &layout_info, NULL, &context->prepare_pipeline_layout), "vkCreatePipelineLayout rate prepare") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(context->vulkan, kernel->spirv, kernel->size, context->prepare_pipeline_layout, &context->prepare_pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_size.descriptorCount *= LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.maxSets = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(context->vulkan->device, &pool_info, NULL, &context->prepare_descriptor_pool), "vkCreateDescriptorPool rate prepare") != SOX_SUCCESS)
    return SOX_EOF;
  set_info.descriptorPool = context->prepare_descriptor_pool;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    layouts[index] = context->prepare_descriptor_layout;
  set_info.descriptorSetCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  set_info.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(context->vulkan->device, &set_info, context->prepare_descriptor_sets), "vkAllocateDescriptorSets rate prepare") != SOX_SUCCESS)
    return SOX_EOF;
  command_info.commandPool = context->vulkan->command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  return vk_result(vkAllocateCommandBuffers(context->vulkan->device, &command_info, context->prepare_command_buffers), "vkAllocateCommandBuffers rate prepare");
}

/* The one constructor the four public forms funnel into; coefficient_lows is
 * non-NULL only for the reference profile, and coefficient_channels is 1 for
 * a shared response.
 *
 * The FIR block must divide evenly by up_factor, or a block would hold a
 * fractional number of input frames and the interpolation phase would drift
 * from block to block.  It is rejected rather than worked around. */
static lsx_rate_vulkan_t *create_rate(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients,
    double const *const *coefficient_lows,
    uint32_t coefficient_channels,
    size_t taps, size_t post_peak,
    uint32_t up_factor, uint32_t down_factor,
    uint32_t channels)
{
  lsx_rate_vulkan_t *context;
  size_t block_frames = lsx_fir_vulkan_block_frames_for(vulkan);
  size_t output_capacity;

  if (!vulkan || !coefficients || !taps || post_peak >= taps ||
      !up_factor || !down_factor || !channels ||
      (coefficient_channels != 1u &&
       coefficient_channels != channels) ||
      block_frames % up_factor) {
    lsx_fail("unsupported Vulkan rate stage");
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->double_precision = vulkan->use_float64;
  context->reference_dd = context->double_precision && vulkan->profile == sox_vulkan_profile_reference;
  context->strict_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_strict;
  context->input_frames = block_frames / up_factor;
  /* The skip is counted in output frames, so it carries post_peak exactly.
   * Rounding it down to a multiple of up_factor -- which mirrored the input
   * frame granularity of the CPU preload -- left the remainder unaccounted
   * for on every filter whose peak is not a multiple of up_factor, that is on
   * every phase other than linear. */
  context->skip_frames = taps - 1u - post_peak;
  context->up_factor = up_factor;
  context->down_factor = down_factor;
  context->channels = channels;
  context->fir = coefficient_lows ?
      (coefficient_channels == 1u ?
       lsx_fir_vulkan_create_reference_dd(
           vulkan, coefficients[0], coefficient_lows[0],
           taps, channels) :
       lsx_fir_vulkan_create_reference_dd_channels(
           vulkan, coefficients, coefficient_lows,
           taps, channels)) :
      (coefficient_channels == 1u ?
       lsx_fir_vulkan_create(
           vulkan, coefficients[0], taps, channels) :
       lsx_fir_vulkan_create_channels(
           vulkan, coefficients, taps, channels));
  if (!context->fir)
    goto error;
  context->stage_input = lsx_calloc(block_frames * channels, sizeof(*context->stage_input));
  output_capacity = (block_frames + down_factor - 1u) / down_factor;
  context->output_capacity = output_capacity;
  context->output = lsx_malloc(output_capacity * channels * sizeof(*context->output));
  lsx_report("Vulkan rate: %u/%u, %lu taps, %u channel%s", up_factor, down_factor, (unsigned long)taps, channels, channels == 1u ? "" : "s");
  lsx_report(
      "Vulkan rate resident helpers precision: %s "
      "(fixed-format sample movement)",
      context->reference_dd ? "FP64x2" :
      context->double_precision ? "FP64" :
      context->strict_fp32 ? "FP32x2" : "FP32");
  return context;

error: lsx_rate_vulkan_destroy(context);
  return NULL;
}

lsx_rate_vulkan_t *lsx_rate_vulkan_create(
    lsx_vulkan_context_t *vulkan,
    double const *coefficients, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels)
{
  double const *channel_coefficients[] = {coefficients};

  return create_rate(vulkan, channel_coefficients, NULL, 1u, taps, post_peak, up_factor, down_factor, channels);
}

lsx_rate_vulkan_t *lsx_rate_vulkan_create_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels)
{
  return create_rate(vulkan, coefficients, NULL, channels, taps, post_peak, up_factor, down_factor, channels);
}

lsx_rate_vulkan_t *lsx_rate_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels)
{
  double const *channel_highs[] = {coefficient_highs};
  double const *channel_lows[] = {coefficient_lows};

  if (!coefficient_lows || !vulkan || vulkan->profile != sox_vulkan_profile_reference)
    return NULL;
  return create_rate(vulkan, channel_highs, channel_lows, 1u, taps, post_peak, up_factor, down_factor, channels);
}

lsx_rate_vulkan_t *lsx_rate_vulkan_create_reference_dd_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_highs,
    double const *const *coefficient_lows, size_t taps,
    size_t post_peak, uint32_t up_factor,
    uint32_t down_factor, uint32_t channels)
{
  if (!coefficient_lows || !vulkan || vulkan->profile != sox_vulkan_profile_reference)
    return NULL;
  return create_rate(
      vulkan, coefficient_highs, coefficient_lows, channels,
      taps, post_peak, up_factor, down_factor,
      channels);
}

void lsx_rate_vulkan_destroy(lsx_rate_vulkan_t *context)
{
  if (!context)
    return;
  vkDeviceWaitIdle(context->vulkan->device);
  if (context->resident_fence)
    vkDestroyFence(context->vulkan->device, context->resident_fence, NULL);
  if (context->resident_command_buffers[0])
    vkFreeCommandBuffers(
        context->vulkan->device, context->vulkan->command_pool,
        LSX_VULKAN_RESIDENT_BATCH_DEPTH, context->resident_command_buffers);
  if (context->prepare_command_buffers[0])
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, LSX_VULKAN_RESIDENT_BATCH_DEPTH, context->prepare_command_buffers);
  if (context->prepare_pipeline)
    vkDestroyPipeline(context->vulkan->device, context->prepare_pipeline, NULL);
  if (context->prepare_pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->prepare_pipeline_layout, NULL);
  if (context->prepare_descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->prepare_descriptor_pool, NULL);
  if (context->prepare_descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->prepare_descriptor_layout, NULL);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_previous);
  if (context->resident_stream_commands[0])
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, LSX_VULKAN_RESIDENT_BATCH_DEPTH * 2u, context->resident_stream_commands);
  if (context->stream_append_pipeline)
    vkDestroyPipeline(context->vulkan->device, context->stream_append_pipeline, NULL);
  if (context->stream_append_pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->stream_append_pipeline_layout, NULL);
  if (context->stream_append_descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->stream_append_descriptor_pool, NULL);
  if (context->stream_append_descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->stream_append_descriptor_layout, NULL);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->stream_append_clips);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_stream[1]);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_stream[0]);
  if (context->resident_pipeline)
    vkDestroyPipeline(context->vulkan->device, context->resident_pipeline, NULL);
  if (context->resident_pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->resident_pipeline_layout, NULL);
  if (context->resident_descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->resident_descriptor_pool, NULL);
  if (context->resident_descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->resident_descriptor_layout, NULL);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_output);
  lsx_fir_vulkan_destroy(context->fir);
  free(context->stage_input);
  free(context->output);
  free(context);
}

size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context)
{
  return context ? context->input_frames : 0;
}

/* Interpolate one block on the host: write each input frame into every
 * up_factor'th slot and leave the rest zero.
 *
 * The gain of up_factor is the compensation interpolation needs: inserting
 * zeros divides the signal's average power by that factor, and the low-pass
 * that follows does not put it back.  Applying it here rather than folding it
 * into the coefficients keeps the same prototype response usable at any ratio.
 *
 * The whole block is zeroed first, since only one slot in up_factor is
 * written and the rest must not carry over from the previous block. */
static void prepare_stage_input(lsx_rate_vulkan_t *context, double const *input)
{
  size_t block_frames = lsx_fir_vulkan_block_frames_for(context->vulkan);
  size_t input_frame;
  size_t channel;

  memset(context->stage_input, 0, block_frames * context->channels * sizeof(*context->stage_input));
  for (input_frame = 0; input_frame < context->input_frames; ++input_frame)
    for (channel = 0; channel < context->channels; ++channel)
      context->stage_input[
          (input_frame * context->up_factor) *
          context->channels + channel] =
          input[input_frame * context->channels + channel] *
          context->up_factor;
}

int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames)
{
  size_t block_frames;
  double const *filtered;
  size_t output_frame = 0;
  size_t frame;

  if (!context || !input || !output || !output_frames)
    return SOX_EOF;
  block_frames = lsx_fir_vulkan_block_frames_for(context->vulkan);
  prepare_stage_input(context, input);
  if (lsx_fir_vulkan_process(context->fir, context->stage_input, &filtered) != SOX_SUCCESS)
    return SOX_EOF;
  /* Decimate, discarding the filter's latency first.  Both counters persist
   * across blocks: the skip runs out during the first blocks and the phase
   * carries on wherever the last block left it. */
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

/* Record and queue the selection dispatch for one block.  Nothing is
 * submitted or waited on: the command buffer joins the pending batch.
 *
 * The descriptor set is rewritten each time because the input buffer differs
 * from block to block, which is also why each block in flight needs its own
 * set and command buffer -- rewriting one the device is still reading would
 * corrupt work already queued.  The input's offset must satisfy the device's
 * storage-buffer alignment, which is checked rather than assumed, since it
 * comes from another effect's description. */
static int run_resident_selection(
    lsx_rate_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    select_parameters_t const *parameters)
{
  VkCommandBuffer command_buffer = context->resident_command_buffers[context->resident_select_bank_index];
  VkDescriptorSet descriptor_set = context->resident_descriptor_sets[context->resident_select_bank_index];
  VkDescriptorBufferInfo infos[RATE_SELECT_BINDINGS];
  VkWriteDescriptorSet writes[RATE_SELECT_BINDINGS];
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkMemoryBarrier input_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    0, VK_ACCESS_SHADER_READ_BIT
  };
  uint32_t index;

  if (input->offset % context->vulkan->properties.limits.minStorageBufferOffsetAlignment) {
    lsx_fail("resident Vulkan rate input is not storage-buffer aligned");
    return SOX_EOF;
  }
  infos[0].buffer = input->buffer->buffer;
  infos[0].offset = input->offset;
  infos[0].range = lsx_vulkan_resident_buffer_size(input);
  infos[1].buffer = context->resident_output.buffer;
  infos[1].offset = 0;
  infos[1].range = context->resident_output.size;
  if (infos[0].range > context->vulkan->properties.limits.maxStorageBufferRange) {
    lsx_fail("resident Vulkan rate input exceeds device storage range");
    return SOX_EOF;
  }
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < RATE_SELECT_BINDINGS; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &infos[index];
  }
  vkUpdateDescriptorSets(context->vulkan->device, RATE_SELECT_BINDINGS, writes, 0, NULL);
  input_barrier.srcAccessMask = input->producer_access;
  if (vk_result(vkResetCommandBuffer(
      command_buffer, 0),
      "vkResetCommandBuffer rate select") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      command_buffer, &begin),
      "vkBeginCommandBuffer rate select") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate resident selector");
  vkCmdPipelineBarrier(
      command_buffer,
      input->producer_stage,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
      1, &input_barrier, 0, NULL, 0, NULL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->resident_pipeline);
  vkCmdBindDescriptorSets(
      command_buffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      context->resident_pipeline_layout, 0, 1,
      &descriptor_set, 0, NULL);
  vkCmdPushConstants(
      command_buffer,
      context->resident_pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(*parameters), parameters);
  vkCmdDispatch(
      command_buffer,
      (parameters->output_frames + RATE_SELECT_LOCAL_SIZE - 1u) /
      RATE_SELECT_LOCAL_SIZE,
      context->channels, 1);
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer rate select") != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_enqueue(context->vulkan, command_buffer) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_select_bank_index = (context->resident_select_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  return SOX_SUCCESS;
}

/* Decimate a filtered block on the device and describe the result.
 *
 * This is where the latency skip and the decimation phase are applied, both
 * being state that spans blocks.  first is the offset to the block's first
 * kept frame: with a phase of p already consumed, the next multiple of
 * down_factor is down_factor - p frames away.  The count that follows is how
 * many multiples fit in what is left, and the phase is then advanced by the
 * block's length modulo the factor, which is what carries it forward.
 *
 * The phase is advanced on every path, the empty one included: a block that
 * yields nothing still consumed frames, and forgetting them would shift every
 * later block.  allow_empty says whether that is acceptable to the caller --
 * for the streaming path it is normal, for a single block it is a bug.
 *
 * The published buffer is interleaved rather than planar, unlike the FIR's:
 * the selection shader writes it that way because the eventual consumer or
 * download wants interleaved frames. */
static int finish_resident_process(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *filtered, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, sox_bool allow_empty, lsx_vulkan_resident_buffer_t *resident)
{
  size_t block_frames = lsx_fir_vulkan_block_frames_for(context->vulkan);
  select_parameters_t parameters;
  size_t skipped;
  size_t available;
  size_t first;
  size_t output_frames;

  if (!context->resident_pipeline && create_resident_output(context) != SOX_SUCCESS) {
    lsx_fail("cannot create resident Vulkan rate selector");
    return SOX_EOF;
  }
  skipped = min(context->skip_frames, block_frames);
  context->skip_frames -= skipped;
  available = block_frames - skipped;
  first = context->decimation_phase ? context->down_factor - context->decimation_phase : 0u;
  output_frames = available > first ? 1u + (available - first - 1u) / context->down_factor : 0u;
  if (output_frames > context->output_capacity) {
    lsx_fail("resident Vulkan rate selector produced invalid frame count %lu", (unsigned long)output_frames);
    return SOX_EOF;
  }
  if (!output_frames) {
    context->decimation_phase =
        (context->decimation_phase +
        (uint32_t)(available % context->down_factor)) %
        context->down_factor;
    if (!allow_empty) {
      lsx_fail("resident Vulkan rate selector produced invalid frame count 0");
      return SOX_EOF;
    }
    memset(resident, 0, sizeof(*resident));
    return SOX_SUCCESS;
  }
  memset(&parameters, 0, sizeof(parameters));
  parameters.output_frames = (uint32_t)output_frames;
  parameters.first_input_frame = (uint32_t)(skipped + first);
  parameters.input_step = context->down_factor;
  parameters.input_channel_stride = (uint32_t)filtered->channel_stride_elements;
  parameters.channels = context->channels;
  parameters.normalize = normalize && !lsx_sample_values_are_normalized() ? 1u : 0u;
  context->decimation_phase =
      (context->decimation_phase +
      (uint32_t)(available % context->down_factor)) %
      context->down_factor;
  if (run_resident_selection(context, filtered, &parameters) != SOX_SUCCESS) {
    lsx_fail("resident Vulkan rate selection failed");
    return SOX_EOF;
  }
  memset(resident, 0, sizeof(*resident));
  resident->buffer = &context->resident_output;
  resident->owner = context;
  resident->producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = context->output_capacity;
  resident->valid_elements = output_frames;
  /* The output buffer bounds every slice this stage can hand over. */
  resident->block_elements = context->output_capacity;
  resident->frame_stride_elements = context->channels;
  resident->channel_stride_elements = 1u;
  resident->frame_offset = frame_offset;
  resident->rate = rate;
  resident->channels = context->channels;
  resident->frames_per_element = 1u;
  resident->format = resident_format(context);
  resident->domain = normalize ? lsx_vulkan_resident_domain_normalized : lsx_vulkan_resident_domain_sox_sample;
  resident->layout = lsx_vulkan_resident_layout_interleaved;
  resident->state = state;
  if (lsx_vulkan_resident_buffer_validate(resident) != SOX_SUCCESS) {
    lsx_fail("resident Vulkan rate output validation failed");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Interpolate a resident input block straight into the FIR's prepared input
 * buffer, the device-side counterpart of prepare_stage_input.
 *
 * The input is checked against everything this stage assumes of it -- format,
 * domain, channel count, and exactly one block's worth of frames -- because
 * it comes from another effect and a mismatch would otherwise be read as
 * samples.  The previous buffer holds the overlap the FIR needs between
 * blocks, which the shader writes as it goes. */
static int record_resident_prepare(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input)
{
  VkCommandBuffer command_buffer;
  VkDescriptorSet descriptor_set;
  VkDescriptorBufferInfo infos[RATE_PREPARE_BINDINGS];
  VkWriteDescriptorSet writes[RATE_PREPARE_BINDINGS];
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier clear_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  VkMemoryBarrier prepared_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  VkBufferMemoryBarrier input_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, 0, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
  prepare_parameters_t parameters;
  lsx_vulkan_buffer_t *prepared;
  size_t block_frames = lsx_fir_vulkan_block_frames_for(context->vulkan);
  uint32_t index;

  if (!input ||
      lsx_vulkan_resident_buffer_validate(input) != SOX_SUCCESS ||
      input->format != resident_format(context) ||
      input->domain != lsx_vulkan_resident_domain_sox_sample ||
      input->frames_per_element != 1u ||
      input->channels != context->channels ||
      input->valid_elements != context->input_frames ||
      input->state == lsx_vulkan_resident_empty)
    return SOX_EOF;
  if (!context->prepare_pipeline && create_resident_prepare(context) != SOX_SUCCESS)
    return SOX_EOF;
  command_buffer = context->prepare_command_buffers[context->resident_prepare_bank_index];
  descriptor_set = context->prepare_descriptor_sets[context->resident_prepare_bank_index];
  prepared = lsx_fir_vulkan_prepared_input_buffer(context->fir);
  if (!prepared || input->offset % context->vulkan->properties.limits.minStorageBufferOffsetAlignment)
    return SOX_EOF;
  infos[0].buffer = input->buffer->buffer;
  infos[0].offset = input->offset;
  infos[0].range = lsx_vulkan_resident_buffer_size(input);
  infos[1].buffer = prepared->buffer;
  infos[1].offset = 0;
  infos[1].range = prepared->size;
  infos[2].buffer = context->resident_previous.buffer;
  infos[2].offset = 0;
  infos[2].range = context->resident_previous.size;
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < RATE_PREPARE_BINDINGS; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &infos[index];
  }
  vkUpdateDescriptorSets(context->vulkan->device, RATE_PREPARE_BINDINGS, writes, 0, NULL);
  memset(&parameters, 0, sizeof(parameters));
  parameters.block_frames = (uint32_t)block_frames;
  parameters.input_frames = (uint32_t)context->input_frames;
  parameters.input_frame_stride = (uint32_t)input->frame_stride_elements;
  parameters.input_channel_stride = (uint32_t)input->channel_stride_elements;
  parameters.prepared_channel_stride = (uint32_t)lsx_fir_vulkan_prepared_stride(context->fir);
  parameters.up_factor = context->up_factor;
  parameters.channels = context->channels;
  input_barrier.srcAccessMask = input->producer_access;
  input_barrier.buffer = input->buffer->buffer;
  input_barrier.offset = input->offset;
  input_barrier.size = infos[0].range;
  if (vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer rate prepare") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer rate prepare") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate resident input prepare");
  /* Zero the overlap on the first block only, so the filter starts against
   * silence rather than against uninitialised memory.  Done here, inside the
   * first recorded block, rather than as a separate submission at creation:
   * the buffer only exists once a resident input actually arrives. */
  if (!context->prepare_initialized) {
    vkCmdFillBuffer(command_buffer, context->resident_previous.buffer, 0, context->resident_previous.size, 0);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clear_barrier, 0, NULL, 0, NULL);
    context->prepare_initialized = sox_true;
  }
  vkCmdPipelineBarrier(command_buffer, input->producer_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &input_barrier, 0, NULL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->prepare_pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->prepare_pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
  vkCmdPushConstants(command_buffer, context->prepare_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
  vkCmdDispatch(command_buffer, ((uint32_t)block_frames + RATE_SELECT_LOCAL_SIZE - 1u) / RATE_SELECT_LOCAL_SIZE, context->channels, 1);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &prepared_barrier, 0, NULL, 0, NULL);
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer rate prepare") != SOX_SUCCESS || lsx_vulkan_enqueue(context->vulkan, command_buffer) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_prepare_bank_index = (context->resident_prepare_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  return SOX_SUCCESS;
}

/* Build the resident stream: two buffers, the append pipeline and the clip
 * counters.  Created lazily, since only a chain whose producer has a
 * different block size needs any of it.
 *
 * The capacity is one whole block plus one call's worth of input, so that an
 * append can always be accepted in full even when the stream already holds
 * nearly a block: without the slack the caller would have to split its block,
 * which is the very thing the stream exists to avoid.  The clip counters are
 * host-visible, one per block in flight, so a count can be read back without
 * waiting for blocks that have not run. */
static int create_resident_stream(lsx_rate_vulkan_t *context)
{
  resident_kernel_blob_t const *kernel = &stream_append_kernels[resident_kernel(context)];
  VkDescriptorSetLayoutBinding bindings[RATE_STREAM_APPEND_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(stream_append_parameters_t)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_STREAM_APPEND_BINDINGS};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  VkDescriptorSetAllocateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  VkDescriptorSetLayout layouts[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  VkDeviceSize size;
  uint32_t index;

  context->resident_stream_capacity = lsx_fir_vulkan_block_frames_for(context->vulkan) + context->input_frames;
  size = (VkDeviceSize)context->resident_stream_capacity * context->channels * resident_sample_size(context);
  if (lsx_vulkan_buffer_create(context->vulkan, &context->resident_stream[0], size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS || lsx_vulkan_buffer_create(context->vulkan, &context->resident_stream[1], size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->stream_append_clips, LSX_VULKAN_RESIDENT_BATCH_DEPTH * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  memset(bindings, 0, sizeof(bindings));
  for (index = 0; index < RATE_STREAM_APPEND_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_STREAM_APPEND_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(context->vulkan->device, &descriptor_info, NULL, &context->stream_append_descriptor_layout), "vkCreateDescriptorSetLayout rate stream append") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->stream_append_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(context->vulkan->device, &layout_info, NULL, &context->stream_append_pipeline_layout), "vkCreatePipelineLayout rate stream append") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(context->vulkan, kernel->spirv, kernel->size, context->stream_append_pipeline_layout, &context->stream_append_pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_size.descriptorCount *= LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.maxSets = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(context->vulkan->device, &pool_info, NULL, &context->stream_append_descriptor_pool), "vkCreateDescriptorPool rate stream append") != SOX_SUCCESS)
    return SOX_EOF;
  set_info.descriptorPool = context->stream_append_descriptor_pool;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    layouts[index] = context->stream_append_descriptor_layout;
  set_info.descriptorSetCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  set_info.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(context->vulkan->device, &set_info, context->stream_append_descriptor_sets), "vkAllocateDescriptorSets rate stream append") != SOX_SUCCESS)
    return SOX_EOF;
  allocation.commandPool = context->vulkan->command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH * 2u;
  return vk_result(vkAllocateCommandBuffers(context->vulkan->device, &allocation, context->resident_stream_commands), "vkAllocateCommandBuffers rate stream");
}

/* Append a resident block to the stream, either as a straight buffer copy or
 * through the append shader.
 *
 * The copy is taken only when nothing has to be changed on the way: no
 * quantisation, and an input already laid out exactly as the stream expects.
 * Otherwise the shader runs, gathering by the input's own strides and, if
 * asked, rounding each sample to SoX's integer grid and counting what it had
 * to clamp.  Everything about the input is checked first, since it comes from
 * another effect and the stream is read as raw samples afterwards. */
static int append_resident_stream(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, sox_bool quantize_sox_sample)
{
  VkCommandBuffer command;
  VkDescriptorSet descriptor_set;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkBufferMemoryBarrier source_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
  VkBufferMemoryBarrier stream_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
  VkBufferMemoryBarrier clip_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
  VkBufferMemoryBarrier barriers[3];
  VkDescriptorBufferInfo infos[RATE_STREAM_APPEND_BINDINGS];
  VkWriteDescriptorSet writes[RATE_STREAM_APPEND_BINDINGS];
  stream_append_parameters_t parameters;
  VkBufferCopy copy;
  VkDeviceSize frame_size;
  sox_bool contiguous;
  uint32_t index;

  if (!context || !input)
    return SOX_EOF;
  if (lsx_vulkan_resident_buffer_validate(input) != SOX_SUCCESS ||
      input->format != resident_format(context) ||
      input->domain != lsx_vulkan_resident_domain_sox_sample ||
      input->frames_per_element != 1u ||
      input->channels != context->channels ||
      input->state == lsx_vulkan_resident_empty ||
      !input->valid_elements ||
      input->offset % resident_sample_size(context)) {
    lsx_fail(
        "invalid resident Vulkan rate stream append: "
        "frames=%lu format=%d/%d domain=%d channels=%u/%u state=%d "
        "offset=%llu",
        (unsigned long)input->valid_elements,
        (int)input->format, (int)resident_format(context),
        (int)input->domain, input->channels, context->channels,
        (int)input->state, (unsigned long long)input->offset);
    return SOX_EOF;
  }
  if (!context->resident_stream[0].buffer && create_resident_stream(context) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->resident_stream_occupancy + input->valid_elements > context->resident_stream_capacity) {
    lsx_fail(
        "resident Vulkan rate stream is full: "
        "%lu occupied + %lu input > %lu capacity frames",
        (unsigned long)context->resident_stream_occupancy,
        (unsigned long)input->valid_elements,
        (unsigned long)context->resident_stream_capacity);
    return SOX_EOF;
  }
  command = context->resident_stream_commands[context->resident_stream_command_index];
  descriptor_set = context->stream_append_descriptor_sets[context->resident_stream_descriptor_index];
  frame_size = (VkDeviceSize)context->channels * resident_sample_size(context);
  copy.srcOffset = input->offset;
  copy.dstOffset = (VkDeviceSize)context->resident_stream_occupancy * frame_size;
  copy.size = (VkDeviceSize)input->valid_elements * frame_size;
  contiguous = !quantize_sox_sample && input->layout == lsx_vulkan_resident_layout_interleaved && input->frame_stride_elements == context->channels && input->channel_stride_elements == 1u;
  source_barrier.srcAccessMask = input->producer_access;
  source_barrier.buffer = input->buffer->buffer;
  source_barrier.offset = input->offset;
  source_barrier.size = lsx_vulkan_resident_buffer_size(input);
  stream_barrier.buffer = context->resident_stream[context->resident_stream_index].buffer;
  stream_barrier.offset = copy.dstOffset;
  stream_barrier.size = copy.size;
  if (vk_result(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer rate stream append") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer rate stream append") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command, "Rate resident stream append");
  if (contiguous) {
    vkCmdPipelineBarrier(command, input->producer_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &source_barrier, 0, NULL);
    vkCmdCopyBuffer(command, input->buffer->buffer, context->resident_stream[context->resident_stream_index].buffer, 1, &copy);
  } else {
    source_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    infos[0].buffer = input->buffer->buffer;
    infos[0].offset = 0;
    infos[0].range = input->buffer->size;
    infos[1].buffer = context->resident_stream[context->resident_stream_index].buffer;
    infos[1].offset = 0;
    infos[1].range = context->resident_stream[context->resident_stream_index].size;
    infos[2].buffer = context->stream_append_clips.buffer;
    infos[2].offset = 0;
    infos[2].range = context->stream_append_clips.size;
    memset(writes, 0, sizeof(writes));
    for (index = 0; index < RATE_STREAM_APPEND_BINDINGS; ++index) {
      writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[index].dstSet = descriptor_set;
      writes[index].dstBinding = index;
      writes[index].descriptorCount = 1;
      writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[index].pBufferInfo = &infos[index];
    }
    vkUpdateDescriptorSets(context->vulkan->device, RATE_STREAM_APPEND_BINDINGS, writes, 0, NULL);
    memset(&parameters, 0, sizeof(parameters));
    parameters.input_base_element = (uint32_t)(input->offset / resident_sample_size(context));
    parameters.output_first_frame = (uint32_t)context->resident_stream_occupancy;
    parameters.frames = (uint32_t)input->valid_elements;
    parameters.input_frame_stride = (uint32_t)input->frame_stride_elements;
    parameters.input_channel_stride = (uint32_t)input->channel_stride_elements;
    parameters.channels = context->channels;
    parameters.quantize_sox_sample = quantize_sox_sample ? 1u : 0u;
    parameters.clip_index = context->resident_stream_descriptor_index;
    barriers[0] = source_barrier;
    barriers[1] = stream_barrier;
    if (quantize_sox_sample) {
      vkCmdFillBuffer(command, context->stream_append_clips.buffer, (VkDeviceSize)parameters.clip_index * sizeof(uint32_t), sizeof(uint32_t), 0);
      clip_barrier.buffer = context->stream_append_clips.buffer;
      clip_barrier.offset = (VkDeviceSize)parameters.clip_index * sizeof(uint32_t);
      clip_barrier.size = sizeof(uint32_t);
      barriers[2] = clip_barrier;
      context->resident_stream_clip_pending_mask |= 1u << context->resident_stream_descriptor_index;
    }
    vkCmdPipelineBarrier(command, input->producer_stage | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, quantize_sox_sample ? 3u : 2u, barriers, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, context->stream_append_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context->stream_append_pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdPushConstants(command, context->stream_append_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDispatch(command, (parameters.frames + RATE_SELECT_LOCAL_SIZE - 1u) / RATE_SELECT_LOCAL_SIZE, context->channels, 1);
  }
  lsx_vulkan_label_end(context->vulkan, command);
  if (vk_result(vkEndCommandBuffer(command), "vkEndCommandBuffer rate stream append") != SOX_SUCCESS || lsx_vulkan_enqueue(context->vulkan, command) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_stream_command_index = (context->resident_stream_command_index + 1u) % (lsx_vulkan_resident_batch_depth(context->vulkan) * 2u);
  context->resident_stream_descriptor_index = (context->resident_stream_descriptor_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  context->resident_stream_occupancy += input->valid_elements;
  return SOX_SUCCESS;
}

size_t lsx_rate_vulkan_resident_stream_room(lsx_rate_vulkan_t const *context)
{
  if (!context)
    return 0;
  /*
   * The stream is allocated by the first append, so before that the room
   * is the capacity it will be given.  Reporting zero here would stop the
   * append that creates it.
   */
  if (!context->resident_stream[0].buffer)
    return lsx_fir_vulkan_block_frames_for(context->vulkan) + context->input_frames;
  if (context->resident_stream_occupancy > context->resident_stream_capacity)
    return 0;
  return context->resident_stream_capacity - context->resident_stream_occupancy;
}

int lsx_rate_vulkan_append_resident_stream(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input)
{
  return append_resident_stream(context, input, sox_false);
}

int lsx_rate_vulkan_append_resident_stream_quantized(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input)
{
  return append_resident_stream(context, input, sox_true);
}

/* Consume one block from the stream: describe its head as a resident input,
 * run it through prepare, the FIR and select, then retain whatever is left.
 *
 * Not yet holding a block is success with *produced false, since that is
 * simply the caller's cue to append more rather than an error.
 *
 * The leftover is copied to the other buffer rather than moved down within
 * this one, and the two are then swapped.  The block just consumed is still
 * being read by work that has only been queued, so a copy inside one buffer
 * would overwrite it while the device is using it; alternating buffers makes
 * the copy read one and write the other.  A stream that empties exactly needs
 * no copy at all, and so does not swap. */
int lsx_rate_vulkan_process_resident_stream(lsx_rate_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  lsx_vulkan_resident_buffer_t input;
  VkCommandBuffer command;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  VkBufferCopy retain;
  size_t remaining;
  VkDeviceSize frame_size;

  if (!context || !resident || !produced || rate <= 0)
    return SOX_EOF;
  *produced = sox_false;
  if (context->resident_stream_occupancy < context->input_frames)
    return SOX_SUCCESS;
  memset(&input, 0, sizeof(input));
  input.buffer = &context->resident_stream[context->resident_stream_index];
  input.owner = context;
  input.producer_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  input.producer_access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  input.capacity_elements = context->input_frames;
  input.valid_elements = context->input_frames;
  input.frame_stride_elements = context->channels;
  input.channel_stride_elements = 1u;
  /* rate is this stage's output rate, so the input's is that scaled back by
   * the ratio; the FIR runs at the interpolated rate, up_factor times the
   * input's, which is what it is told below. */
  input.rate = rate * context->down_factor / context->up_factor;
  input.channels = context->channels;
  input.frames_per_element = 1u;
  input.format = resident_format(context);
  input.domain = lsx_vulkan_resident_domain_sox_sample;
  input.layout = lsx_vulkan_resident_layout_interleaved;
  input.state = state;
  if (record_resident_prepare(context, &input) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_fir_vulkan_process_prepared_resident(context->fir, rate * context->down_factor, 0, state, resident) != SOX_SUCCESS)
    return SOX_EOF;
  {
    lsx_vulkan_resident_buffer_t filtered = *resident;

    if (finish_resident_process(
        context, &filtered, rate, frame_offset, state,
        normalize, sox_true, resident) != SOX_SUCCESS)
      return SOX_EOF;
  }
  remaining = context->resident_stream_occupancy - context->input_frames;
  if (remaining) {
    command = context->resident_stream_commands[context->resident_stream_command_index];
    frame_size = (VkDeviceSize)context->channels * resident_sample_size(context);
    retain.srcOffset = (VkDeviceSize)context->input_frames * frame_size;
    retain.dstOffset = 0;
    retain.size = (VkDeviceSize)remaining * frame_size;
    if (vk_result(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer rate stream retain") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer rate stream retain") != SOX_SUCCESS)
      return SOX_EOF;
    lsx_vulkan_label_begin(context->vulkan, command, "Rate resident stream retain");
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdCopyBuffer(command, context->resident_stream[context->resident_stream_index].buffer, context->resident_stream[context->resident_stream_index ^ 1u].buffer, 1, &retain);
    lsx_vulkan_label_end(context->vulkan, command);
    if (vk_result(vkEndCommandBuffer(command), "vkEndCommandBuffer rate stream retain") != SOX_SUCCESS || lsx_vulkan_enqueue(context->vulkan, command) != SOX_SUCCESS)
      return SOX_EOF;
    context->resident_stream_command_index = (context->resident_stream_command_index + 1u) % (lsx_vulkan_resident_batch_depth(context->vulkan) * 2u);
    context->resident_stream_index ^= 1u;
  }
  context->resident_stream_occupancy = remaining;
  *produced = resident->valid_elements ? sox_true : sox_false;
  return SOX_SUCCESS;
}

int lsx_rate_vulkan_flush_resident(lsx_rate_vulkan_t *context)
{
  return context ? lsx_fir_vulkan_flush_resident(context->fir) : SOX_EOF;
}

sox_bool lsx_rate_vulkan_resident_stream_ready(lsx_rate_vulkan_t const *context)
{
  return context && context->resident_stream_occupancy >= context->input_frames ? sox_true : sox_false;
}

/* Sum and clear the clip counters of the blocks that have written one.  The
 * pending mask is what says which of the per-block slots hold a count this
 * time round, so a stale value from an earlier rotation is not added again. */
static uint32_t take_resident_stream_clips(lsx_rate_vulkan_t *context)
{
  uint32_t *clips;
  uint32_t total = 0;
  uint32_t index;

  if (!context || !context->stream_append_clips.mapped)
    return 0;
  clips = context->stream_append_clips.mapped;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    if (context->resident_stream_clip_pending_mask & (1u << index)) {
      total += clips[index];
      clips[index] = 0;
    }
  context->resident_stream_clip_pending_mask = 0;
  return total;
}

/* Report clips only when nothing is still queued: the counters are written by
 * the device, so with work outstanding they do not yet hold the final values
 * and reading them would both undercount and clear what was never counted.
 * The caller collects the rest through the _completed form after a flush. */
uint32_t lsx_rate_vulkan_resident_stream_clips(lsx_rate_vulkan_t *context)
{
  return context && !context->vulkan->pending_command_buffer_count ? take_resident_stream_clips(context) : 0;
}

uint32_t lsx_rate_vulkan_resident_stream_clips_completed(lsx_rate_vulkan_t *context)
{
  return take_resident_stream_clips(context);
}

uint32_t lsx_rate_vulkan_resident_batch_depth(lsx_rate_vulkan_t const *context)
{
  return context ? lsx_vulkan_resident_batch_depth(context->vulkan) : LSX_VULKAN_RESIDENT_BATCH_DEPTH;
}

/* Zero-fill the rest of the current block at end of stream, so the tail is
 * pushed through the filter rather than left in the buffer.  The occupancy is
 * then a whole block, which is what makes the next process call produce.  A
 * stream already holding a block needs nothing and says so. */
int lsx_rate_vulkan_pad_resident_stream(lsx_rate_vulkan_t *context)
{
  VkCommandBuffer command;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkDeviceSize frame_size;
  VkDeviceSize offset;
  VkDeviceSize size;

  if (!context)
    return SOX_EOF;
  if (!context->resident_stream[0].buffer && create_resident_stream(context) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->resident_stream_occupancy >= context->input_frames)
    return SOX_SUCCESS;
  command = context->resident_stream_commands[context->resident_stream_command_index];
  frame_size = (VkDeviceSize)context->channels * resident_sample_size(context);
  offset = (VkDeviceSize)context->resident_stream_occupancy * frame_size;
  size = (VkDeviceSize)(context->input_frames - context->resident_stream_occupancy) * frame_size;
  if (vk_result(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer rate stream pad") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer rate stream pad") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command, "Rate resident stream pad");
  vkCmdFillBuffer(command, context->resident_stream[context->resident_stream_index].buffer, offset, size, 0);
  lsx_vulkan_label_end(context->vulkan, command);
  if (vk_result(vkEndCommandBuffer(command), "vkEndCommandBuffer rate stream pad") != SOX_SUCCESS || lsx_vulkan_enqueue(context->vulkan, command) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_stream_command_index = (context->resident_stream_command_index + 1u) % (lsx_vulkan_resident_batch_depth(context->vulkan) * 2u);
  context->resident_stream_occupancy = context->input_frames;
  return SOX_SUCCESS;
}

int lsx_rate_vulkan_process_resident(lsx_rate_vulkan_t *context, double const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident)
{
  lsx_vulkan_resident_buffer_t filtered;

  if (!context || !input || !resident || rate <= 0)
    return SOX_EOF;
  prepare_stage_input(context, input);
  if (lsx_fir_vulkan_process_resident(context->fir, context->stage_input, rate * context->down_factor, 0, state, &filtered) != SOX_SUCCESS) {
    lsx_fail("resident Vulkan rate FIR failed");
    return SOX_EOF;
  }
  return finish_resident_process(context, &filtered, rate, frame_offset, state, normalize, sox_false, resident);
}
