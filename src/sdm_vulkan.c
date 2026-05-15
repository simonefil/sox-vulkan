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
#else
#include <time.h>
#endif

/*
 * The Vulkan encoder is deliberately split into two GPU stages:
 *
 *  1. An ingest shader converts the incoming stream, host or resident, to
 *     fixed DSD rate without materialising an intermediate SoX stream.
 *  2. A conservative MASH-2 produces values in [-1, 2].  Two modular prefix
 *     scans expose its otherwise serial accumulators.  The final one-bit
 *     reducer is a 69-state finite-state machine: every 256-sample block
 *     first builds a transition for every possible state, the transitions
 *     are prefix-composed to find independent block seeds, and all blocks
 *     are then replayed concurrently while packing 32 output bits per word.
 *
 * Buffer indices and push constants must remain identical to the bindings
 * declared in the sdm_resident_*.comp ingest family and sdm_mash2_fsm.comp.
 */
#define SDM_VULKAN_LOCAL_SIZE 256u
#define SDM_VULKAN_INGEST_LOCAL_SIZE 128u
#define SDM_VULKAN_BLOCK_SAMPLES 256u
#define SDM_VULKAN_INPUT_FRAMES 16384u
#define SDM_VULKAN_RESIDENT_BATCH_SAMPLES \
  (128u * SDM_VULKAN_INPUT_FRAMES)
#define SDM_VULKAN_STATE_COUNT 69u
#define SDM_VULKAN_LOOKUP_COUNT 297u
#define SDM_VULKAN_MASH_BINDINGS 11u
#define SDM_VULKAN_RESIDENT_BINDINGS 3u
#define SDM_VULKAN_INPUT_GAIN 0.7079457844f

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

#include "sdm_mash2_fsm_spv.inc"
#include "sdm_resident_f32_spv.inc"
#include "sdm_resident_strict_f32_spv.inc"
#include "sdm_resident_f64_spv.inc"
#include "sdm_resident_reference_dd_spv.inc"

enum {
  BUFFER_MODULATOR_INPUT,
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
  uint32_t input_frames;
  uint32_t output_frames;
  uint32_t channels;
  float scale;
} resident_parameters_t;

lsx_static_assert(sizeof(state_t) == 8, vulkan_state_layout);
lsx_static_assert(sizeof(mash_parameters_t) == 64, vulkan_mash_push_layout);
lsx_static_assert(
    sizeof(resident_parameters_t) == 16, vulkan_resident_push_layout);

typedef lsx_vulkan_buffer_t buffer_t;

struct lsx_sdm_vulkan {
  lsx_vulkan_context_t *vulkan;
  VkDescriptorSetLayout mash_descriptor_layout;
  VkDescriptorSetLayout resident_descriptor_layout;
  VkPipelineLayout mash_pipeline_layout;
  VkPipelineLayout resident_pipeline_layout;
  VkPipeline mash_pipeline;
  VkPipeline resident_pipeline;
  VkDescriptorPool descriptor_pool;
  VkDescriptorPool resident_descriptor_pool;
  VkDescriptorSet mash_descriptor_set;
  VkDescriptorSet resident_descriptor_set;
  VkCommandBuffer command_buffer;
  VkCommandBuffer resident_append_commands[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkFence fence;
  VkQueryPool query_pool;
  buffer_t buffers[BUFFER_COUNT];
  buffer_t upload;
  buffer_t download;
  buffer_t resident_input;
  buffer_t resident_clips;
  uint32_t dsd_factor;
  uint32_t input_rate;
  uint32_t channels;
  uint32_t input_frames;
  uint32_t output_frames;
  uint32_t valid_output_words;
  uint32_t resident_pending_frames;
  uint32_t resident_append_bank_index;
  uint32_t resident_append_pending;
  lsx_vulkan_resident_format_t resident_input_format;
  sox_bool resident_input_format_known;
  sox_bool resident_final;
  float resident_scale;
  uint32_t block_count;
  uint32_t scan_count;
  uint32_t offsets[8];
  uint32_t counts[8];
  uint32_t level_count;
  mash_parameters_t mash_parameters;
  double process_seconds;
  double resident_gpu_seconds;
  double mash_gpu_seconds;
  uint64_t process_calls;
};

static int submit_and_wait(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_wait_reason_t reason);

static double monotonic_seconds(void)
{
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter))
    return 0.0;
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec value;

  if (clock_gettime(CLOCK_MONOTONIC, &value))
    return 0.0;
  return (double)value.tv_sec + (double)value.tv_nsec * 1e-9;
