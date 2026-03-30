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
#include "rate_prepare_f64_spv.inc"
#include "rate_prepare_f32_spv.inc"
#include "rate_stream_append_f64_spv.inc"
#include "rate_stream_append_f32_spv.inc"

#define RATE_SELECT_BINDINGS 2u
#define RATE_SELECT_LOCAL_SIZE 128u
#define RATE_PREPARE_BINDINGS 3u
#define RATE_STREAM_APPEND_BINDINGS 3u

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
  lsx_vulkan_context_t *vulkan;
  lsx_fir_vulkan_t *fir;
  lsx_vulkan_buffer_t resident_output;
  VkDescriptorSetLayout resident_descriptor_layout;
  VkDescriptorPool resident_descriptor_pool;
  VkDescriptorSet resident_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout resident_pipeline_layout;
  VkPipeline resident_pipeline;
  VkCommandBuffer resident_command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkFence resident_fence;
  lsx_vulkan_buffer_t resident_previous;
  VkDescriptorSetLayout prepare_descriptor_layout;
  VkDescriptorPool prepare_descriptor_pool;
  VkDescriptorSet prepare_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout prepare_pipeline_layout;
  VkPipeline prepare_pipeline;
  VkCommandBuffer prepare_command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  sox_bool prepare_initialized;
  lsx_vulkan_buffer_t resident_stream[2];
  lsx_vulkan_buffer_t stream_append_clips;
  VkCommandBuffer resident_stream_commands[LSX_VULKAN_RESIDENT_BATCH_DEPTH * 2u];
  VkDescriptorSetLayout stream_append_descriptor_layout;
  VkDescriptorPool stream_append_descriptor_pool;
  VkDescriptorSet stream_append_descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout stream_append_pipeline_layout;
  VkPipeline stream_append_pipeline;
  size_t resident_stream_capacity;
  size_t resident_stream_occupancy;
  uint32_t resident_stream_index;
  uint32_t resident_select_bank_index;
  uint32_t resident_prepare_bank_index;
  uint32_t resident_stream_command_index;
  uint32_t resident_stream_descriptor_index;
  uint32_t resident_stream_clip_pending_mask;
  double *stage_input;
  double *output;
  size_t input_frames;
  size_t output_capacity;
  size_t skip_frames;
  uint32_t up_factor;
  uint32_t down_factor;
  uint32_t channels;
  uint32_t decimation_phase;
  sox_bool double_precision;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static size_t resident_sample_size(
    lsx_rate_vulkan_t const *context)
{
  return context->double_precision ? sizeof(double) : sizeof(float);
}

static lsx_vulkan_resident_format_t resident_format(
    lsx_rate_vulkan_t const *context)
{
  return context->double_precision ?
      lsx_vulkan_resident_format_f64 :
      lsx_vulkan_resident_format_f32;
}

static int create_resident_output(lsx_rate_vulkan_t *context)
{
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
      (context->double_precision ?
      lsx_vulkan_create_compute_pipeline(
      context->vulkan, rate_select_f64_spv,
      sizeof(rate_select_f64_spv),
      context->resident_pipeline_layout,
      &context->resident_pipeline) :
      lsx_vulkan_create_compute_pipeline(
      context->vulkan, rate_select_f32_spv,
      sizeof(rate_select_f32_spv),
      context->resident_pipeline_layout,
      &context->resident_pipeline)) != SOX_SUCCESS)
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
      (VkDeviceSize)lsx_fir_vulkan_block_frames() *
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
      (context->double_precision ?
       lsx_vulkan_create_compute_pipeline(context->vulkan, rate_prepare_f64_spv, sizeof(rate_prepare_f64_spv), context->prepare_pipeline_layout, &context->prepare_pipeline) :
       lsx_vulkan_create_compute_pipeline(context->vulkan, rate_prepare_f32_spv, sizeof(rate_prepare_f32_spv), context->prepare_pipeline_layout, &context->prepare_pipeline)) != SOX_SUCCESS)
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
  context->vulkan = vulkan;
  context->double_precision = vulkan->shader_float64;
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
  context->output_capacity = output_capacity;
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
  if (context->vulkan && context->vulkan->device)
    vkDeviceWaitIdle(context->vulkan->device);
  if (context->resident_fence)
    vkDestroyFence(
        context->vulkan->device, context->resident_fence, NULL);
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
  if (context->vulkan)
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
  if (context->vulkan) {
    lsx_vulkan_buffer_destroy(context->vulkan, &context->stream_append_clips);
    lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_stream[1]);
    lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_stream[0]);
  }
  if (context->resident_pipeline)
    vkDestroyPipeline(
        context->vulkan->device, context->resident_pipeline, NULL);
  if (context->resident_pipeline_layout)
    vkDestroyPipelineLayout(
        context->vulkan->device,
        context->resident_pipeline_layout, NULL);
  if (context->resident_descriptor_pool)
    vkDestroyDescriptorPool(
        context->vulkan->device,
        context->resident_descriptor_pool, NULL);
  if (context->resident_descriptor_layout)
    vkDestroyDescriptorSetLayout(
        context->vulkan->device,
        context->resident_descriptor_layout, NULL);
  if (context->vulkan)
    lsx_vulkan_buffer_destroy(
        context->vulkan, &context->resident_output);
  lsx_fir_vulkan_destroy(context->fir);
  free(context->stage_input);
  free(context->output);
  free(context);
}

