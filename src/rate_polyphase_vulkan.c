/* FP64 Vulkan polyphase stage for the SoX rate planner.
 *
 * (c) Simone Filippini <info@simonefilippini.it> 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "sox_i.h"
#include "rate_polyphase_vulkan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rate_polyphase_f64_spv.inc"
#include "rate_polyphase_f32_spv.inc"
#include "rate_polyphase_precise_f32_spv.inc"
#include "rate_polyphase_reference_dd_spv.inc"
#include "rate_polyphase_reference_dd_normalized_f32_spv.inc"
#include "rate_polyphase_normalized_f32_spv.inc"

/* Frames of input per dispatch. One block per submit, each waited on, so this
 * sets how often the host and the device synchronise; the submit count, not
 * the kernels, was the cost. The ceiling follows --buffer and has to clear
 * what rate.c asks for, or the min() there caps every stage back down.
 *
 * Read once and kept: it sizes the device buffers at create time and is
 * compared against them later, so it must not move under a running stage. */
static uint32_t rate_polyphase_block_frames(void)
{
  static uint32_t cached;

  if (!cached) {
    size_t derived = sox_globals.bufsiz;

    cached = derived > 65536u ? (uint32_t)min(derived, (size_t)(1L << 22)) : 65536u;
  }
  return cached;
}

#define RATE_POLYPHASE_BLOCK_FRAMES rate_polyphase_block_frames()
#define RATE_POLYPHASE_BINDINGS 3u
#define RATE_POLYPHASE_LOCAL_SIZE 128u

/* Push constants. The phase counter advances by phase_step and wraps at
 * phase_count; its whole quotient is the input frame and its remainder the
 * sub-filter, which is how one counter carries both. phase_start is where
 * this batch begins, so the counter continues across calls. */
typedef struct {
  uint32_t output_frames;
  uint32_t phase_count;
  uint32_t phase_step;
  uint32_t phase_start;
  uint32_t taps;
  uint32_t channels;
  uint32_t normalize;
  uint32_t symmetric_presum;
} parameters_t;

lsx_static_assert(sizeof(parameters_t) == 32, vulkan_rate_polyphase_push_layout);