#endif
}

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static uint32_t divide_up(uint32_t value, uint32_t divisor)
{
  return value / divisor + (value % divisor != 0);
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
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkPushConstantRange mash_push = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mash_parameters_t)
  };
  VkPipelineLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    SDM_VULKAN_MASH_BINDINGS
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
  };
  VkDescriptorSetAllocateInfo set_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorBufferInfo
      mash_infos[SDM_VULKAN_MASH_BINDINGS];
  VkWriteDescriptorSet mash_writes[SDM_VULKAN_MASH_BINDINGS];
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

  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->mash_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &mash_push;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->mash_pipeline_layout),
      "vkCreatePipelineLayout MASH") != SOX_SUCCESS)
    return SOX_EOF;
  if (create_pipeline(
      context, sdm_vulkan_mash_spv, sdm_vulkan_mash_spv_size,
      context->mash_pipeline_layout,
      &context->mash_pipeline) != SOX_SUCCESS)
    return SOX_EOF;

  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(
      context->vulkan->device, &pool_info, NULL,
      &context->descriptor_pool),
      "vkCreateDescriptorPool") != SOX_SUCCESS)
    return SOX_EOF;
  set_info.descriptorPool = context->descriptor_pool;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &context->mash_descriptor_layout;
  if (vk_result(vkAllocateDescriptorSets(
      context->vulkan->device, &set_info,
      &context->mash_descriptor_set),
      "vkAllocateDescriptorSets") != SOX_SUCCESS)
    return SOX_EOF;

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
  vkUpdateDescriptorSets(
      context->vulkan->device, SDM_VULKAN_MASH_BINDINGS,
      mash_writes, 0, NULL);
  return SOX_SUCCESS;
}

static VkDeviceSize resident_sample_size(
    lsx_vulkan_resident_format_t format)
{
  switch (format) {
    case lsx_vulkan_resident_format_f32: return sizeof(float);
    case lsx_vulkan_resident_format_f32x2: return 2u * sizeof(float);
    case lsx_vulkan_resident_format_f64: return sizeof(double);
    case lsx_vulkan_resident_format_f64x2: return 2u * sizeof(double);
    default: return 0;
  }
}

/*
 * One ingest per producer format: the four the resident chain can carry.
 * Each writes the same planar FP32 the modulator reads, so the profile
 * chooses the arithmetic that reaches the modulator, not a different route.
 */
