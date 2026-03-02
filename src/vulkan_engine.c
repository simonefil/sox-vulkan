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
#endif

#define NVIDIA_VENDOR_ID 0x10deu

static double monotonic_seconds(void)
{
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter))
    return 0.0;
  return (double)counter.QuadPart / (double)frequency.QuadPart;
}

int lsx_vulkan_result(VkResult result, char const *operation)
{
  if (result == VK_SUCCESS)
    return SOX_SUCCESS;
  lsx_fail("%s failed with Vulkan result %d", operation, (int)result);
  return SOX_EOF;
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

int lsx_vulkan_submit_and_wait(
    lsx_vulkan_context_t *context, VkCommandBuffer command_buffer,
    VkFence fence)
{
  VkSubmitInfo submit = {
    VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL,
    0, NULL, NULL, 1, &command_buffer, 0, NULL
  };

  if (lsx_vulkan_result(vkResetFences(
      context->device, 1, &fence),
      "vkResetFences") != SOX_SUCCESS ||
      lsx_vulkan_result(vkQueueSubmit(
      context->queue, 1, &submit, fence),
      "vkQueueSubmit") != SOX_SUCCESS ||
      lsx_vulkan_result(vkWaitForFences(
      context->device, 1, &fence, VK_TRUE, UINT64_MAX),
      "vkWaitForFences") != SOX_SUCCESS)
    return SOX_EOF;
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
  context->timestamp_valid_bits = best_timestamp_bits;
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
  VkDeviceQueueCreateInfo queue_info = {
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    NULL, 0, 0, 1, &priority
  };
  VkPhysicalDeviceFeatures available_features;
  VkPhysicalDeviceFeatures enabled_features;
  VkDeviceCreateInfo device_info = {
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
  };
  VkCommandPoolCreateInfo command_pool_info = {
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
  };
  VkPipelineCacheCreateInfo cache_info = {
    VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
  };
  lsx_vulkan_context_t *context =
      lsx_calloc(1, sizeof(*context));
  double started = monotonic_seconds();

  if (lsx_vulkan_result(vkCreateInstance(
      &instance_info, NULL, &context->instance),
      "vkCreateInstance") != SOX_SUCCESS ||
      choose_device(context) != SOX_SUCCESS)
    goto error;
  vkGetPhysicalDeviceFeatures(
      context->physical_device, &available_features);
  memset(&enabled_features, 0, sizeof(enabled_features));
  enabled_features.shaderFloat64 = available_features.shaderFloat64;
  context->shader_float64 =
      available_features.shaderFloat64 ? sox_true : sox_false;
  queue_info.queueFamilyIndex = context->queue_family;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
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
      "Vulkan core: %s, API %u.%u.%u, compute queue %u, "
      "timestamps %s, startup %.6f seconds",
      context->properties.deviceName,
      VK_VERSION_MAJOR(context->properties.apiVersion),
      VK_VERSION_MINOR(context->properties.apiVersion),
      VK_VERSION_PATCH(context->properties.apiVersion),
      context->queue_family,
      context->timestamp_valid_bits ? "available" : "unavailable",
      context->startup_seconds);
  return context;

error:
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
  return effects_globals->vulkan_context;
}

void lsx_vulkan_context_destroy(void *opaque_context)
{
  lsx_vulkan_context_t *context = opaque_context;

  if (!context)
    return;
  if (context->device)
    vkDeviceWaitIdle(context->device);
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
