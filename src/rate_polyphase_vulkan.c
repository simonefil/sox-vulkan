/* FP64 Vulkan polyphase stage for the SoX rate planner.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "rate_polyphase_vulkan.h"

#include <stdint.h>
#include <string.h>

#include "rate_polyphase_f64_spv.inc"

#define RATE_POLYPHASE_BLOCK_FRAMES 16384u
#define RATE_POLYPHASE_BINDINGS 3u
#define RATE_POLYPHASE_LOCAL_SIZE 128u

typedef struct {
  uint32_t output_frames;
  uint32_t phase_count;
  uint32_t phase_step;
  uint32_t phase_start;
  uint32_t taps;
  uint32_t channels;
} parameters_t;

struct lsx_rate_polyphase_vulkan {
  lsx_vulkan_context_t *vulkan;
  lsx_vulkan_buffer_t coefficients;
  lsx_vulkan_buffer_t input;
  lsx_vulkan_buffer_t output;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandBuffer command_buffer;
  VkFence fence;
  parameters_t parameters;
  uint32_t phase_start;
  uint32_t max_output_frames;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static int create_buffers(lsx_rate_polyphase_vulkan_t *context, double const *coefficients)
{
  VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkDeviceSize coefficient_size = (VkDeviceSize)context->parameters.phase_count * context->parameters.taps * sizeof(double);
  VkDeviceSize input_size = (VkDeviceSize)(RATE_POLYPHASE_BLOCK_FRAMES + context->parameters.taps - 1u) * context->parameters.channels * sizeof(double);
  VkDeviceSize output_size = (VkDeviceSize)context->max_output_frames * context->parameters.channels * sizeof(double);

  if (lsx_vulkan_buffer_create(context->vulkan, &context->coefficients, coefficient_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) != SOX_SUCCESS || lsx_vulkan_buffer_create(context->vulkan, &context->input, input_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) != SOX_SUCCESS || lsx_vulkan_buffer_create(context->vulkan, &context->output, output_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) != SOX_SUCCESS)
    return SOX_EOF;
  memcpy(context->coefficients.mapped, coefficients, (size_t)coefficient_size);
  return SOX_SUCCESS;
}

static int create_pipeline(lsx_rate_polyphase_vulkan_t *context)
{
  VkDescriptorSetLayoutBinding bindings[RATE_POLYPHASE_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters_t)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_POLYPHASE_BINDINGS};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  VkDescriptorSetAllocateInfo allocation = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  VkDescriptorBufferInfo buffer_info[RATE_POLYPHASE_BINDINGS];
  VkWriteDescriptorSet writes[RATE_POLYPHASE_BINDINGS];
  lsx_vulkan_buffer_t *buffers[RATE_POLYPHASE_BINDINGS] = {&context->coefficients, &context->input, &context->output};
  uint32_t index;

  memset(bindings, 0, sizeof(bindings));
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < RATE_POLYPHASE_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_POLYPHASE_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(context->vulkan->device, &descriptor_info, NULL, &context->descriptor_layout), "vkCreateDescriptorSetLayout rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(context->vulkan->device, &layout_info, NULL, &context->pipeline_layout), "vkCreatePipelineLayout rate polyphase") != SOX_SUCCESS || lsx_vulkan_create_compute_pipeline(context->vulkan, rate_polyphase_f64_spv, sizeof(rate_polyphase_f64_spv), context->pipeline_layout, &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(context->vulkan->device, &pool_info, NULL, &context->descriptor_pool), "vkCreateDescriptorPool rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  allocation.descriptorPool = context->descriptor_pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &context->descriptor_layout;
  if (vk_result(vkAllocateDescriptorSets(context->vulkan->device, &allocation, &context->descriptor_set), "vkAllocateDescriptorSets rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < RATE_POLYPHASE_BINDINGS; ++index) {
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
  vkUpdateDescriptorSets(context->vulkan->device, RATE_POLYPHASE_BINDINGS, writes, 0, NULL);
  return SOX_SUCCESS;
}

static int create_commands(lsx_rate_polyphase_vulkan_t *context)
{
  VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

  allocation.commandPool = context->vulkan->command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  if (vk_result(vkAllocateCommandBuffers(context->vulkan->device, &allocation, &context->command_buffer), "vkAllocateCommandBuffers rate polyphase") != SOX_SUCCESS || vk_result(vkCreateFence(context->vulkan->device, &fence_info, NULL, &context->fence), "vkCreateFence rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels)
{
  lsx_rate_polyphase_vulkan_t *context;
  uint64_t max_output_frames;

  if (!vulkan || !vulkan->shader_float64 || !coefficients || !taps || !phase_count || !phase_step || phase_start >= phase_count || !channels)
    return NULL;
  max_output_frames = ((uint64_t)RATE_POLYPHASE_BLOCK_FRAMES * phase_count + phase_step - 1u) / phase_step + 1u;
  if (max_output_frames > UINT32_MAX)
    return NULL;
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->parameters.phase_count = phase_count;
  context->parameters.phase_step = phase_step;
  context->parameters.taps = taps;
  context->parameters.channels = channels;
  context->phase_start = phase_start;
  context->max_output_frames = (uint32_t)max_output_frames;
  if (create_buffers(context, coefficients) != SOX_SUCCESS || create_pipeline(context) != SOX_SUCCESS || create_commands(context) != SOX_SUCCESS)
    goto error;
  lsx_report("Vulkan rate polyphase: %u/%u, %u taps/phase, %u channel%s", phase_count, phase_step, taps, channels, channels == 1u ? "" : "s");
  return context;

error:
  lsx_rate_polyphase_vulkan_destroy(context);
  return NULL;
}

void lsx_rate_polyphase_vulkan_destroy(lsx_rate_polyphase_vulkan_t *context)
{
  if (!context)
    return;
  if (context->vulkan && context->vulkan->device)
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
  lsx_vulkan_buffer_destroy(context->vulkan, &context->coefficients);
  free(context);
}

size_t lsx_rate_polyphase_vulkan_block_frames(void)
{
  return RATE_POLYPHASE_BLOCK_FRAMES;
}

uint32_t lsx_rate_polyphase_vulkan_taps(lsx_rate_polyphase_vulkan_t const *context)
{
  return context ? context->parameters.taps : 0;
}

int lsx_rate_polyphase_vulkan_process(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, double const **output, size_t *output_frames, size_t *consumed_frames)
{
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  uint64_t limit;
  uint64_t count;
  uint64_t end_position;
  size_t input_frames;

  if (!context || !input || !output || !output_frames || !consumed_frames || !processable_frames || processable_frames > RATE_POLYPHASE_BLOCK_FRAMES)
    return SOX_EOF;
  limit = (uint64_t)processable_frames * context->parameters.phase_count;
  count = limit > context->phase_start ? (limit - context->phase_start + context->parameters.phase_step - 1u) / context->parameters.phase_step : 0;
  if (!count || count > context->max_output_frames)
    return SOX_EOF;
  end_position = context->phase_start + count * context->parameters.phase_step;
  input_frames = processable_frames + context->parameters.taps - 1u;
  memcpy(context->input.mapped, input, input_frames * context->parameters.channels * sizeof(double));
  context->parameters.output_frames = (uint32_t)count;
  context->parameters.phase_start = context->phase_start;
  if (vk_result(vkResetFences(context->vulkan->device, 1, &context->fence), "vkResetFences rate polyphase") != SOX_SUCCESS || vk_result(vkResetCommandBuffer(context->command_buffer, 0), "vkResetCommandBuffer rate polyphase") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(context->command_buffer, &begin), "vkBeginCommandBuffer rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdBindPipeline(context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline_layout, 0, 1, &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(context->command_buffer, context->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(context->parameters), &context->parameters);
  vkCmdDispatch(context->command_buffer, ((uint32_t)count + RATE_POLYPHASE_LOCAL_SIZE - 1u) / RATE_POLYPHASE_LOCAL_SIZE, context->parameters.channels, 1);
  vkCmdPipelineBarrier(context->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
  if (vk_result(vkEndCommandBuffer(context->command_buffer), "vkEndCommandBuffer rate polyphase") != SOX_SUCCESS || lsx_vulkan_submit_and_wait(context->vulkan, context->command_buffer, context->fence) != SOX_SUCCESS)
    return SOX_EOF;
  *output = context->output.mapped;
  *output_frames = (size_t)count;
  *consumed_frames = (size_t)(end_position / context->parameters.phase_count);
  context->phase_start = (uint32_t)(end_position % context->parameters.phase_count);
  return SOX_SUCCESS;
}