static int create_resident_pipeline(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_format_t input_format)
{
  VkDescriptorSetLayoutBinding bindings[SDM_VULKAN_RESIDENT_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(resident_parameters_t)
  };
  VkPipelineLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    SDM_VULKAN_RESIDENT_BINDINGS
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
  };
  VkDescriptorSetAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  uint32_t index;
  VkDeviceSize input_sample_size = resident_sample_size(input_format);
  uint32_t const *shader;
  size_t shader_size;

  if (!input_sample_size) {
    lsx_fail("unsupported resident Vulkan SDM input format");
    return SOX_EOF;
  }
  if (input_format == lsx_vulkan_resident_format_f64 ||
      input_format == lsx_vulkan_resident_format_f64x2) {
    if (!context->vulkan->shader_float64) {
      lsx_fail(
          "resident FP64 Vulkan SDM input requires shaderFloat64");
      return SOX_EOF;
    }
  }
  if (context->resident_pipeline)
    return context->resident_input_format == input_format ?
        SOX_SUCCESS : SOX_EOF;
  switch (input_format) {
    case lsx_vulkan_resident_format_f32x2:
      shader = sdm_vulkan_resident_strict_f32_spv;
      shader_size = sdm_vulkan_resident_strict_f32_spv_size;
      break;
    case lsx_vulkan_resident_format_f64:
      shader = sdm_vulkan_resident_f64_spv;
      shader_size = sdm_vulkan_resident_f64_spv_size;
      break;
    case lsx_vulkan_resident_format_f64x2:
      shader = sdm_vulkan_resident_reference_dd_spv;
      shader_size = sdm_vulkan_resident_reference_dd_spv_size;
      break;
    default:
      shader = sdm_vulkan_resident_spv;
      shader_size = sdm_vulkan_resident_spv_size;
      break;
  }
  context->resident_input_format = input_format;
  context->resident_input_format_known = sox_true;
  if (lsx_vulkan_buffer_create(
      context->vulkan, &context->resident_input,
      (VkDeviceSize)context->input_frames *
      context->channels * input_sample_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      lsx_vulkan_buffer_create(
      context->vulkan, &context->resident_clips,
      sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  memset(context->resident_clips.mapped, 0, sizeof(uint32_t));
  memset(bindings, 0, sizeof(bindings));
  for (index = 0; index < SDM_VULKAN_RESIDENT_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = SDM_VULKAN_RESIDENT_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      context->vulkan->device, &descriptor_info, NULL,
      &context->resident_descriptor_layout),
      "vkCreateDescriptorSetLayout resident DSD") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->resident_descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->resident_pipeline_layout),
      "vkCreatePipelineLayout resident DSD") != SOX_SUCCESS ||
      create_pipeline(
      context, shader, shader_size,
      context->resident_pipeline_layout,
      &context->resident_pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(
      context->vulkan->device, &pool_info, NULL,
      &context->resident_descriptor_pool),
      "vkCreateDescriptorPool resident DSD") != SOX_SUCCESS)
    return SOX_EOF;
  allocation.descriptorPool = context->resident_descriptor_pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &context->resident_descriptor_layout;
  return vk_result(vkAllocateDescriptorSets(
      context->vulkan->device, &allocation,
      &context->resident_descriptor_set),
      "vkAllocateDescriptorSets resident DSD");
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

  sizes[BUFFER_MODULATOR_INPUT] = (VkDeviceSize)context->channels *
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

  for (index = 0; index < BUFFER_COUNT; ++index) {
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (sizes[index] >
        context->vulkan->properties.limits.maxStorageBufferRange) {
      lsx_fail("Vulkan DSD buffer exceeds device storage range");
      return SOX_EOF;
    }
    if (index == BUFFER_STATES || index == BUFFER_STATE_LOOKUP ||
        index == BUFFER_PERSISTENT_STATE)
      usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (index == BUFFER_OUTPUT_WORDS)
      usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (create_buffer(
        context, &context->buffers[index], sizes[index], usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
      return SOX_EOF;
  }
  upload_size = (VkDeviceSize)context->channels *
      context->input_frames * sizeof(float);
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
  command_info.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  if (vk_result(vkAllocateCommandBuffers(context->vulkan->device, &command_info, context->resident_append_commands), "vkAllocateCommandBuffers resident DSD append") != SOX_SUCCESS)
    return SOX_EOF;
  if (context->vulkan->timestamp_valid_bits &&
      vk_result(vkCreateQueryPool(
      context->vulkan->device, &query_info, NULL, &context->query_pool),
      "vkCreateQueryPool") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int submit_and_wait(lsx_sdm_vulkan_t *context, lsx_vulkan_wait_reason_t reason)
{
  return lsx_vulkan_submit_and_wait(
      context->vulkan, context->command_buffer, context->fence, reason);
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
  return submit_and_wait(context, lsx_vulkan_wait_sdm_setup);
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

  /* scan_storage_count always produces at least one level, but the down-sweep
   * below counts backwards from level_count - 1: a zero here would wrap the
   * unsigned counter and index offsets[] far out of bounds. */
  if (!context->level_count)
    return;
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH prefix scan");
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
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
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
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH stage 1");
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
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH stage 2");
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
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);

  /*
   * A table entry maps one of 69 reducer states across one 256-sample block.
   * Parallel prefix composition finds the true seed of every block; replay
   * then emits all blocks concurrently without breaking causal equivalence.
   */
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH reducer tables");
  parameters->mode = MODE_TABLES;
  mash_dispatch(context, parameters,
      divide_up(table_items, SDM_VULKAN_LOCAL_SIZE));
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH reducer compose");
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
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "MASH reducer seeds and replay");
  parameters->table_source = table_source;
  parameters->mode = MODE_SEEDS;
  mash_dispatch(context, parameters,
      divide_up(parameters->block_count,
                SDM_VULKAN_LOCAL_SIZE));
  parameters->mode = MODE_REPLAY;
  mash_dispatch(context, parameters,
      divide_up(parameters->block_count,
                SDM_VULKAN_LOCAL_SIZE));
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
}

static int ensure_resident_pipeline(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_format_t format,
    lsx_vulkan_resident_domain_t domain)
{
  float scale;

  switch (domain) {
    case lsx_vulkan_resident_domain_normalized: scale = 1.0f; break;
    case lsx_vulkan_resident_domain_sox_sample:
      scale = 1.0f / 2147483648.0f;
      break;
    default:
      lsx_fail("unsupported resident Vulkan SDM input domain");
      return SOX_EOF;
  }
  if (create_resident_pipeline(context, format) != SOX_SUCCESS) {
    lsx_fail(
        "the Vulkan SDM ingest cannot change input format mid-stream");
    return SOX_EOF;
  }
  context->resident_scale = scale;
  return SOX_SUCCESS;
}

static int upload_resident_input(
    lsx_sdm_vulkan_t *context, void const *data, VkDeviceSize size)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkBufferCopy copy = {0, 0, size};

  if (size > context->upload.size ||
      size > context->resident_input.size) {
    lsx_fail("invalid Vulkan DSD upload");
    return SOX_EOF;
  }
  memcpy(context->upload.mapped, data, (size_t)size);
  if (vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer host DSD upload") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer host DSD upload") != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdCopyBuffer(
      context->command_buffer, context->upload.buffer,
      context->resident_input.buffer, 1, &copy);
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer host DSD upload") != SOX_SUCCESS)
    return SOX_EOF;
  return submit_and_wait(context, lsx_vulkan_wait_sdm_setup);
}

