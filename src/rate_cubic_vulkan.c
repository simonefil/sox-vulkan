/* FP64 Vulkan cubic stage for the SoX rate planner.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "rate_cubic_vulkan.h"

#include <stdint.h>
#include <string.h>

#include "rate_cubic_f64_spv.inc"
#include "rate_cubic_f32_spv.inc"
#include "rate_cubic_accurate_f32_spv.inc"
#include "rate_cubic_strict_f32_spv.inc"
#include "rate_cubic_reference_dd_spv.inc"

#define RATE_CUBIC_BLOCK_FRAMES 16384u
#define RATE_CUBIC_BINDINGS 2u
#define RATE_CUBIC_LOCAL_SIZE 128u

typedef struct {
  uint32_t output_frames;
  uint32_t step_integer;
  uint32_t step_fraction;
  uint32_t phase_fraction;
  uint32_t channels;
  uint32_t padding[3];
} parameters_t;

lsx_static_assert(sizeof(parameters_t) == 32, vulkan_rate_cubic_push_layout);

struct lsx_rate_cubic_vulkan {
  lsx_vulkan_context_t *vulkan;
  lsx_vulkan_buffer_t input;
  lsx_vulkan_buffer_t output;
  double *host_output;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandBuffer command_buffer;
  VkFence fence;
  parameters_t parameters;
  uint64_t step;
  uint32_t phase_fraction;
  uint32_t pre_post;
  uint32_t max_output_frames;
  sox_bool double_precision;
  sox_bool accurate_fp32;
  sox_bool strict_fp32;
  sox_bool reference_dd;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

/* Bytes one sample occupies in the input and output buffers.  The two emulated
 * profiles carry a high and a low component per sample, so they need twice the
 * room of the plain type they are built from.  reference_dd is set as
 * double_precision && profile == reference, so it has to be tested first;
 * strict_fp32 is set only when double_precision is false. */
static size_t sample_size(lsx_rate_cubic_vulkan_t const *context)
{
  if (context->reference_dd)
    return 2u * sizeof(double);
  if (context->strict_fp32)
    return 2u * sizeof(float);
  return context->double_precision ? sizeof(double) : sizeof(float);
}

static int create_buffers(lsx_rate_cubic_vulkan_t *context)
{
  VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkDeviceSize input_size = (VkDeviceSize)(RATE_CUBIC_BLOCK_FRAMES + context->pre_post) * context->parameters.channels * sample_size(context);
  VkDeviceSize output_size = (VkDeviceSize)context->max_output_frames * context->parameters.channels * sample_size(context);

  if (lsx_vulkan_buffer_create(
          context->vulkan, &context->input, input_size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) !=
          SOX_SUCCESS ||
      lsx_vulkan_buffer_create(
          context->vulkan, &context->output, output_size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) !=
          SOX_SUCCESS)
    return SOX_EOF;
  if (context->reference_dd || !context->double_precision)
    context->host_output = lsx_malloc(
        (size_t)context->max_output_frames *
        context->parameters.channels * sizeof(*context->host_output));
  return SOX_SUCCESS;
}

