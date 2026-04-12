/* Shared Vulkan execution core for SoX effects.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "vulkan_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define NVIDIA_VENDOR_ID 0x10deu

char const *lsx_vulkan_profile_name(sox_vulkan_profile_t profile)
{
  switch (profile) {
    case sox_vulkan_profile_none:
      return "none";
    case sox_vulkan_profile_fast:
      return "fast";
    case sox_vulkan_profile_accurate:
      return "accurate";
    case sox_vulkan_profile_strict:
      return "strict";
    case sox_vulkan_profile_reference:
      return "reference";
  }
  return "unknown";
}

char const *lsx_vulkan_numerical_family_name(
    lsx_vulkan_numerical_family_t family)
{
  switch (family) {
    case lsx_vulkan_numerical_family_fp32_emulated:
      return "FP32 emulated";
    case lsx_vulkan_numerical_family_fp64:
      return "FP64";
  }
  return "unknown";
}

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

int lsx_vulkan_result(VkResult result, char const *operation)
{
  if (result == VK_SUCCESS)
    return SOX_SUCCESS;
  lsx_fail("%s failed with Vulkan result %d", operation, (int)result);
  return SOX_EOF;
}

void lsx_vulkan_label_begin(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer, char const *name)
{
  VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, NULL, name, {0.22f, 0.55f, 0.95f, 1.0f}};

  if (context && context->cmd_begin_debug_utils_label)
    context->cmd_begin_debug_utils_label(command_buffer, &label);
}

void lsx_vulkan_label_end(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer)
{
  if (context && context->cmd_end_debug_utils_label)
    context->cmd_end_debug_utils_label(command_buffer);
}

static sox_bool extension_available(VkExtensionProperties const *extensions, uint32_t count, char const *name)
{
  uint32_t index;

  for (index = 0; index < count; ++index)
    if (!strcmp(extensions[index].extensionName, name))
      return sox_true;
  return sox_false;
}

static uint32_t memory_type(
    lsx_vulkan_context_t const *context, uint32_t bits,
    VkMemoryPropertyFlags required)
{
  uint32_t index;

  for (index = 0;
       index < context->memory_properties.memoryTypeCount; ++index)
    if ((bits & (1u << index)) &&
        (context->memory_properties.memoryTypes[index].propertyFlags &
         required) == required)
      return index;
  return UINT32_MAX;
}

int lsx_vulkan_buffer_create(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties)
{
  VkBufferCreateInfo buffer_info = {
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage,
    VK_SHARING_MODE_EXCLUSIVE, 0, NULL
  };
  VkMemoryRequirements requirements;
  VkMemoryAllocateInfo allocation = {
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 0, 0
  };

  if (!context || !buffer || !size) {
    lsx_fail("invalid Vulkan buffer request");
    return SOX_EOF;
  }
  memset(buffer, 0, sizeof(*buffer));
  buffer->size = size;
  buffer->usage = usage;
  buffer->memory_flags = properties;
  if (lsx_vulkan_result(vkCreateBuffer(
      context->device, &buffer_info, NULL, &buffer->buffer),
      "vkCreateBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  vkGetBufferMemoryRequirements(
      context->device, buffer->buffer, &requirements);
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type(
      context, requirements.memoryTypeBits, properties);
  if (allocation.memoryTypeIndex == UINT32_MAX) {
    lsx_fail("no compatible Vulkan memory type");
    return SOX_EOF;
  }
  if (lsx_vulkan_result(vkAllocateMemory(
      context->device, &allocation, NULL, &buffer->memory),
      "vkAllocateMemory") != SOX_SUCCESS ||
      lsx_vulkan_result(vkBindBufferMemory(
      context->device, buffer->buffer, buffer->memory, 0),
      "vkBindBufferMemory") != SOX_SUCCESS)
    return SOX_EOF;
  if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
      lsx_vulkan_result(vkMapMemory(
      context->device, buffer->memory, 0, size, 0,
      &buffer->mapped), "vkMapMemory") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

void lsx_vulkan_buffer_destroy(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer)
{
  if (!context || !buffer)
    return;
  if (buffer->mapped)
    vkUnmapMemory(context->device, buffer->memory);
  if (buffer->buffer)
    vkDestroyBuffer(context->device, buffer->buffer, NULL);
  if (buffer->memory)
    vkFreeMemory(context->device, buffer->memory, NULL);
  memset(buffer, 0, sizeof(*buffer));
}

static VkDeviceSize resident_element_size(
    lsx_vulkan_resident_format_t format)
{
  switch (format) {
    case lsx_vulkan_resident_format_f32:
      return sizeof(float);
    case lsx_vulkan_resident_format_f32x2:
      return 2u * sizeof(float);
    case lsx_vulkan_resident_format_f64:
      return sizeof(double);
    case lsx_vulkan_resident_format_dsd_u32:
      return sizeof(uint32_t);
  }
  return 0;
}

VkDeviceSize lsx_vulkan_resident_buffer_size(
    lsx_vulkan_resident_buffer_t const *resident)
{
  VkDeviceSize element_size;
  VkDeviceSize frame_span;
  VkDeviceSize channel_span;
  VkDeviceSize last_element;

  if (!resident)
    return 0;
  element_size = resident_element_size(resident->format);
  if (!element_size || !resident->channels ||
      !resident->capacity_elements ||
      !resident->frame_stride_elements ||
      !resident->channel_stride_elements ||
      resident->capacity_elements - 1u >
      UINT64_MAX / resident->frame_stride_elements ||
      resident->channels - 1u >
      UINT64_MAX / resident->channel_stride_elements)
    return 0;
  frame_span = (VkDeviceSize)(resident->capacity_elements - 1u) *
      resident->frame_stride_elements;
  channel_span = (VkDeviceSize)(resident->channels - 1u) *
      resident->channel_stride_elements;
  if (channel_span == UINT64_MAX ||
      frame_span > UINT64_MAX - channel_span - 1u)
    return 0;
  last_element = frame_span + channel_span + 1u;
  if (last_element > UINT64_MAX / element_size)
    return 0;
  return last_element * element_size;
}

int lsx_vulkan_resident_buffer_validate(
    lsx_vulkan_resident_buffer_t const *resident)
{
  VkDeviceSize size = lsx_vulkan_resident_buffer_size(resident);

  if (!resident || !resident->buffer || !resident->buffer->buffer ||
      !resident->owner || !resident->capacity_elements ||
      !resident->producer_stage || !resident->producer_access ||
      !resident->frame_stride_elements ||
      !resident->channel_stride_elements ||
      resident->valid_elements > resident->capacity_elements ||
      !resident->frames_per_element || resident->rate <= 0 ||
      resident->domain > lsx_vulkan_resident_domain_dsd ||
      resident->layout > lsx_vulkan_resident_layout_planar ||
      resident->state > lsx_vulkan_resident_final ||
      !size || resident->offset > resident->buffer->size ||
      size > resident->buffer->size - resident->offset) {
    lsx_fail("invalid Vulkan resident buffer");
    return SOX_EOF;
  }
  if (resident->format == lsx_vulkan_resident_format_dsd_u32 &&
      (resident->frames_per_element != 32u || resident->domain != lsx_vulkan_resident_domain_dsd)) {
    lsx_fail("invalid packed DSD resident buffer");
    return SOX_EOF;
  }
  if (resident->format != lsx_vulkan_resident_format_dsd_u32 &&
      (resident->frames_per_element != 1u || resident->domain == lsx_vulkan_resident_domain_dsd)) {
    lsx_fail("invalid PCM resident buffer");
    return SOX_EOF;
  }
  if (resident->state == lsx_vulkan_resident_empty &&
      resident->valid_elements) {
    lsx_fail("non-empty Vulkan resident buffer marked empty");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int create_resident_download(
    lsx_vulkan_context_t *context, VkDeviceSize size)
{
  VkCommandBufferAllocateInfo command_info = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
  };
  VkFenceCreateInfo fence_info = {
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  if (context->resident_download.size >= size)
    return SOX_SUCCESS;
  if (context->resident_download.buffer)
    lsx_vulkan_buffer_destroy(
        context, &context->resident_download);
  if (lsx_vulkan_buffer_create(
      context, &context->resident_download, size,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (!context->resident_download_command) {
    command_info.commandPool = context->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (lsx_vulkan_result(vkAllocateCommandBuffers(
        context->device, &command_info,
        &context->resident_download_command),
        "vkAllocateCommandBuffers resident output") != SOX_SUCCESS ||
        lsx_vulkan_result(vkCreateFence(
        context->device, &fence_info, NULL,
        &context->resident_download_fence),
        "vkCreateFence resident output") != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

int lsx_vulkan_download_resident_pcm(
    lsx_vulkan_context_t *context,
    lsx_vulkan_resident_buffer_t const *resident,
    double *output, size_t output_samples)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkBufferMemoryBarrier source_barrier = {
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
  };
  VkMemoryBarrier host_barrier = {
    VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT
  };
  VkBufferCopy copy;
  VkDeviceSize size;
  size_t required_samples;
  size_t element_size;
  size_t frame;
  uint32_t channel;

  if (!context || !resident || !output ||
      lsx_vulkan_resident_buffer_validate(resident) != SOX_SUCCESS ||
      resident->domain == lsx_vulkan_resident_domain_dsd)
    return SOX_EOF;
  required_samples = resident->valid_elements * resident->channels;
  if (resident->channels &&
      required_samples / resident->channels != resident->valid_elements)
    return SOX_EOF;
  if (output_samples < required_samples)
    return SOX_EOF;
  size = lsx_vulkan_resident_buffer_size(resident);
  if (create_resident_download(context, size) != SOX_SUCCESS)
    return SOX_EOF;
  source_barrier.srcAccessMask = resident->producer_access;
  source_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  source_barrier.buffer = resident->buffer->buffer;
  source_barrier.offset = resident->offset;
  source_barrier.size = size;
  copy.srcOffset = resident->offset;
  copy.dstOffset = 0;
  copy.size = size;
  if (lsx_vulkan_result(vkResetCommandBuffer(
      context->resident_download_command, 0),
      "vkResetCommandBuffer resident output") != SOX_SUCCESS ||
      lsx_vulkan_result(vkBeginCommandBuffer(
      context->resident_download_command, &begin),
      "vkBeginCommandBuffer resident output") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(
      context, context->resident_download_command,
      "Resident PCM final output download");
  vkCmdPipelineBarrier(
      context->resident_download_command,
      resident->producer_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, NULL, 1, &source_barrier, 0, NULL);
  vkCmdCopyBuffer(
      context->resident_download_command,
      resident->buffer->buffer, context->resident_download.buffer,
      1, &copy);
  vkCmdPipelineBarrier(
      context->resident_download_command,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &host_barrier, 0, NULL, 0, NULL);
  lsx_vulkan_label_end(
      context, context->resident_download_command);
  if (lsx_vulkan_result(vkEndCommandBuffer(
      context->resident_download_command),
      "vkEndCommandBuffer resident output") != SOX_SUCCESS ||
      lsx_vulkan_submit_and_wait(
      context, context->resident_download_command,
      context->resident_download_fence,
      lsx_vulkan_wait_resident_output) != SOX_SUCCESS)
    return SOX_EOF;
  element_size = resident->format == lsx_vulkan_resident_format_f32x2 ?
      2u * sizeof(float) :
      resident->format == lsx_vulkan_resident_format_f64 ?
      sizeof(double) : sizeof(float);
  for (frame = 0; frame < resident->valid_elements; ++frame)
    for (channel = 0; channel < resident->channels; ++channel) {
      size_t source = frame * resident->frame_stride_elements +
          channel * resident->channel_stride_elements;
      double value;

      if (resident->format == lsx_vulkan_resident_format_f32x2) {
        float const *pair = (float const *)
            ((char const *)context->resident_download.mapped +
            source * element_size);
        value = (double)pair[0] + (double)pair[1];
      }
      else if (resident->format == lsx_vulkan_resident_format_f64)
        value = ((double const *)
            context->resident_download.mapped)[source];
      else
        value = ((float const *)
            context->resident_download.mapped)[source];
      output[frame * resident->channels + channel] = value;
    }
  return SOX_SUCCESS;
}

int lsx_vulkan_create_compute_pipeline(
    lsx_vulkan_context_t *context, uint32_t const *spirv,
    size_t spirv_size, VkPipelineLayout layout, VkPipeline *pipeline)
{
  VkShaderModuleCreateInfo shader_info = {
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    NULL, 0, spirv_size, spirv
  };
  VkPipelineShaderStageCreateInfo stage = {
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
  };
  VkComputePipelineCreateInfo pipeline_info = {
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
  };
  VkShaderModule shader;
  int result;

  result = lsx_vulkan_result(vkCreateShaderModule(
      context->device, &shader_info, NULL, &shader),
      "vkCreateShaderModule");
  if (result != SOX_SUCCESS)
    return result;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader;
  stage.pName = "main";
  pipeline_info.stage = stage;
  pipeline_info.layout = layout;
  result = lsx_vulkan_result(vkCreateComputePipelines(
      context->device, context->pipeline_cache, 1,
      &pipeline_info, NULL, pipeline), "vkCreateComputePipelines");
  vkDestroyShaderModule(context->device, shader, NULL);
  return result;
}

static int submit(lsx_vulkan_context_t *context, VkCommandBuffer const *command_buffers, uint32_t command_buffer_count, VkFence fence)
{
  VkFrameBoundaryEXT frame_boundary = {VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, NULL, VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT, ++context->frame_id, 0, NULL, 0, NULL, 0, 0, NULL};
  VkSubmitInfo submit = {
    VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL,
    0, NULL, NULL, command_buffer_count, command_buffers, 0, NULL
  };

  if (context->frame_boundary)
    submit.pNext = &frame_boundary;
  if (fence && lsx_vulkan_result(vkResetFences(context->device, 1, &fence), "vkResetFences") != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_result(vkQueueSubmit(context->queue, 1, &submit, fence), "vkQueueSubmit") != SOX_SUCCESS)
    return SOX_EOF;
  ++context->submit_batch_counts[min(command_buffer_count, 9u)];
  ++context->submit_count;
  return SOX_SUCCESS;
}

int lsx_vulkan_enqueue(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer)
{
  if (!context || !command_buffer || context->pending_command_buffer_count >= sizeof(context->pending_command_buffers) / sizeof(context->pending_command_buffers[0])) {
    lsx_fail("Vulkan pending command buffer capacity exceeded");
    return SOX_EOF;
  }
  context->pending_command_buffers[context->pending_command_buffer_count++] = command_buffer;
  return SOX_SUCCESS;
}

int lsx_vulkan_submit_and_wait(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer, VkFence fence, lsx_vulkan_wait_reason_t reason)
{
  VkCommandBuffer command_buffers[sizeof(context->pending_command_buffers) / sizeof(context->pending_command_buffers[0]) + 1u];
  uint32_t command_buffer_count = context->pending_command_buffer_count;

  memcpy(command_buffers, context->pending_command_buffers, command_buffer_count * sizeof(command_buffers[0]));
  command_buffers[command_buffer_count++] = command_buffer;
  if (submit(context, command_buffers, command_buffer_count, fence) != SOX_SUCCESS)
    return SOX_EOF;
  context->pending_command_buffer_count = 0;
  ++context->host_wait_count;
  if ((unsigned)reason < lsx_vulkan_wait_reason_count)
    ++context->wait_reason_counts[reason];
  if (lsx_vulkan_result(vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

uint32_t lsx_vulkan_resident_batch_depth(lsx_vulkan_context_t const *context)
{
  return context && context->resident_batch_depth ? context->resident_batch_depth : LSX_VULKAN_RESIDENT_BATCH_DEPTH;
}

int lsx_vulkan_configure_resident_batch_depth(lsx_vulkan_context_t *context, sox_rate_t input_rate, sox_rate_t output_rate, uint32_t channels, uint64_t input_samples, lsx_vulkan_resident_topology_t topology)
{
  double duration = input_rate > 0 && channels && input_samples && input_samples != SOX_UNKNOWN_LEN ? (double)(input_samples / channels) / input_rate : 0;
  double output_samples = duration * output_rate * channels;
  sox_bool calibrated_device;
  char const *selection = "qualified fallback";

  if (!context || input_rate <= 0 || output_rate <= 0 || !channels ||
      topology > lsx_vulkan_resident_topology_chained)
    return SOX_EOF;
  calibrated_device =
      context->properties.vendorID == NVIDIA_VENDOR_ID &&
      !strcmp(context->properties.deviceName, "NVIDIA GeForce RTX 3080");
  if (context->resident_batch_depth_overridden)
    selection = "environment override";
  else {
    context->resident_batch_depth = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
    if (calibrated_device)
      selection = topology == lsx_vulkan_resident_topology_dft_only ?
          "calibrated DFT throughput" :
          "calibrated chained throughput";
  }
  lsx_report("Vulkan resident cost model: depth %u, topology %s, input %.0f Hz, output %.0f Hz, %u channel%s, duration %s%.6g s, output work %.0f samples, device %s, %s", context->resident_batch_depth, topology == lsx_vulkan_resident_topology_dft_only ? "DFT-only" : "chained", input_rate, output_rate, channels, channels == 1u ? "" : "s", duration > 0 ? "" : "unknown/", duration, output_samples, context->properties.deviceName, selection);
  return SOX_SUCCESS;
}

static int choose_device(lsx_vulkan_context_t *context)
{
  VkPhysicalDevice *devices;
  VkPhysicalDevice best_device = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties best_properties;
  uint32_t best_queue = UINT32_MAX;
  uint32_t best_timestamp_bits = 0;
  int best_score = -1;
  uint32_t count = 0;
  uint32_t device_index;

  if (lsx_vulkan_result(vkEnumeratePhysicalDevices(
      context->instance, &count, NULL),
      "vkEnumeratePhysicalDevices") != SOX_SUCCESS)
    return SOX_EOF;
  if (!count) {
    lsx_fail("no Vulkan physical device found");
    return SOX_EOF;
  }
  devices = lsx_calloc(count, sizeof(*devices));
  if (lsx_vulkan_result(vkEnumeratePhysicalDevices(
      context->instance, &count, devices),
      "vkEnumeratePhysicalDevices") != SOX_SUCCESS) {
    free(devices);
    return SOX_EOF;
  }
  memset(&best_properties, 0, sizeof(best_properties));
  for (device_index = 0; device_index < count; ++device_index) {
    VkPhysicalDeviceProperties properties;
    VkQueueFamilyProperties *queues;
    uint32_t queue_count = 0;
    uint32_t queue_index;

    vkGetPhysicalDeviceProperties(devices[device_index], &properties);
    vkGetPhysicalDeviceQueueFamilyProperties(
        devices[device_index], &queue_count, NULL);
    queues = lsx_calloc(queue_count, sizeof(*queues));
    vkGetPhysicalDeviceQueueFamilyProperties(
        devices[device_index], &queue_count, queues);
    for (queue_index = 0; queue_index < queue_count; ++queue_index)
      if (queues[queue_index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        int score =
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ?
            1000 : 0;

        score += properties.vendorID == NVIDIA_VENDOR_ID ? 100 : 0;
        score +=
            !(queues[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) ?
            10 : 0;
        if (score > best_score) {
          best_score = score;
          best_device = devices[device_index];
          best_properties = properties;
          best_queue = queue_index;
          best_timestamp_bits =
              queues[queue_index].timestampValidBits;
        }
      }
    free(queues);
  }
  free(devices);
  if (best_device == VK_NULL_HANDLE) {
    lsx_fail("no Vulkan compute device found");
    return SOX_EOF;
  }
  context->physical_device = best_device;
  context->properties = best_properties;
  context->queue_family = best_queue;
  context->graphics_queue_family = UINT32_MAX;
  context->timestamp_valid_bits = best_timestamp_bits;
  {
    VkQueueFamilyProperties *queues;
    uint32_t queue_count = 0;
    uint32_t queue_index;

    vkGetPhysicalDeviceQueueFamilyProperties(best_device, &queue_count, NULL);
    queues = lsx_calloc(queue_count, sizeof(*queues));
    vkGetPhysicalDeviceQueueFamilyProperties(best_device, &queue_count, queues);
    for (queue_index = 0; queue_index < queue_count; ++queue_index)
      if (queues[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        context->graphics_queue_family = queue_index;
        break;
      }
    free(queues);
  }
  vkGetPhysicalDeviceMemoryProperties(
      best_device, &context->memory_properties);
  return SOX_SUCCESS;
}

static lsx_vulkan_context_t *create_context(void)
{
  VkApplicationInfo app = {
    VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
    "SoX Vulkan Effects", 1, "SoX", 1, VK_API_VERSION_1_1
  };
  VkInstanceCreateInfo instance_info = {
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app,
    0, NULL, 0, NULL
  };
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_infos[2];
  VkPhysicalDeviceFeatures available_features;
  VkPhysicalDeviceFeatures enabled_features;
  VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceFrameBoundaryFeaturesEXT frame_boundary_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT};
  VkDeviceCreateInfo device_info = {
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
  };
  VkCommandPoolCreateInfo command_pool_info = {
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
  };
  VkPipelineCacheCreateInfo cache_info = {
    VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
  };
  VkExtensionProperties *instance_extensions = NULL;
  VkExtensionProperties *device_extensions = NULL;
  char const *enabled_instance_extensions[2];
  char const *enabled_device_extensions[2];
  uint32_t instance_extension_count = 0;
  uint32_t enabled_instance_extension_count = 0;
  uint32_t device_extension_count = 0;
  uint32_t enabled_device_extension_count = 0;
  lsx_vulkan_context_t *context =
      lsx_calloc(1, sizeof(*context));
  char const *graphics_capture = getenv("SOX_VULKAN_NSIGHT_GRAPHICS");
  char const *depth_override = getenv("SOX_VULKAN_RESIDENT_DEPTH");
  double started = monotonic_seconds();

  context->resident_batch_depth = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  if (depth_override && depth_override[0]) {
    char *end;
    unsigned long depth = strtoul(depth_override, &end, 10);

    if (*end || depth < 1u || depth > LSX_VULKAN_RESIDENT_BATCH_DEPTH) {
      lsx_fail("SOX_VULKAN_RESIDENT_DEPTH must be between 1 and %u", LSX_VULKAN_RESIDENT_BATCH_DEPTH);
      goto error;
    }
    context->resident_batch_depth = (uint32_t)depth;
    context->resident_batch_depth_overridden = sox_true;
  }
  context->graphics_capture = graphics_capture && graphics_capture[0] && strcmp(graphics_capture, "0") ? sox_true : sox_false;
  if (lsx_vulkan_result(vkEnumerateInstanceExtensionProperties(NULL, &instance_extension_count, NULL), "vkEnumerateInstanceExtensionProperties") != SOX_SUCCESS)
    goto error;
  instance_extensions = lsx_calloc(instance_extension_count, sizeof(*instance_extensions));
  if (lsx_vulkan_result(vkEnumerateInstanceExtensionProperties(NULL, &instance_extension_count, instance_extensions), "vkEnumerateInstanceExtensionProperties") != SOX_SUCCESS)
    goto error;
  if (extension_available(instance_extensions, instance_extension_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (extension_available(instance_extensions, instance_extension_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  instance_info.enabledExtensionCount = enabled_instance_extension_count;
  instance_info.ppEnabledExtensionNames = enabled_instance_extensions;
  if (lsx_vulkan_result(vkCreateInstance(
      &instance_info, NULL, &context->instance),
      "vkCreateInstance") != SOX_SUCCESS ||
      choose_device(context) != SOX_SUCCESS)
    goto error;
  free(instance_extensions);
  instance_extensions = NULL;
  context->cmd_begin_debug_utils_label = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(context->instance, "vkCmdBeginDebugUtilsLabelEXT");
  context->cmd_end_debug_utils_label = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(context->instance, "vkCmdEndDebugUtilsLabelEXT");
  context->debug_utils = context->cmd_begin_debug_utils_label && context->cmd_end_debug_utils_label ? sox_true : sox_false;
  memset(queue_infos, 0, sizeof(queue_infos));
  queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_infos[0].queueFamilyIndex = context->queue_family;
  queue_infos[0].queueCount = 1;
  queue_infos[0].pQueuePriorities = &priority;
  device_info.queueCreateInfoCount = 1;
  if (context->graphics_capture && context->graphics_queue_family != UINT32_MAX && context->graphics_queue_family != context->queue_family) {
    queue_infos[1] = queue_infos[0];
    queue_infos[1].queueFamilyIndex = context->graphics_queue_family;
    device_info.queueCreateInfoCount = 2;
  }
  device_info.pQueueCreateInfos = queue_infos;
  if (lsx_vulkan_result(vkEnumerateDeviceExtensionProperties(context->physical_device, NULL, &device_extension_count, NULL), "vkEnumerateDeviceExtensionProperties") != SOX_SUCCESS)
    goto error;
  device_extensions = lsx_calloc(device_extension_count, sizeof(*device_extensions));
  if (lsx_vulkan_result(vkEnumerateDeviceExtensionProperties(context->physical_device, NULL, &device_extension_count, device_extensions), "vkEnumerateDeviceExtensionProperties") != SOX_SUCCESS)
    goto error;
  if (context->graphics_capture && extension_available(device_extensions, device_extension_count, VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME)) {
    features2.pNext = &frame_boundary_features;
    vkGetPhysicalDeviceFeatures2(context->physical_device, &features2);
    if (frame_boundary_features.frameBoundary) {
      enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME;
      device_info.pNext = &frame_boundary_features;
      context->frame_boundary = sox_true;
    }
  }
  if (extension_available(device_extensions, device_extension_count, "VK_KHR_portability_subset"))
    enabled_device_extensions[enabled_device_extension_count++] = "VK_KHR_portability_subset";
  free(device_extensions);
  device_extensions = NULL;
  device_info.enabledExtensionCount = enabled_device_extension_count;
  device_info.ppEnabledExtensionNames = enabled_device_extensions;
  vkGetPhysicalDeviceFeatures(
      context->physical_device, &available_features);
  memset(&enabled_features, 0, sizeof(enabled_features));
  enabled_features.shaderFloat64 = available_features.shaderFloat64;
  context->shader_float64 =
      available_features.shaderFloat64 ? sox_true : sox_false;
  context->profile = sox_globals.vulkan_profile;
  context->use_float64 =
      context->shader_float64 &&
      (context->profile == sox_vulkan_profile_strict ||
       context->profile == sox_vulkan_profile_reference);
  context->numerical_family = context->use_float64 ?
      lsx_vulkan_numerical_family_fp64 :
      lsx_vulkan_numerical_family_fp32_emulated;
  lsx_report(
      "Vulkan profile requested: %s",
      lsx_vulkan_profile_name(context->profile));
  lsx_report(
      "Vulkan capability shaderFloat64: %s",
      context->shader_float64 ? "true" : "false");
  lsx_report(
      "Vulkan numerical family: %s",
      lsx_vulkan_numerical_family_name(
          context->numerical_family));
  if (context->profile == sox_vulkan_profile_none) {
    lsx_fail("Vulkan context requested without a numerical profile");
    goto error;
  }
  if (context->profile == sox_vulkan_profile_reference &&
      !context->shader_float64) {
    lsx_fail(
        "Vulkan reference profile requires hardware shaderFloat64");
    goto error;
  }
  device_info.pEnabledFeatures = &enabled_features;
  if (lsx_vulkan_result(vkCreateDevice(
      context->physical_device, &device_info, NULL,
      &context->device), "vkCreateDevice") != SOX_SUCCESS)
    goto error;
  vkGetDeviceQueue(
      context->device, context->queue_family, 0, &context->queue);
  command_pool_info.flags =
      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  command_pool_info.queueFamilyIndex = context->queue_family;
  if (lsx_vulkan_result(vkCreateCommandPool(
      context->device, &command_pool_info, NULL,
      &context->command_pool), "vkCreateCommandPool") != SOX_SUCCESS ||
      lsx_vulkan_result(vkCreatePipelineCache(
      context->device, &cache_info, NULL,
      &context->pipeline_cache), "vkCreatePipelineCache") != SOX_SUCCESS)
    goto error;
  context->startup_seconds = monotonic_seconds() - started;
  lsx_report(
      "Vulkan core: %s, API %u.%u.%u, compute queue %u, graphics queue %s, "
      "timestamps %s, debug labels %s, frame boundaries %s, startup %.6f seconds",
      context->properties.deviceName,
      VK_VERSION_MAJOR(context->properties.apiVersion),
      VK_VERSION_MINOR(context->properties.apiVersion),
      VK_VERSION_PATCH(context->properties.apiVersion),
      context->queue_family,
      context->graphics_capture && context->graphics_queue_family != UINT32_MAX ? "requested" : "not requested",
      context->timestamp_valid_bits ? "available" : "unavailable",
      context->debug_utils ? "available" : "unavailable",
      context->frame_boundary ? "available" : "unavailable",
      context->startup_seconds);
  return context;

error:
  free(instance_extensions);
  free(device_extensions);
  lsx_vulkan_context_destroy(context);
  return NULL;
}

lsx_vulkan_context_t *lsx_vulkan_context_get(
    sox_effects_globals_t *effects_globals)
{
  if (!effects_globals) {
    lsx_fail("Vulkan effect has no effects-chain context");
    return NULL;
  }
  if (!effects_globals->vulkan_context)
    effects_globals->vulkan_context = create_context();
  else {
    lsx_vulkan_context_t *context =
        effects_globals->vulkan_context;

    if (context->profile != sox_globals.vulkan_profile) {
      lsx_fail(
          "Vulkan profile cannot change during an effects chain");
      return NULL;
    }
  }
  return effects_globals->vulkan_context;
}

void lsx_vulkan_context_destroy(void *opaque_context)
{
  lsx_vulkan_context_t *context = opaque_context;

  if (!context)
    return;
  if (context->device)
    vkDeviceWaitIdle(context->device);
  if (context->device && context->resident_download_fence)
    vkDestroyFence(
        context->device, context->resident_download_fence, NULL);
  if (context->device)
    lsx_vulkan_buffer_destroy(
        context, &context->resident_download);
  if (context->submit_count || context->host_wait_count)
    lsx_report(
        "Vulkan core execution: %llu submits, %llu host waits",
        (unsigned long long)context->submit_count,
        (unsigned long long)context->host_wait_count);
  if (context->submit_count)
    lsx_report("Vulkan submit batch histogram: 1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu 9+=%llu", (unsigned long long)context->submit_batch_counts[1], (unsigned long long)context->submit_batch_counts[2], (unsigned long long)context->submit_batch_counts[3], (unsigned long long)context->submit_batch_counts[4], (unsigned long long)context->submit_batch_counts[5], (unsigned long long)context->submit_batch_counts[6], (unsigned long long)context->submit_batch_counts[7], (unsigned long long)context->submit_batch_counts[8], (unsigned long long)context->submit_batch_counts[9]);
  if (context->host_wait_count)
    lsx_report("Vulkan wait reasons: fir_setup=%llu fir_sync=%llu fir_resident_flush=%llu rate_sync=%llu sdm_setup=%llu sdm_sync=%llu sdm_resident_flush=%llu packed_output=%llu resident_output=%llu", (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_fir_setup], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_fir_synchronous], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_fir_resident_flush], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_rate_synchronous], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_sdm_setup], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_sdm_synchronous], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_sdm_resident_flush], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_packed_output], (unsigned long long)context->wait_reason_counts[lsx_vulkan_wait_resident_output]);
  if (context->device && context->pipeline_cache)
    vkDestroyPipelineCache(
        context->device, context->pipeline_cache, NULL);
  if (context->device && context->command_pool)
    vkDestroyCommandPool(
        context->device, context->command_pool, NULL);
  if (context->device)
    vkDestroyDevice(context->device, NULL);
  if (context->instance)
    vkDestroyInstance(context->instance, NULL);
  free(context);
}