static int update_resident_descriptors(lsx_sdm_vulkan_t *context)
{
  VkDescriptorBufferInfo infos[SDM_VULKAN_RESIDENT_BINDINGS];
  VkWriteDescriptorSet writes[SDM_VULKAN_RESIDENT_BINDINGS];
  uint32_t index;

  if (context->resident_input.size >
      context->vulkan->properties.limits.maxStorageBufferRange) {
    lsx_fail("resident Vulkan DSD input exceeds device storage range");
    return SOX_EOF;
  }
  infos[0].buffer = context->resident_input.buffer;
  infos[0].offset = 0;
  infos[0].range = context->resident_input.size;
  infos[1].buffer = context->buffers[BUFFER_MODULATOR_INPUT].buffer;
  infos[1].offset = 0;
  infos[1].range = context->buffers[BUFFER_MODULATOR_INPUT].size;
  infos[2].buffer = context->resident_clips.buffer;
  infos[2].offset = 0;
  infos[2].range = context->resident_clips.size;
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < SDM_VULKAN_RESIDENT_BINDINGS; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = context->resident_descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &infos[index];
  }
  vkUpdateDescriptorSets(
      context->vulkan->device, SDM_VULKAN_RESIDENT_BINDINGS,
      writes, 0, NULL);
  return SOX_SUCCESS;
}

static int append_resident_input(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    uint32_t source_frame, uint32_t frames)
{
  VkCommandBuffer command_buffer = context->resident_append_commands[context->resident_append_bank_index];
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkMemoryBarrier source_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    0, VK_ACCESS_TRANSFER_READ_BIT
  };
  VkDeviceSize sample_size =
      resident_sample_size(context->resident_input_format);
  VkDeviceSize frame_size =
      (VkDeviceSize)context->channels * sample_size;
  VkBufferCopy copy = {
    input->offset + (VkDeviceSize)source_frame * frame_size,
    (VkDeviceSize)context->resident_pending_frames * frame_size,
    (VkDeviceSize)frames * frame_size
  };

  if (!frames)
    return SOX_SUCCESS;
  if (!(input->buffer->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
    lsx_fail("resident Vulkan DSD input is not transferable");
    return SOX_EOF;
  }
  source_barrier.srcAccessMask = input->producer_access;
  if (vk_result(vkResetCommandBuffer(
      command_buffer, 0),
      "vkResetCommandBuffer resident DSD append") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      command_buffer, &begin),
      "vkBeginCommandBuffer resident DSD append") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Resident PCM append");
  vkCmdPipelineBarrier(
      command_buffer, input->producer_stage,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      1, &source_barrier, 0, NULL, 0, NULL);
  vkCmdCopyBuffer(
      command_buffer, input->buffer->buffer,
      context->resident_input.buffer, 1, &copy);
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(
      command_buffer),
      "vkEndCommandBuffer resident DSD append") != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_enqueue(context->vulkan, command_buffer) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_append_bank_index = (context->resident_append_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  ++context->resident_append_pending;
  return SOX_SUCCESS;
}

static int retire_resident_appends(lsx_sdm_vulkan_t *context)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };

  if (context->resident_append_pending <
      lsx_vulkan_resident_batch_depth(context->vulkan))
    return SOX_SUCCESS;
  if (vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer resident DSD flush") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer resident DSD flush") != SOX_SUCCESS ||
      vk_result(vkEndCommandBuffer(context->command_buffer),
      "vkEndCommandBuffer resident DSD flush") != SOX_SUCCESS ||
      submit_and_wait(
      context, lsx_vulkan_wait_sdm_resident_flush) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_append_pending = 0;
  return SOX_SUCCESS;
}

