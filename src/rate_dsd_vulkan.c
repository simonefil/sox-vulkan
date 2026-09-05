/* Fused packed-DSD to PCM stage for the SoX rate planner, on Vulkan.
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
#include "rate_dsd_vulkan.h"
#include "diagnostics.h"

#include <stdint.h>
#include <string.h>

#include "rate_dsd_fir_f64_spv.inc"
#include "rate_dsd_fir_f32_spv.inc"
#include "rate_dsd_fir_precise_f32_spv.inc"
#include "rate_dsd_fir_reference_dd_spv.inc"

/* Output frames per dispatch, which is also how much of the device one
 * dispatch fills: the grid is this many invocations by the channel count, so
 * at 4096 a stereo stream launched 8192 threads: 64 workgroups, fewer than
 * one per SM on a discrete GPU, with the host waiting after each. Measured
 * across both, 65536 is where the curve flattens: on M5 Pro it takes DSD64
 * from 17x to 31x and DSD256 from 63x to 100x, on an RTX 3080 DSD64 from 7.3x
 * to 19.7x, and 262144 adds nothing.
 *
 * The cost is the window one block reads, taps + (this - 1) * decimation
 * frames: about 4 MiB per channel at the deepest decimation this stage is
 * built for, so 25 MiB for six channels of DSD1024. */
#define RATE_DSD_BLOCK_FRAMES 65536u
#define RATE_DSD_BINDINGS 3u
#define RATE_DSD_LOCAL_SIZE 128u
#define RATE_DSD_FRAMES_PER_WORD 32u

/* Push constants. frame_offset is where output frame zero's window begins
 * within the uploaded words: the stage consumes whole words from its caller
 * but advances by decimation frames, so the remainder of that division lives
 * here and the stream's position stays exact to the frame. lead_frames and
 * valid_frames bound the frames the stream really has within that window; a
 * tap outside them reads a zero sample, not a bit.
 *
 * scale_high and scale_low carry the factor between this stage's natural
 * output (a normalised sample, since the bits are +-1) and whatever
 * domain the build's samples live in. It travels as a pair so the reference
 * profile can apply it before collapsing: scaling a collapsed double rounds
 * twice, once into the double and once into the product, and this stage's
 * whole claim is that it rounds no more than the result demands. The three
 * other forms round far above this anyway and take the same factor on the
 * host. Only the reference shader declares the pair; a shader may cover a
 * prefix of the range and the others stop at 40 bytes. */
typedef struct {
  uint32_t output_frames;
  uint32_t decimation;
  uint32_t taps;
  uint32_t channels;
  uint32_t words_per_channel;
  uint32_t frame_offset;
  uint32_t lead_frames;
  uint32_t valid_frames;
  uint32_t normalize;
  uint32_t reserved;
  double scale_high;
  double scale_low;
} parameters_t;

lsx_static_assert(sizeof(parameters_t) == 56, vulkan_rate_dsd_push_layout);

