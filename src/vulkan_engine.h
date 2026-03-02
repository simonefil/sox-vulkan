/* Shared Vulkan execution core for SoX effects.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_VULKAN_ENGINE_H
#define LSX_VULKAN_ENGINE_H

#include "sox.h"

#include <vulkan/vulkan.h>

typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  VkMemoryPropertyFlags memory_flags;
  void *mapped;
} lsx_vulkan_buffer_t;

typedef struct lsx_vulkan_context {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  uint32_t timestamp_valid_bits;
  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
  sox_bool shader_float64;
  VkCommandPool command_pool;
  VkPipelineCache pipeline_cache;
  double startup_seconds;
} lsx_vulkan_context_t;

lsx_vulkan_context_t *lsx_vulkan_context_get(
    sox_effects_globals_t *effects_globals);
void lsx_vulkan_context_destroy(void *opaque_context);

int lsx_vulkan_result(VkResult result, char const *operation);
int lsx_vulkan_buffer_create(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties);
void lsx_vulkan_buffer_destroy(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer);
int lsx_vulkan_create_compute_pipeline(
    lsx_vulkan_context_t *context, uint32_t const *spirv,
    size_t spirv_size, VkPipelineLayout layout, VkPipeline *pipeline);
int lsx_vulkan_submit_and_wait(
    lsx_vulkan_context_t *context, VkCommandBuffer command_buffer,
    VkFence fence);

#endif