static int record_resident_and_run(
    lsx_sdm_vulkan_t *context, uint32_t input_frames,
    uint32_t retained_frames)
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
  VkBufferCopy output_copy = {
    0, 0, context->download.size
  };
  VkBufferCopy retain_copy;
  VkDeviceSize resident_frame_size =
      (VkDeviceSize)context->channels *
      resident_sample_size(context->resident_input_format);
  resident_parameters_t parameters;
  uint64_t timestamps[3];
  double period =
      (double)context->vulkan->properties.limits.timestampPeriod * 1e-9;

  memset(&parameters, 0, sizeof(parameters));
  parameters.input_frames = input_frames;
  parameters.output_frames = context->output_frames;
  parameters.channels = context->channels;
  parameters.scale = context->resident_scale;
  if (update_resident_descriptors(context) != SOX_SUCCESS ||
      vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
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
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "Resident PCM to SDM input");
  vkCmdBindPipeline(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->resident_pipeline);
  vkCmdBindDescriptorSets(
      context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->resident_pipeline_layout, 0, 1,
      &context->resident_descriptor_set, 0, NULL);
  vkCmdPushConstants(
      context->command_buffer, context->resident_pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(parameters), &parameters);
  vkCmdDispatch(
      context->command_buffer,
      divide_up(context->output_frames,
      SDM_VULKAN_INGEST_LOCAL_SIZE),
      context->channels, 1);
  shader_barrier(context->command_buffer);
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
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
  lsx_vulkan_label_begin(context->vulkan, context->command_buffer, "Packed DSD download");
  vkCmdCopyBuffer(
      context->command_buffer,
      context->buffers[BUFFER_OUTPUT_WORDS].buffer,
      context->download.buffer, 1, &output_copy);
  lsx_vulkan_label_end(context->vulkan, context->command_buffer);
  vkCmdPipelineBarrier(
      context->command_buffer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0,
      1, &download_barrier, 0, NULL, 0, NULL);
  if (retained_frames) {
    retain_copy.srcOffset =
        (VkDeviceSize)input_frames * resident_frame_size;
    retain_copy.dstOffset = 0;
    retain_copy.size =
        (VkDeviceSize)retained_frames * resident_frame_size;
    lsx_vulkan_label_begin(
        context->vulkan, context->command_buffer,
        "Retain partial DSD input block");
    vkCmdCopyBuffer(
        context->command_buffer, context->resident_input.buffer,
        context->resident_input.buffer, 1, &retain_copy);
    lsx_vulkan_label_end(
        context->vulkan, context->command_buffer);
  }
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer") != SOX_SUCCESS ||
      submit_and_wait(context, lsx_vulkan_wait_packed_output) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_append_pending = 0;
  if (context->query_pool) {
    if (vk_result(vkGetQueryPoolResults(
        context->vulkan->device, context->query_pool, 0, 3,
        sizeof(timestamps), timestamps, sizeof(timestamps[0]),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
        "vkGetQueryPoolResults") != SOX_SUCCESS)
      return SOX_EOF;
    context->resident_gpu_seconds +=
        (double)(timestamps[1] - timestamps[0]) * period;
    context->mash_gpu_seconds +=
        (double)(timestamps[2] - timestamps[1]) * period;
  }
  return SOX_SUCCESS;
}

sox_bool lsx_sdm_vulkan_dsd_rate_supported(unsigned rate)
{
  unsigned factor = rate % 44100u ? 0u : rate / 44100u;

  return factor == 64u || factor == 128u || factor == 256u ||
      factor == 512u || factor == 1024u;
}

