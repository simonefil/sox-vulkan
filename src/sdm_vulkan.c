/* Vulkan FIR and DSD modulation backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "sdm_vulkan.h"

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * The Vulkan encoder is deliberately split into two GPU stages:
 *
 *  1. A rational polyphase FIR converts PCM at input_rate to the selected
 *     fixed DSD rate without materialising an intermediate SoX stream.
 *  2. A conservative MASH-2 produces values in [-1, 2].  Two modular prefix
 *     scans expose its otherwise serial accumulators.  The final one-bit
 *     reducer is a 69-state finite-state machine: every 256-sample block
 *     first builds a transition for every possible state, the transitions
 *     are prefix-composed to find independent block seeds, and all blocks
 *     are then replayed concurrently while packing 32 output bits per word.
 *
 * Buffer indices and push constants must remain identical to the bindings
 * declared in sdm_polyphase.comp and sdm_mash2_fsm.comp.
 */
#define SDM_VULKAN_LOCAL_SIZE 256u
#define SDM_VULKAN_FIR_LOCAL_SIZE 128u
#define SDM_VULKAN_BLOCK_SAMPLES 256u
#define SDM_VULKAN_INPUT_FRAMES 16384u
#define SDM_VULKAN_TAPS_PER_PHASE 257u
#define SDM_VULKAN_INPUT_PADDING 128u
#define SDM_VULKAN_STATE_COUNT 69u
#define SDM_VULKAN_LOOKUP_COUNT 297u
#define SDM_VULKAN_MASH_BINDINGS 11u
#define SDM_VULKAN_FIR_BINDINGS 3u
#define SDM_VULKAN_INPUT_GAIN 0.7079457844f
#define SDM_VULKAN_PASSBAND_HZ 20000.0
#define SDM_VULKAN_KAISER_BETA 17.5

#define MODE_INPUT 0u
#define MODE_SCAN 1u
#define MODE_ADD 2u
#define MODE_STAGE1 3u
#define MODE_STAGE2_INPUT 4u
#define MODE_STAGE2 5u
#define MODE_TABLES 6u
#define MODE_COMPOSE 7u
#define MODE_SEEDS 8u
#define MODE_REPLAY 9u
#define MODE_ADD_INITIAL_PHASE 10u
#define MODE_UPDATE_MASH_STATE 11u

#include "sdm_polyphase_spv.inc"
#include "sdm_mash2_fsm_spv.inc"

enum {
  BUFFER_FIR_OUTPUT,
  BUFFER_SCAN,
  BUFFER_STAGE1,
  BUFFER_MASH,
  BUFFER_STATES,
  BUFFER_STATE_LOOKUP,
  BUFFER_TABLES_A,
  BUFFER_TABLES_B,
  BUFFER_BLOCK_SEEDS,
  BUFFER_OUTPUT_WORDS,
  BUFFER_PERSISTENT_STATE,
  BUFFER_FIR_COEFFICIENTS,
  BUFFER_PCM_INPUT,
  BUFFER_COUNT
};

typedef struct {
  int32_t first;
  int32_t second;
} state_t;

typedef struct {
  uint32_t mode;
  uint32_t sample_count;
  uint32_t block_count;
  uint32_t state_count;
  uint32_t source_offset;
  uint32_t target_offset;
  uint32_t parent_offset;
  uint32_t element_count;
  uint32_t compose_offset;
  uint32_t table_source;
  uint32_t block_samples;
  uint32_t initial_state;
  float input_gain;
  uint32_t stage_index;
  uint32_t scan_storage_count;
  uint32_t padding;
} mash_parameters_t;

typedef struct {
  uint32_t output_frames;
  uint32_t padded_input_frames;
  uint32_t phase_count;
  uint32_t taps_per_phase;
  uint32_t input_padding;
  uint32_t channel_count;
  uint32_t phase_step;
  uint32_t phase_start;
} fir_parameters_t;

lsx_static_assert(sizeof(state_t) == 8, vulkan_state_layout);
lsx_static_assert(sizeof(mash_parameters_t) == 64, vulkan_mash_push_layout);
lsx_static_assert(sizeof(fir_parameters_t) == 32, vulkan_fir_push_layout);

typedef lsx_vulkan_buffer_t buffer_t;

struct lsx_sdm_vulkan {
  lsx_vulkan_context_t *vulkan;
  VkDescriptorSetLayout mash_descriptor_layout;
  VkDescriptorSetLayout fir_descriptor_layout;
  VkPipelineLayout mash_pipeline_layout;
  VkPipelineLayout fir_pipeline_layout;
  VkPipeline mash_pipeline;
  VkPipeline fir_pipeline;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet mash_descriptor_set;
  VkDescriptorSet fir_descriptor_set;
  VkCommandBuffer command_buffer;
  VkFence fence;
  VkQueryPool query_pool;
  buffer_t buffers[BUFFER_COUNT];
  buffer_t upload;
  buffer_t download;
  uint32_t dsd_factor;
  uint32_t up_factor;
  uint32_t down_factor;
  uint32_t channels;
  uint32_t input_frames;
  uint32_t output_frames;
  uint32_t padded_input_frames;
  uint32_t block_count;
  uint32_t scan_count;
  uint32_t offsets[8];
  uint32_t counts[8];
  uint32_t level_count;
  fir_parameters_t fir_parameters;
  mash_parameters_t mash_parameters;
  float *history;
  double process_seconds;
  double fir_gpu_seconds;
  double mash_gpu_seconds;
  uint64_t process_calls;
};