static int create_pipeline(lsx_rate_cubic_vulkan_t *context)
{
  VkDescriptorSetLayoutBinding bindings[RATE_CUBIC_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters_t)
  };
  VkPipelineLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_CUBIC_BINDINGS
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
  };
  VkDescriptorSetAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorBufferInfo buffer_info[RATE_CUBIC_BINDINGS];
  VkWriteDescriptorSet writes[RATE_CUBIC_BINDINGS];
  lsx_vulkan_buffer_t *buffers[RATE_CUBIC_BINDINGS] = {
    &context->input, &context->output
  };
  uint32_t const *kernel_spirv;
  size_t kernel_size;
  uint32_t index;

  memset(bindings, 0, sizeof(bindings));
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < RATE_CUBIC_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_CUBIC_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(
          vkCreateDescriptorSetLayout(
              context->vulkan->device, &descriptor_info,
              NULL, &context->descriptor_layout),
          "vkCreateDescriptorSetLayout rate cubic") !=
          SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(
          vkCreatePipelineLayout(
              context->vulkan->device, &layout_info,
              NULL, &context->pipeline_layout),
          "vkCreatePipelineLayout rate cubic") !=
          SOX_SUCCESS)
    return SOX_EOF;
  /* Pick the kernel once, so its SPIR-V blob and the size passed with it can
   * never disagree.  Only the first test is order-sensitive: reference_dd
   * implies double_precision, while strict_fp32 and accurate_fp32 are set only
   * when double_precision is false and exclude each other. */
  if (context->reference_dd) {
    kernel_spirv = rate_cubic_reference_dd_spv;
    kernel_size = sizeof(rate_cubic_reference_dd_spv);
  } else if (context->double_precision) {
    kernel_spirv = rate_cubic_f64_spv;
    kernel_size = sizeof(rate_cubic_f64_spv);
  } else if (context->strict_fp32) {
    kernel_spirv = rate_cubic_strict_f32_spv;
    kernel_size = sizeof(rate_cubic_strict_f32_spv);
  } else if (context->accurate_fp32) {
    kernel_spirv = rate_cubic_accurate_f32_spv;
    kernel_size = sizeof(rate_cubic_accurate_f32_spv);
  } else {
    kernel_spirv = rate_cubic_f32_spv;
    kernel_size = sizeof(rate_cubic_f32_spv);
  }
  if (lsx_vulkan_create_compute_pipeline(context->vulkan, kernel_spirv, kernel_size, context->pipeline_layout, &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(
          vkCreateDescriptorPool(
              context->vulkan->device, &pool_info,
              NULL, &context->descriptor_pool),
          "vkCreateDescriptorPool rate cubic") !=
          SOX_SUCCESS)
    return SOX_EOF;
  allocation.descriptorPool = context->descriptor_pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &context->descriptor_layout;
  if (vk_result(
          vkAllocateDescriptorSets(
              context->vulkan->device, &allocation,
              &context->descriptor_set),
          "vkAllocateDescriptorSets rate cubic") !=
          SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < RATE_CUBIC_BINDINGS; ++index) {
    buffer_info[index].buffer = buffers[index]->buffer;
    buffer_info[index].offset = 0;
    buffer_info[index].range = buffers[index]->size;
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = context->descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(context->vulkan->device, RATE_CUBIC_BINDINGS, writes, 0, NULL);
  return SOX_SUCCESS;
}

static int create_commands(lsx_rate_cubic_vulkan_t *context)
{
  VkCommandBufferAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
  };
  VkFenceCreateInfo fence_info = {
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  allocation.commandPool = context->vulkan->command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  if (vk_result(
          vkAllocateCommandBuffers(
              context->vulkan->device, &allocation,
              &context->command_buffer),
          "vkAllocateCommandBuffers rate cubic") != SOX_SUCCESS ||
      vk_result(
          vkCreateFence(
              context->vulkan->device, &fence_info,
              NULL, &context->fence),
          "vkCreateFence rate cubic") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

lsx_rate_cubic_vulkan_t *lsx_rate_cubic_vulkan_create(
    lsx_vulkan_context_t *vulkan, uint64_t step,
    uint32_t pre_post, uint32_t channels)
{
  lsx_rate_cubic_vulkan_t *context;
  uint64_t limit;
  uint64_t maximum;

  if (!vulkan || (!vulkan->shader_float64 &&
      vulkan->profile != sox_vulkan_profile_fast &&
      vulkan->profile != sox_vulkan_profile_accurate &&
      vulkan->profile != sox_vulkan_profile_strict) || !step ||
      pre_post < 3u || !channels)
    return NULL;
  limit = (uint64_t)RATE_CUBIC_BLOCK_FRAMES << 32;
  maximum = (limit + step - 1u) / step + 1u;
  if (maximum > UINT32_MAX)
    return NULL;
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->double_precision = vulkan->use_float64;
  context->reference_dd = context->double_precision && vulkan->profile == sox_vulkan_profile_reference;
  context->accurate_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_accurate;
  context->strict_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_strict;
  context->step = step;
  context->pre_post = pre_post;
  context->max_output_frames = (uint32_t)maximum;
  context->parameters.step_integer = (uint32_t)(step >> 32);
  context->parameters.step_fraction = (uint32_t)step;
  context->parameters.channels = channels;
  if (create_buffers(context) != SOX_SUCCESS ||
      create_pipeline(context) != SOX_SUCCESS ||
      create_commands(context) != SOX_SUCCESS)
    goto error;
  lsx_report(
      "Vulkan rate cubic: step %u+%u/2^32, %u channel%s",
      context->parameters.step_integer,
      context->parameters.step_fraction, channels,
      channels == 1u ? "" : "s");
  lsx_report(
      "Vulkan rate cubic precision: %s",
      context->reference_dd ? "FP64x2" :
      context->double_precision ? "FP64" :
      context->strict_fp32 ? "FP32x2" :
      context->accurate_fp32 ? "compensated FP32" : "FP32");
  return context;

error: lsx_rate_cubic_vulkan_destroy(context);
  return NULL;
}

void lsx_rate_cubic_vulkan_destroy(lsx_rate_cubic_vulkan_t *context)
{
  if (!context)
    return;
  vkDeviceWaitIdle(context->vulkan->device);
  if (context->fence)
    vkDestroyFence(context->vulkan->device, context->fence, NULL);
  if (context->command_buffer)
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, 1, &context->command_buffer);
  if (context->pipeline)
    vkDestroyPipeline(context->vulkan->device, context->pipeline, NULL);
  if (context->pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->pipeline_layout, NULL);
  if (context->descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->descriptor_pool, NULL);
  if (context->descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->descriptor_layout, NULL);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->output);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->input);
  free(context->host_output);
  free(context);
}