static lsx_sdm_vulkan_t *create_with_input_target(
    lsx_vulkan_context_t *vulkan, unsigned rate,
    unsigned channels, uint32_t input_target_frames)
{
  lsx_sdm_vulkan_t *context;
  uint64_t input_capacity;
  state_t states[SDM_VULKAN_STATE_COUNT];
  int32_t lookup[SDM_VULKAN_LOOKUP_COUNT];
  uint32_t persistent[24] = {0};
  uint32_t initial_state = UINT32_MAX;
  uint32_t state_count;
  uint16_t endian = 1;
  unsigned channel;

  if (!vulkan || !input_target_frames) {
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
  if (!lsx_sdm_vulkan_dsd_rate_supported(rate)) {
    lsx_fail("Vulkan DSD supports DSD64 through DSD1024");
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->input_rate = rate;
  context->dsd_factor = rate / 44100u;
  input_capacity =
      ((uint64_t)input_target_frames +
       2u * SDM_VULKAN_BLOCK_SAMPLES - 2u) /
      SDM_VULKAN_BLOCK_SAMPLES * SDM_VULKAN_BLOCK_SAMPLES;
  if (input_capacity > UINT32_MAX) {
    lsx_fail("Vulkan DSD input batch is too large");
    free(context);
    return NULL;
  }
  /*
   * Keep one producer block plus a partial FSM block.  Non-final input is
   * dispatched only in complete 256-sample blocks, so padding can occur
   * once, at the logical end of the stream, instead of between slices.
   */
  context->input_frames = (uint32_t)input_capacity;
  if (!context->input_frames)
    context->input_frames = SDM_VULKAN_BLOCK_SAMPLES;
  context->channels = channels;
  context->output_frames = context->input_frames;
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
  if (initialize_vulkan(context) != SOX_SUCCESS)
    goto error;

  state_count =
      discover_states(states, lookup, &initial_state);
  if (state_count != SDM_VULKAN_STATE_COUNT ||
      initial_state == UINT32_MAX) {
    lsx_fail("Vulkan DSD FSM initialization failed");
    goto error;
  }
  if (upload_buffer(
      context, BUFFER_STATES, states, sizeof(states)) != SOX_SUCCESS ||
      upload_buffer(
      context, BUFFER_STATE_LOOKUP, lookup, sizeof(lookup)) !=
      SOX_SUCCESS)
    goto error;
  for (channel = 0; channel < channels; ++channel)
    persistent[channel * 4u + 3u] = initial_state;
  if (upload_buffer(
      context, BUFFER_PERSISTENT_STATE, persistent,
      (VkDeviceSize)channels * 4u * sizeof(uint32_t)) !=
      SOX_SUCCESS)
    goto error;

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

lsx_sdm_vulkan_t *lsx_sdm_vulkan_create(
    lsx_vulkan_context_t *vulkan, unsigned rate, unsigned channels,
    size_t batch_frames)
{
  if (batch_frames > UINT32_MAX)
    return NULL;
  return create_with_input_target(
      vulkan, rate, channels,
      batch_frames ? (uint32_t)batch_frames :
      SDM_VULKAN_INPUT_FRAMES);
}

void lsx_sdm_vulkan_destroy(lsx_sdm_vulkan_t *context)
{
  uint32_t index;

  if (!context)
    return;
  if (context->process_calls)
    lsx_debug(
        "Vulkan DSD process: %.6f seconds in %llu calls; "
        "ingest GPU %.6f, MASH GPU %.6f seconds",
        context->process_seconds,
        (unsigned long long)context->process_calls,
        context->resident_gpu_seconds, context->mash_gpu_seconds);
  if (context->vulkan->device)
    vkDeviceWaitIdle(context->vulkan->device);
  if (context->vulkan->device) {
    for (index = 0; index < BUFFER_COUNT; ++index)
      destroy_buffer(context, &context->buffers[index]);
    destroy_buffer(context, &context->upload);
    destroy_buffer(context, &context->download);
    destroy_buffer(context, &context->resident_input);
    destroy_buffer(context, &context->resident_clips);
    if (context->query_pool)
      vkDestroyQueryPool(
          context->vulkan->device, context->query_pool, NULL);
    if (context->fence)
      vkDestroyFence(context->vulkan->device, context->fence, NULL);
    if (context->command_buffer)
      vkFreeCommandBuffers(
          context->vulkan->device, context->vulkan->command_pool, 1,
          &context->command_buffer);
    if (context->resident_append_commands[0])
      vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, LSX_VULKAN_RESIDENT_BATCH_DEPTH, context->resident_append_commands);
    if (context->descriptor_pool)
      vkDestroyDescriptorPool(
          context->vulkan->device, context->descriptor_pool, NULL);
    if (context->resident_descriptor_pool)
      vkDestroyDescriptorPool(
          context->vulkan->device,
          context->resident_descriptor_pool, NULL);
    if (context->resident_pipeline)
      vkDestroyPipeline(
          context->vulkan->device,
          context->resident_pipeline, NULL);
    if (context->mash_pipeline)
      vkDestroyPipeline(
          context->vulkan->device, context->mash_pipeline, NULL);
    if (context->resident_pipeline_layout)
      vkDestroyPipelineLayout(
          context->vulkan->device,
          context->resident_pipeline_layout, NULL);
    if (context->mash_pipeline_layout)
      vkDestroyPipelineLayout(
          context->vulkan->device, context->mash_pipeline_layout, NULL);
    if (context->resident_descriptor_layout)
      vkDestroyDescriptorSetLayout(
          context->vulkan->device,
          context->resident_descriptor_layout, NULL);
    if (context->mash_descriptor_layout)
      vkDestroyDescriptorSetLayout(
          context->vulkan->device, context->mash_descriptor_layout, NULL);
  }
  free(context);
}

size_t lsx_sdm_vulkan_input_capacity(
    lsx_sdm_vulkan_t const *context)
{
  return context ? context->input_frames : 0;
}

static int process_resident_pending(
    lsx_sdm_vulkan_t *context, uint32_t input_frames,
    uint32_t retained_frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride);

int lsx_sdm_vulkan_process(
    lsx_sdm_vulkan_t *context, float const *input,
    size_t frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride)
{
  double started;

  if (!context || !input || !channel_bytes ||
      !bytes_per_channel || !channel_stride ||
      !frames || frames > context->input_frames) {
    lsx_fail("invalid Vulkan DSD process request");
    return SOX_EOF;
  }
  started = monotonic_seconds();
  /*
   * The host stream is already at the DSD rate and normalized, so it takes
   * the same ingest as a resident producer: no filter, only the interleaved
   * to planar conversion the modulator reads.
   */
  if (ensure_resident_pipeline(
      context, lsx_vulkan_resident_format_f32,
      lsx_vulkan_resident_domain_normalized) != SOX_SUCCESS ||
      upload_resident_input(
      context, input,
      (VkDeviceSize)frames * context->channels * sizeof(float)) !=
      SOX_SUCCESS)
    return SOX_EOF;
  if (process_resident_pending(
      context, (uint32_t)frames, 0, channel_bytes,
      bytes_per_channel, channel_stride) != SOX_SUCCESS)
    return SOX_EOF;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  return SOX_SUCCESS;
}

static int process_resident_pending(
    lsx_sdm_vulkan_t *context, uint32_t input_frames,
    uint32_t retained_frames,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride)
{
  VkDeviceSize output_capacity_frames;

  context->output_frames = divide_up(
      input_frames, SDM_VULKAN_BLOCK_SAMPLES) *
      SDM_VULKAN_BLOCK_SAMPLES;
  output_capacity_frames =
      context->buffers[BUFFER_MODULATOR_INPUT].size /
      (context->channels * sizeof(float));
  if (context->output_frames > output_capacity_frames) {
    lsx_fail("resident Vulkan DSD input exceeds modulator capacity");
    return SOX_EOF;
  }
  context->block_count =
      context->output_frames / SDM_VULKAN_BLOCK_SAMPLES;
  context->scan_count = scan_storage_count(
      context->output_frames, context->offsets,
      context->counts, &context->level_count);
  context->mash_parameters.sample_count = context->output_frames;
  context->mash_parameters.block_count = context->block_count;
  context->mash_parameters.scan_storage_count = context->scan_count;
  if (!context->scan_count || !context->resident_pipeline)
    return SOX_EOF;
  if (record_resident_and_run(
      context, input_frames, retained_frames) != SOX_SUCCESS)
    return SOX_EOF;
  context->valid_output_words = divide_up(input_frames, 32u);
  *channel_bytes = context->download.mapped;
  *bytes_per_channel =
      (size_t)context->valid_output_words * sizeof(uint32_t);
  *channel_stride = context->output_frames / 8u;
  return SOX_SUCCESS;
}

int lsx_sdm_vulkan_consume_resident(
    lsx_sdm_vulkan_t *context,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride)
{
  sox_rate_t rate;
  double started;

  if (!context || !input_consumed || !output_ready ||
      !channel_bytes || !bytes_per_channel || !channel_stride)
    return SOX_EOF;
  started = monotonic_seconds();
  *input_consumed = sox_false;
  *output_ready = sox_false;
  *channel_bytes = NULL;
  *bytes_per_channel = 0;
  *channel_stride = 0;
  if (input) {
    if (lsx_vulkan_resident_buffer_validate(input) != SOX_SUCCESS)
      return SOX_EOF;
    rate = (sox_rate_t)context->dsd_factor * 44100.;
    /*
     * Any of the four resident formats is accepted, so the profile chosen
     * for the chain reaches the modulator unchanged.  What is fixed is the
     * shape: interleaved frames at the DSD rate, one frame per element.
     */
    if (input->layout != lsx_vulkan_resident_layout_interleaved ||
        input->frames_per_element != 1u ||
        input->channels != context->channels ||
        input->frame_stride_elements != context->channels ||
        input->channel_stride_elements != 1u ||
        input->rate != rate ||
        input->state == lsx_vulkan_resident_empty ||
        input->valid_elements > UINT32_MAX) {
      lsx_fail("unsupported resident Vulkan DSD input");
      return SOX_EOF;
    }
    if (!input->valid_elements &&
        input->state != lsx_vulkan_resident_final) {
      lsx_fail("empty resident Vulkan DSD input is not final");
      return SOX_EOF;
    }
    if (input->valid_elements >
        context->input_frames - context->resident_pending_frames) {
      lsx_fail(
          "resident Vulkan DSD input of %lu frames plus %u retained "
          "frames exceeds the %lu-frame modulator batch",
          (unsigned long)input->valid_elements,
          context->resident_pending_frames,
          (unsigned long)context->input_frames);
      return SOX_EOF;
    }
    if (ensure_resident_pipeline(
        context, input->format, input->domain) != SOX_SUCCESS)
      return SOX_EOF;
    lsx_debug_more(
        "resident Vulkan DSD input: valid=%u block=%u state=%u pending=%u",
        (unsigned)input->valid_elements,
        (unsigned)input->block_elements, (unsigned)input->state,
        context->resident_pending_frames);
    if (append_resident_input(
        context, input, 0, (uint32_t)input->valid_elements) !=
        SOX_SUCCESS)
      return SOX_EOF;
    context->resident_pending_frames +=
        (uint32_t)input->valid_elements;
    *input_consumed = sox_true;
    if (input->state == lsx_vulkan_resident_final)
      context->resident_final = sox_true;
  }
  if (context->resident_pending_frames) {
    uint32_t process_frames = context->resident_final ?
        context->resident_pending_frames :
        context->resident_pending_frames /
        SDM_VULKAN_BLOCK_SAMPLES * SDM_VULKAN_BLOCK_SAMPLES;
    uint32_t retained_frames =
        context->resident_pending_frames - process_frames;

    if (!process_frames)
      goto retire_appends;
    if (process_resident_pending(
        context, process_frames, retained_frames, channel_bytes,
        bytes_per_channel, channel_stride) != SOX_SUCCESS)
      return SOX_EOF;
    context->resident_pending_frames = retained_frames;
    *output_ready = sox_true;
    ++context->process_calls;
  }
retire_appends:
  if (retire_resident_appends(context) != SOX_SUCCESS)
    return SOX_EOF;
  context->process_seconds += monotonic_seconds() - started;
  return SOX_SUCCESS;
}

sox_bool lsx_sdm_vulkan_resident_active(
    lsx_sdm_vulkan_t const *context)
{
  return context && context->resident_final &&
      context->resident_pending_frames != 0;
}

uint64_t lsx_sdm_vulkan_resident_clips(
    lsx_sdm_vulkan_t const *context)
{
  return context && context->resident_clips.mapped ?
      *(uint32_t const *)context->resident_clips.mapped : 0;
}

int lsx_sdm_vulkan_drain_resident(
    lsx_sdm_vulkan_t *context, sox_bool *output_ready,
    uint8_t const **channel_bytes,
    size_t *bytes_per_channel, size_t *channel_stride)
{
  double started;

  if (!context || !output_ready || !channel_bytes ||
      !bytes_per_channel || !channel_stride)
    return SOX_EOF;
  *output_ready = sox_false;
  *channel_bytes = NULL;
  *bytes_per_channel = 0;
  *channel_stride = 0;
  if (!context->resident_pending_frames)
    return SOX_SUCCESS;
  started = monotonic_seconds();
  if (process_resident_pending(
      context, context->resident_pending_frames,
      0,
      channel_bytes, bytes_per_channel,
      channel_stride) != SOX_SUCCESS)
    return SOX_EOF;
  context->resident_pending_frames = 0;
  *output_ready = sox_true;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  return SOX_SUCCESS;
}

int lsx_sdm_vulkan_resident_output(lsx_sdm_vulkan_t *context, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  if (!context || !resident)
    return SOX_EOF;
  memset(resident, 0, sizeof(*resident));
  resident->buffer = &context->buffers[BUFFER_OUTPUT_WORDS];
  resident->owner = context;
  resident->producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = context->buffers[BUFFER_OUTPUT_WORDS].size / (context->channels * sizeof(uint32_t));
  resident->valid_elements = context->valid_output_words;
  resident->frame_stride_elements = 1u;
  resident->channel_stride_elements = resident->capacity_elements;
  resident->frame_offset = frame_offset;
  resident->rate = (sox_rate_t)context->dsd_factor * 44100.;
  resident->channels = context->channels;
  resident->frames_per_element = 32u;
  resident->format = lsx_vulkan_resident_format_dsd_u32;
  resident->domain = lsx_vulkan_resident_domain_dsd;
  resident->layout = lsx_vulkan_resident_layout_planar;
  resident->state = state;
  return lsx_vulkan_resident_buffer_validate(resident);
}