static double monotonic_seconds(void)
{
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter))
    return 0.0;
  return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static uint32_t divide_up(uint32_t value, uint32_t divisor)
{
  return value / divisor + (value % divisor != 0);
}

static uint32_t greatest_common_divisor(uint32_t left, uint32_t right)
{
  while (right) {
    uint32_t remainder = left % right;
    left = right;
    right = remainder;
  }
  return left;
}

static int create_buffer(
    lsx_sdm_vulkan_t *context, buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties)
{
  return lsx_vulkan_buffer_create(
      context->vulkan, buffer, size, usage, properties);
}

static void destroy_buffer(lsx_sdm_vulkan_t *context, buffer_t *buffer)
{
  lsx_vulkan_buffer_destroy(context->vulkan, buffer);
}

static uint32_t scan_storage_count(
    uint32_t samples, uint32_t *offsets,
    uint32_t *counts, uint32_t *level_count)
{
  uint64_t total = 0;
  uint32_t count = samples;
  uint32_t level = 0;

  /*
   * Each scan level stores its own values followed by one aggregate per
   * workgroup.  The last aggregate is kept as a sentinel so target_offset is
   * always inside the allocation, including the single-workgroup top level.
   */
  for (;;) {
    if (level >= 8u)
      return 0;
    if (total + count >= UINT32_MAX)
      return 0;
    offsets[level] = (uint32_t)total;
    counts[level] = count;
    total += count;
    ++level;
    if (count <= SDM_VULKAN_LOCAL_SIZE)
      break;
    count = divide_up(count, SDM_VULKAN_LOCAL_SIZE);
  }
  *level_count = level;
  return (uint32_t)total + 1u;
}

static uint32_t discover_states(
    state_t *states, int32_t *lookup, uint32_t *initial_state)
{
  uint8_t present[SDM_VULKAN_LOOKUP_COUNT];
  uint32_t count = 1;
  uint32_t previous_count = 0;
  uint32_t index;

  memset(present, 0, sizeof(present));
  states[0].first = 0;
  states[0].second = 0;
  present[(0 + 13) * 11 + 0 + 5] = 1;
  /*
   * Enumerate the closure of the reducer for every possible MASH-2 value.
   * The result is intentionally checked against the known 69-state topology:
   * silently changing this set would invalidate every transition table.
   */
  while (previous_count != count) {
    previous_count = count;
    for (index = 0; index < previous_count; ++index) {
      int32_t value;
      for (value = -1; value <= 2; ++value) {
        int32_t quantizer_input = value + states[index].first;
        int32_t output = quantizer_input > 0 ? 1 : 0;
        int32_t correction = output - quantizer_input;
        state_t next;
        int32_t slot;

        if (correction < -5)
          correction = -5;
        if (correction > 5)
          correction = 5;
        next.first = states[index].second - 2 * correction;
        next.second = correction;
        slot = (next.first + 13) * 11 + next.second + 5;
        if (slot < 0 ||
            slot >= (int32_t)SDM_VULKAN_LOOKUP_COUNT)
          return 0;
        if (!present[slot]) {
          if (count >= SDM_VULKAN_STATE_COUNT)
            return 0;
          present[slot] = 1;
          states[count++] = next;
        }
      }
    }
  }
  if (count != SDM_VULKAN_STATE_COUNT)
    return 0;
  for (index = 0; index < SDM_VULKAN_LOOKUP_COUNT; ++index)
    lookup[index] = -1;
  for (index = 0; index < count; ++index) {
    uint32_t slot = (uint32_t)(
        (states[index].first + 13) * 11 +
        states[index].second + 5);
    lookup[slot] = (int32_t)index;
    if (!states[index].first && !states[index].second)
      *initial_state = index;
  }
  return count;
}

static double bessel_i0(double value)
{
  double sum = 1.0;
  double term = 1.0;
  double quarter = value * value * 0.25;
  uint32_t index;

  for (index = 1; index < 80u; ++index) {
    term *= quarter / ((double)index * index);
    sum += term;
    if (term < sum * 1e-18)
      break;
  }
  return sum;
}

static double sinc_value(double value)
{
  double argument;

  if (fabs(value) < 1e-15)
    return 1.0;
  argument = 3.14159265358979323846 * value;
  return sin(argument) / argument;
}