struct lsx_rate_polyphase_vulkan {
  lsx_vulkan_context_t *vulkan;
  lsx_vulkan_buffer_t coefficients;
  lsx_vulkan_buffer_t input;
  lsx_vulkan_buffer_t output;
  /* Host-side staging for the two device-local buffers above. A DMA copy
   * moves each across, so the shader never reaches over the bus. */
  lsx_vulkan_buffer_t input_staging;
  lsx_vulkan_buffer_t output_staging;
  lsx_vulkan_buffer_t normalized_output;
  lsx_vulkan_buffer_t resident_upload[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  lsx_vulkan_buffer_t resident_input[2];
  double *host_output;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkPipeline normalized_pipeline;
  VkCommandBuffer command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkFence fence;
  parameters_t parameters;
  uint32_t phase_start;
  uint32_t max_output_frames;
  uint32_t valid_output_frames;
  /* Resident-input state. The stage keeps its own device-side input across
   * calls, since the window margin of the previous block is needed for this
   * one: an arriving block is appended to what was retained, and the two
   * buffers alternate so the copy never reads and writes the same one while
   * the device is still using it. */
  uint32_t resident_input_index;
  uint32_t resident_bank_index;
  /* Frames already in the current input buffer. Starts at the caller's
   * preload, which is the margin the first block has no history for and
   * which is zero-filled on first use, so the stream begins against silence
   * rather than against uninitialised memory. */
  uint32_t resident_occupancy_frames;
  sox_bool resident_initialized;
  sox_bool double_precision;
  sox_bool precise_fp32;
  sox_bool reference_dd;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static uint32_t dispatch_items(lsx_rate_polyphase_vulkan_t const *context, uint32_t output_frames)
{
  (void)context;
  return output_frames;
}

static size_t sample_size(lsx_rate_polyphase_vulkan_t const *context)
{
  return context->reference_dd ?
      2u * sizeof(double) :
      context->precise_fp32 ?
      2u * sizeof(float) :
      context->double_precision ? sizeof(double) : sizeof(float);
}

static lsx_vulkan_resident_format_t sample_format(lsx_rate_polyphase_vulkan_t const *context)
{
  return context->reference_dd ?
      lsx_vulkan_resident_format_f64x2 :
      context->precise_fp32 ?
      lsx_vulkan_resident_format_f32x2 :
      context->double_precision ?
      lsx_vulkan_resident_format_f64 :
      lsx_vulkan_resident_format_f32;
}

/* Convert host doubles into the element form this profile's buffers use. The
 * paired forms store a high word and a correction: exact zero for the
 * double-double, whose input already fits one double, and the rounding
 * remainder for the split-float, where it is what carries the bits a single
 * float cannot. */
static void upload_samples(lsx_rate_polyphase_vulkan_t const *context, void *target, double const *source, size_t count)
{
  size_t index;

  if (context->reference_dd) {
    for (index = 0; index < count; ++index) {
      ((double *)target)[2u * index] = source[index];
      ((double *)target)[2u * index + 1u] = 0.;
    }
    return;
  }
  if (context->double_precision) {
    memcpy(target, source, count * sizeof(*source));
    return;
  }
  if (context->precise_fp32) {
    for (index = 0; index < count; ++index) {
      float high = (float)source[index];

      ((float *)target)[2u * index] = high;
      ((float *)target)[2u * index + 1u] = (float)(source[index] - (double)high);
    }
    return;
  }
  for (index = 0; index < count; ++index)
    ((float *)target)[index] = (float)source[index];
}

/* The inverse: a pointer to count output samples as host doubles. Plain FP64
 * is returned in place, the mapping already being doubles; the other forms
 * are converted into the scratch buffer, the paired ones collapsing through
 * the shared routine so that every collapse in the engine agrees. */
static double const *host_samples(lsx_rate_polyphase_vulkan_t *context, size_t count)
{
  size_t index;

  if (context->reference_dd) {
    for (index = 0; index < count; ++index) {
      double const *value = (double const *)context->output_staging.mapped + 2u * index;

      context->host_output[index] = lsx_vulkan_collapse_pair(value[0], value[1]);
    }
    return context->host_output;
  }
  if (context->double_precision)
    return context->output_staging.mapped;
  if (context->precise_fp32) {
    for (index = 0; index < count; ++index) {
      float const *value = (float const *)context->output_staging.mapped + 2u * index;

      context->host_output[index] = (double)value[0] + (double)value[1];
    }
    return context->host_output;
  }
  for (index = 0; index < count; ++index)
    context->host_output[index] = (double)((float const *)context->output_staging.mapped)[index];
  return context->host_output;
}

static int create_buffers(lsx_rate_polyphase_vulkan_t *context)
{
  VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkDeviceSize sample_bytes = sample_size(context);
  VkDeviceSize coefficient_size = (VkDeviceSize)context->parameters.phase_count * context->parameters.taps * sample_bytes;
  VkDeviceSize input_size = (VkDeviceSize)(RATE_POLYPHASE_BLOCK_FRAMES + context->parameters.taps - 1u) * context->parameters.channels * sample_bytes;
  VkDeviceSize output_size = (VkDeviceSize)context->max_output_frames * context->parameters.channels * sample_bytes;
  VkDeviceSize normalized_output_size =
      (VkDeviceSize)context->max_output_frames *
      context->parameters.channels *
      (context->precise_fp32 ?
       2u * sizeof(float) : sizeof(float));
  VkDeviceSize resident_input_size = input_size;
  uint32_t index;

  /* Everything the shader touches is device-local. The host reaches the
   * input and the output through the staging buffers above, and the
   * coefficients through a one-time copy in upload_coefficients(). */
  if (lsx_vulkan_buffer_create(context->vulkan, &context->coefficients, coefficient_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->input, input_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->input_staging, input_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, memory) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->output, output_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->output_staging, output_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, memory) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->normalized_output, normalized_output_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->resident_input[0], resident_input_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_buffer_create(context->vulkan, &context->resident_input[1], resident_input_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index) {
    if (lsx_vulkan_buffer_create(context->vulkan, &context->resident_upload[index], input_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, memory) != SOX_SUCCESS)
      return SOX_EOF;
  }
  if (context->reference_dd || !context->double_precision)
    context->host_output = lsx_malloc(
        (size_t)context->max_output_frames *
        context->parameters.channels * sizeof(*context->host_output));
  return SOX_SUCCESS;
}

/* Lay the coefficients out for the shader and hand them to the device. This
 * runs after the command buffers exist because device-local memory is reached
 * only through a copy, and the copy needs one to be recorded in. */
static int upload_coefficients(lsx_rate_polyphase_vulkan_t *context, double const *coefficients, double const *coefficient_lows)
{
  VkCommandBufferBeginInfo begin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
  };
  VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  lsx_vulkan_buffer_t staging;
  VkBufferCopy copy;
  VkDeviceSize size = context->coefficients.size;
  uint32_t phase;
  uint32_t tap;
  int result = SOX_EOF;

  if (lsx_vulkan_buffer_create(
      context->vulkan, &staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  for (tap = 0; tap < context->parameters.taps; ++tap)
    for (phase = 0; phase < context->parameters.phase_count; ++phase) {
      size_t target = (size_t)tap * context->parameters.phase_count + phase;
      double value = coefficients[(size_t)phase * context->parameters.taps + tap];

      if (context->reference_dd) {
        double *target_values = (double *)staging.mapped + 2u * target;
        size_t source = (size_t)phase * context->parameters.taps + tap;

        target_values[0] = value;
        target_values[1] = coefficient_lows ? coefficient_lows[source] : 0.;
      }
      else if (context->precise_fp32) {
        float high = (float)value;
        float *target_values = (float *)staging.mapped + 2u * target;

        target_values[0] = high;
        target_values[1] = (float)(value - (double)high);
      }
      else if (context->double_precision)
        ((double *)staging.mapped)[target] = value;
      else
        ((float *)staging.mapped)[target] = (float)value;
    }
  copy.srcOffset = copy.dstOffset = 0;
  copy.size = size;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = context->coefficients.buffer;
  barrier.offset = 0;
  barrier.size = size;
  if (vk_result(vkResetCommandBuffer(context->command_buffers[0], 0), "vkResetCommandBuffer rate polyphase coefficients") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(context->command_buffers[0], &begin), "vkBeginCommandBuffer rate polyphase coefficients") != SOX_SUCCESS)
    goto done;
  vkCmdCopyBuffer(context->command_buffers[0], staging.buffer, context->coefficients.buffer, 1, &copy);
  vkCmdPipelineBarrier(context->command_buffers[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
  if (vk_result(vkEndCommandBuffer(context->command_buffers[0]), "vkEndCommandBuffer rate polyphase coefficients") != SOX_SUCCESS ||
      lsx_vulkan_submit_and_wait(context->vulkan, context->command_buffers[0], context->fence, lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
    goto done;
  result = SOX_SUCCESS;
done:
  lsx_vulkan_buffer_destroy(context->vulkan, &staging);
  return result;
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
  VkDescriptorSetLayout layouts[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkDescriptorBufferInfo buffer_info[RATE_POLYPHASE_BINDINGS];
  VkWriteDescriptorSet writes[RATE_POLYPHASE_BINDINGS];
  lsx_vulkan_buffer_t *buffers[RATE_POLYPHASE_BINDINGS] = {&context->coefficients, &context->input, &context->output};
  uint32_t const *kernel_spirv, *normalized_spirv;
  size_t kernel_size, normalized_size;
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
  if (vk_result(vkCreatePipelineLayout(context->vulkan->device, &layout_info, NULL, &context->pipeline_layout), "vkCreatePipelineLayout rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  /* Pick each kernel once, so its SPIR-V blob and the size passed with it can
   * never disagree. Only the first test is order-sensitive: reference_dd is
   * set as double_precision && profile == reference, so it has to come before
   * the plain FP64 family; precise_fp32 is set only when double precision is
   * unavailable.
   *
   * The two selections are deliberately not the same mapping. The normalized
   * kernel writes host-scaled FP32 samples, so above FP32 it uses a dedicated
   * shader (reference_dd_normalized_f32 for the reference profile and
   * normalized_f32 for plain FP64) while precise shares the kernel it
   * already uses for the resident path. */
  if (context->reference_dd) {
    kernel_spirv = rate_polyphase_reference_dd_spv;
    kernel_size = sizeof(rate_polyphase_reference_dd_spv);
    normalized_spirv = rate_polyphase_reference_dd_normalized_f32_spv;
    normalized_size = sizeof(rate_polyphase_reference_dd_normalized_f32_spv);
  } else if (context->double_precision) {
    kernel_spirv = rate_polyphase_f64_spv;
    kernel_size = sizeof(rate_polyphase_f64_spv);
    normalized_spirv = rate_polyphase_normalized_f32_spv;
    normalized_size = sizeof(rate_polyphase_normalized_f32_spv);
  } else if (context->precise_fp32) {
    kernel_spirv = normalized_spirv = rate_polyphase_precise_f32_spv;
    kernel_size = normalized_size = sizeof(rate_polyphase_precise_f32_spv);
  } else {
    kernel_spirv = normalized_spirv = rate_polyphase_f32_spv;
    kernel_size = normalized_size = sizeof(rate_polyphase_f32_spv);
  }
  if (lsx_vulkan_create_compute_pipeline(context->vulkan, kernel_spirv, kernel_size, context->pipeline_layout, &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  if (lsx_vulkan_create_compute_pipeline(context->vulkan, normalized_spirv, normalized_size, context->pipeline_layout, &context->normalized_pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_size.descriptorCount *= LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.maxSets = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(context->vulkan->device, &pool_info, NULL, &context->descriptor_pool), "vkCreateDescriptorPool rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    layouts[index] = context->descriptor_layout;
  allocation.descriptorPool = context->descriptor_pool;
  allocation.descriptorSetCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  allocation.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(context->vulkan->device, &allocation, context->descriptor_sets), "vkAllocateDescriptorSets rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < RATE_POLYPHASE_BINDINGS; ++index) {
    uint32_t slot;
    buffer_info[index].buffer = buffers[index]->buffer;
    buffer_info[index].offset = 0;
    buffer_info[index].range = buffers[index]->size;
    for (slot = 0; slot < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++slot) {
      writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[index].dstSet = context->descriptor_sets[slot];
      writes[index].dstBinding = index;
      writes[index].descriptorCount = 1;
      writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[index].pBufferInfo = &buffer_info[index];
      vkUpdateDescriptorSets(context->vulkan->device, 1, &writes[index], 0, NULL);
    }
  }
  return SOX_SUCCESS;
}

static int create_commands(lsx_rate_polyphase_vulkan_t *context)
{
  VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

  allocation.commandPool = context->vulkan->command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  if (vk_result(vkAllocateCommandBuffers(context->vulkan->device, &allocation, context->command_buffers), "vkAllocateCommandBuffers rate polyphase") != SOX_SUCCESS || vk_result(vkCreateFence(context->vulkan->device, &fence_info, NULL, &context->fence), "vkCreateFence rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

/* The one body behind both entry points. coefficient_lows is the low half of
 * a double-double response and is used only where the device arithmetic can
 * hold it: outside the reference profile the pair has nowhere to go, so it is
 * ignored rather than refused. Passing NULL is what every other profile
 * does, and leaves the low halves zero as before. */
static lsx_rate_polyphase_vulkan_t *polyphase_create(lsx_vulkan_context_t *vulkan, double const *coefficients, double const *coefficient_lows, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum)
{
  lsx_rate_polyphase_vulkan_t *context;
  uint64_t max_output_frames;

  if (!vulkan || (!vulkan->shader_float64 &&
      vulkan->profile != sox_vulkan_profile_fast &&
      vulkan->profile != sox_vulkan_profile_precise) ||
      !coefficients || !taps || !phase_count || !phase_step ||
      phase_start >= phase_count || !channels ||
      resident_preload_frames > taps - 1u ||
      (symmetric_presum &&
       (phase_count != 1u || !(taps & 1u))))
    return NULL;
  max_output_frames = ((uint64_t)RATE_POLYPHASE_BLOCK_FRAMES * phase_count + phase_step - 1u) / phase_step + 1u;
  if (max_output_frames > UINT32_MAX)
    return NULL;
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->double_precision = vulkan->use_float64;
  context->reference_dd = context->double_precision && vulkan->profile == sox_vulkan_profile_reference;
  context->precise_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_precise;
  context->parameters.phase_count = phase_count;
  context->parameters.phase_step = phase_step;
  context->parameters.taps = taps;
  context->parameters.channels = channels;
  context->parameters.symmetric_presum = symmetric_presum ? 1u : 0u;
  context->phase_start = phase_start;
  context->max_output_frames = (uint32_t)max_output_frames;
  context->resident_occupancy_frames = resident_preload_frames;
  if (create_buffers(context) != SOX_SUCCESS ||
      create_pipeline(context) != SOX_SUCCESS ||
      create_commands(context) != SOX_SUCCESS ||
      upload_coefficients(context, coefficients, coefficient_lows) != SOX_SUCCESS)
    goto error;
  lsx_report(
      "Vulkan rate polyphase: %u/%u, %u taps/phase, "
      "%u channel%s, %s%s",
      phase_count, phase_step, taps, channels,
      channels == 1u ? "" : "s",
      context->reference_dd ?
          (coefficient_lows ? "FP64x2, double-double coefficients" : "FP64x2") :
      context->double_precision ? "FP64" :
      context->precise_fp32 ? "FP32x2" : "FP32",
      symmetric_presum ? ", symmetric presumming" : "");
  return context;

error: lsx_rate_polyphase_vulkan_destroy(context);
  return NULL;
}

lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create(lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum)
{
  return polyphase_create(vulkan, coefficients, NULL, taps, phase_count, phase_step, phase_start, channels, resident_preload_frames, symmetric_presum);
}

lsx_rate_polyphase_vulkan_t *lsx_rate_polyphase_vulkan_create_reference_dd(lsx_vulkan_context_t *vulkan, double const *coefficient_highs, double const *coefficient_lows, uint32_t taps, uint32_t phase_count, uint32_t phase_step, uint32_t phase_start, uint32_t channels, uint32_t resident_preload_frames, sox_bool symmetric_presum)
{
  return polyphase_create(vulkan, coefficient_highs, coefficient_lows, taps, phase_count, phase_step, phase_start, channels, resident_preload_frames, symmetric_presum);
}

void lsx_rate_polyphase_vulkan_destroy(lsx_rate_polyphase_vulkan_t *context)
{
  if (!context)
    return;
  vkDeviceWaitIdle(context->vulkan->device);
  if (context->fence)
    vkDestroyFence(context->vulkan->device, context->fence, NULL);
  if (context->command_buffers[0])
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, LSX_VULKAN_RESIDENT_BATCH_DEPTH, context->command_buffers);
  if (context->pipeline)
    vkDestroyPipeline(context->vulkan->device, context->pipeline, NULL);
  if (context->normalized_pipeline)
    vkDestroyPipeline(context->vulkan->device, context->normalized_pipeline, NULL);
  if (context->pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->pipeline_layout, NULL);
  if (context->descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->descriptor_pool, NULL);
  if (context->descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->descriptor_layout, NULL);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_input[1]);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_input[0]);
  {
    uint32_t index;

    for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
      lsx_vulkan_buffer_destroy(context->vulkan, &context->resident_upload[index]);
  }
  lsx_vulkan_buffer_destroy(context->vulkan, &context->normalized_output);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->output_staging);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->output);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->input_staging);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->input);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->coefficients);
  free(context->host_output);
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
  VkCommandBuffer command_buffer;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  VkMemoryBarrier upload_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
  VkMemoryBarrier host_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  VkBufferCopy input_copy;
  VkBufferCopy output_copy;
  uint64_t limit;
  uint64_t count;
  uint64_t end_position;
  size_t input_frames;

  if (!context || !input || !output || !output_frames || !consumed_frames || !processable_frames || processable_frames > RATE_POLYPHASE_BLOCK_FRAMES)
    return SOX_EOF;
  command_buffer = context->command_buffers[0];
  /* How many output frames the phase counter can produce before it runs past
   * the processable input. Everything is in counter units, an input frame
   * being phase_count of them, so the division is a ceiling of the number
   * of steps that fit, and the remainder afterwards is the phase to resume
   * from. Working in the counter's own units is what keeps the ratio exact:
   * nothing here rounds. */
  limit = (uint64_t)processable_frames * context->parameters.phase_count;
  count = limit > context->phase_start ? (limit - context->phase_start + context->parameters.phase_step - 1u) / context->parameters.phase_step : 0;
  if (!count || count > context->max_output_frames)
    return SOX_EOF;
  end_position = context->phase_start + count * context->parameters.phase_step;
  /* The response's window is uploaded as well as the frames to be consumed:
   * the last output frame reads taps - 1 samples past its own position. */
  input_frames = processable_frames + context->parameters.taps - 1u;
  upload_samples(context, context->input_staging.mapped, input, input_frames * context->parameters.channels);
  context->parameters.output_frames = (uint32_t)count;
  context->parameters.phase_start = context->phase_start;
  context->parameters.normalize = 0;
  if (vk_result(vkResetFences(context->vulkan->device, 1, &context->fence), "vkResetFences rate polyphase") != SOX_SUCCESS || vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer rate polyphase") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate polyphase");
  input_copy.srcOffset = input_copy.dstOffset = 0;
  input_copy.size = (VkDeviceSize)input_frames * context->parameters.channels * sample_size(context);
  output_copy.srcOffset = output_copy.dstOffset = 0;
  output_copy.size = (VkDeviceSize)count * context->parameters.channels * sample_size(context);
  vkCmdCopyBuffer(command_buffer, context->input_staging.buffer, context->input.buffer, 1, &input_copy);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload_barrier, 0, NULL, 0, NULL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline_layout, 0, 1, &context->descriptor_sets[0], 0, NULL);
  vkCmdPushConstants(command_buffer, context->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(context->parameters), &context->parameters);
  vkCmdDispatch(command_buffer, (dispatch_items(context, (uint32_t)count) + RATE_POLYPHASE_LOCAL_SIZE - 1u) / RATE_POLYPHASE_LOCAL_SIZE, context->parameters.channels, 1);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
  vkCmdCopyBuffer(command_buffer, context->output.buffer, context->output_staging.buffer, 1, &output_copy);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, NULL, 0, NULL);
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer rate polyphase") != SOX_SUCCESS || lsx_vulkan_submit_and_wait(context->vulkan, command_buffer, context->fence, lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
    return SOX_EOF;
  *output = host_samples(
      context, (size_t)count * context->parameters.channels);
  *output_frames = (size_t)count;
  /* The counter's quotient by phase_count is the input frames used up; its
   * remainder, stored below, is where the next call starts. */
  *consumed_frames = (size_t)(end_position / context->parameters.phase_count);
  context->valid_output_frames = (uint32_t)count;
  context->phase_start = (uint32_t)(end_position % context->parameters.phase_count);
  return SOX_SUCCESS;
}

int lsx_rate_polyphase_vulkan_process_resident_normalized(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident)
{
  lsx_vulkan_resident_buffer_t upload;
  uint64_t limit;
  uint64_t count;
  uint64_t end_position;
  size_t available_frames;
  size_t append_frames;
  size_t append_offset;
  uint32_t bank_index;

  if (!context || !input || !processable_frames || processable_frames > RATE_POLYPHASE_BLOCK_FRAMES || !output_frames || !consumed_frames || rate <= 0 || !resident)
    return SOX_EOF;
  available_frames = processable_frames + context->parameters.taps - 1u;
  if (available_frames <= context->resident_occupancy_frames)
    return SOX_EOF;
  append_offset = context->resident_occupancy_frames;
  append_frames = available_frames - append_offset;
  /* How many output frames the phase counter can produce before it runs past
   * the processable input. Everything is in counter units, an input frame
   * being phase_count of them, so the division is a ceiling of the number
   * of steps that fit, and the remainder afterwards is the phase to resume
   * from. Working in the counter's own units is what keeps the ratio exact:
   * nothing here rounds. */
  limit = (uint64_t)processable_frames * context->parameters.phase_count;
  count = limit > context->phase_start ? (limit - context->phase_start + context->parameters.phase_step - 1u) / context->parameters.phase_step : 0;
  if (!count || count > context->max_output_frames)
    return SOX_EOF;
  end_position = context->phase_start + count * context->parameters.phase_step;
  bank_index = context->resident_bank_index;
  upload_samples(
      context, context->resident_upload[bank_index].mapped,
      input + append_offset * context->parameters.channels,
      append_frames * context->parameters.channels);
  memset(&upload, 0, sizeof(upload));
  upload.buffer = &context->resident_upload[bank_index];
  upload.owner = context;
  upload.producer_stage = VK_PIPELINE_STAGE_HOST_BIT;
  upload.producer_access = VK_ACCESS_HOST_WRITE_BIT;
  upload.capacity_elements =
      context->resident_upload[bank_index].size /
      (context->parameters.channels * sample_size(context));
  upload.valid_elements = append_frames;
  upload.frame_stride_elements = context->parameters.channels;
  upload.channel_stride_elements = 1u;
  upload.rate = rate;
  upload.channels = context->parameters.channels;
  upload.frames_per_element = 1u;
  upload.format = sample_format(context);
  upload.domain = lsx_vulkan_resident_domain_sox_sample;
  upload.layout = lsx_vulkan_resident_layout_interleaved;
  upload.state = state;
  if (lsx_rate_polyphase_vulkan_process_resident_input_normalized(context, &upload, NULL, output_frames, rate, state, normalize, resident) != SOX_SUCCESS)
    return SOX_EOF;
  /* The counter's quotient by phase_count is the input frames used up; its
   * remainder, stored below, is where the next call starts. */
  *consumed_frames = (size_t)(end_position / context->parameters.phase_count);
  return SOX_SUCCESS;
}

int lsx_rate_polyphase_vulkan_process_resident(lsx_rate_polyphase_vulkan_t *context, double const *input, size_t processable_frames, size_t *output_frames, size_t *consumed_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  return lsx_rate_polyphase_vulkan_process_resident_normalized(context, input, processable_frames, output_frames, consumed_frames, rate, state, sox_false, resident);
}

/* Append one producer slice to the device-side window and run every output
 * whose complete tap window is now available.
 *
 * The function deliberately combines five operations in one command buffer:
 * append the producer slice, dispatch the polyphase kernel, make its output
 * visible, retain the unconsumed window tail in the alternate input buffer,
 * and either publish the output resident or wait and expose it to the host.
 * Keeping that order in one submission is what lets the two input buffers
 * alternate without copying samples through host memory. */
int lsx_rate_polyphase_vulkan_process_resident_input_normalized(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident)
{
  VkCommandBuffer command_buffer;
  VkDescriptorSet descriptor_set;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier input_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
  VkMemoryBarrier output_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
  VkBufferMemoryBarrier source_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
  VkDescriptorBufferInfo buffer_info[RATE_POLYPHASE_BINDINGS];
  VkWriteDescriptorSet writes[RATE_POLYPHASE_BINDINGS];
  lsx_vulkan_buffer_t *output_buffer;
  lsx_vulkan_buffer_t *current;
  lsx_vulkan_buffer_t *next;
  VkDeviceSize frame_size;
  VkBufferCopy append;
  VkBufferCopy retain;
  uint64_t limit;
  uint64_t count;
  uint64_t end_position;
  size_t processable_frames;
  size_t consumed_frames;
  size_t remaining_frames;
  size_t available_frames;
  uint32_t index;

  /* Phase 1: validate the complete cross-effect contract before interpreting
   * the producer buffer with this stage's element type and strides. */
  if (!context || !input || !output_frames ||
      (!output && !resident) || (resident && rate <= 0) ||
      lsx_vulkan_resident_buffer_validate(input) != SOX_SUCCESS ||
      input->format != sample_format(context) ||
      input->domain != lsx_vulkan_resident_domain_sox_sample ||
      input->layout != lsx_vulkan_resident_layout_interleaved ||
      input->frames_per_element != 1u ||
      input->channels != context->parameters.channels ||
      input->frame_stride_elements != context->parameters.channels ||
      input->channel_stride_elements != 1u ||
      input->state == lsx_vulkan_resident_empty ||
      !input->valid_elements)
    return SOX_EOF;
  command_buffer = context->command_buffers[context->resident_bank_index];
  descriptor_set = context->descriptor_sets[context->resident_bank_index];
  if (context->resident_occupancy_frames + input->valid_elements > RATE_POLYPHASE_BLOCK_FRAMES + context->parameters.taps - 1u)
    return SOX_EOF;
  available_frames = context->resident_occupancy_frames + input->valid_elements;
  if (available_frames <= context->parameters.taps - 1u)
    return SOX_EOF;
  frame_size = (VkDeviceSize)context->parameters.channels * sample_size(context);
  current = &context->resident_input[context->resident_input_index];
  next = &context->resident_input[context->resident_input_index ^ 1u];
  processable_frames = min((size_t)RATE_POLYPHASE_BLOCK_FRAMES, available_frames - (context->parameters.taps - 1u));
  /* How many output frames the phase counter can produce before it runs past
   * the processable input. Everything is in counter units, an input frame
   * being phase_count of them, so the division is a ceiling of the number
   * of steps that fit, and the remainder afterwards is the phase to resume
   * from. Working in the counter's own units is what keeps the ratio exact:
   * nothing here rounds. */
  limit = (uint64_t)processable_frames * context->parameters.phase_count;
  count = limit > context->phase_start ? (limit - context->phase_start + context->parameters.phase_step - 1u) / context->parameters.phase_step : 0;
  if (!processable_frames || !count || count > context->max_output_frames)
    return SOX_EOF;
  end_position = context->phase_start + count * context->parameters.phase_step;
  consumed_frames = (size_t)(end_position / context->parameters.phase_count);
  remaining_frames = available_frames - consumed_frames;

  /* Phase 2: describe both transfers. append extends the current window;
   * retain moves its still-needed suffix to the alternate buffer after the
   * dispatch, avoiding overlap with data the queued shader is reading. */
  append.srcOffset = input->offset;
  append.dstOffset = (VkDeviceSize)context->resident_occupancy_frames * frame_size;
  append.size = (VkDeviceSize)input->valid_elements * frame_size;
  retain.srcOffset = (VkDeviceSize)consumed_frames * frame_size;
  retain.dstOffset = 0;
  retain.size = (VkDeviceSize)remaining_frames * frame_size;
  source_barrier.srcAccessMask = input->producer_access;
  source_barrier.buffer = input->buffer->buffer;
  source_barrier.offset = input->offset;
  source_barrier.size = append.size;
  context->parameters.output_frames = (uint32_t)count;
  context->parameters.phase_start = context->phase_start;
  context->parameters.normalize = normalize && !lsx_sample_values_are_normalized() ? 1u : 0u;
  output_buffer = normalize ? &context->normalized_output : &context->output;

  /* Phase 3: bind coefficients, the completed input window and the selected
   * output representation to this in-flight bank. */
  buffer_info[0].buffer = context->coefficients.buffer;
  buffer_info[0].offset = 0;
  buffer_info[0].range = context->coefficients.size;
  buffer_info[1].buffer = current->buffer;
  buffer_info[1].offset = 0;
  buffer_info[1].range = current->size;
  buffer_info[2].buffer = output_buffer->buffer;
  buffer_info[2].offset = 0;
  buffer_info[2].range = output_buffer->size;
  memset(writes, 0, sizeof(writes));
  for (index = 0; index < RATE_POLYPHASE_BINDINGS; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(context->vulkan->device, RATE_POLYPHASE_BINDINGS, writes, 0, NULL);
  if (vk_result(vkResetFences(context->vulkan->device, 1, &context->fence), "vkResetFences resident rate polyphase") != SOX_SUCCESS || vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer resident rate polyphase") != SOX_SUCCESS || vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer resident rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate resident polyphase");
  /* Phase 4: initialise history if needed, append, filter, then preserve the
   * suffix. Barriers make each step visible to the next within this queue. */
  /* Zero the preload on the first block only: those frames stand in for the
   * history the first output has none of, so they must be silence. Done
   * inside the first recorded block rather than at creation, since it must
   * be ordered before the append that follows it. */
  if (!context->resident_initialized) {
    if (context->resident_occupancy_frames)
      vkCmdFillBuffer(command_buffer, current->buffer, 0, (VkDeviceSize)context->resident_occupancy_frames * frame_size, 0);
    context->resident_initialized = sox_true;
  }
  vkCmdPipelineBarrier(command_buffer, input->producer_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &source_barrier, 0, NULL);
  vkCmdCopyBuffer(command_buffer, input->buffer->buffer, current->buffer, 1, &append);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input_barrier, 0, NULL, 0, NULL);
  vkCmdBindPipeline(
      command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      normalize ? context->normalized_pipeline : context->pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
  vkCmdPushConstants(command_buffer, context->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(context->parameters), &context->parameters);
  vkCmdDispatch(command_buffer, (dispatch_items(context, (uint32_t)count) + RATE_POLYPHASE_LOCAL_SIZE - 1u) / RATE_POLYPHASE_LOCAL_SIZE, context->parameters.channels, 1);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &output_barrier, 0, NULL, 0, NULL);
  /* Retain the tail (the window margin the next block will read behind
   * itself) by copying it to the other buffer, which is then swapped in.
   * Moving it down within this buffer would overwrite samples the dispatch
   * just queued is still reading. */
  if (remaining_frames)
    vkCmdCopyBuffer(command_buffer, current->buffer, next->buffer, 1, &retain);
  /* A caller that wants the samples on the host reads them from the staging
   * buffer, so the copy that fills it has to be in this same batch. */
  if (!resident) {
    VkBufferCopy output_copy;
    VkMemoryBarrier host_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};

    output_copy.srcOffset = output_copy.dstOffset = 0;
    output_copy.size = (VkDeviceSize)count * context->parameters.channels * sample_size(context);
    vkCmdCopyBuffer(command_buffer, output_buffer->buffer, context->output_staging.buffer, 1, &output_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, NULL, 0, NULL);
  }
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer resident rate polyphase") != SOX_SUCCESS)
    return SOX_EOF;
  context->phase_start = (uint32_t)(end_position % context->parameters.phase_count);
  context->resident_occupancy_frames = (uint32_t)remaining_frames;
  context->resident_input_index ^= 1u;
  context->valid_output_frames = (uint32_t)count;
  *output_frames = (size_t)count;

  /* Phase 5: resident publication queues the command without waiting and
   * transfers its dependency metadata to the next effect. The host form
   * waits on the same command and converts the mapped output to doubles. */
  if (resident) {
    memset(resident, 0, sizeof(*resident));
    resident->buffer = output_buffer;
    resident->owner = context;
    resident->producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
    resident->capacity_elements = context->max_output_frames;
    resident->valid_elements = (size_t)count;
    resident->block_elements = context->max_output_frames;
    resident->frame_stride_elements = context->parameters.channels;
    resident->channel_stride_elements = 1u;
    resident->rate = rate;
    resident->channels = context->parameters.channels;
    resident->frames_per_element = 1u;
    resident->format = context->reference_dd && !normalize ?
        lsx_vulkan_resident_format_f64x2 :
        context->precise_fp32 ?
        lsx_vulkan_resident_format_f32x2 :
        normalize || !context->double_precision ?
        lsx_vulkan_resident_format_f32 :
        lsx_vulkan_resident_format_f64;
    resident->domain = normalize ? lsx_vulkan_resident_domain_normalized : lsx_vulkan_resident_domain_sox_sample;
    resident->layout = lsx_vulkan_resident_layout_interleaved;
    resident->state = state;
    if (lsx_vulkan_resident_buffer_validate(resident) != SOX_SUCCESS || lsx_vulkan_enqueue(context->vulkan, command_buffer) != SOX_SUCCESS)
      return SOX_EOF;
    context->resident_bank_index = (context->resident_bank_index + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  }
  else {
    if (lsx_vulkan_submit_and_wait(context->vulkan, command_buffer, context->fence, lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
      return SOX_EOF;
    *output = host_samples(
        context, (size_t)count * context->parameters.channels);
  }
  return SOX_SUCCESS;
}

int lsx_rate_polyphase_vulkan_process_resident_input(lsx_rate_polyphase_vulkan_t *context, lsx_vulkan_resident_buffer_t const *input, double const **output, size_t *output_frames, sox_rate_t rate, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  return lsx_rate_polyphase_vulkan_process_resident_input_normalized(context, input, output, output_frames, rate, state, sox_false, resident);
}