size_t lsx_rate_vulkan_input_frames(lsx_rate_vulkan_t const *context)
{
  return context ? context->input_frames : 0;
}

static void prepare_stage_input(
    lsx_rate_vulkan_t *context, double const *input)
{
  size_t block_frames = lsx_fir_vulkan_block_frames();
  size_t input_frame;
  size_t channel;

  memset(
      context->stage_input, 0,
      block_frames * context->channels *
      sizeof(*context->stage_input));
  for (input_frame = 0;
       input_frame < context->input_frames; ++input_frame)
    for (channel = 0; channel < context->channels; ++channel)
      context->stage_input[
          (input_frame * context->up_factor) *
          context->channels + channel] =
          input[input_frame * context->channels + channel] *
          context->up_factor;
}

int lsx_rate_vulkan_process(lsx_rate_vulkan_t *context, double const *input, double const **output, size_t *output_frames)
{
  size_t block_frames = lsx_fir_vulkan_block_frames();
  double const *filtered;
  size_t output_frame = 0;
  size_t frame;

  if (!context || !input || !output || !output_frames)
    return SOX_EOF;
  prepare_stage_input(context, input);
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

  if (input->offset %
      context->vulkan->properties.limits.minStorageBufferOffsetAlignment) {
    lsx_fail("resident Vulkan rate input is not storage-buffer aligned");
    return SOX_EOF;
  }
  infos[0].buffer = input->buffer->buffer;
  infos[0].offset = input->offset;
  infos[0].range = lsx_vulkan_resident_buffer_size(input);
  infos[1].buffer = context->resident_output.buffer;
  infos[1].offset = 0;
  infos[1].range = context->resident_output.size;
  if (infos[0].range >
      context->vulkan->properties.limits.maxStorageBufferRange) {
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
  vkUpdateDescriptorSets(
      context->vulkan->device, RATE_SELECT_BINDINGS,
      writes, 0, NULL);
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
  vkCmdBindPipeline(
      command_buffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      context->resident_pipeline);
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
  if (vk_result(vkEndCommandBuffer(
      command_buffer),
      "vkEndCommandBuffer rate select") != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_enqueue(context->vulkan, command_buffer) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_select_bank_index = (context->resident_select_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  return SOX_SUCCESS;
}

static int finish_resident_process(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *filtered, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident)
{
  size_t block_frames = lsx_fir_vulkan_block_frames();
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
  first = context->decimation_phase ?
      context->down_factor - context->decimation_phase : 0u;
  output_frames = available > first ?
      1u + (available - first - 1u) / context->down_factor : 0u;
  if (!output_frames || output_frames > context->output_capacity) {
    lsx_fail("resident Vulkan rate selector produced invalid frame count %lu", (unsigned long)output_frames);
    return SOX_EOF;
  }
  memset(&parameters, 0, sizeof(parameters));
  parameters.output_frames = (uint32_t)output_frames;
  parameters.first_input_frame = (uint32_t)(skipped + first);
  parameters.input_step = context->down_factor;
  parameters.input_channel_stride = (uint32_t)filtered->channel_stride_elements;
  parameters.channels = context->channels;
  parameters.normalize = normalize ? 1u : 0u;
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
  resident->producer_stage =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = context->output_capacity;
  resident->valid_elements = output_frames;
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
  size_t block_frames = lsx_fir_vulkan_block_frames();
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
  parameters.prepared_channel_stride = (uint32_t)lsx_fir_vulkan_prepared_stride();
  parameters.up_factor = context->up_factor;
  parameters.channels = context->channels;
  input_barrier.srcAccessMask = input->producer_access;
  input_barrier.buffer = input->buffer->buffer;
  input_barrier.offset = input->offset;
  input_barrier.size = infos[0].range;
  if (vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer rate prepare") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer rate prepare") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate resident input prepare");
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

static int create_resident_stream(lsx_rate_vulkan_t *context)
{
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

  context->resident_stream_capacity = lsx_fir_vulkan_block_frames() + context->input_frames;
  size = (VkDeviceSize)context->resident_stream_capacity *
      context->channels * resident_sample_size(context);
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
      (context->double_precision ?
       lsx_vulkan_create_compute_pipeline(context->vulkan, rate_stream_append_f64_spv, sizeof(rate_stream_append_f64_spv), context->stream_append_pipeline_layout, &context->stream_append_pipeline) :
       lsx_vulkan_create_compute_pipeline(context->vulkan, rate_stream_append_f32_spv, sizeof(rate_stream_append_f32_spv), context->stream_append_pipeline_layout, &context->stream_append_pipeline)) != SOX_SUCCESS)
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

  if (!context || !input ||
      lsx_vulkan_resident_buffer_validate(input) != SOX_SUCCESS ||
      input->format != resident_format(context) ||
      input->domain != lsx_vulkan_resident_domain_sox_sample ||
      input->frames_per_element != 1u ||
      input->channels != context->channels ||
      input->state == lsx_vulkan_resident_empty ||
      !input->valid_elements ||
      input->offset % resident_sample_size(context))
    return SOX_EOF;
  if (!context->resident_stream[0].buffer && create_resident_stream(context) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->resident_stream_occupancy + input->valid_elements > context->resident_stream_capacity)
    return SOX_EOF;
  command = context->resident_stream_commands[context->resident_stream_command_index];
  descriptor_set = context->stream_append_descriptor_sets[context->resident_stream_descriptor_index];
  frame_size = (VkDeviceSize)context->channels *
      resident_sample_size(context);
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
    parameters.input_base_element = (uint32_t)(
        input->offset / resident_sample_size(context));
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

int lsx_rate_vulkan_append_resident_stream(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input)
{
  return append_resident_stream(context, input, sox_false);
}

int lsx_rate_vulkan_append_resident_stream_quantized(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input)
{
  return append_resident_stream(context, input, sox_true);
}

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
  input.rate = rate * context->down_factor / context->up_factor;
  input.channels = context->channels;
  input.frames_per_element = 1u;
  input.format = resident_format(context);
  input.domain = lsx_vulkan_resident_domain_sox_sample;
  input.layout = lsx_vulkan_resident_layout_interleaved;
  input.state = state;
  if (lsx_rate_vulkan_process_resident_input(context, &input, rate, frame_offset, state, normalize, resident) != SOX_SUCCESS)
    return SOX_EOF;
  remaining = context->resident_stream_occupancy - context->input_frames;
  if (remaining) {
    command = context->resident_stream_commands[context->resident_stream_command_index];
    frame_size = (VkDeviceSize)context->channels *
        resident_sample_size(context);
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
  *produced = sox_true;
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
  frame_size = (VkDeviceSize)context->channels *
      resident_sample_size(context);
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

int lsx_rate_vulkan_process_resident_input(lsx_rate_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident)
{
  lsx_vulkan_resident_buffer_t filtered;

  if (!context || !resident || rate <= 0 || record_resident_prepare(context, input) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_fir_vulkan_process_prepared_resident(context->fir, rate * context->down_factor, 0, state, &filtered) != SOX_SUCCESS)
    return SOX_EOF;
  return finish_resident_process(context, &filtered, rate, frame_offset, state, normalize, resident);
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
  return finish_resident_process(context, &filtered, rate, frame_offset, state, normalize, resident);
}