static void design_filter(
    float *polyphase, uint32_t up_factor,
    uint32_t input_rate, uint32_t output_rate)
{
  uint32_t taps =
      up_factor * (SDM_VULKAN_TAPS_PER_PHASE - 1u) + 1u;
  double intermediate_rate = (double)input_rate * up_factor;
  double stopband = 0.5 * min(input_rate, output_rate);
  double cutoff =
      0.5 * (SDM_VULKAN_PASSBAND_HZ + stopband);
  double normalized = cutoff / intermediate_rate;
  double denominator = bessel_i0(SDM_VULKAN_KAISER_BETA);
  double *linear = lsx_calloc(taps, sizeof(*linear));
  double sum = 0.0;
  uint32_t index;
  uint32_t phase;
  uint32_t slot;

  /*
   * Design in FP64 at the L-times intermediate rate, normalize DC gain, then
   * transpose to tap-major polyphase storage.  Tap-major storage lets one
   * shader invocation fetch four adjacent phases contiguously.
   */
  memset(polyphase, 0,
      (size_t)up_factor * SDM_VULKAN_TAPS_PER_PHASE *
      sizeof(*polyphase));
  for (index = 0; index < taps; ++index) {
    double offset = (double)index - (double)(taps / 2u);
    double position = offset / (double)(taps / 2u);
    double window = bessel_i0(
        SDM_VULKAN_KAISER_BETA *
        sqrt(max(0.0, 1.0 - position * position))) /
        denominator;
    linear[index] = 2.0 * normalized *
        sinc_value(2.0 * normalized * offset) * window;
    sum += linear[index];
  }
  for (index = 0; index < taps; ++index)
    linear[index] /= sum;
  for (phase = 0; phase < up_factor; ++phase)
    for (slot = 0; slot < SDM_VULKAN_TAPS_PER_PHASE; ++slot) {
      int32_t k =
          (int32_t)slot - (int32_t)SDM_VULKAN_INPUT_PADDING;
      int64_t linear_index = (int64_t)(taps / 2u) +
          phase + (int64_t)k * up_factor;
      if (linear_index >= 0 &&
          linear_index < (int64_t)taps)
        polyphase[(size_t)slot * up_factor + phase] =
            (float)(linear[linear_index] * up_factor);
    }
  free(linear);
}

static int create_pipeline(
    lsx_sdm_vulkan_t *context, uint32_t const *spirv,
    size_t spirv_size, VkPipelineLayout layout,
    VkPipeline *pipeline)
{
  return lsx_vulkan_create_compute_pipeline(
      context->vulkan, spirv, spirv_size, layout, pipeline);
}

