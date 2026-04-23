/* Partitioned VkFFT FIR backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "fir_vulkan.h"
#include "vulkan_fft.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "fir_partition_f64_spv.inc"
#include "fir_partition_f32_spv.inc"
#include "fir_partition_accurate_f32_spv.inc"
#include "fir_partition_strict_f32_spv.inc"
#include "fir_partition_precise_f64_spv.inc"
#include "fir_partition_reference_dd_spv.inc"
#include "fir_spectrum_multiply_reference_dd_spv.inc"

#define FIR_DEFAULT_FFT_SIZE 32768u
#define FIR_FAST_FFT_SIZE 131072u
#define FIR_DEFAULT_BLOCK_FRAMES (FIR_DEFAULT_FFT_SIZE / 2u)
#define FIR_FAST_BLOCK_FRAMES (FIR_FAST_FFT_SIZE / 2u)
#define FIR_FFT_SIZE (context->fft_size)
#define FIR_BLOCK_FRAMES (context->block_frames)
#define FIR_LOCAL_SIZE 256u

typedef lsx_vulkan_buffer_t buffer_t;

typedef struct {
  uint32_t spectrum_bins;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
} partition_parameters_t;

typedef struct {
  uint32_t operation;
  uint32_t stage;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
  uint32_t inverse;
  uint32_t source_is_a;
  uint32_t reserved;
} strict_fp32_parameters_t;

lsx_static_assert(
    sizeof(partition_parameters_t) == 16,
    vulkan_fir_partition_push_layout);
lsx_static_assert(
    sizeof(strict_fp32_parameters_t) == 32,
    vulkan_fir_strict_fp32_push_layout);

struct lsx_fir_vulkan {
  lsx_vulkan_context_t *vulkan;
  lsx_vulkan_fft_t *fft;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandBuffer command_buffer;
  VkCommandBuffer *process_commands;
  VkCommandBuffer *resident_process_commands;
  VkFence fence;
  buffer_t working;
  buffer_t working_scratch;
  buffer_t history;
  buffer_t kernels;
  buffer_t kernels_low;
  buffer_t twiddles;
  buffer_t strict_output;
  buffer_t upload;
  buffer_t resident_upload[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  buffer_t download;
  void *working_host;
  double *previous;
  double *output;
  size_t element_size;
  sox_bool double_precision;
  sox_bool accurate_fp32;
  sox_bool strict_fp32;
  sox_bool authoritative_fp64_kernels;
  sox_bool precise_fp64;
  sox_bool reference_dd;
  int emit_low_residual;
  uint32_t taps;
  uint32_t fft_size;
  uint32_t block_frames;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
  uint32_t resident_bank_index;
  double startup_seconds;
  double process_seconds;
  uint64_t process_calls;
};

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

static int create_buffer(
    lsx_fir_vulkan_t *context, buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties)
{
  return lsx_vulkan_buffer_create(
      context->vulkan, buffer, size, usage, properties);
}

static void destroy_buffer(
    lsx_fir_vulkan_t *context, buffer_t *buffer)
{
  lsx_vulkan_buffer_destroy(context->vulkan, buffer);
}

static int begin_commands(lsx_fir_vulkan_t *context)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };

  if (vk_result(vkResetCommandBuffer(
      context->command_buffer, 0),
      "vkResetCommandBuffer") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(
      context->command_buffer, &begin),
      "vkBeginCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int submit_commands(lsx_fir_vulkan_t *context, lsx_vulkan_wait_reason_t reason)
{
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  return lsx_vulkan_submit_and_wait(
      context->vulkan, context->command_buffer, context->fence, reason);
}

static void memory_barrier(
    VkCommandBuffer command_buffer,
    VkAccessFlags source_access,
    VkAccessFlags destination_access,
    VkPipelineStageFlags source_stage,
    VkPipelineStageFlags destination_stage)
{
  VkMemoryBarrier barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    source_access, destination_access
  };

  vkCmdPipelineBarrier(
      command_buffer, source_stage, destination_stage,
      0, 1, &barrier, 0, NULL, 0, NULL);
}

static int create_commands(lsx_fir_vulkan_t *context)
{
  VkCommandBufferAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
    context->vulkan->command_pool,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1
  };
  VkFenceCreateInfo fence_info = {
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &allocation,
      &context->command_buffer),
      "vkAllocateCommandBuffers") != SOX_SUCCESS ||
      vk_result(vkCreateFence(
      context->vulkan->device, &fence_info, NULL,
      &context->fence), "vkCreateFence") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

int lsx_fir_vulkan_fuse_reference_coefficients(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_sets,
    size_t const *tap_counts, size_t set_count,
    double **result_highs, double **result_lows,
    size_t *result_count)
{
  lsx_fir_vulkan_t scratch;
  lsx_vulkan_fft_t *fft = NULL;
  buffer_t accumulated = {0};
  buffer_t download = {0};
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayoutBinding bindings[2];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
    0, 1, 1, &pool_size
  };
  VkDescriptorSetAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)
  };
  VkPipelineLayoutCreateInfo pipeline_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
  };
  VkDescriptorBufferInfo buffer_infos[2];
  VkWriteDescriptorSet writes[2];
  VkBufferCopy full_copy;
  VkBufferCopy output_copy;
  VkDeviceSize working_size;
  VkDeviceSize output_size;
  size_t combined_count = 1u;
  size_t transform_count;
  uint32_t fft_size = 1u;
  uint32_t oversampling;
  uint32_t spectrum_bins;
  size_t set_index;
  int status = SOX_EOF;

  if (!vulkan || !coefficient_sets || !tap_counts || !set_count ||
      !result_highs || !result_lows || !result_count ||
      vulkan->profile != sox_vulkan_profile_reference ||
      !vulkan->use_float64)
    return SOX_EOF;
  *result_highs = NULL;
  *result_lows = NULL;
  *result_count = 0;
  for (set_index = 0; set_index < set_count; ++set_index) {
    if (!coefficient_sets[set_index] || !tap_counts[set_index] ||
        tap_counts[set_index] - 1u > SIZE_MAX - combined_count)
      return SOX_EOF;
    combined_count += tap_counts[set_index] - 1u;
  }
  if (combined_count > UINT32_MAX)
    return SOX_EOF;
  /* combined_count is already the exact length of the composite response, so
   * a transform that merely reaches it is free of circular aliasing and any
   * factor above one buys resolution the linear convolution does not need --
   * while costing memory linearly, which is what puts the deepest chains over
   * the device budget. The factor stays configurable because the earlier
   * sweep that chose sixteen was run while VkFFT was returning empty low
   * words, and measured nothing about the double-double route. */
  {
    char const *selector =
        getenv("SOX_VULKAN_REFERENCE_FUSION_OVERSAMPLING");
    unsigned long requested =
        selector && selector[0] ? strtoul(selector, NULL, 10) : 16ul;

    oversampling = requested >= 1ul && requested <= 64ul ?
        (uint32_t)requested : 16u;
  }
  if (combined_count > UINT32_MAX / oversampling)
    return SOX_EOF;
  transform_count = combined_count * oversampling;
  while (fft_size < transform_count) {
    if (fft_size > UINT32_MAX / 2u)
      return SOX_EOF;
    fft_size *= 2u;
  }
  spectrum_bins = fft_size / 2u + 1u;
  working_size = (VkDeviceSize)(fft_size + 2u) *
      2u * sizeof(double);
  output_size = (VkDeviceSize)combined_count *
      2u * sizeof(double);
  memset(&scratch, 0, sizeof(scratch));
  scratch.vulkan = vulkan;
  if (create_commands(&scratch) != SOX_SUCCESS ||
      create_buffer(
      &scratch, &scratch.working, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      create_buffer(
      &scratch, &accumulated, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      create_buffer(
      &scratch, &scratch.upload, working_size,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS ||
      create_buffer(
      &scratch, &download, output_size,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != SOX_SUCCESS)
    goto cleanup;
  fft = lsx_vulkan_fft_create(
      vulkan, &scratch.working, fft_size, 1u,
      sox_true, sox_true, sox_true, sox_true,
      &scratch.fence);
  if (!fft)
    goto cleanup;
  memset(bindings, 0, sizeof(bindings));
  memset(writes, 0, sizeof(writes));
  bindings[0].binding = 0;
  bindings[1].binding = 1;
  bindings[0].descriptorType = bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = bindings[1].descriptorCount = 1;
  bindings[0].stageFlags = bindings[1].stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_info.bindingCount = 2;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      vulkan->device, &descriptor_info, NULL,
      &descriptor_layout),
      "vkCreateDescriptorSetLayout reference fusion") != SOX_SUCCESS ||
      vk_result(vkCreateDescriptorPool(
      vulkan->device, &pool_info, NULL,
      &descriptor_pool),
      "vkCreateDescriptorPool reference fusion") != SOX_SUCCESS)
    goto cleanup;
  allocation.descriptorPool = descriptor_pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &descriptor_layout;
  if (vk_result(vkAllocateDescriptorSets(
      vulkan->device, &allocation, &descriptor_set),
      "vkAllocateDescriptorSets reference fusion") != SOX_SUCCESS)
    goto cleanup;
  pipeline_info.setLayoutCount = 1;
  pipeline_info.pSetLayouts = &descriptor_layout;
  pipeline_info.pushConstantRangeCount = 1;
  pipeline_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(
      vulkan->device, &pipeline_info, NULL,
      &pipeline_layout),
      "vkCreatePipelineLayout reference fusion") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(
      vulkan, fir_spectrum_multiply_reference_dd_spv,
      sizeof(fir_spectrum_multiply_reference_dd_spv),
      pipeline_layout, &pipeline) != SOX_SUCCESS)
    goto cleanup;
  buffer_infos[0].buffer = scratch.working.buffer;
  buffer_infos[0].offset = 0;
  buffer_infos[0].range = working_size;
  buffer_infos[1].buffer = accumulated.buffer;
  buffer_infos[1].offset = 0;
  buffer_infos[1].range = working_size;
  for (set_index = 0; set_index < 2u; ++set_index) {
    writes[set_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[set_index].dstSet = descriptor_set;
    writes[set_index].dstBinding = (uint32_t)set_index;
    writes[set_index].descriptorCount = 1;
    writes[set_index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[set_index].pBufferInfo = &buffer_infos[set_index];
  }
  vkUpdateDescriptorSets(vulkan->device, 2, writes, 0, NULL);
  full_copy.srcOffset = 0;
  full_copy.dstOffset = 0;
  full_copy.size = working_size;
  for (set_index = 0; set_index < set_count; ++set_index) {
    double *upload = scratch.upload.mapped;
    size_t tap_index;

    memset(upload, 0, (size_t)working_size);
    for (tap_index = 0; tap_index < tap_counts[set_index]; ++tap_index)
      upload[2u * tap_index] =
          coefficient_sets[set_index][tap_index];
    if (begin_commands(&scratch) != SOX_SUCCESS)
      goto cleanup;
    vkCmdCopyBuffer(
        scratch.command_buffer, scratch.upload.buffer,
        scratch.working.buffer, 1, &full_copy);
    memory_barrier(
        scratch.command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (lsx_vulkan_fft_append(
        fft, scratch.command_buffer, sox_false) != SOX_SUCCESS)
      goto cleanup;
    if (!set_index) {
      memory_barrier(
          scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(
          scratch.command_buffer, scratch.working.buffer,
          accumulated.buffer, 1, &full_copy);
    }
    else {
      memory_barrier(
          scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      vkCmdBindPipeline(
          scratch.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
          pipeline);
      vkCmdBindDescriptorSets(
          scratch.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
          pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
      vkCmdPushConstants(
          scratch.command_buffer, pipeline_layout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0,
          sizeof(spectrum_bins), &spectrum_bins);
      vkCmdDispatch(
          scratch.command_buffer,
          (spectrum_bins + FIR_LOCAL_SIZE - 1u) /
          FIR_LOCAL_SIZE, 1, 1);
    }
    if (submit_commands(
        &scratch, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
      goto cleanup;
  }
  output_copy.srcOffset = 0;
  output_copy.dstOffset = 0;
  output_copy.size = output_size;
  if (begin_commands(&scratch) != SOX_SUCCESS)
    goto cleanup;
  memory_barrier(
      scratch.command_buffer,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBuffer(
      scratch.command_buffer, accumulated.buffer,
      scratch.working.buffer, 1, &full_copy);
  memory_barrier(
      scratch.command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  if (lsx_vulkan_fft_append(
      fft, scratch.command_buffer, sox_true) != SOX_SUCCESS)
    goto cleanup;
  memory_barrier(
      scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBuffer(
      scratch.command_buffer, scratch.working.buffer,
      download.buffer, 1, &output_copy);
  if (submit_commands(
      &scratch, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
    goto cleanup;
  {
    char const *raw_dump_path = getenv(
        "SOX_VULKAN_REFERENCE_FUSION_RAW_DUMP");

    if (raw_dump_path && raw_dump_path[0]) {
      FILE *raw_dump = fopen(raw_dump_path, "wb");
      size_t written = raw_dump ? fwrite(
          download.mapped, 1, (size_t)output_size,
          raw_dump) : 0;
      int close_result = raw_dump ? fclose(raw_dump) : EOF;

      if (!raw_dump || written != (size_t)output_size || close_result)
        lsx_warn("cannot write raw Vulkan reference fusion dump");
    }
  }
  *result_highs = lsx_malloc(
      combined_count * sizeof(**result_highs));
  *result_lows = lsx_malloc(
      combined_count * sizeof(**result_lows));
  for (set_index = 0; set_index < combined_count; ++set_index) {
    double const *pair =
        (double const *)download.mapped + 2u * set_index;

    (*result_highs)[set_index] = pair[0];
    (*result_lows)[set_index] = pair[1];
  }
  *result_count = combined_count;
  {
    char const *dump_path = getenv(
        "SOX_VULKAN_REFERENCE_FUSION_DUMP");

    if (dump_path && dump_path[0]) {
      FILE *dump = fopen(dump_path, "wb");
      size_t written = dump ? fwrite(
          *result_highs, sizeof(**result_highs),
          combined_count, dump) : 0;
      int close_result = dump ? fclose(dump) : EOF;

      if (!dump || written != combined_count || close_result)
        lsx_warn("cannot write Vulkan reference fusion dump");
    }
  }
  status = SOX_SUCCESS;
  lsx_report(
      "Vulkan REFERENCE spectral fusion: %lu filters, "
      "%lu taps, %u-point FP64x2 setup FFT",
      (unsigned long)set_count, (unsigned long)combined_count,
      fft_size);

cleanup:
  if (status != SOX_SUCCESS) {
    free(*result_highs);
    free(*result_lows);
    *result_highs = NULL;
    *result_lows = NULL;
    *result_count = 0;
  }
  if (vulkan && vulkan->device)
    vkDeviceWaitIdle(vulkan->device);
  if (pipeline)
    vkDestroyPipeline(vulkan->device, pipeline, NULL);
  if (pipeline_layout)
    vkDestroyPipelineLayout(vulkan->device, pipeline_layout, NULL);
  if (descriptor_pool)
    vkDestroyDescriptorPool(vulkan->device, descriptor_pool, NULL);
  if (descriptor_layout)
    vkDestroyDescriptorSetLayout(vulkan->device, descriptor_layout, NULL);
  lsx_vulkan_fft_destroy(fft);
  destroy_buffer(&scratch, &download);
  destroy_buffer(&scratch, &accumulated);
  destroy_buffer(&scratch, &scratch.upload);
  destroy_buffer(&scratch, &scratch.working);
  if (scratch.fence)
    vkDestroyFence(vulkan->device, scratch.fence, NULL);
  if (scratch.command_buffer)
    vkFreeCommandBuffers(
        vulkan->device, vulkan->command_pool, 1,
        &scratch.command_buffer);
  return status;
}

static int create_partition_pipeline(lsx_fir_vulkan_t *context)
{
  uint32_t binding_count =
      context->strict_fp32 ? 6u :
      context->accurate_fp32 ? 4u : 3u;
  VkDescriptorSetLayoutBinding bindings[6];
  VkDescriptorSetLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
    0, 1, 1, &pool_size
  };
  VkDescriptorSetAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorBufferInfo buffer_info[6] = {
    {context->working.buffer, 0, context->working.size},
    {
      context->strict_fp32 ?
      context->working_scratch.buffer : context->history.buffer,
      0,
      context->strict_fp32 ?
      context->working_scratch.size : context->history.size
    },
    {
      context->strict_fp32 ?
      context->history.buffer : context->kernels.buffer,
      0,
      context->strict_fp32 ?
      context->history.size : context->kernels.size
    },
    {
      context->strict_fp32 ?
      context->kernels.buffer : context->kernels_low.buffer,
      0,
      context->strict_fp32 ?
      context->kernels.size : context->kernels_low.size
    },
    {context->twiddles.buffer, 0, context->twiddles.size},
    {context->strict_output.buffer, 0, context->strict_output.size}
  };
  VkWriteDescriptorSet writes[6];
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT,
    0,
    context->strict_fp32 ?
    sizeof(strict_fp32_parameters_t) :
    sizeof(partition_parameters_t)
  };
  VkPipelineLayoutCreateInfo pipeline_layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    NULL, 0, 1, NULL, 1, &push_range
  };
  uint32_t index;

  memset(bindings, 0, sizeof(bindings));
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < binding_count; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  layout_info.bindingCount = binding_count;
  layout_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(
      context->vulkan->device, &layout_info, NULL,
      &context->descriptor_layout),
      "vkCreateDescriptorSetLayout") != SOX_SUCCESS ||
      vk_result(vkCreateDescriptorPool(
      context->vulkan->device, &pool_info, NULL,
      &context->descriptor_pool),
      "vkCreateDescriptorPool") != SOX_SUCCESS)
    return SOX_EOF;
  allocation.descriptorPool = context->descriptor_pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &context->descriptor_layout;
  if (vk_result(vkAllocateDescriptorSets(
      context->vulkan->device, &allocation,
      &context->descriptor_set),
      "vkAllocateDescriptorSets") != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < binding_count; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = context->descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(
      context->vulkan->device, binding_count, writes, 0, NULL);
  pipeline_layout_info.pSetLayouts = &context->descriptor_layout;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &pipeline_layout_info, NULL,
      &context->pipeline_layout),
      "vkCreatePipelineLayout") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(
      context->vulkan,
      context->double_precision ?
          (context->reference_dd ?
           fir_partition_reference_dd_spv :
           context->precise_fp64 ?
           fir_partition_precise_f64_spv :
           fir_partition_f64_spv) :
          (context->strict_fp32 ?
           fir_partition_strict_f32_spv :
           context->accurate_fp32 ?
           fir_partition_accurate_f32_spv :
           fir_partition_f32_spv),
      context->double_precision ?
          (context->reference_dd ?
           sizeof(fir_partition_reference_dd_spv) :
           context->precise_fp64 ?
           sizeof(fir_partition_precise_f64_spv) :
           sizeof(fir_partition_f64_spv)) :
          (context->strict_fp32 ?
           sizeof(fir_partition_strict_f32_spv) :
           context->accurate_fp32 ?
           sizeof(fir_partition_accurate_f32_spv) :
           sizeof(fir_partition_f32_spv)),
      context->pipeline_layout,
      &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int initialize_fft(lsx_fir_vulkan_t *context)
{
  /* strict on a device without shaderFloat64 is the one route that cannot
   * use VkFFT: the library's precision ladder is half, float, double and
   * double-double, with no FP32x2 mode, and doublePrecisionFloatMemory still
   * needs FP64 in the shader.  On Apple Silicon, where Metal reports
   * shaderFloat64 = false, VkFFT can therefore only offer the FP32 that
   * strict exists to beat, so the profile runs its own double-single
   * transform with precomputed split twiddles instead.  Every other route,
   * strict on an FP64 device included, goes through VkFFT. */
  if (context->strict_fp32)
    return SOX_SUCCESS;
  context->fft = lsx_vulkan_fft_create(
      context->vulkan, &context->working,
      FIR_FFT_SIZE, context->channels,
      context->double_precision,
      context->vulkan->profile ==
      sox_vulkan_profile_reference,
      sox_true, sox_true,
      &context->fence);
  return context->fft ? SOX_SUCCESS : SOX_EOF;
}

static void store_double_single(
    float *target, size_t index, double value)
{
  float high = (float)value;

  target[index] = high;
  target[index + 1u] =
      (float)(value - (double)high);
}

static int initialize_strict_fp32_twiddles(
    lsx_fir_vulkan_t *context)
{
  float *upload = context->upload.mapped;
  VkBufferCopy copy = {
    0, 0,
    (VkDeviceSize)(FIR_FFT_SIZE / 2u) *
    4u * sizeof(*upload)
  };
  uint32_t index;

  memset(upload, 0, (size_t)copy.size);
  for (index = 0; index < FIR_FFT_SIZE / 2u; ++index) {
    double angle =
        2.0 * acos(-1.0) * (double)index /
        (double)FIR_FFT_SIZE;

    store_double_single(
        upload, (size_t)index * 4u, cos(angle));
    store_double_single(
        upload, (size_t)index * 4u + 2u, sin(angle));
  }
  if (begin_commands(context) != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdCopyBuffer(
      context->command_buffer, context->upload.buffer,
      context->twiddles.buffer, 1, &copy);
  return submit_commands(
      context, lsx_vulkan_wait_fir_setup);
}

static int initialize_strict_fp32_kernels(
    lsx_fir_vulkan_t *context,
    double const *coefficients, size_t taps)
{
  double *spectrum = lsx_calloc(
      FIR_FFT_SIZE, sizeof(*spectrum));
  uint32_t partition;

  if (initialize_strict_fp32_twiddles(context) !=
      SOX_SUCCESS) {
    free(spectrum);
    return SOX_EOF;
  }
  for (partition = 0;
       partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min(
        (size_t)FIR_BLOCK_FRAMES, taps - first);
    float *upload = context->upload.mapped;
    VkBufferCopy copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };
    uint32_t channel;

    memset(
        spectrum, 0, FIR_FFT_SIZE * sizeof(*spectrum));
    memcpy(
        spectrum, coefficients + first,
        length * sizeof(*spectrum));
    lsx_safe_rdft(FIR_FFT_SIZE, 1, spectrum);
    memset(upload, 0, (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      uint32_t bin;

      for (bin = 0; bin < FIR_FFT_SIZE; ++bin) {
        uint32_t source_bin =
            bin <= FIR_FFT_SIZE / 2u ?
            bin : FIR_FFT_SIZE - bin;
        double real =
            source_bin == 0u ? spectrum[0] :
            source_bin == FIR_FFT_SIZE / 2u ?
            spectrum[1] : spectrum[2u * source_bin];
        double imaginary =
            source_bin == 0u ||
            source_bin == FIR_FFT_SIZE / 2u ?
            0. : -spectrum[2u * source_bin + 1u];
        size_t target =
            ((size_t)channel * FIR_FFT_SIZE + bin) * 4u;

        if (bin > FIR_FFT_SIZE / 2u)
          imaginary = -imaginary;
        store_double_single(upload, target, real);
        store_double_single(
            upload, target + 2u, imaginary);
      }
    }
    if (begin_commands(context) != SOX_SUCCESS) {
      free(spectrum);
      return SOX_EOF;
    }
    vkCmdCopyBuffer(
        context->command_buffer, context->upload.buffer,
        context->kernels.buffer, 1, &copy);
    if (submit_commands(
        context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS) {
      free(spectrum);
      return SOX_EOF;
    }
  }
  free(spectrum);
  return SOX_SUCCESS;
}

static int initialize_accurate_kernels(
    lsx_fir_vulkan_t *context,
    double const *coefficients, size_t taps)
{
  double *spectrum = lsx_calloc(
      FIR_FFT_SIZE, sizeof(*spectrum));
  uint32_t partition;

  for (partition = 0;
       partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min(
        (size_t)FIR_BLOCK_FRAMES, taps - first);
    uint32_t component;

    memset(
        spectrum, 0, FIR_FFT_SIZE * sizeof(*spectrum));
    memcpy(
        spectrum, coefficients + first,
        length * sizeof(*spectrum));
    lsx_safe_rdft(FIR_FFT_SIZE, 1, spectrum);
    for (component = 0; component < 2u; ++component) {
      buffer_t *target =
          component ? &context->kernels_low : &context->kernels;
      VkBufferCopy copy = {
        0,
        (VkDeviceSize)partition * context->working.size,
        context->working.size
      };
      uint32_t channel;

      memset(
          context->working_host, 0,
          (size_t)context->working.size);
      for (channel = 0; channel < context->channels; ++channel) {
        float *output = (float *)context->working_host +
            (size_t)channel * (FIR_FFT_SIZE + 2u);
        uint32_t bin;

        for (bin = 0; bin <= FIR_FFT_SIZE / 2u; ++bin) {
          double real = bin == 0 ? spectrum[0] :
              bin == FIR_FFT_SIZE / 2u ?
              spectrum[1] : spectrum[2u * bin];
          double imaginary =
              bin == 0 || bin == FIR_FFT_SIZE / 2u ?
              0. : -spectrum[2u * bin + 1u];
          float real_high = (float)real;
          float imaginary_high = (float)imaginary;

          output[2u * bin] = component ?
              (float)(real - (double)real_high) : real_high;
          output[2u * bin + 1u] = component ?
              (float)(imaginary - (double)imaginary_high) :
              imaginary_high;
        }
      }
      memcpy(
          context->upload.mapped, context->working_host,
          (size_t)context->working.size);
      if (begin_commands(context) != SOX_SUCCESS) {
        free(spectrum);
        return SOX_EOF;
      }
      vkCmdCopyBuffer(
          context->command_buffer, context->upload.buffer,
          target->buffer, 1, &copy);
      if (submit_commands(
          context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS) {
        free(spectrum);
        return SOX_EOF;
      }
    }
  }
  free(spectrum);
  return SOX_SUCCESS;
}

static int initialize_precise_f64_kernels(
    lsx_fir_vulkan_t *context,
    double const *coefficients, size_t taps)
{
  double *spectrum = lsx_calloc(
      FIR_FFT_SIZE, sizeof(*spectrum));
  uint32_t partition;

  for (partition = 0;
       partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min(
        (size_t)FIR_BLOCK_FRAMES, taps - first);
    VkBufferCopy copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };
    uint32_t channel;

    memset(
        spectrum, 0, FIR_FFT_SIZE * sizeof(*spectrum));
    memcpy(
        spectrum, coefficients + first,
        length * sizeof(*spectrum));
    lsx_safe_rdft(FIR_FFT_SIZE, 1, spectrum);
    memset(
        context->working_host, 0,
        (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      double *output = (double *)context->working_host +
          (size_t)channel * (FIR_FFT_SIZE + 2u);
      uint32_t bin;

      for (bin = 0; bin <= FIR_FFT_SIZE / 2u; ++bin) {
        output[2u * bin] = bin == 0 ? spectrum[0] :
            bin == FIR_FFT_SIZE / 2u ?
            spectrum[1] : spectrum[2u * bin];
        output[2u * bin + 1u] =
            bin == 0 || bin == FIR_FFT_SIZE / 2u ?
            0. : -spectrum[2u * bin + 1u];
      }
    }
    memcpy(
        context->upload.mapped, context->working_host,
        (size_t)context->working.size);
    if (begin_commands(context) != SOX_SUCCESS) {
      free(spectrum);
      return SOX_EOF;
    }
    vkCmdCopyBuffer(
        context->command_buffer, context->upload.buffer,
        context->kernels.buffer, 1, &copy);
    if (submit_commands(
        context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS) {
      free(spectrum);
      return SOX_EOF;
    }
  }
  free(spectrum);
  return SOX_SUCCESS;
}

static int initialize_kernels(
    lsx_fir_vulkan_t *context,
    double const *coefficients,
    double const *coefficient_lows, size_t taps)
{
  VkBufferCopy upload_copy = {
    0, 0, context->working.size
  };
  uint32_t partition;
  uint32_t channel;
  uint32_t component;

  if (context->strict_fp32)
    return initialize_strict_fp32_kernels(
        context, coefficients, taps);
  if (context->accurate_fp32)
    return initialize_accurate_kernels(
        context, coefficients, taps);
  if (context->authoritative_fp64_kernels)
    return initialize_precise_f64_kernels(
        context, coefficients, taps);
  for (partition = 0;
       partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min(
        (size_t)FIR_BLOCK_FRAMES, taps - first);
    uint32_t component_count = context->accurate_fp32 ? 2u : 1u;

    for (component = 0; component < component_count; ++component) {
      VkBufferCopy kernel_copy = {
        0,
        (VkDeviceSize)partition * context->working.size,
        context->working.size
      };
      buffer_t *target =
          component ? &context->kernels_low : &context->kernels;

      memset(
          context->working_host, 0,
          (size_t)context->working.size);
      for (channel = 0; channel < context->channels; ++channel) {
        size_t index;

        for (index = 0; index < length; ++index) {
          double coefficient = coefficients[first + index];
          size_t target_index =
              (size_t)channel * (FIR_FFT_SIZE + 2u) + index;

          if (context->reference_dd) {
            ((double *)context->working_host)[
                2u * target_index] = coefficient;
            ((double *)context->working_host)[
                2u * target_index + 1u] =
                coefficient_lows ?
                coefficient_lows[first + index] : 0.;
          }
          else if (context->double_precision)
            ((double *)context->working_host)[
                target_index] = coefficient;
          else if (!component)
            ((float *)context->working_host)[target_index] =
                (float)coefficient;
          else
            ((float *)context->working_host)[target_index] =
                (float)(coefficient - (double)(float)coefficient);
        }
      }
      memcpy(
          context->upload.mapped, context->working_host,
          (size_t)context->working.size);
      if (begin_commands(context) != SOX_SUCCESS)
        return SOX_EOF;
      vkCmdCopyBuffer(
          context->command_buffer, context->upload.buffer,
          context->working.buffer, 1, &upload_copy);
      memory_barrier(
          context->command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      if (lsx_vulkan_fft_append(
          context->fft, context->command_buffer,
          sox_false) != SOX_SUCCESS) {
        lsx_fail("VkFFT FIR kernel transform failed");
        return SOX_EOF;
      }
      memory_barrier(
          context->command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(
          context->command_buffer, context->working.buffer,
          target->buffer, 1, &kernel_copy);
      if (submit_commands(
          context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
        return SOX_EOF;
    }
  }
  return SOX_SUCCESS;
}

static int clear_history(lsx_fir_vulkan_t *context)
{
  if (begin_commands(context) != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdFillBuffer(
      context->command_buffer, context->history.buffer,
      0, context->history.size, 0);
  return submit_commands(context, lsx_vulkan_wait_fir_setup);
}

static void append_partition_accumulation(
    lsx_fir_vulkan_t *context,
    VkCommandBuffer command_buffer, uint32_t current_slot)
{
  uint32_t spectrum_bins = FIR_FFT_SIZE / 2u + 1u;
  partition_parameters_t parameters = {
    spectrum_bins, context->partitions,
    context->channels, current_slot
  };
  uint32_t count = context->channels * spectrum_bins;

  vkCmdBindPipeline(
      command_buffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline);
  vkCmdBindDescriptorSets(
      command_buffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline_layout, 0, 1,
      &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(
      command_buffer, context->pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(parameters), &parameters);
  vkCmdDispatch(
      command_buffer,
      count / FIR_LOCAL_SIZE +
      (count % FIR_LOCAL_SIZE != 0), 1, 1);
}

static void append_strict_fp32_dispatch(
    lsx_fir_vulkan_t *context,
    VkCommandBuffer command_buffer,
    strict_fp32_parameters_t const *parameters,
    uint32_t count)
{
  vkCmdBindPipeline(
      command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline);
  vkCmdBindDescriptorSets(
      command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline_layout, 0, 1,
      &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(
      command_buffer, context->pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(*parameters), parameters);
  vkCmdDispatch(
      command_buffer,
      count / FIR_LOCAL_SIZE +
      (count % FIR_LOCAL_SIZE != 0), 1, 1);
}

static int record_strict_fp32_command_bank(
    lsx_fir_vulkan_t *context, VkCommandBuffer **commands,
    sox_bool download_output, uint32_t bank_depth)
{
  VkCommandBufferAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
    context->vulkan->command_pool,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    context->partitions * bank_depth
  };
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
  };
  VkDeviceSize channel_spectrum_size =
      (VkDeviceSize)FIR_FFT_SIZE *
      4u * sizeof(float);
  VkCommandBuffer *process_commands = lsx_calloc(
      context->partitions * bank_depth,
      sizeof(*process_commands));
  uint32_t command_index;

  *commands = NULL;
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &allocation,
      process_commands),
      "vkAllocateCommandBuffers FIR strict FP32 process") !=
      SOX_SUCCESS) {
    free(process_commands);
    return SOX_EOF;
  }
  for (command_index = 0;
       command_index < context->partitions * bank_depth;
       ++command_index) {
    uint32_t current_slot =
        command_index % context->partitions;
    uint32_t bank_index =
        command_index / context->partitions;
    VkCommandBuffer command_buffer =
        process_commands[command_index];
    VkBuffer upload_buffer = download_output ?
        context->upload.buffer :
        context->resident_upload[bank_index].buffer;
    VkBufferCopy upload_copy = {
      0, 0, context->working.size
    };
    strict_fp32_parameters_t parameters = {
      0u, 0u, context->partitions,
      context->channels, current_slot,
      0u, 1u, 0u
    };
    uint32_t stage;
    uint32_t channel;

    if (vk_result(vkBeginCommandBuffer(
        command_buffer, &begin),
        "vkBeginCommandBuffer FIR strict FP32 process") !=
        SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_begin(
        context->vulkan, command_buffer,
        download_output ?
        "FIR strict FP32 process and download" :
        "FIR strict FP32 resident process");
    vkCmdCopyBuffer(
        command_buffer, upload_buffer,
        context->working.buffer, 1, &upload_copy);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(
        context->vulkan, command_buffer,
        "FIR strict FP32 forward double-single FFT");
    for (stage = 0; stage < 15u; ++stage) {
      parameters.operation = 0u;
      parameters.stage = stage;
      parameters.inverse = 0u;
      parameters.source_is_a =
          stage % 2u == 0u;
      append_strict_fp32_dispatch(
          context, command_buffer, &parameters,
          context->channels * FIR_FFT_SIZE / 2u);
      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (channel = 0;
         channel < context->channels; ++channel) {
      VkBufferCopy history_copy = {
        (VkDeviceSize)channel * channel_spectrum_size,
        ((VkDeviceSize)channel * context->partitions +
         current_slot) * channel_spectrum_size,
        channel_spectrum_size
      };

      vkCmdCopyBuffer(
          command_buffer, context->working_scratch.buffer,
          context->history.buffer, 1, &history_copy);
    }
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    parameters.operation = 1u;
    parameters.inverse = 0u;
    parameters.source_is_a = 0u;
    append_strict_fp32_dispatch(
        context, command_buffer, &parameters,
        context->channels * FIR_FFT_SIZE);
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(
        context->vulkan, command_buffer,
        "FIR strict FP32 inverse double-single FFT");
    for (stage = 0; stage < 15u; ++stage) {
      parameters.operation = 0u;
      parameters.stage = stage;
      parameters.inverse = 1u;
      parameters.source_is_a =
          stage % 2u == 0u;
      append_strict_fp32_dispatch(
          context, command_buffer, &parameters,
          context->channels * FIR_FFT_SIZE / 2u);
      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    parameters.operation = 2u;
    parameters.inverse = 0u;
    parameters.source_is_a = 0u;
    append_strict_fp32_dispatch(
        context, command_buffer, &parameters,
        context->channels * FIR_BLOCK_FRAMES);
    if (download_output) {
      VkBufferCopy output_copy = {
        0, 0, context->strict_output.size
      };

      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(
          command_buffer, context->strict_output.buffer,
          context->download.buffer, 1, &output_copy);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (vk_result(vkEndCommandBuffer(command_buffer),
        "vkEndCommandBuffer FIR strict FP32 process") !=
        SOX_SUCCESS)
      goto error;
  }
  *commands = process_commands;
  return SOX_SUCCESS;

error:
  vkFreeCommandBuffers(
      context->vulkan->device, context->vulkan->command_pool,
      context->partitions * bank_depth, process_commands);
  free(process_commands);
  return SOX_EOF;
}

static int record_process_command_bank(lsx_fir_vulkan_t *context, VkCommandBuffer **commands, sox_bool download_output, uint32_t bank_depth)
{
  VkDeviceSize complex_size = 2u * context->element_size;
  VkDeviceSize spectrum_size =
      (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize real_stride =
      (VkDeviceSize)(FIR_FFT_SIZE + 2u) *
      context->element_size;
  VkDeviceSize block_size =
      (VkDeviceSize)FIR_BLOCK_FRAMES *
      context->element_size;
  VkBufferCopy upload_copy = {
    0, 0, context->working.size
  };
  VkCommandBufferAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
    context->vulkan->command_pool,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY, context->partitions * bank_depth
  };
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
  };
  uint32_t command_index;

  if (context->strict_fp32)
    return record_strict_fp32_command_bank(
        context, commands, download_output, bank_depth);
  VkCommandBuffer *process_commands = lsx_calloc(context->partitions * bank_depth, sizeof(*process_commands));

  *commands = NULL;
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &allocation,
      process_commands),
      "vkAllocateCommandBuffers FIR process") != SOX_SUCCESS) {
    free(process_commands);
    return SOX_EOF;
  }

  for (command_index = 0; command_index < context->partitions * bank_depth; ++command_index) {
    uint32_t current_slot = command_index % context->partitions;
    uint32_t bank_index = command_index / context->partitions;
    VkCommandBuffer command_buffer = process_commands[command_index];
    VkBuffer upload_buffer = download_output ? context->upload.buffer : context->resident_upload[bank_index].buffer;
    uint32_t channel;

    if (vk_result(vkBeginCommandBuffer(
        command_buffer, &begin),
        "vkBeginCommandBuffer FIR process") != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_begin(context->vulkan, command_buffer, download_output ? "FIR process and download" : "FIR resident process");
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR input upload");
    vkCmdCopyBuffer(
        command_buffer, upload_buffer,
        context->working.buffer, 1, &upload_copy);
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR forward FFT");
    if (lsx_vulkan_fft_append(
        context->fft, command_buffer,
        sox_false) != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR history update");
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (channel = 0; channel < context->channels; ++channel) {
      VkBufferCopy history_copy = {
        (VkDeviceSize)channel * spectrum_size,
        ((VkDeviceSize)channel * context->partitions +
         current_slot) * spectrum_size,
        spectrum_size
      };

      vkCmdCopyBuffer(
          command_buffer, context->working.buffer,
          context->history.buffer, 1, &history_copy);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR partition accumulation");
    append_partition_accumulation(context, command_buffer, current_slot);
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    /* Diagnostic tap: copies the spectrum the partition shader just wrote,
     * before the inverse transform runs, so that a capture can tell whether
     * the low half of each pair is already missing here or is lost later. */
    if (context->emit_low_residual >= 3 && download_output) {
      VkBufferCopy spectrum_copy = { 0, 0, block_size };

      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(
          command_buffer, context->working.buffer,
          context->download.buffer, 1, &spectrum_copy);
    }
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR inverse FFT");
    if (lsx_vulkan_fft_append(
        context->fft, command_buffer,
        sox_true) != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (download_output && context->emit_low_residual < 3) {
      lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR output download");
      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      for (channel = 0; channel < context->channels; ++channel) {
        VkBufferCopy output_copy = {
          (VkDeviceSize)channel * real_stride + block_size,
          (VkDeviceSize)channel * block_size,
          block_size
        };

        vkCmdCopyBuffer(
            command_buffer, context->working.buffer,
            context->download.buffer, 1, &output_copy);
      }
      lsx_vulkan_label_end(context->vulkan, command_buffer);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (vk_result(vkEndCommandBuffer(command_buffer),
        "vkEndCommandBuffer FIR process") != SOX_SUCCESS)
      goto error;
  }
  *commands = process_commands;
  return SOX_SUCCESS;

error:
  vkFreeCommandBuffers(
      context->vulkan->device, context->vulkan->command_pool,
      context->partitions * bank_depth, process_commands);
  free(process_commands);
  return SOX_EOF;
}

static int record_process_commands(lsx_fir_vulkan_t *context)
{
  return record_process_command_bank(context, &context->process_commands, sox_true, 1u);
}

static int create_buffers(lsx_fir_vulkan_t *context)
{
  VkDeviceSize complex_size = 2u * context->element_size;
  VkDeviceSize spectrum_size =
      (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize working_size =
      context->channels * spectrum_size;
  VkDeviceSize bank_size;
  VkDeviceSize download_size =
      (VkDeviceSize)context->channels *
      FIR_BLOCK_FRAMES * context->element_size;
  VkPhysicalDeviceLimits const *limits =
      &context->vulkan->properties.limits;

  if (context->strict_fp32) {
    working_size =
        (VkDeviceSize)context->channels *
        FIR_FFT_SIZE * 4u * sizeof(float);
    download_size =
        (VkDeviceSize)context->channels *
        FIR_BLOCK_FRAMES * 2u * sizeof(float);
  }
  if (context->partitions > UINT64_MAX / working_size) {
    lsx_fail("Vulkan FIR buffer size overflow");
    return SOX_EOF;
  }
  bank_size = working_size * context->partitions;
  if (bank_size > limits->maxStorageBufferRange) {
    lsx_fail(
        "Vulkan FIR requires %" PRIu64
        " bytes per coefficient/history bank, device limit is %u",
        (uint64_t)bank_size, limits->maxStorageBufferRange);
    return SOX_EOF;
  }
  if (create_buffer(
      context, &context->working, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      create_buffer(
      context, &context->history, bank_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      create_buffer(
      context, &context->kernels, bank_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      create_buffer(
      context, &context->upload, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS ||
      create_buffer(
      context, &context->download, download_size,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->strict_fp32 &&
      (create_buffer(
       context, &context->working_scratch, working_size,
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
       create_buffer(
       context, &context->twiddles,
       (VkDeviceSize)(FIR_FFT_SIZE / 2u) *
       4u * sizeof(float),
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
       create_buffer(
       context, &context->strict_output, download_size,
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS))
    return SOX_EOF;
  if (context->accurate_fp32 &&
      create_buffer(
      context, &context->kernels_low, bank_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  {
    uint32_t index;
    for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
      if (create_buffer(context, &context->resident_upload[index], working_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
        return SOX_EOF;
  }
  context->working_host =
      lsx_calloc(1, (size_t)working_size);
  context->previous = lsx_calloc(
      (size_t)context->channels * FIR_BLOCK_FRAMES,
      sizeof(*context->previous));
  context->output = lsx_calloc(
      (size_t)context->channels * FIR_BLOCK_FRAMES,
      sizeof(*context->output));
  return SOX_SUCCESS;
}

static lsx_fir_vulkan_t *create_fir(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    double const *coefficient_lows,
    size_t taps, uint32_t channels)
{
  lsx_fir_vulkan_t *context;
  double started = monotonic_seconds();

  if (!vulkan || !coefficients || !taps || !channels ||
      taps > UINT32_MAX - FIR_FAST_BLOCK_FRAMES) {
    lsx_fail("invalid Vulkan FIR configuration");
    return NULL;
  }
  if (!vulkan->shader_float64 &&
      vulkan->profile != sox_vulkan_profile_fast &&
      vulkan->profile != sox_vulkan_profile_accurate &&
      vulkan->profile != sox_vulkan_profile_strict) {
    lsx_fail(
        "Vulkan FIR profile %s is not implemented for the FP32 "
        "emulated numerical family",
        lsx_vulkan_profile_name(vulkan->profile));
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->fft_size =
      vulkan->profile == sox_vulkan_profile_fast ?
      FIR_FAST_FFT_SIZE : FIR_DEFAULT_FFT_SIZE;
  context->block_frames = context->fft_size / 2u;
  context->double_precision = vulkan->use_float64;
  context->strict_fp32 =
      !context->double_precision &&
      vulkan->profile == sox_vulkan_profile_strict;
  context->accurate_fp32 =
      !context->double_precision &&
      vulkan->profile == sox_vulkan_profile_accurate &&
      !getenv("SOX_VULKAN_PLAIN_FP32_PARTITION");
  context->precise_fp64 =
      context->double_precision &&
      (vulkan->profile == sox_vulkan_profile_strict ||
       vulkan->profile == sox_vulkan_profile_reference);
  context->reference_dd =
      context->double_precision &&
      vulkan->profile == sox_vulkan_profile_reference;
  {
    char const *residual = getenv("SOX_VULKAN_REFERENCE_LOW_RESIDUAL");

    context->emit_low_residual =
        context->reference_dd && residual && residual[0] ?
        atoi(residual) : 0;
  }
  context->authoritative_fp64_kernels =
      context->double_precision &&
      (vulkan->profile == sox_vulkan_profile_accurate ||
       vulkan->profile == sox_vulkan_profile_strict);
  context->taps = (uint32_t)taps;
  context->element_size = context->reference_dd ?
      2u * sizeof(double) :
      context->double_precision ? sizeof(double) : sizeof(float);
  context->channels = channels;
  context->partitions = (uint32_t)(
      (taps + FIR_BLOCK_FRAMES - 1u) /
      FIR_BLOCK_FRAMES);
  if (create_commands(context) != SOX_SUCCESS ||
      create_buffers(context) != SOX_SUCCESS ||
      initialize_fft(context) != SOX_SUCCESS ||
      create_partition_pipeline(context) != SOX_SUCCESS ||
      initialize_kernels(
          context, coefficients, coefficient_lows,
          taps) != SOX_SUCCESS ||
      clear_history(context) != SOX_SUCCESS ||
      record_process_commands(context) != SOX_SUCCESS)
    goto error;
  context->startup_seconds = monotonic_seconds() - started;
  lsx_report(
      "Vulkan FIR: %zu taps, %u partitions, %u channels, "
      "%.3f MiB kernels, %.3f MiB history, startup %.6f seconds, "
      "precision %s",
      taps, context->partitions, channels,
      (double)context->kernels.size / (1024.0 * 1024.0),
      (double)context->history.size / (1024.0 * 1024.0),
      context->startup_seconds,
      context->reference_dd ? "FP64x2" :
      context->double_precision ? "FP64" : "FP32");
  lsx_report(
      "Vulkan FIR strategy: %s",
      context->strict_fp32 ?
      "double-single FFT + split twiddles + double-single accumulation" :
      context->accurate_fp32 ?
      "FP32 FFT + split coefficients + compensated accumulation" :
      context->reference_dd ?
      "FP64 double-double FFT + double-double memory/product/accumulation" :
      context->authoritative_fp64_kernels &&
      context->precise_fp64 ?
      "FP64 FFT + authoritative coefficient spectra + compensated "
      "FP64 accumulation" :
      context->authoritative_fp64_kernels ?
      "FP64 FFT + authoritative coefficient spectra + FP64 accumulation" :
      context->precise_fp64 ?
      "FP64 FFT + compensated FP64 accumulation" :
      context->double_precision ?
      "FP64 FFT + FP64 accumulation" :
      "FP32 FFT + FP32 accumulation");
  return context;

error:
  lsx_fir_vulkan_destroy(context);
  return NULL;
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    size_t taps, uint32_t channels)
{
  return create_fir(
      vulkan, coefficients, NULL, taps, channels);
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows,
    size_t taps, uint32_t channels)
{
  if (!coefficient_lows || !vulkan ||
      vulkan->profile != sox_vulkan_profile_reference)
    return NULL;
  return create_fir(
      vulkan, coefficient_highs, coefficient_lows,
      taps, channels);
}

void lsx_fir_vulkan_destroy(lsx_fir_vulkan_t *context)
{
  if (!context)
    return;
  if (context->vulkan && context->vulkan->device)
    vkDeviceWaitIdle(context->vulkan->device);
  if (context->process_calls)
    lsx_report(
        "Vulkan FIR processed %" PRIu64
        " blocks in %.6f seconds (%.6f ms/block)",
        context->process_calls, context->process_seconds,
        context->process_seconds * 1000.0 /
        context->process_calls);
  if (context->process_commands) {
    vkFreeCommandBuffers(
        context->vulkan->device,
        context->vulkan->command_pool,
        context->partitions, context->process_commands);
    free(context->process_commands);
  }
  if (context->resident_process_commands) {
    vkFreeCommandBuffers(
        context->vulkan->device,
        context->vulkan->command_pool,
        context->partitions * LSX_VULKAN_RESIDENT_BATCH_DEPTH,
        context->resident_process_commands);
    free(context->resident_process_commands);
  }
  {
    uint32_t index;
    for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
      destroy_buffer(context, &context->resident_upload[index]);
  }
  if (context->command_buffer)
    vkFreeCommandBuffers(
        context->vulkan->device,
        context->vulkan->command_pool, 1,
        &context->command_buffer);
  if (context->pipeline)
    vkDestroyPipeline(
        context->vulkan->device, context->pipeline, NULL);
  if (context->pipeline_layout)
    vkDestroyPipelineLayout(
        context->vulkan->device,
        context->pipeline_layout, NULL);
  if (context->descriptor_pool)
    vkDestroyDescriptorPool(
        context->vulkan->device,
        context->descriptor_pool, NULL);
  if (context->descriptor_layout)
    vkDestroyDescriptorSetLayout(
        context->vulkan->device,
        context->descriptor_layout, NULL);
  lsx_vulkan_fft_destroy(context->fft);
  if (context->fence)
    vkDestroyFence(
        context->vulkan->device, context->fence, NULL);
  destroy_buffer(context, &context->download);
  destroy_buffer(context, &context->upload);
  destroy_buffer(context, &context->strict_output);
  destroy_buffer(context, &context->twiddles);
  destroy_buffer(context, &context->kernels);
  destroy_buffer(context, &context->kernels_low);
  destroy_buffer(context, &context->history);
  destroy_buffer(context, &context->working_scratch);
  destroy_buffer(context, &context->working);
  free(context->output);
  free(context->previous);
  free(context->working_host);
  free(context);
}

size_t lsx_fir_vulkan_block_frames(void)
{
  return FIR_DEFAULT_BLOCK_FRAMES;
}

size_t lsx_fir_vulkan_block_frames_for(
    lsx_vulkan_context_t const *context)
{
  return context &&
      context->profile == sox_vulkan_profile_fast ?
      FIR_FAST_BLOCK_FRAMES :
      FIR_DEFAULT_BLOCK_FRAMES;
}

size_t lsx_fir_vulkan_prepared_stride(
    lsx_fir_vulkan_t const *context)
{
  return context && context->strict_fp32 ?
      FIR_FFT_SIZE : FIR_FFT_SIZE + 2u;
}

lsx_vulkan_buffer_t *lsx_fir_vulkan_prepared_input_buffer(lsx_fir_vulkan_t *context)
{
  return context ? &context->resident_upload[context->resident_bank_index] : NULL;
}

static uint32_t reverse_fft_index(uint32_t value)
{
  uint32_t reversed = 0u;
  uint32_t bit;

  for (bit = 0u; bit < 15u; ++bit) {
    reversed = (reversed << 1u) | (value & 1u);
    value >>= 1u;
  }
  return reversed;
}

static void prepare_process_input(lsx_fir_vulkan_t *context, double const *input, buffer_t *upload)
{
  uint32_t channel;
  size_t frame;

  memset(
      context->working_host, 0,
      (size_t)context->working.size);
  for (channel = 0; channel < context->channels; ++channel)
    for (frame = 0; frame < FIR_BLOCK_FRAMES; ++frame) {
      double value =
          input[frame * context->channels + channel];
      size_t previous_index =
          (size_t)channel * FIR_BLOCK_FRAMES + frame;
      size_t first_index =
          (size_t)channel * (FIR_FFT_SIZE + 2u) + frame;
      size_t second_index = first_index + FIR_BLOCK_FRAMES;

      if (context->strict_fp32) {
        float *prepared = context->working_host;
        uint32_t first_reversed =
            reverse_fft_index((uint32_t)frame);
        uint32_t second_reversed =
            reverse_fft_index(
                (uint32_t)frame + FIR_BLOCK_FRAMES);
        size_t channel_base =
            (size_t)channel * FIR_FFT_SIZE;

        store_double_single(
            prepared,
            (channel_base + first_reversed) * 4u,
            context->previous[previous_index]);
        store_double_single(
            prepared,
            (channel_base + second_reversed) * 4u,
            value);
      }
      else if (context->reference_dd) {
        double *prepared = context->working_host;

        prepared[2u * first_index] =
            context->previous[previous_index];
        prepared[2u * first_index + 1u] = 0.;
        prepared[2u * second_index] = value;
        prepared[2u * second_index + 1u] = 0.;
      }
      else if (context->double_precision) {
        ((double *)context->working_host)[first_index] =
            context->previous[previous_index];
        ((double *)context->working_host)[second_index] = value;
      }
      else {
        float previous_high =
            (float)context->previous[previous_index];
        float value_high = (float)value;

        ((float *)context->working_host)[first_index] =
            previous_high;
        ((float *)context->working_host)[second_index] =
            value_high;
      }
      context->previous[
          previous_index] = value;
    }
  memcpy(
      upload->mapped, context->working_host,
      (size_t)context->working.size);
}

int lsx_fir_vulkan_process(
    lsx_fir_vulkan_t *context, double const *input,
    double const **output)
{
  double const *download;
  double started = monotonic_seconds();
  uint32_t channel;
  size_t frame;

  if (!context || !input || !output)
    return SOX_EOF;
  prepare_process_input(context, input, &context->upload);
  if (lsx_vulkan_submit_and_wait(
      context->vulkan,
      context->process_commands[context->current_slot],
      context->fence, lsx_vulkan_wait_fir_synchronous) != SOX_SUCCESS)
    return SOX_EOF;
  download = context->download.mapped;
  for (frame = 0; frame < FIR_BLOCK_FRAMES; ++frame)
    for (channel = 0; channel < context->channels; ++channel) {
      size_t index =
          (size_t)channel * FIR_BLOCK_FRAMES + frame;

      if (context->strict_fp32) {
        float const *double_single =
            (float const *)download + index * 2u;

        context->output[
            frame * context->channels + channel] =
            (double)double_single[0] +
            (double)double_single[1];
      }
      else if (context->reference_dd) {
        double const *double_double =
            (double const *)download + index * 2u;

        /* Collapsing the pair costs the very precision this profile
         * exists to provide, and a single double cannot carry it to the
         * host. lsx_vulkan_collapse_pair() is shared with the resident
         * download so that both paths answer the same diagnostic variable
         * and a second, deterministic run recovers the full pair offline
         * as sum + residual. */
        context->output[
            frame * context->channels + channel] =
            lsx_vulkan_collapse_pair(
                double_double[0], double_double[1]);
      }
      else
        context->output[frame * context->channels + channel] =
            context->double_precision ?
            download[index] : ((float const *)download)[index];
    }
  context->current_slot =
      (context->current_slot + 1u) % context->partitions;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  *output = context->output;
  return SOX_SUCCESS;
}

static int describe_resident_output(lsx_fir_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  memset(resident, 0, sizeof(*resident));
  resident->buffer =
      context->strict_fp32 ?
      &context->strict_output : &context->working;
  resident->owner = context;
  resident->offset =
      context->strict_fp32 ? 0 :
      (VkDeviceSize)FIR_BLOCK_FRAMES *
      context->element_size;
  resident->producer_stage =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = FIR_BLOCK_FRAMES;
  resident->valid_elements = FIR_BLOCK_FRAMES;
  resident->frame_stride_elements = 1u;
  resident->channel_stride_elements = FIR_FFT_SIZE + 2u;
  if (context->strict_fp32)
    resident->channel_stride_elements = FIR_BLOCK_FRAMES;
  resident->frame_offset = frame_offset;
  resident->rate = rate;
  resident->channels = context->channels;
  resident->frames_per_element = 1u;
  resident->format = context->strict_fp32 ?
      lsx_vulkan_resident_format_f32x2 :
      context->reference_dd ?
      lsx_vulkan_resident_format_f64x2 :
      context->double_precision ?
      lsx_vulkan_resident_format_f64 :
      lsx_vulkan_resident_format_f32;
  resident->domain = lsx_vulkan_resident_domain_sox_sample;
  resident->layout = lsx_vulkan_resident_layout_planar;
  resident->state = state;
  return lsx_vulkan_resident_buffer_validate(resident);
}

int lsx_fir_vulkan_process_prepared_resident(lsx_fir_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  double started = monotonic_seconds();

  if (!context || !resident || rate <= 0)
    return SOX_EOF;
  if (!context->resident_process_commands && record_process_command_bank(context, &context->resident_process_commands, sox_false, LSX_VULKAN_RESIDENT_BATCH_DEPTH) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_enqueue(context->vulkan, context->resident_process_commands[context->resident_bank_index * context->partitions + context->current_slot]) != SOX_SUCCESS)
    return SOX_EOF;
  if (describe_resident_output(context, rate, frame_offset, state, resident) != SOX_SUCCESS)
    return SOX_EOF;
  context->current_slot =
      (context->current_slot + 1u) % context->partitions;
  context->resident_bank_index = (context->resident_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  return SOX_SUCCESS;
}

int lsx_fir_vulkan_flush_resident(lsx_fir_vulkan_t *context)
{
  if (!context || begin_commands(context) != SOX_SUCCESS)
    return SOX_EOF;
  return submit_commands(context, lsx_vulkan_wait_fir_resident_flush);
}

int lsx_fir_vulkan_process_resident(lsx_fir_vulkan_t *context, double const *input, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  if (!context || !input)
    return SOX_EOF;
  prepare_process_input(context, input, &context->resident_upload[context->resident_bank_index]);
  return lsx_fir_vulkan_process_prepared_resident(context, rate, frame_offset, state, resident);
}
