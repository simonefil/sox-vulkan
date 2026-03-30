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
  VkBufferUsageFlags usage;
  VkMemoryPropertyFlags memory_flags;
  void *mapped;
} lsx_vulkan_buffer_t;

typedef enum {
  lsx_vulkan_resident_format_f32,
  lsx_vulkan_resident_format_f64,
  lsx_vulkan_resident_format_dsd_u32
} lsx_vulkan_resident_format_t;

typedef enum {
  lsx_vulkan_resident_domain_sox_sample,
  lsx_vulkan_resident_domain_normalized,
  lsx_vulkan_resident_domain_dsd
} lsx_vulkan_resident_domain_t;

typedef enum {
  lsx_vulkan_resident_layout_interleaved,
  lsx_vulkan_resident_layout_planar
} lsx_vulkan_resident_layout_t;

typedef enum {
  lsx_vulkan_resident_empty,
  lsx_vulkan_resident_ready,
  lsx_vulkan_resident_draining,
  lsx_vulkan_resident_final
} lsx_vulkan_resident_state_t;

typedef enum {
  lsx_vulkan_wait_fir_setup,
  lsx_vulkan_wait_fir_synchronous,
  lsx_vulkan_wait_fir_resident_flush,
  lsx_vulkan_wait_rate_synchronous,
  lsx_vulkan_wait_sdm_setup,
  lsx_vulkan_wait_sdm_synchronous,
  lsx_vulkan_wait_sdm_resident_flush,
  lsx_vulkan_wait_packed_output,
  lsx_vulkan_wait_reason_count
} lsx_vulkan_wait_reason_t;

typedef enum {
  lsx_vulkan_resident_topology_dft_only,
  lsx_vulkan_resident_topology_chained
} lsx_vulkan_resident_topology_t;

typedef enum {
  lsx_vulkan_numerical_family_fp32_emulated,
  lsx_vulkan_numerical_family_fp64
} lsx_vulkan_numerical_family_t;

/*
 * Non-owning description of a stream region that remains in Vulkan memory.
 * The producer identified by owner retains allocation lifetime responsibility.
 * producer_stage and producer_access describe the dependency required before a
 * consumer reads the region.  capacity_elements and valid_elements are per
 * channel.  domain distinguishes raw SoX sample units from normalized PCM;
 * packed DSD words contain 32 frames.
 */
typedef struct {
  lsx_vulkan_buffer_t *buffer;
  void *owner;
  VkDeviceSize offset;
  VkPipelineStageFlags producer_stage;
  VkAccessFlags producer_access;
  size_t capacity_elements;
  size_t valid_elements;
  size_t frame_stride_elements;
  size_t channel_stride_elements;
  uint64_t frame_offset;
  sox_rate_t rate;
  uint32_t channels;
  uint32_t frames_per_element;
  lsx_vulkan_resident_format_t format;
  lsx_vulkan_resident_domain_t domain;
  lsx_vulkan_resident_layout_t layout;
  lsx_vulkan_resident_state_t state;
} lsx_vulkan_resident_buffer_t;

typedef struct lsx_vulkan_context {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  uint32_t graphics_queue_family;
  uint32_t timestamp_valid_bits;
  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
  sox_vulkan_profile_t profile;
  lsx_vulkan_numerical_family_t numerical_family;
  sox_bool shader_float64;
  sox_bool debug_utils;
  sox_bool graphics_capture;
  sox_bool frame_boundary;
  PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_debug_utils_label;
  PFN_vkCmdEndDebugUtilsLabelEXT cmd_end_debug_utils_label;
  VkCommandPool command_pool;
  VkPipelineCache pipeline_cache;
  double startup_seconds;
  uint64_t submit_count;
  uint64_t host_wait_count;
  uint64_t frame_id;
  uint64_t submit_batch_counts[10];
  uint64_t wait_reason_counts[lsx_vulkan_wait_reason_count];
  VkCommandBuffer pending_command_buffers[64];
  uint32_t pending_command_buffer_count;
  uint32_t resident_batch_depth;
  sox_bool resident_batch_depth_overridden;
} lsx_vulkan_context_t;

#define LSX_VULKAN_RESIDENT_BATCH_DEPTH 4u

lsx_vulkan_context_t *lsx_vulkan_context_get(
    sox_effects_globals_t *effects_globals);
void lsx_vulkan_context_destroy(void *opaque_context);
char const *lsx_vulkan_profile_name(sox_vulkan_profile_t profile);
char const *lsx_vulkan_numerical_family_name(
    lsx_vulkan_numerical_family_t family);

int lsx_vulkan_result(VkResult result, char const *operation);
void lsx_vulkan_label_begin(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer, char const *name);
void lsx_vulkan_label_end(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer);
int lsx_vulkan_buffer_create(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties);
void lsx_vulkan_buffer_destroy(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer);
VkDeviceSize lsx_vulkan_resident_buffer_size(
    lsx_vulkan_resident_buffer_t const *resident);
int lsx_vulkan_resident_buffer_validate(
    lsx_vulkan_resident_buffer_t const *resident);
int lsx_vulkan_create_compute_pipeline(
    lsx_vulkan_context_t *context, uint32_t const *spirv,
    size_t spirv_size, VkPipelineLayout layout, VkPipeline *pipeline);
int lsx_vulkan_enqueue(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer);
int lsx_vulkan_submit_and_wait(
    lsx_vulkan_context_t *context, VkCommandBuffer command_buffer,
    VkFence fence, lsx_vulkan_wait_reason_t reason);
uint32_t lsx_vulkan_resident_batch_depth(lsx_vulkan_context_t const *context);
int lsx_vulkan_configure_resident_batch_depth(
    lsx_vulkan_context_t *context, sox_rate_t input_rate,
    sox_rate_t output_rate, uint32_t channels, uint64_t input_samples,
    lsx_vulkan_resident_topology_t topology);

#endif