static int create_descriptors_and_pipelines(
    lsx_sdm_vulkan_t *context)
{
  VkDescriptorSetLayoutBinding
      mash_bindings[SDM_VULKAN_MASH_BINDINGS];
  VkDescriptorSetLayoutBinding
      fir_bindings[SDM_VULKAN_FIR_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkPushConstantRange mash_push = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mash_parameters_t)
  };
  VkPushConstantRange fir_push = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fir_parameters_t)
  };
  VkPipelineLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    SDM_VULKAN_MASH_BINDINGS + SDM_VULKAN_FIR_BINDINGS
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
  };
  VkDescriptorSetLayout layouts[2];
  VkDescriptorSetAllocateInfo set_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorSet sets[2];
  VkDescriptorBufferInfo
      mash_infos[SDM_VULKAN_MASH_BINDINGS];
  VkDescriptorBufferInfo
      fir_infos[SDM_VULKAN_FIR_BINDINGS];
  VkWriteDescriptorSet mash_writes[SDM_VULKAN_MASH_BINDINGS];
  VkWriteDescriptorSet fir_writes[SDM_VULKAN_FIR_BINDINGS];
  uint32_t index;

  memset(mash_bindings, 0, sizeof(mash_bindings));
  for (index = 0; index < SDM_VULKAN_MASH_BINDINGS; ++index) {
    mash_bindings[index].binding = index;
    mash_bindings[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mash_bindings[index].descriptorCount = 1;
    mash_bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = SDM_VULKAN_MASH_BINDINGS;
  descriptor_info.pBindings = mash_bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      context->vulkan->device, &descriptor_info, NULL,
      &context->mash_descriptor_layout),
      "vkCreateDescriptorSetLayout MASH") != SOX_SUCCESS)
    return SOX_EOF;
  memset(fir_bindings, 0, sizeof(fir_bindings));
  for (index = 0; index < SDM_VULKAN_FIR_BINDINGS; ++index) {
    fir_bindings[index].binding = index;
    fir_bindings[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    fir_bindings[index].descriptorCount = 1;
    fir_bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = SDM_VULKAN_FIR_BINDINGS;
  descriptor_info.pBindings = fir_bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      context->vulkan->device, &descriptor_info, NULL,
      &context->fir_descriptor_layout),
      "vkCreateDescriptorSetLayout FIR") != SOX_SUCCESS)
    return SOX_EOF;

  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->mash_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &mash_push;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->mash_pipeline_layout),
      "vkCreatePipelineLayout MASH") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.pSetLayouts = &context->fir_descriptor_layout;
  layout_info.pPushConstantRanges = &fir_push;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->fir_pipeline_layout),
      "vkCreatePipelineLayout FIR") != SOX_SUCCESS)
    return SOX_EOF;
  if (create_pipeline(
      context, sdm_vulkan_mash_spv, sdm_vulkan_mash_spv_size,
      context->mash_pipeline_layout,
      &context->mash_pipeline) != SOX_SUCCESS ||
      create_pipeline(
      context, sdm_vulkan_fir_spv, sdm_vulkan_fir_spv_size,
      context->fir_pipeline_layout,
      &context->fir_pipeline) != SOX_SUCCESS)
    return SOX_EOF;

  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(
      context->vulkan->device, &pool_info, NULL,
      &context->descriptor_pool),
      "vkCreateDescriptorPool") != SOX_SUCCESS)
    return SOX_EOF;
  layouts[0] = context->mash_descriptor_layout;
  layouts[1] = context->fir_descriptor_layout;
  set_info.descriptorPool = context->descriptor_pool;
  set_info.descriptorSetCount = 2;
  set_info.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(
      context->vulkan->device, &set_info, sets),
      "vkAllocateDescriptorSets") != SOX_SUCCESS)
    return SOX_EOF;
  context->mash_descriptor_set = sets[0];
  context->fir_descriptor_set = sets[1];

  memset(mash_writes, 0, sizeof(mash_writes));
  for (index = 0; index < SDM_VULKAN_MASH_BINDINGS; ++index) {
    mash_infos[index].buffer = context->buffers[index].buffer;
    mash_infos[index].offset = 0;
    mash_infos[index].range = context->buffers[index].size;
    mash_writes[index].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mash_writes[index].dstSet = context->mash_descriptor_set;
    mash_writes[index].dstBinding = index;
    mash_writes[index].descriptorCount = 1;
    mash_writes[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mash_writes[index].pBufferInfo = &mash_infos[index];
  }
  fir_infos[0].buffer = context->buffers[BUFFER_FIR_COEFFICIENTS].buffer;
  fir_infos[0].offset = 0;
  fir_infos[0].range = context->buffers[BUFFER_FIR_COEFFICIENTS].size;
  fir_infos[1].buffer = context->buffers[BUFFER_PCM_INPUT].buffer;
  fir_infos[1].offset = 0;
  fir_infos[1].range = context->buffers[BUFFER_PCM_INPUT].size;
  fir_infos[2].buffer = context->buffers[BUFFER_FIR_OUTPUT].buffer;
  fir_infos[2].offset = 0;
  fir_infos[2].range = context->buffers[BUFFER_FIR_OUTPUT].size;
  memset(fir_writes, 0, sizeof(fir_writes));
  for (index = 0; index < SDM_VULKAN_FIR_BINDINGS; ++index) {
    fir_writes[index].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    fir_writes[index].dstSet = context->fir_descriptor_set;
    fir_writes[index].dstBinding = index;
    fir_writes[index].descriptorCount = 1;
    fir_writes[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    fir_writes[index].pBufferInfo = &fir_infos[index];
  }
  vkUpdateDescriptorSets(
      context->vulkan->device, SDM_VULKAN_MASH_BINDINGS,
      mash_writes, 0, NULL);
  vkUpdateDescriptorSets(
      context->vulkan->device, SDM_VULKAN_FIR_BINDINGS,
      fir_writes, 0, NULL);
  return SOX_SUCCESS;
}

static int initialize_vulkan(lsx_sdm_vulkan_t *context)
{
  VkCommandBufferAllocateInfo command_info = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
  };
  VkFenceCreateInfo fence_info = {
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };
  VkQueryPoolCreateInfo query_info = {
    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
    VK_QUERY_TYPE_TIMESTAMP, 3, 0
  };
  VkDeviceSize sizes[BUFFER_COUNT];
  uint32_t table_items =
      context->block_count * SDM_VULKAN_STATE_COUNT;
  uint32_t index;
  VkDeviceSize upload_size;

  sizes[BUFFER_FIR_OUTPUT] = (VkDeviceSize)context->channels *
      context->output_frames * sizeof(float);
  sizes[BUFFER_SCAN] = (VkDeviceSize)context->channels *
      context->scan_count * sizeof(uint32_t);
  sizes[BUFFER_STAGE1] = (VkDeviceSize)context->channels *
      context->output_frames * sizeof(uint32_t);
  sizes[BUFFER_MASH] = (VkDeviceSize)context->channels *
      context->output_frames * sizeof(int32_t);
  sizes[BUFFER_STATES] = SDM_VULKAN_STATE_COUNT * sizeof(state_t);
  sizes[BUFFER_STATE_LOOKUP] =
      SDM_VULKAN_LOOKUP_COUNT * sizeof(int32_t);
  sizes[BUFFER_TABLES_A] = (VkDeviceSize)context->channels *
      table_items * sizeof(uint32_t);
  sizes[BUFFER_TABLES_B] = sizes[BUFFER_TABLES_A];
  sizes[BUFFER_BLOCK_SEEDS] = (VkDeviceSize)context->channels *
      context->block_count * sizeof(uint32_t);
  sizes[BUFFER_OUTPUT_WORDS] = (VkDeviceSize)context->channels *
      (context->output_frames / 32u) * sizeof(uint32_t);
  sizes[BUFFER_PERSISTENT_STATE] = (VkDeviceSize)context->channels *
      4u * sizeof(uint32_t);
  sizes[BUFFER_FIR_COEFFICIENTS] = (VkDeviceSize)context->up_factor *
      SDM_VULKAN_TAPS_PER_PHASE * sizeof(float);
  sizes[BUFFER_PCM_INPUT] = (VkDeviceSize)context->channels *
      context->padded_input_frames * sizeof(float);

  for (index = 0; index < BUFFER_COUNT; ++index) {
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (sizes[index] >
        context->vulkan->properties.limits.maxStorageBufferRange) {
      lsx_fail("Vulkan DSD buffer exceeds device storage range");
      return SOX_EOF;
    }
    if (index == BUFFER_STATES || index == BUFFER_STATE_LOOKUP ||
        index == BUFFER_PERSISTENT_STATE ||
        index == BUFFER_FIR_COEFFICIENTS ||
        index == BUFFER_PCM_INPUT)
      usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (index == BUFFER_OUTPUT_WORDS)
      usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (create_buffer(
        context, &context->buffers[index], sizes[index], usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
      return SOX_EOF;
  }
  upload_size = max(sizes[BUFFER_FIR_COEFFICIENTS],
      sizes[BUFFER_PCM_INPUT]);
  upload_size = max(upload_size, sizes[BUFFER_STATES]);
  upload_size = max(upload_size, sizes[BUFFER_STATE_LOOKUP]);
  upload_size = max(upload_size, sizes[BUFFER_PERSISTENT_STATE]);
  if (create_buffer(
      context, &context->upload, upload_size,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS ||
      create_buffer(
      context, &context->download, sizes[BUFFER_OUTPUT_WORDS],
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (create_descriptors_and_pipelines(context) != SOX_SUCCESS)
    return SOX_EOF;

  command_info.commandPool = context->vulkan->command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &command_info, &context->command_buffer),
      "vkAllocateCommandBuffers") != SOX_SUCCESS ||
      vk_result(vkCreateFence(
      context->vulkan->device, &fence_info, NULL, &context->fence),
      "vkCreateFence") != SOX_SUCCESS)
    return SOX_EOF;
  if (context->vulkan->timestamp_valid_bits &&
      vk_result(vkCreateQueryPool(
      context->vulkan->device, &query_info, NULL, &context->query_pool),
      "vkCreateQueryPool") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int submit_and_wait(lsx_sdm_vulkan_t *context)
{
  return lsx_vulkan_submit_and_wait(
      context->vulkan, context->command_buffer, context->fence);
}

static int upload_buffer(
    lsx_sdm_vulkan_t *context, uint32_t target,
    void const *data, VkDeviceSize size)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkBufferCopy copy = {0, 0, size};
  VkMemoryBarrier barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
  };

  if (target >= BUFFER_COUNT ||
      size > context->upload.size ||
      size > context->buffers[target].size) {
    lsx_fail("invalid Vulkan DSD upload");
    return SOX_EOF;
  }
  memcpy(context->upload.mapped, data, (size_t)size);
  if (vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdCopyBuffer(
      context->command_buffer, context->upload.buffer,
      context->buffers[target].buffer, 1, &copy);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
      1, &barrier, 0, NULL, 0, NULL);
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  return submit_and_wait(context);
}

static void shader_barrier(VkCommandBuffer command_buffer)
{
  VkMemoryBarrier barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_SHADER_WRITE_BIT,
    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
  };

  vkCmdPipelineBarrier(
      command_buffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
      1, &barrier, 0, NULL, 0, NULL);
}

static void mash_dispatch(
    lsx_sdm_vulkan_t *context,
    mash_parameters_t const *parameters, uint32_t groups)
{
  vkCmdPushConstants(
      context->command_buffer, context->mash_pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(*parameters), parameters);
  vkCmdDispatch(
      context->command_buffer, groups, context->channels, 1);
  shader_barrier(context->command_buffer);
}

static void record_scan(
    lsx_sdm_vulkan_t *context,
    mash_parameters_t *parameters)
{
  uint32_t level;

  parameters->mode = MODE_SCAN;
  /* Up-sweep: scan each 256-element workgroup and recursively scan its sums. */
  for (level = 0; level < context->level_count; ++level) {
    parameters->source_offset = context->offsets[level];
    parameters->target_offset =
        level + 1u < context->level_count ?
        context->offsets[level + 1u] :
        context->offsets[level] + context->counts[level];
    parameters->element_count = context->counts[level];
    mash_dispatch(context, parameters,
        divide_up(context->counts[level],
                  SDM_VULKAN_LOCAL_SIZE));
  }
  parameters->mode = MODE_ADD;
  /* Down-sweep: add each preceding parent prefix to its child workgroup. */
  for (level = context->level_count - 1u; level > 0u; --level) {
    parameters->source_offset = context->offsets[level - 1u];
    parameters->parent_offset = context->offsets[level];
    parameters->element_count = context->counts[level - 1u];
    mash_dispatch(context, parameters,
        divide_up(context->counts[level - 1u],
                  SDM_VULKAN_LOCAL_SIZE));
  }
}

static void record_mash(lsx_sdm_vulkan_t *context)
{
  mash_parameters_t *parameters = &context->mash_parameters;
  uint32_t table_items =
      parameters->block_count * parameters->state_count;
  uint32_t compose_offset;
  uint32_t table_source = 0;

  vkCmdBindPipeline(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->mash_pipeline);
  vkCmdBindDescriptorSets(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->mash_pipeline_layout, 0, 1,
      &context->mash_descriptor_set, 0, NULL);

  /*
   * Both MASH accumulators are modular prefix sums.  Their carries form the
   * small integer stream consumed by the finite-state reducer.
   */
  parameters->mode = MODE_INPUT;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  record_scan(context, parameters);
  parameters->mode = MODE_ADD_INITIAL_PHASE;
  parameters->source_offset = 0;
  parameters->stage_index = 0;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_STAGE1;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_UPDATE_MASH_STATE;
  parameters->stage_index = 0;
  mash_dispatch(context, parameters, 1);
  parameters->mode = MODE_STAGE2_INPUT;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  record_scan(context, parameters);
  parameters->mode = MODE_ADD_INITIAL_PHASE;
  parameters->source_offset = 0;
  parameters->stage_index = 1;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_STAGE2;
  mash_dispatch(context, parameters,
      divide_up(parameters->sample_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_UPDATE_MASH_STATE;
  parameters->stage_index = 1;
  mash_dispatch(context, parameters, 1);

  /*
   * A table entry maps one of 69 reducer states across one 256-sample block.
   * Parallel prefix composition finds the true seed of every block; replay
   * then emits all blocks concurrently without breaking causal equivalence.
   */
  parameters->mode = MODE_TABLES;
  mash_dispatch(context, parameters,
      divide_up(table_items, SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_COMPOSE;
  for (compose_offset = 1;
       compose_offset < parameters->block_count;
       compose_offset <<= 1u) {
    parameters->compose_offset = compose_offset;
    parameters->table_source = table_source;
    mash_dispatch(context, parameters,
        divide_up(table_items, SDM_VULKAN_LOCAL_SIZE));
    table_source ^= 1u;
  }
  parameters->table_source = table_source;
  parameters->mode = MODE_SEEDS;
  mash_dispatch(context, parameters,
      divide_up(parameters->block_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_REPLAY;
  mash_dispatch(context, parameters,
      divide_up(parameters->block_count,
                SDM_VULKAN_LOCAL_SIZE));
}

static int record_and_run(lsx_sdm_vulkan_t *context)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkMemoryBarrier input_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
  };
  VkMemoryBarrier output_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT
  };
  VkMemoryBarrier download_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT
  };
  VkBufferCopy input_copy = {
    0, 0, context->buffers[BUFFER_PCM_INPUT].size
  };
  VkBufferCopy output_copy = {
    0, 0, context->download.size
  };
  uint32_t fir_groups = divide_up(
      context->output_frames / 4u,
      SDM_VULKAN_FIR_LOCAL_SIZE);
  uint64_t timestamps[3];
  double period =
      (double)context->vulkan->properties.limits.timestampPeriod * 1e-9;

  if (vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdCopyBuffer(
      context->command_buffer, context->upload.buffer,
      context->buffers[BUFFER_PCM_INPUT].buffer, 1, &input_copy);
  if (context->query_pool)
    vkCmdResetQueryPool(
        context->command_buffer, context->query_pool, 0, 3);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
      1, &input_barrier, 0, NULL, 0, NULL);
  if (context->query_pool)
    vkCmdWriteTimestamp(
        context->command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        context->query_pool, 0);
  vkCmdBindPipeline(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->fir_pipeline);
  vkCmdBindDescriptorSets(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->fir_pipeline_layout, 0, 1,
      &context->fir_descriptor_set, 0, NULL);
  vkCmdPushConstants(
      context->command_buffer, context->fir_pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(context->fir_parameters),
      &context->fir_parameters);
  vkCmdDispatch(
      context->command_buffer, fir_groups,
      context->channels, 1);
  shader_barrier(context->command_buffer);
  if (context->query_pool)
    vkCmdWriteTimestamp(
        context->command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        context->query_pool, 1);
  record_mash(context);
  if (context->query_pool)
    vkCmdWriteTimestamp(
        context->command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        context->query_pool, 2);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      1, &output_barrier, 0, NULL, 0, NULL);
  vkCmdCopyBuffer(
      context->command_buffer, context->buffers[BUFFER_OUTPUT_WORDS].buffer,
      context->download.buffer, 1, &output_copy);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0,
      1, &download_barrier, 0, NULL, 0, NULL);
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  if (submit_and_wait(context) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->query_pool) {
    if (vk_result(vkGetQueryPoolResults(
        context->vulkan->device, context->query_pool, 0, 3,
        sizeof(timestamps), timestamps, sizeof(timestamps[0]),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
        "vkGetQueryPoolResults") != SOX_SUCCESS)
      return SOX_EOF;
    context->fir_gpu_seconds +=
        (double)(timestamps[1] - timestamps[0]) * period;
    context->mash_gpu_seconds +=
        (double)(timestamps[2] - timestamps[1]) * period;
  }
  return SOX_SUCCESS;
}

lsx_sdm_vulkan_t *lsx_sdm_vulkan_create(
    lsx_vulkan_context_t *vulkan, unsigned input_rate,
    unsigned output_rate, unsigned channels)
{
  lsx_sdm_vulkan_t *context;
  state_t states[SDM_VULKAN_STATE_COUNT];
  int32_t lookup[SDM_VULKAN_LOOKUP_COUNT];
  uint32_t persistent[24] = {0};
  uint32_t initial_state = UINT32_MAX;
  uint32_t state_count;
  float *coefficients;
  uint16_t endian = 1;
  uint32_t rate_divisor;
  uint32_t input_alignment;
  uint64_t input_alignment_wide;
  uint64_t output_frames_wide;
  unsigned channel;

  if (!vulkan) {
    lsx_fail("Vulkan DSD requires a shared Vulkan context");
    return NULL;
  }
  if (*(uint8_t const *)&endian != 1) {
    lsx_fail("Vulkan DSD requires a little-endian host");
    return NULL;
  }
  if (channels < 1u || channels > 6u) {
    lsx_fail("Vulkan DSD supports one through six channels");
    return NULL;
  }
  if (input_rate < 44100u ||
      (input_rate % 44100u && input_rate % 48000u)) {
    lsx_fail(
        "Vulkan DSD input rate must be a multiple of 44100 or 48000 Hz");
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  if (!output_rate || output_rate % 44100u) {
    lsx_fail("Vulkan DSD requires a standard DSD output rate");
    free(context);
    return NULL;
  }
  context->dsd_factor = output_rate / 44100u;
  if (context->dsd_factor != 64u &&
      context->dsd_factor != 128u &&
      context->dsd_factor != 256u &&
      context->dsd_factor != 512u &&
      context->dsd_factor != 1024u) {
    lsx_fail("Vulkan DSD supports DSD64 through DSD1024");
    free(context);
    return NULL;
  }
  rate_divisor = greatest_common_divisor(input_rate, output_rate);
  context->up_factor = output_rate / rate_divisor;
  context->down_factor = input_rate / rate_divisor;
  input_alignment_wide =
      (uint64_t)context->down_factor *
      SDM_VULKAN_BLOCK_SAMPLES /
      greatest_common_divisor(
          context->up_factor, SDM_VULKAN_BLOCK_SAMPLES);
  if (input_alignment_wide >
      UINT32_MAX - 2u * SDM_VULKAN_INPUT_PADDING) {
    lsx_fail(
        "Vulkan DSD rational ratio exceeds the 32-bit buffer index range");
    free(context);
    return NULL;
  }
  input_alignment = (uint32_t)input_alignment_wide;
  context->input_frames = input_alignment <= SDM_VULKAN_INPUT_FRAMES ?
      SDM_VULKAN_INPUT_FRAMES / input_alignment * input_alignment :
      input_alignment;
  context->channels = channels;
  output_frames_wide =
      (uint64_t)(context->input_frames / context->down_factor) *
      context->up_factor;
  if (output_frames_wide > UINT32_MAX ||
      output_frames_wide / SDM_VULKAN_BLOCK_SAMPLES >
          UINT32_MAX / SDM_VULKAN_STATE_COUNT) {
    lsx_fail(
        "Vulkan DSD rational ratio exceeds the 32-bit output index range");
    free(context);
    return NULL;
  }
  context->output_frames = (uint32_t)output_frames_wide;
  context->padded_input_frames =
      context->input_frames +
      2u * SDM_VULKAN_INPUT_PADDING;
  context->block_count =
      context->output_frames / SDM_VULKAN_BLOCK_SAMPLES;
  context->scan_count = scan_storage_count(
      context->output_frames, context->offsets,
      context->counts, &context->level_count);
  if (!context->scan_count) {
    lsx_fail("Vulkan DSD scan hierarchy is too deep");
    free(context);
    return NULL;
  }
  context->history = lsx_calloc(
      (size_t)channels * SDM_VULKAN_INPUT_PADDING,
      sizeof(*context->history));
  if (initialize_vulkan(context) != SOX_SUCCESS)
    goto error;

  state_count =
      discover_states(states, lookup, &initial_state);
  if (state_count != SDM_VULKAN_STATE_COUNT ||
      initial_state == UINT32_MAX) {
    lsx_fail("Vulkan DSD FSM initialization failed");
    goto error;
  }
  coefficients = lsx_malloc(
      context->buffers[BUFFER_FIR_COEFFICIENTS].size);
  design_filter(
      coefficients, context->up_factor, input_rate, output_rate);
  if (upload_buffer(
      context, BUFFER_STATES, states, sizeof(states)) != SOX_SUCCESS ||
      upload_buffer(
      context, BUFFER_STATE_LOOKUP, lookup, sizeof(lookup)) != SOX_SUCCESS ||
      upload_buffer(
      context, BUFFER_FIR_COEFFICIENTS, coefficients,
      context->buffers[BUFFER_FIR_COEFFICIENTS].size) != SOX_SUCCESS) {
    free(coefficients);
    goto error;
  }
  free(coefficients);
  for (channel = 0; channel < channels; ++channel)
    persistent[channel * 4u + 3u] = initial_state;
  if (upload_buffer(
      context, BUFFER_PERSISTENT_STATE, persistent,
      (VkDeviceSize)channels * 4u * sizeof(uint32_t)) !=
      SOX_SUCCESS)
    goto error;

  memset(&context->fir_parameters, 0,
      sizeof(context->fir_parameters));
  context->fir_parameters.output_frames =
      context->output_frames;
  context->fir_parameters.padded_input_frames =
      context->padded_input_frames;
  context->fir_parameters.phase_count = context->up_factor;
  context->fir_parameters.taps_per_phase =
      SDM_VULKAN_TAPS_PER_PHASE;
  context->fir_parameters.input_padding =
      SDM_VULKAN_INPUT_PADDING;
  context->fir_parameters.channel_count = channels;
  context->fir_parameters.phase_step = context->down_factor;
  memset(&context->mash_parameters, 0,
      sizeof(context->mash_parameters));
  context->mash_parameters.sample_count =
      context->output_frames;
  context->mash_parameters.block_count =
      context->block_count;
  context->mash_parameters.state_count = state_count;
  context->mash_parameters.block_samples =
      SDM_VULKAN_BLOCK_SAMPLES;
  context->mash_parameters.initial_state = initial_state;
  context->mash_parameters.input_gain =
      SDM_VULKAN_INPUT_GAIN;
  context->mash_parameters.scan_storage_count = context->scan_count;
  lsx_report(
      "Vulkan DSD backend: %s, DSD%u, %u channel%s",
      context->vulkan->properties.deviceName, context->dsd_factor,
      channels, channels == 1u ? "" : "s");
  return context;

error:
  lsx_sdm_vulkan_destroy(context);
  return NULL;
}

void lsx_sdm_vulkan_destroy(lsx_sdm_vulkan_t *context)
{
  uint32_t index;

  if (!context)
    return;
  if (context->process_calls)
    lsx_debug(
        "Vulkan DSD process: %.6f seconds in %llu calls; "
        "FIR GPU %.6f, MASH GPU %.6f seconds",
        context->process_seconds,
        (unsigned long long)context->process_calls,
        context->fir_gpu_seconds, context->mash_gpu_seconds);
  if (context->vulkan->device)
    vkDeviceWaitIdle(context->vulkan->device);
  if (context->vulkan->device) {
    for (index = 0; index < BUFFER_COUNT; ++index)
      destroy_buffer(context, &context->buffers[index]);
    destroy_buffer(context, &context->upload);
    destroy_buffer(context, &context->download);
    if (context->query_pool)
      vkDestroyQueryPool(
          context->vulkan->device, context->query_pool, NULL);
    if (context->fence)
      vkDestroyFence(context->vulkan->device, context->fence, NULL);
    if (context->command_buffer)
      vkFreeCommandBuffers(
          context->vulkan->device, context->vulkan->command_pool, 1,
          &context->command_buffer);
    if (context->descriptor_pool)
      vkDestroyDescriptorPool(
          context->vulkan->device, context->descriptor_pool, NULL);
    if (context->fir_pipeline)
      vkDestroyPipeline(
          context->vulkan->device, context->fir_pipeline, NULL);
    if (context->mash_pipeline)
      vkDestroyPipeline(
          context->vulkan->device, context->mash_pipeline, NULL);
    if (context->fir_pipeline_layout)
      vkDestroyPipelineLayout(
          context->vulkan->device, context->fir_pipeline_layout, NULL);
    if (context->mash_pipeline_layout)
      vkDestroyPipelineLayout(
          context->vulkan->device, context->mash_pipeline_layout, NULL);
    if (context->fir_descriptor_layout)
      vkDestroyDescriptorSetLayout(
          context->vulkan->device, context->fir_descriptor_layout, NULL);
    if (context->mash_descriptor_layout)
      vkDestroyDescriptorSetLayout(
          context->vulkan->device, context->mash_descriptor_layout, NULL);
  }
  free(context->history);
  free(context);
}

size_t lsx_sdm_vulkan_input_capacity(
    lsx_sdm_vulkan_t const *context)
{
  return context ? context->input_frames : 0;
}

size_t lsx_sdm_vulkan_lookahead(void)
{
  return SDM_VULKAN_INPUT_PADDING;
}

int lsx_sdm_vulkan_process(
    lsx_sdm_vulkan_t *context, float const *input,
    size_t frames, size_t available_frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride)
{
  float *padded;
  size_t channel;
  size_t frame;
  uint64_t rational_output_frames;
  uint32_t valid_output_words;
  double started;

  if (!context || !input || !channel_bytes ||
      !bytes_per_channel || !channel_stride ||
      !frames || frames > context->input_frames ||
      available_frames < frames ||
      available_frames >
          context->input_frames +
          SDM_VULKAN_INPUT_PADDING) {
    lsx_fail("invalid Vulkan DSD process request");
    return SOX_EOF;
  }
  started = monotonic_seconds();
  padded = context->upload.mapped;
  memset(padded, 0, (size_t)context->buffers[BUFFER_PCM_INPUT].size);
  /*
   * The FIR is centred, so every channel receives 128 historical frames and
   * up to 128 look-ahead frames.  Full chunks are aligned to the rational
   * phase period; phase_start can therefore remain zero across calls.
   */
  for (channel = 0; channel < context->channels; ++channel) {
    float *target =
        padded + channel * context->padded_input_frames;
    float *history = context->history +
        channel * SDM_VULKAN_INPUT_PADDING;

    memcpy(target, history,
        SDM_VULKAN_INPUT_PADDING * sizeof(*target));
    for (frame = 0; frame < available_frames; ++frame)
      target[SDM_VULKAN_INPUT_PADDING + frame] =
          input[frame * context->channels + channel];
    if (frames >= SDM_VULKAN_INPUT_PADDING)
      for (frame = 0;
           frame < SDM_VULKAN_INPUT_PADDING; ++frame)
        history[frame] = input[
            (frames - SDM_VULKAN_INPUT_PADDING + frame) *
            context->channels + channel];
    else {
      memmove(history, history + frames,
          (SDM_VULKAN_INPUT_PADDING - frames) *
          sizeof(*history));
      for (frame = 0; frame < frames; ++frame)
        history[SDM_VULKAN_INPUT_PADDING - frames + frame] =
            input[frame * context->channels + channel];
    }
  }
  rational_output_frames =
      ((uint64_t)frames * context->up_factor +
       context->down_factor - 1u) / context->down_factor;
  /*
   * The FSM replays complete 256-sample blocks and the packed transport writes
   * complete 32-bit words.  Padding is silence produced by the zero-filled FIR
   * tail and changes file duration by at most 31 DSD samples.
   */
  context->output_frames = divide_up(
      (uint32_t)rational_output_frames,
      SDM_VULKAN_BLOCK_SAMPLES) * SDM_VULKAN_BLOCK_SAMPLES;
  context->block_count =
      context->output_frames / SDM_VULKAN_BLOCK_SAMPLES;
  context->scan_count = scan_storage_count(
      context->output_frames, context->offsets,
      context->counts, &context->level_count);
  context->fir_parameters.output_frames = context->output_frames;
  context->mash_parameters.sample_count = context->output_frames;
  context->mash_parameters.block_count = context->block_count;
  context->mash_parameters.scan_storage_count = context->scan_count;
  if (!context->scan_count || record_and_run(context) != SOX_SUCCESS)
    return SOX_EOF;
  valid_output_words = divide_up((uint32_t)rational_output_frames, 32u);
  *channel_bytes = context->download.mapped;
  *bytes_per_channel = (size_t)valid_output_words * sizeof(uint32_t);
  *channel_stride = context->output_frames / 8u;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  return SOX_SUCCESS;
}