size_t lsx_rate_cubic_vulkan_block_frames(void)
{
  return RATE_CUBIC_BLOCK_FRAMES;
}

uint32_t lsx_rate_cubic_vulkan_pre_post(lsx_rate_cubic_vulkan_t const *context)
{
  return context ? context->pre_post : 0;
}

int lsx_rate_cubic_vulkan_process(
    lsx_rate_cubic_vulkan_t *context, double const *input,
    size_t input_frames, double const **output,
    size_t *output_frames, size_t *consumed_frames)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkMemoryBarrier barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT
  };
  uint64_t limit;
  uint64_t count;
  uint64_t end_position;
  size_t processable_frames;
  size_t copied_frames;
  size_t sample_count;
  size_t index;

  if (!context || !input || !output || !output_frames || !consumed_frames || input_frames <= context->pre_post)
    return SOX_EOF;
  processable_frames = min((size_t)RATE_CUBIC_BLOCK_FRAMES, input_frames - context->pre_post);
  limit = (uint64_t)processable_frames << 32;
  count = limit > context->phase_fraction ? (limit - 1u - context->phase_fraction) / context->step + 1u : 0;
  if (!count || count > context->max_output_frames)
    return SOX_EOF;
  end_position = context->phase_fraction + count * context->step;
  copied_frames = processable_frames + context->pre_post;
  sample_count = copied_frames * context->parameters.channels;
  if (context->reference_dd)
    for (index = 0; index < sample_count; ++index) {
      double *target = (double *)context->input.mapped + 2u * index;

      target[0] = input[index];
      target[1] = 0.;
    }
  else if (context->double_precision)
    memcpy(context->input.mapped, input, sample_count * sizeof(*input));
  else if (context->strict_fp32)
    for (index = 0; index < sample_count; ++index) {
      float high = (float)input[index];
      float *target = (float *)context->input.mapped + 2u * index;

      target[0] = high;
      target[1] = (float)(input[index] - (double)high);
    }
  else
    for (index = 0; index < sample_count; ++index)
      ((float *)context->input.mapped)[index] = (float)input[index];
  context->parameters.output_frames = (uint32_t)count;
  context->parameters.phase_fraction = context->phase_fraction;
  if (vk_result(
          vkResetFences(
              context->vulkan->device, 1, &context->fence),
          "vkResetFences rate cubic") != SOX_SUCCESS ||
      vk_result(
          vkResetCommandBuffer(context->command_buffer, 0),
          "vkResetCommandBuffer rate cubic") != SOX_SUCCESS ||
      vk_result(
          vkBeginCommandBuffer(
              context->command_buffer, &begin),
          "vkBeginCommandBuffer rate cubic") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "Rate cubic");
  vkCmdBindPipeline(context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline_layout, 0, 1,
      &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(
      context->command_buffer, context->pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(context->parameters), &context->parameters);
  vkCmdDispatch(
      context->command_buffer,
      ((uint32_t)count + RATE_CUBIC_LOCAL_SIZE - 1u) /
      RATE_CUBIC_LOCAL_SIZE,
      context->parameters.channels, 1);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0,
      1, &barrier, 0, NULL, 0, NULL);
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
  if (vk_result(
          vkEndCommandBuffer(context->command_buffer),
          "vkEndCommandBuffer rate cubic") != SOX_SUCCESS ||
      lsx_vulkan_submit_and_wait(
          context->vulkan, context->command_buffer,
          context->fence,
          lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
    return SOX_EOF;
  sample_count = (size_t)count * context->parameters.channels;
  if (context->reference_dd) {
    for (index = 0; index < sample_count; ++index) {
      double const *value = (double const *)context->output.mapped + 2u * index;

      context->host_output[index] = lsx_vulkan_collapse_pair(value[0], value[1]);
    }
    *output = context->host_output;
  }
  else if (context->double_precision)
    *output = context->output.mapped;
  else if (context->strict_fp32) {
    for (index = 0; index < sample_count; ++index) {
      float const *value = (float const *)context->output.mapped + 2u * index;

      context->host_output[index] = (double)value[0] + (double)value[1];
    }
    *output = context->host_output;
  }
  else {
    for (index = 0; index < sample_count; ++index)
      context->host_output[index] = (double)((float const *)context->output.mapped)[index];
    *output = context->host_output;
  }
  *output_frames = (size_t)count;
  *consumed_frames = (size_t)(end_position >> 32);
  context->phase_fraction = (uint32_t)end_position;
  return SOX_SUCCESS;
}