struct lsx_rate_dsd_vulkan {
  lsx_vulkan_context_t *vulkan;
  lsx_vulkan_buffer_t coefficients;
  lsx_vulkan_buffer_t input[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  lsx_vulkan_buffer_t output;
  /* Host-side staging for the device-local output. A DMA copy brings it
   * across, so the shader never writes over the bus. */
  lsx_vulkan_buffer_t output_staging;
  double *host_output;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_sets[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandBuffer command_buffers[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  VkFence fence;
  parameters_t parameters;
  uint32_t max_output_frames;
  uint32_t preload_words;
  uint32_t bank_index;
  sox_bool double_precision;
  sox_bool precise_fp32;
  sox_bool reference_dd;
};

static int vk_result(VkResult result, char const *operation)
{
  return lsx_vulkan_result(result, operation);
}

static size_t sample_size(lsx_rate_dsd_vulkan_t const *context)
{
  return context->reference_dd ?
      2u * sizeof(double) :
      context->precise_fp32 ?
      2u * sizeof(float) :
      context->double_precision ? sizeof(double) : sizeof(float);
}

static lsx_vulkan_resident_format_t sample_format(lsx_rate_dsd_vulkan_t const *context)
{
  return context->reference_dd ?
      lsx_vulkan_resident_format_f64x2 :
      context->precise_fp32 ?
      lsx_vulkan_resident_format_f32x2 :
      context->double_precision ?
      lsx_vulkan_resident_format_f64 :
      lsx_vulkan_resident_format_f32;
}

/* The factor between what this stage computes and what the chain carries.
 *
 * The stage sums coefficients against bits of +-1, so its output is a
 * normalised sample by construction, whatever the device. Half of SoX's
 * builds carry samples that way and half carry them scaled to SOX_SAMPLE_MAX,
 * effects_i_dsp.c has both, and lsx_sample_values_are_normalized() says
 * which one this build took. On a scaled build the stage's output is about
 * 2^31 times too small and lsx_save_samples() rounds every sample of it to
 * zero: correct arithmetic, silent file. That is what a Windows build did
 * while macOS was fine.
 *
 * The push constant named `normalize` cannot serve here: it divides, existing
 * for paths whose device data is already scaled. This has to multiply. */
static double output_scale(void)
{
  return lsx_sample_values_are_normalized() ? 1. : (double)SOX_SAMPLE_MAX;
}

/* A pointer to count output samples as host doubles. Plain FP64 is returned
 * in place, the mapping already being doubles; the other forms are converted
 * into the scratch buffer, the paired ones collapsing through the shared
 * routine so that every collapse in the engine agrees. */
static double const *host_samples(lsx_rate_dsd_vulkan_t *context, size_t count)
{
  double const scale = output_scale();
  size_t index;

  if (context->reference_dd) {
    /* The pairs go out whole before either half is thrown away. This is the
     * only place in a DSD plan where one filter's output exists as a pair and
     * as nothing else, and it is what a measurement past the double's own
     * ~320 dB has to read: chain-out.dd holds this stage's collapses mixed
     * with the trailing half-band's, which no reader can separate. */
    lsx_diagnostics_capture_dsd_dd((double const *)context->output_staging.mapped, count);
    for (index = 0; index < count; ++index) {
      double const *value = (double const *)context->output_staging.mapped + 2u * index;

      /* Already scaled on the device, in the pair. */
      context->host_output[index] =
          lsx_vulkan_collapse_pair(value[0], value[1]);
    }
    return context->host_output;
  }
  if (context->double_precision) {
    double const *source = context->output_staging.mapped;

    if (scale == 1.)
      return source;
    for (index = 0; index < count; ++index)
      context->host_output[index] = source[index] * scale;
    return context->host_output;
  }
  if (context->precise_fp32) {
    for (index = 0; index < count; ++index) {
      float const *value = (float const *)context->output_staging.mapped + 2u * index;

      context->host_output[index] = ((double)value[0] + (double)value[1]) * scale;
    }
    return context->host_output;
  }
  for (index = 0; index < count; ++index)
    context->host_output[index] =
        (double)((float const *)context->output_staging.mapped)[index] * scale;
  return context->host_output;
}

static int create_buffers(lsx_rate_dsd_vulkan_t *context)
{
  VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkDeviceSize sample_bytes = sample_size(context);
  VkDeviceSize coefficient_size = (VkDeviceSize)context->parameters.taps * sample_bytes;
  VkDeviceSize input_size =
      (VkDeviceSize)context->parameters.words_per_channel *
      context->parameters.channels * sizeof(uint32_t);
  VkDeviceSize output_size = (VkDeviceSize)context->max_output_frames * context->parameters.channels * sample_bytes;
  uint32_t index;

  if (lsx_vulkan_buffer_create(context->vulkan, &context->coefficients, coefficient_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      lsx_vulkan_buffer_create(context->vulkan, &context->output, output_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS ||
      lsx_vulkan_buffer_create(context->vulkan, &context->output_staging, output_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, memory) != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    if (lsx_vulkan_buffer_create(context->vulkan, &context->input[index], input_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory) != SOX_SUCCESS)
      return SOX_EOF;
  /* The scratch buffer is also what a scaled build needs for the plain FP64
   * form, which would otherwise be handed back in place. */
  if (context->reference_dd || !context->double_precision ||
      output_scale() != 1.)
    context->host_output = lsx_malloc(
        (size_t)context->max_output_frames *
        context->parameters.channels * sizeof(*context->host_output));
  return SOX_SUCCESS;
}

/* Lay the coefficients out for the shader and hand them to the device. This
 * runs after the command buffers exist because device-local memory is reached
 * only through a copy, and the copy needs one to be recorded in. */
static int upload_coefficients(lsx_rate_dsd_vulkan_t *context, double const *coefficients)
{
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  VkCommandBuffer command_buffer = context->command_buffers[0];
  lsx_vulkan_buffer_t staging;
  VkBufferCopy copy;
  VkDeviceSize size = context->coefficients.size;
  uint32_t tap;
  int result = SOX_EOF;

  if (lsx_vulkan_buffer_create(
      context->vulkan, &staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  for (tap = 0; tap < context->parameters.taps; ++tap) {
    double value = coefficients[tap];

    if (context->reference_dd) {
      double *target = (double *)staging.mapped + 2u * tap;

      target[0] = value;
      target[1] = 0.;
    }
    else if (context->precise_fp32) {
      float high = (float)value;
      float *target = (float *)staging.mapped + 2u * tap;

      target[0] = high;
      target[1] = (float)(value - (double)high);
    }
    else if (context->double_precision)
      ((double *)staging.mapped)[tap] = value;
    else
      ((float *)staging.mapped)[tap] = (float)value;
  }
  copy.srcOffset = copy.dstOffset = 0;
  copy.size = size;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = context->coefficients.buffer;
  barrier.offset = 0;
  barrier.size = size;
  if (vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer rate dsd coefficients") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer rate dsd coefficients") != SOX_SUCCESS)
    goto done;
  vkCmdCopyBuffer(command_buffer, staging.buffer, context->coefficients.buffer, 1, &copy);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer rate dsd coefficients") != SOX_SUCCESS ||
      lsx_vulkan_submit_and_wait(context->vulkan, command_buffer, context->fence, lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
    goto done;
  result = SOX_SUCCESS;
done:
  lsx_vulkan_buffer_destroy(context->vulkan, &staging);
  return result;
}

static int create_pipeline(lsx_rate_dsd_vulkan_t *context)
{
  VkDescriptorSetLayoutBinding bindings[RATE_DSD_BINDINGS];
  VkDescriptorSetLayoutCreateInfo descriptor_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters_t)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RATE_DSD_BINDINGS};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  VkDescriptorSetAllocateInfo allocation = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  VkDescriptorSetLayout layouts[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  uint32_t const *kernel_spirv;
  size_t kernel_size;
  uint32_t index;

  memset(bindings, 0, sizeof(bindings));
  for (index = 0; index < RATE_DSD_BINDINGS; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  descriptor_info.bindingCount = RATE_DSD_BINDINGS;
  descriptor_info.pBindings = bindings;
  if (vk_result(vkCreateDescriptorSetLayout(context->vulkan->device, &descriptor_info, NULL, &context->descriptor_layout), "vkCreateDescriptorSetLayout rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &context->descriptor_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vk_result(vkCreatePipelineLayout(context->vulkan->device, &layout_info, NULL, &context->pipeline_layout), "vkCreatePipelineLayout rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  /* Pick the kernel once, so its SPIR-V blob and the size passed with it can
   * never disagree. Only the first test is order-sensitive: reference_dd is
   * set as double_precision && profile == reference, so it has to come before
   * the plain FP64 family; precise_fp32 is set only when double precision is
   * unavailable. */
  if (context->reference_dd) {
    kernel_spirv = rate_dsd_fir_reference_dd_spv;
    kernel_size = sizeof(rate_dsd_fir_reference_dd_spv);
  } else if (context->double_precision) {
    kernel_spirv = rate_dsd_fir_f64_spv;
    kernel_size = sizeof(rate_dsd_fir_f64_spv);
  } else if (context->precise_fp32) {
    kernel_spirv = rate_dsd_fir_precise_f32_spv;
    kernel_size = sizeof(rate_dsd_fir_precise_f32_spv);
  } else {
    kernel_spirv = rate_dsd_fir_f32_spv;
    kernel_size = sizeof(rate_dsd_fir_f32_spv);
  }
  if (lsx_vulkan_create_compute_pipeline(context->vulkan, kernel_spirv, kernel_size, context->pipeline_layout, &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  pool_size.descriptorCount *= LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.maxSets = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vk_result(vkCreateDescriptorPool(context->vulkan->device, &pool_info, NULL, &context->descriptor_pool), "vkCreateDescriptorPool rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    layouts[index] = context->descriptor_layout;
  allocation.descriptorPool = context->descriptor_pool;
  allocation.descriptorSetCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  allocation.pSetLayouts = layouts;
  if (vk_result(vkAllocateDescriptorSets(context->vulkan->device, &allocation, context->descriptor_sets), "vkAllocateDescriptorSets rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  /* Each bank binds its own input buffer and shares the coefficients and the
   * output, which is the same division polyphase makes: the input is what the
   * host is still writing while an earlier block is in flight. */
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index) {
    VkDescriptorBufferInfo buffer_info[RATE_DSD_BINDINGS];
    VkWriteDescriptorSet writes[RATE_DSD_BINDINGS];
    lsx_vulkan_buffer_t *buffers[RATE_DSD_BINDINGS];
    uint32_t binding;

    buffers[0] = &context->coefficients;
    buffers[1] = &context->input[index];
    buffers[2] = &context->output;
    memset(writes, 0, sizeof(writes));
    for (binding = 0; binding < RATE_DSD_BINDINGS; ++binding) {
      buffer_info[binding].buffer = buffers[binding]->buffer;
      buffer_info[binding].offset = 0;
      buffer_info[binding].range = buffers[binding]->size;
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = context->descriptor_sets[index];
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[binding].pBufferInfo = &buffer_info[binding];
    }
    vkUpdateDescriptorSets(context->vulkan->device, RATE_DSD_BINDINGS, writes, 0, NULL);
  }
  return SOX_SUCCESS;
}

static int create_commands(lsx_rate_dsd_vulkan_t *context)
{
  VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

  allocation.commandPool = context->vulkan->command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = LSX_VULKAN_RESIDENT_BATCH_DEPTH;
  if (vk_result(vkAllocateCommandBuffers(context->vulkan->device, &allocation, context->command_buffers), "vkAllocateCommandBuffers rate dsd") != SOX_SUCCESS ||
      vk_result(vkCreateFence(context->vulkan->device, &fence_info, NULL, &context->fence), "vkCreateFence rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

lsx_rate_dsd_vulkan_t *lsx_rate_dsd_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients, uint32_t taps,
    uint32_t decimation, uint32_t channels, uint32_t preload_frames)
{
  lsx_rate_dsd_vulkan_t *context;
  uint64_t window_frames;
  uint64_t window_words;
  uint32_t preload_words;

  if (!vulkan || (!vulkan->shader_float64 &&
      vulkan->profile != sox_vulkan_profile_fast &&
      vulkan->profile != sox_vulkan_profile_precise) ||
      !coefficients || !taps || !decimation || !channels ||
      taps < decimation || preload_frames >= taps)
    return NULL;
  preload_words = (preload_frames + RATE_DSD_FRAMES_PER_WORD - 1u) / RATE_DSD_FRAMES_PER_WORD;
  /* The window one full block reads: the last output's taps, everything the
   * outputs before it stepped over, and the sub-word offset the stage may be
   * carrying. Rounded up to whole words, since words are what arrives. */
  window_frames = (uint64_t)taps +
      (uint64_t)(RATE_DSD_BLOCK_FRAMES - 1u) * decimation +
      (RATE_DSD_FRAMES_PER_WORD - 1u);
  window_words = (window_frames + RATE_DSD_FRAMES_PER_WORD - 1u) / RATE_DSD_FRAMES_PER_WORD;
  if (window_words > UINT32_MAX / channels)
    return NULL;
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->double_precision = vulkan->use_float64;
  context->reference_dd = context->double_precision && vulkan->profile == sox_vulkan_profile_reference;
  context->precise_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_precise;
  context->parameters.decimation = decimation;
  context->parameters.taps = taps;
  context->parameters.channels = channels;
  context->parameters.words_per_channel = (uint32_t)window_words;
  context->max_output_frames = RATE_DSD_BLOCK_FRAMES;
  /* The silence in front of the stream arrives as whole words, but the delay
   * it stands in for is a frame count. The difference between the two is the
   * position output frame zero starts at, which is what keeps the alignment
   * exact when the delay is not a multiple of thirty-two. */
  context->preload_words = preload_words;
  context->parameters.lead_frames = preload_words * RATE_DSD_FRAMES_PER_WORD;
  /* SOX_SAMPLE_MAX is exactly a double, so the pair is the factor and a zero
   * low word; the shader's multiply then rounds once, at the end. */
  context->parameters.scale_high = output_scale();
  context->parameters.scale_low = 0.;
  context->parameters.frame_offset =
      preload_words * RATE_DSD_FRAMES_PER_WORD - preload_frames;
  if (create_buffers(context) != SOX_SUCCESS ||
      create_pipeline(context) != SOX_SUCCESS ||
      create_commands(context) != SOX_SUCCESS ||
      upload_coefficients(context, coefficients) != SOX_SUCCESS)
    goto error;
  lsx_report(
      "Vulkan rate fused DSD: 1/%u, %u taps, %u channel%s, %s",
      decimation, taps, channels, channels == 1u ? "" : "s",
      context->reference_dd ? "FP64x2" :
      context->double_precision ? "FP64" :
      context->precise_fp32 ? "FP32x2" : "FP32");
  return context;

error: lsx_rate_dsd_vulkan_destroy(context);
  return NULL;
}

void lsx_rate_dsd_vulkan_destroy(lsx_rate_dsd_vulkan_t *context)
{
  uint32_t index;

  if (!context)
    return;
  vkDeviceWaitIdle(context->vulkan->device);
  if (context->fence)
    vkDestroyFence(context->vulkan->device, context->fence, NULL);
  if (context->command_buffers[0])
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, LSX_VULKAN_RESIDENT_BATCH_DEPTH, context->command_buffers);
  if (context->pipeline)
    vkDestroyPipeline(context->vulkan->device, context->pipeline, NULL);
  if (context->pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->pipeline_layout, NULL);
  if (context->descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->descriptor_pool, NULL);
  if (context->descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->descriptor_layout, NULL);
  for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
    lsx_vulkan_buffer_destroy(context->vulkan, &context->input[index]);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->output_staging);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->output);
  lsx_vulkan_buffer_destroy(context->vulkan, &context->coefficients);
  free(context->host_output);
  free(context);
}

size_t lsx_rate_dsd_vulkan_block_frames(void)
{
  return RATE_DSD_BLOCK_FRAMES;
}

uint32_t lsx_rate_dsd_vulkan_taps(lsx_rate_dsd_vulkan_t const *context)
{
  return context ? context->parameters.taps : 0;
}

size_t lsx_rate_dsd_vulkan_window_words(lsx_rate_dsd_vulkan_t const *context)
{
  return context ? context->parameters.words_per_channel : 0;
}

uint32_t lsx_rate_dsd_vulkan_preload_words(lsx_rate_dsd_vulkan_t const *context)
{
  return context ? context->preload_words : 0;
}

/* How many outputs the words in hand can complete, how many of those words
 * the dispatch will read, and how many of them it may treat as real.
 *
 * An output's window is taps frames long and starts decimation frames after
 * the one before it, so mid-stream the count is a floor division and never
 * rounds up: an output whose window runs past the words supplied has to wait
 * for the next call, when the same words will still be there. Flushing drops
 * that condition and the shader reads the missing frames as silence instead. */
static uint32_t available_outputs(
    lsx_rate_dsd_vulkan_t *context, size_t *available_words, sox_bool flush,
    uint32_t *needed_words)
{
  uint64_t available_frames;
  uint64_t first = context->parameters.frame_offset;
  uint64_t span;
  uint64_t count;

  *available_words = min(*available_words, (size_t)context->parameters.words_per_channel);
  available_frames = (uint64_t)*available_words * RATE_DSD_FRAMES_PER_WORD;
  if (flush)
    count = context->max_output_frames;
  else {
    if (available_frames < first + context->parameters.taps)
      return 0;
    count = (available_frames - first - context->parameters.taps) /
        context->parameters.decimation + 1u;
    if (count > context->max_output_frames)
      count = context->max_output_frames;
  }
  span = first + (count - 1u) * context->parameters.decimation +
      context->parameters.taps;
  *needed_words = (uint32_t)min(
      (span + RATE_DSD_FRAMES_PER_WORD - 1u) / RATE_DSD_FRAMES_PER_WORD,
      (uint64_t)*available_words);
  context->parameters.valid_frames = (uint32_t)available_frames;
  return (uint32_t)count;
}

/* Stage one block's worth of words into the bank the next dispatch will read,
 * gathering the caller's per-channel runs into the device's fixed stride. */
static void upload_words(
    lsx_rate_dsd_vulkan_t *context, uint32_t bank,
    uint32_t const *const *words, uint32_t needed_words)
{
  uint32_t *target = context->input[bank].mapped;
  uint32_t channel;

  for (channel = 0; channel < context->parameters.channels; ++channel)
    memcpy(target + (size_t)channel * context->parameters.words_per_channel,
        words[channel], needed_words * sizeof(*target));
}

/* Record one dispatch into the given bank. Everything before the submission
 * is common to the two entry points below, which differ only in whether they
 * wait for the result or hand it on. */
/* to_host adds the copy into the staging buffer; a resident caller leaves the
 * samples on the device and does not want it. */
static int record_block(
    lsx_rate_dsd_vulkan_t *context, uint32_t bank, uint32_t count,
    sox_bool normalize, sox_bool to_host)
{
  VkBufferCopy output_copy;
  VkMemoryBarrier host_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  VkCommandBuffer command_buffer = context->command_buffers[bank];
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
  VkMemoryBarrier input_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
  VkMemoryBarrier output_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};

  context->parameters.output_frames = count;
  context->parameters.normalize =
      normalize && !lsx_sample_values_are_normalized() ? 1u : 0u;
  if (vk_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer rate dsd") != SOX_SUCCESS ||
      vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  lsx_vulkan_label_begin(context->vulkan, command_buffer, "Rate fused DSD");
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input_barrier, 0, NULL, 0, NULL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline_layout, 0, 1, &context->descriptor_sets[bank], 0, NULL);
  vkCmdPushConstants(command_buffer, context->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(context->parameters), &context->parameters);
  vkCmdDispatch(command_buffer, (count + RATE_DSD_LOCAL_SIZE - 1u) / RATE_DSD_LOCAL_SIZE, context->parameters.channels, 1);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &output_barrier, 0, NULL, 0, NULL);
  if (to_host) {
    output_copy.srcOffset = output_copy.dstOffset = 0;
    output_copy.size = (VkDeviceSize)count * context->parameters.channels * sample_size(context);
    vkCmdCopyBuffer(command_buffer, context->output.buffer, context->output_staging.buffer, 1, &output_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, NULL, 0, NULL);
  }
  lsx_vulkan_label_end(context->vulkan, command_buffer);
  if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer rate dsd") != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

/* Advance the stream past the frames this block consumed, reporting the whole
 * words the caller may drop and keeping the sub-word remainder here. The
 * silence in front of the stream is consumed by the same movement, so
 * lead_frames retires once and never has to be reset. */
static void advance(
    lsx_rate_dsd_vulkan_t *context, uint32_t count, size_t available_words,
    size_t *consumed_words)
{
  uint64_t position = (uint64_t)context->parameters.frame_offset +
      (uint64_t)count * context->parameters.decimation;
  uint64_t words = position / RATE_DSD_FRAMES_PER_WORD;
  uint64_t retired;

  if (words > available_words)
    words = available_words;
  retired = words * RATE_DSD_FRAMES_PER_WORD;
  *consumed_words = (size_t)words;
  context->parameters.frame_offset =
      (uint32_t)(position - retired);
  context->parameters.lead_frames = context->parameters.lead_frames > retired ?
      (uint32_t)(context->parameters.lead_frames - retired) : 0u;
}

int lsx_rate_dsd_vulkan_process(
    lsx_rate_dsd_vulkan_t *context, uint32_t const *const *words,
    size_t available_words, sox_bool flush, double const **output,
    size_t *output_frames, size_t *consumed_words)
{
  uint32_t needed_words = 0;
  uint32_t count;

  if (!context || !words || !output || !output_frames || !consumed_words)
    return SOX_EOF;
  count = available_outputs(context, &available_words, flush, &needed_words);
  if (!count) {
    *output = NULL;
    *output_frames = 0;
    *consumed_words = 0;
    return SOX_SUCCESS;
  }
  upload_words(context, 0, words, needed_words);
  if (vk_result(vkResetFences(context->vulkan->device, 1, &context->fence), "vkResetFences rate dsd") != SOX_SUCCESS ||
      record_block(context, 0, count, sox_false, sox_true) != SOX_SUCCESS ||
      lsx_vulkan_submit_and_wait(context->vulkan, context->command_buffers[0], context->fence, lsx_vulkan_wait_rate_synchronous) != SOX_SUCCESS)
    return SOX_EOF;
  *output = host_samples(context, (size_t)count * context->parameters.channels);
  *output_frames = count;
  advance(context, count, available_words, consumed_words);
  return SOX_SUCCESS;
}

int lsx_rate_dsd_vulkan_process_resident(
    lsx_rate_dsd_vulkan_t *context, uint32_t const *const *words,
    size_t available_words, sox_bool flush, size_t *output_frames,
    size_t *consumed_words, sox_rate_t rate,
    lsx_vulkan_resident_state_t state, sox_bool normalize,
    lsx_vulkan_resident_buffer_t *resident)
{
  uint32_t needed_words = 0;
  uint32_t bank;
  uint32_t count;

  if (!context || !words || !output_frames || !consumed_words || !resident || rate <= 0)
    return SOX_EOF;
  count = available_outputs(context, &available_words, flush, &needed_words);
  if (!count) {
    *output_frames = 0;
    *consumed_words = 0;
    memset(resident, 0, sizeof(*resident));
    return SOX_SUCCESS;
  }
  bank = context->bank_index;
  upload_words(context, bank, words, needed_words);
  if (record_block(context, bank, count, normalize, sox_false) != SOX_SUCCESS)
    return SOX_EOF;
  memset(resident, 0, sizeof(*resident));
  resident->buffer = &context->output;
  resident->owner = context;
  resident->producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = context->max_output_frames;
  resident->valid_elements = count;
  resident->block_elements = context->max_output_frames;
  resident->frame_stride_elements = context->parameters.channels;
  resident->channel_stride_elements = 1u;
  resident->rate = rate;
  resident->channels = context->parameters.channels;
  resident->frames_per_element = 1u;
  resident->format = sample_format(context);
  resident->domain = context->parameters.normalize ?
      lsx_vulkan_resident_domain_normalized :
      lsx_vulkan_resident_domain_sox_sample;
  resident->layout = lsx_vulkan_resident_layout_interleaved;
  resident->state = state;
  if (lsx_vulkan_resident_buffer_validate(resident) != SOX_SUCCESS ||
      lsx_vulkan_enqueue(context->vulkan, context->command_buffers[bank]) != SOX_SUCCESS)
    return SOX_EOF;
  context->bank_index = (bank + 1u) % lsx_vulkan_resident_batch_depth(context->vulkan);
  *output_frames = count;
  advance(context, count, available_words, consumed_words);
  return SOX_SUCCESS;
}
