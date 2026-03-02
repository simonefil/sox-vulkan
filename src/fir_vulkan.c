/* Partitioned VkFFT FIR backend for SoX.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "fir_vulkan.h"

#include <glslang/Include/glslang_c_interface.h>
#include <vkFFT.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "fir_partition_f64_spv.inc"

#define FIR_FFT_SIZE 32768u
#define FIR_BLOCK_FRAMES (FIR_FFT_SIZE / 2u)
#define FIR_LOCAL_SIZE 256u

typedef lsx_vulkan_buffer_t buffer_t;

typedef struct {
  uint32_t spectrum_bins;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
} partition_parameters_t;

lsx_static_assert(
    sizeof(partition_parameters_t) == 16,
    vulkan_fir_partition_push_layout);

struct lsx_fir_vulkan {
  lsx_vulkan_context_t *vulkan;
  VkFFTApplication fft;
  sox_bool fft_initialized;
  sox_bool compiler_acquired;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandBuffer command_buffer;
  VkCommandBuffer *process_commands;
  VkFence fence;
  buffer_t working;
  buffer_t history;
  buffer_t kernels;
  buffer_t upload;
  buffer_t download;
  double *working_host;
  double *previous;
  double *output;
  uint64_t configuration_buffer_size;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
  double startup_seconds;
  double process_seconds;
  uint64_t process_calls;
};

static unsigned compiler_users;

static double monotonic_seconds(void)
{
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter))
    return 0.0;
  return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static int compiler_acquire(void)
{
  if (!compiler_users && !glslang_initialize_process()) {
    lsx_fail("glslang initialization failed");
    return SOX_EOF;
  }
  ++compiler_users;
  return SOX_SUCCESS;
}

static void compiler_release(void)
{
  if (compiler_users && !--compiler_users)
    glslang_finalize_process();
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

static int submit_commands(lsx_fir_vulkan_t *context)
{
  if (vk_result(vkEndCommandBuffer(
      context->command_buffer),
      "vkEndCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  return lsx_vulkan_submit_and_wait(
      context->vulkan, context->command_buffer, context->fence);
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

static int create_partition_pipeline(lsx_fir_vulkan_t *context)
{
  VkDescriptorSetLayoutBinding bindings[3];
  VkDescriptorSetLayoutCreateInfo layout_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  };
  VkDescriptorPoolSize pool_size = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3
  };
  VkDescriptorPoolCreateInfo pool_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
    0, 1, 1, &pool_size
  };
  VkDescriptorSetAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  };
  VkDescriptorBufferInfo buffer_info[3] = {
    {context->working.buffer, 0, context->working.size},
    {context->history.buffer, 0, context->history.size},
    {context->kernels.buffer, 0, context->kernels.size}
  };
  VkWriteDescriptorSet writes[3];
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT,
    0, sizeof(partition_parameters_t)
  };
  VkPipelineLayoutCreateInfo pipeline_layout_info = {
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    NULL, 0, 1, NULL, 1, &push_range
  };
  uint32_t index;

  memset(bindings, 0, sizeof(bindings));
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < 3u; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  layout_info.bindingCount = 3;
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
  for (index = 0; index < 3u; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = context->descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(
      context->vulkan->device, 3, writes, 0, NULL);
  pipeline_layout_info.pSetLayouts = &context->descriptor_layout;
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &pipeline_layout_info, NULL,
      &context->pipeline_layout),
      "vkCreatePipelineLayout") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(
      context->vulkan, fir_partition_f64_spv,
      sizeof(fir_partition_f64_spv),
      context->pipeline_layout,
      &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int initialize_fft(lsx_fir_vulkan_t *context)
{
  VkFFTConfiguration configuration = VKFFT_ZERO_INIT;
  VkFFTResult result;

  configuration.FFTdim = 1;
  configuration.size[0] = FIR_FFT_SIZE;
  configuration.numberBatches = context->channels;
  configuration.doublePrecision = 1;
  configuration.performR2C = 1;
  configuration.normalize = 1;
  configuration.device = &context->vulkan->device;
  configuration.physicalDevice =
      &context->vulkan->physical_device;
  configuration.queue = &context->vulkan->queue;
  configuration.commandPool = &context->vulkan->command_pool;
  configuration.fence = &context->fence;
  configuration.buffer = &context->working.buffer;
  configuration.bufferSize =
      &context->configuration_buffer_size;
  configuration.isCompilerInitialized = 1;
  if (vk_result(vkResetFences(
      context->vulkan->device, 1, &context->fence),
      "vkResetFences before initializeVkFFT") != SOX_SUCCESS)
    return SOX_EOF;
  result = initializeVkFFT(&context->fft, configuration);
  if (result != VKFFT_SUCCESS) {
    lsx_fail("initializeVkFFT failed with result %d", (int)result);
    return SOX_EOF;
  }
  context->fft_initialized = sox_true;
  return SOX_SUCCESS;
}

static int initialize_kernels(
    lsx_fir_vulkan_t *context,
    double const *coefficients, size_t taps)
{
  VkFFTLaunchParams launch = VKFFT_ZERO_INIT;
  VkBufferCopy upload_copy = {
    0, 0, context->working.size
  };
  VkFFTResult fft_result;
  uint32_t partition;
  uint32_t channel;

  launch.commandBuffer = &context->command_buffer;
  for (partition = 0;
       partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min(
        (size_t)FIR_BLOCK_FRAMES, taps - first);
    VkBufferCopy kernel_copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };

    memset(
        context->working_host, 0,
        (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      size_t index;
      for (index = 0; index < length; ++index)
        context->working_host[
            (size_t)channel * (FIR_FFT_SIZE + 2u) + index] =
            coefficients[first + index];
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
    fft_result = VkFFTAppend(&context->fft, -1, &launch);
    if (fft_result != VKFFT_SUCCESS) {
      lsx_fail("VkFFT FIR kernel transform failed with result %d",
          (int)fft_result);
      return SOX_EOF;
    }
    memory_barrier(
        context->command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBuffer(
        context->command_buffer, context->working.buffer,
        context->kernels.buffer, 1, &kernel_copy);
    if (submit_commands(context) != SOX_SUCCESS)
      return SOX_EOF;
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
  return submit_commands(context);
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

static int record_process_commands(lsx_fir_vulkan_t *context)
{
  VkDeviceSize complex_size = 2u * sizeof(double);
  VkDeviceSize spectrum_size =
      (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize real_stride =
      (VkDeviceSize)(FIR_FFT_SIZE + 2u) * sizeof(double);
  VkDeviceSize block_size =
      (VkDeviceSize)FIR_BLOCK_FRAMES * sizeof(double);
  VkBufferCopy upload_copy = {
    0, 0, context->working.size
  };
  VkCommandBufferAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
    context->vulkan->command_pool,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY, context->partitions
  };
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
  };
  uint32_t current_slot;

  context->process_commands = lsx_calloc(
      context->partitions, sizeof(*context->process_commands));
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &allocation,
      context->process_commands),
      "vkAllocateCommandBuffers FIR process") != SOX_SUCCESS)
    return SOX_EOF;

  for (current_slot = 0;
       current_slot < context->partitions; ++current_slot) {
    VkCommandBuffer command_buffer =
        context->process_commands[current_slot];
    VkFFTLaunchParams launch = VKFFT_ZERO_INIT;
    VkFFTResult fft_result;
    uint32_t channel;

    if (vk_result(vkBeginCommandBuffer(
        command_buffer, &begin),
        "vkBeginCommandBuffer FIR process") != SOX_SUCCESS)
      return SOX_EOF;
    vkCmdCopyBuffer(
        command_buffer, context->upload.buffer,
        context->working.buffer, 1, &upload_copy);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    launch.commandBuffer = &command_buffer;
    fft_result = VkFFTAppend(&context->fft, -1, &launch);
    if (fft_result != VKFFT_SUCCESS) {
      lsx_fail(
          "VkFFT FIR forward command recording failed "
          "for slot %u with result %d",
          current_slot, (int)fft_result);
      return SOX_EOF;
    }
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
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    append_partition_accumulation(
        context, command_buffer, current_slot);
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    fft_result = VkFFTAppend(&context->fft, 1, &launch);
    if (fft_result != VKFFT_SUCCESS) {
      lsx_fail(
          "VkFFT FIR inverse command recording failed "
          "for slot %u with result %d",
          current_slot, (int)fft_result);
      return SOX_EOF;
    }
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
    if (vk_result(vkEndCommandBuffer(command_buffer),
        "vkEndCommandBuffer FIR process") != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int create_buffers(lsx_fir_vulkan_t *context)
{
  VkDeviceSize complex_size = 2u * sizeof(double);
  VkDeviceSize spectrum_size =
      (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize working_size =
      context->channels * spectrum_size;
  VkDeviceSize bank_size;
  VkDeviceSize download_size =
      (VkDeviceSize)context->channels *
      FIR_BLOCK_FRAMES * sizeof(double);
  VkPhysicalDeviceLimits const *limits =
      &context->vulkan->properties.limits;

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
  context->configuration_buffer_size = working_size;
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
  context->working_host =
      lsx_calloc((size_t)(working_size / sizeof(double)),
      sizeof(*context->working_host));
  context->previous = lsx_calloc(
      (size_t)context->channels * FIR_BLOCK_FRAMES,
      sizeof(*context->previous));
  context->output = lsx_calloc(
      (size_t)context->channels * FIR_BLOCK_FRAMES,
      sizeof(*context->output));
  return SOX_SUCCESS;
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    size_t taps, uint32_t channels)
{
  lsx_fir_vulkan_t *context;
  double started = monotonic_seconds();

  if (!vulkan || !coefficients || !taps || !channels ||
      taps > UINT32_MAX - FIR_BLOCK_FRAMES) {
    lsx_fail("invalid Vulkan FIR configuration");
    return NULL;
  }
  if (!vulkan->shader_float64) {
    lsx_fail("Vulkan FIR requires shaderFloat64 support");
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->channels = channels;
  context->partitions = (uint32_t)(
      (taps + FIR_BLOCK_FRAMES - 1u) /
      FIR_BLOCK_FRAMES);
  if (compiler_acquire() != SOX_SUCCESS)
    goto error;
  context->compiler_acquired = sox_true;
  if (create_commands(context) != SOX_SUCCESS ||
      create_buffers(context) != SOX_SUCCESS ||
      initialize_fft(context) != SOX_SUCCESS ||
      create_partition_pipeline(context) != SOX_SUCCESS ||
      initialize_kernels(context, coefficients, taps) != SOX_SUCCESS ||
      clear_history(context) != SOX_SUCCESS ||
      record_process_commands(context) != SOX_SUCCESS)
    goto error;
  context->startup_seconds = monotonic_seconds() - started;
  lsx_report(
      "Vulkan FIR: %zu taps, %u partitions, %u channels, "
      "%.3f MiB kernels, %.3f MiB history, startup %.6f seconds",
      taps, context->partitions, channels,
      (double)context->kernels.size / (1024.0 * 1024.0),
      (double)context->history.size / (1024.0 * 1024.0),
      context->startup_seconds);
  return context;

error:
  lsx_fir_vulkan_destroy(context);
  return NULL;
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
  if (context->fft_initialized)
    deleteVkFFT(&context->fft);
  if (context->fence)
    vkDestroyFence(
        context->vulkan->device, context->fence, NULL);
  destroy_buffer(context, &context->download);
  destroy_buffer(context, &context->upload);
  destroy_buffer(context, &context->kernels);
  destroy_buffer(context, &context->history);
  destroy_buffer(context, &context->working);
  free(context->output);
  free(context->previous);
  free(context->working_host);
  if (context->compiler_acquired)
    compiler_release();
  free(context);
}

size_t lsx_fir_vulkan_block_frames(void)
{
  return FIR_BLOCK_FRAMES;
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
  memset(
      context->working_host, 0,
      (size_t)context->working.size);
  for (channel = 0; channel < context->channels; ++channel)
    for (frame = 0; frame < FIR_BLOCK_FRAMES; ++frame) {
      double value =
          input[frame * context->channels + channel];
      context->working_host[
          (size_t)channel * (FIR_FFT_SIZE + 2u) + frame] =
          context->previous[
              (size_t)channel * FIR_BLOCK_FRAMES + frame];
      context->working_host[
          (size_t)channel * (FIR_FFT_SIZE + 2u) +
          FIR_BLOCK_FRAMES + frame] = value;
      context->previous[
          (size_t)channel * FIR_BLOCK_FRAMES + frame] = value;
    }
  memcpy(
      context->upload.mapped, context->working_host,
      (size_t)context->working.size);
  if (lsx_vulkan_submit_and_wait(
      context->vulkan,
      context->process_commands[context->current_slot],
      context->fence) != SOX_SUCCESS)
    return SOX_EOF;
  download = context->download.mapped;
  for (frame = 0; frame < FIR_BLOCK_FRAMES; ++frame)
    for (channel = 0; channel < context->channels; ++channel) {
      context->output[
          frame * context->channels + channel] =
          download[
              (size_t)channel * FIR_BLOCK_FRAMES + frame];
    }
  context->current_slot =
      (context->current_slot + 1u) % context->partitions;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  *output = context->output;
  return SOX_SUCCESS;
}
