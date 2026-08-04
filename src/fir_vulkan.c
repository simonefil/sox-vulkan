/* Partitioned VkFFT FIR backend for SoX.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "sox_i.h"
#include "fir_vulkan.h"
#include "diagnostics.h"
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
#include "fir_partition_precise_f32_spv.inc"
#include "fir_partition_precise_f64_spv.inc"
#include "fir_partition_reference_dd_spv.inc"
#include "fir_spectrum_multiply_reference_dd_spv.inc"

/* Transform sizes, and the block size that follows from each: overlap-save
 * needs a transform twice the block, so the block is always half.
 *
 * The fast profile uses the larger one because a longer block means fewer
 * partitions and fewer transforms per sample; the other profiles use the
 * smaller because their per-element cost is higher, in memory as well as
 * arithmetic, and a 128k transform of double-doubles is far more than the
 * accuracy is worth.  Both are powers of two, which is what VkFFT is
 * fastest on. */
#define FIR_DEFAULT_FFT_SIZE 32768u
#define FIR_FAST_FFT_SIZE 131072u
#define FIR_DEFAULT_BLOCK_FRAMES (FIR_DEFAULT_FFT_SIZE / 2u)
#define FIR_FAST_BLOCK_FRAMES (FIR_FAST_FFT_SIZE / 2u)
/* Both read context, so they are only usable where one is in scope; they
 * exist because these two appear in nearly every size expression here. */
#define FIR_FFT_SIZE (context->fft_size)
#define FIR_BLOCK_FRAMES (context->block_frames)
/* Workgroup size, matching the local_size_x the shaders declare. */
#define FIR_LOCAL_SIZE 256u

typedef lsx_vulkan_buffer_t buffer_t;

/* Push constants for the partition-accumulate shader.  The layouts are fixed
 * by the shaders, hence the assertions below: a mismatch would misread every
 * field rather than fail, and the two declarations are in different
 * languages, so no compiler checks them against each other. */
typedef struct {
  uint32_t spectrum_bins;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;       /* Which history slot holds the newest block. */
} partition_parameters_t;

/* Push constants for the precise FP32 shader, which does more than accumulate:
 * it runs the transform too, hence operation and stage, and ping-pongs
 * between two buffers, hence source_is_a. */
typedef struct {
  uint32_t operation;
  uint32_t stage;
  uint32_t partitions;
  uint32_t channels;
  uint32_t current_slot;
  uint32_t inverse;
  uint32_t source_is_a;
} precise_fp32_parameters_t;

lsx_static_assert(sizeof(partition_parameters_t) == 16, vulkan_fir_partition_push_layout);
lsx_static_assert(sizeof(precise_fp32_parameters_t) == 28, vulkan_fir_precise_fp32_push_layout);

struct lsx_fir_vulkan {
  lsx_vulkan_context_t *vulkan;  /* Not owned; shared with the rest of the chain. */
  lsx_vulkan_fft_t *fft;         /* NULL for precise FP32, which transforms itself. */
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;

  /* One command buffer for setup and flushes, plus banks of pre-recorded ones
   * for the steady state.  A block's work depends only on which history slot
   * is current, so the whole rotation is recorded once at startup and then
   * replayed; the resident bank is recorded lazily, since a chain that never
   * goes resident should not pay for it. */
  VkCommandBuffer command_buffer;
  VkCommandBuffer *process_commands;
  VkCommandBuffer *resident_process_commands;
  VkFence fence;

  /* The spectrum of the current block, and the output the inverse transform
   * lands in.  Also what a resident consumer reads. */
  buffer_t working;
  buffer_t working_scratch;      /* Precise FP32 only: the ping-pong partner. */

  /* Banks of `partitions` spectra each.  history is a ring of past input
   * blocks, kernels the transformed response.  Their layouts match, so the
   * accumulation walks both with one index. */
  buffer_t history;
  buffer_t kernels;

  buffer_t twiddles;             /* Precise FP32: split-precision transform factors. */
  buffer_t precise_output;        /* Precise FP32: output, kept apart from working. */

  buffer_t upload;               /* Staging for the synchronous path. */
  /* One staging buffer per block in flight, so preparing the next block does
   * not overwrite one the device has not yet read. */
  buffer_t resident_upload[LSX_VULKAN_RESIDENT_BATCH_DEPTH];
  buffer_t download;

  void *working_host;            /* Host-side scratch the input is assembled in. */
  double *previous;              /* Last block per channel: the overlap-save history. */
  double *output;                /* Interleaved result handed back by _process. */

  size_t element_size;           /* Bytes per real value, per the strategy. */

  /* Which numerical strategy is in force.  Set once at creation from the
   * context's profile; several may be true at once, and strategy_name states
   * the order in which they override one another. */
  sox_bool double_precision;
  sox_bool precise_fp32;
  sox_bool authoritative_fp64_kernels;
  sox_bool precise_fp64;
  sox_bool reference_dd;

  uint32_t taps;
  uint32_t fft_size;
  uint32_t block_frames;
  uint32_t partitions;           /* taps rounded up to whole blocks. */
  uint32_t channels;
  uint32_t current_slot;         /* Newest history slot; advances per block. */
  uint32_t resident_bank_index;  /* Which staging buffer and command bank is next. */

  double startup_seconds;
  double process_seconds;
  uint64_t process_calls;
};

/* Element format the resident output carries.  rate_vulkan.c and
 * rate_polyphase_vulkan.c name the same rule; the precise profile is checked
 * first here because it emits an FP32 pair while still being an FP32 build. */
static lsx_vulkan_resident_format_t resident_format(lsx_fir_vulkan_t const *context)
{
  if (context->precise_fp32)
    return lsx_vulkan_resident_format_f32x2;
  if (context->reference_dd)
    return lsx_vulkan_resident_format_f64x2;
  return context->double_precision ? lsx_vulkan_resident_format_f64 : lsx_vulkan_resident_format_f32;
}

/* One line of -V3 output describing how this context computes the transform,
 * the coefficient spectra and the accumulation.  The order is the order the
 * flags override one another, so the first match is the strategy in force. */
static char const *strategy_name(lsx_fir_vulkan_t const *context)
{
  if (context->precise_fp32)
    return "double-single FFT + split twiddles + double-single accumulation";
  if (context->reference_dd)
    return "FP64 double-double FFT + double-double memory/product/accumulation";
  if (context->authoritative_fp64_kernels)
    return context->precise_fp64 ?
        "FP64 FFT + authoritative coefficient spectra + compensated FP64 accumulation" :
        "FP64 FFT + authoritative coefficient spectra + FP64 accumulation";
  if (context->precise_fp64)
    return "FP64 FFT + compensated FP64 accumulation";
  return context->double_precision ? "FP64 FFT + FP64 accumulation" : "FP32 FFT + FP32 accumulation";
}

/* Monotonic clock for the -V3 timings; 0 if unavailable.  Duplicated from
 * vulkan_engine.c rather than shared, so that timing this backend does not
 * require a context. */
static double monotonic_seconds(void)
{
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter))
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
  return lsx_vulkan_buffer_create(context->vulkan, buffer, size, usage, properties);
}

static void destroy_buffer(lsx_fir_vulkan_t *context, buffer_t *buffer)
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
  if (vk_result(vkEndCommandBuffer(context->command_buffer), "vkEndCommandBuffer") != SOX_SUCCESS)
    return SOX_EOF;
  return lsx_vulkan_submit_and_wait(context->vulkan, context->command_buffer, context->fence, reason);
}

/* A global memory barrier between two stages.  Global rather than per buffer:
 * a step here typically touches several of the buffers at once -- working,
 * history, kernels -- and enumerating them would add nothing, since they are
 * all being made visible to the same following stage anyway. */
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

  vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 1, &barrier, 0, NULL, 0, NULL);
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

/* Collapse a cascade of reference-profile FIR responses into one response
 * without first rounding it to FP64.
 *
 * The operation has five phases: derive the exact linear-convolution size;
 * allocate a temporary double-double transform workspace; transform every
 * response and multiply its spectrum into the accumulator; inverse-transform
 * the product; then split the retained time-domain response into caller-owned
 * high and low arrays.  All Vulkan objects in this function are temporary and
 * are released through the single cleanup path, including partial setup.
 *
 * The frequency-domain product represents convolution only because the FFT
 * is at least as long as the final m+n-1 response.  That size invariant is
 * established before any allocation and is the reason no time-domain tail or
 * circular wrap is copied back. */
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

  /* Phase 1: establish the exact output length and the smallest power-of-two
   * transform that can contain it without circular aliasing. */
  /* Convolving responses of m and n taps gives m + n - 1, so a cascade of
   * several starts from one and adds each length less one. */
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
   * words, and measured nothing about the double-double route.
   *
   * Re-swept once the double-double route was real: 1x 618.27 dB, 4x 618.29,
   * 8x 622.32, 16x 624.77 -- the whole range inside 6.5 dB, on a profile
   * sitting nearly 300 dB above the FP64 representation floor. The setup
   * transform is meanwhile the whole of this profile's fixed cost, so the
   * qualified value is therefore one. */
  oversampling = 1u;
  if (combined_count > UINT32_MAX / oversampling)
    return SOX_EOF;
  transform_count = combined_count * oversampling;
  while (fft_size < transform_count) {
    if (fft_size > UINT32_MAX / 2u)
      return SOX_EOF;
    fft_size *= 2u;
  }
  spectrum_bins = fft_size / 2u + 1u;
  working_size = (VkDeviceSize)(fft_size + 2u) * 2u * sizeof(double);
  output_size = (VkDeviceSize)combined_count * 2u * sizeof(double);

  /* Phase 2: build a self-contained setup workspace.  working carries the
   * response currently being transformed, accumulated carries the spectral
   * product, and download receives only the valid time-domain response. */
  /* A stack context, zeroed, so the buffer and command helpers above can be
   * reused without building a whole FIR: this runs once at setup and has
   * nothing to do with the steady state. */
  memset(&scratch, 0, sizeof(scratch));
  scratch.vulkan = vulkan;
  if (create_commands(&scratch) != SOX_SUCCESS)
    goto cleanup;
  if (create_buffer(
      &scratch, &scratch.working, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    goto cleanup;
  if (create_buffer(
      &scratch, &accumulated, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    goto cleanup;
  if (create_buffer(
      &scratch, &scratch.upload, working_size,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    goto cleanup;
  if (create_buffer(
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

  /* Phase 3a: create the two-buffer multiply pipeline.  Binding 0 is the
   * newly transformed response and binding 1 is the product accumulated from
   * all earlier responses. */
  bindings[0].binding = 0;
  bindings[1].binding = 1;
  bindings[0].descriptorType = bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = bindings[1].descriptorCount = 1;
  bindings[0].stageFlags = bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
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
    writes[set_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[set_index].pBufferInfo = &buffer_infos[set_index];
  }
  vkUpdateDescriptorSets(vulkan->device, 2, writes, 0, NULL);
  full_copy.srcOffset = 0;
  full_copy.dstOffset = 0;
  full_copy.size = working_size;

  /* Phase 3b: transform each response.  The first seeds accumulated through
   * a copy; every later response is multiplied into it in double-double. */
  for (set_index = 0; set_index < set_count; ++set_index) {
    double *upload = scratch.upload.mapped;
    size_t tap_index;

    memset(upload, 0, (size_t)working_size);
    for (tap_index = 0; tap_index < tap_counts[set_index]; ++tap_index)
      upload[2u * tap_index] = coefficient_sets[set_index][tap_index];
    if (begin_commands(&scratch) != SOX_SUCCESS)
      goto cleanup;
    vkCmdCopyBuffer(scratch.command_buffer, scratch.upload.buffer, scratch.working.buffer, 1, &full_copy);
    memory_barrier(
        scratch.command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (lsx_vulkan_fft_append(fft, scratch.command_buffer, sox_false) != SOX_SUCCESS)
      goto cleanup;
    if (!set_index) {
      memory_barrier(
          scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(scratch.command_buffer, scratch.working.buffer, accumulated.buffer, 1, &full_copy);
    }
    else {
      memory_barrier(
          scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      vkCmdBindPipeline(scratch.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(
          scratch.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
          pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
      vkCmdPushConstants(
          scratch.command_buffer, pipeline_layout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0,
          sizeof(spectrum_bins), &spectrum_bins);
      vkCmdDispatch(scratch.command_buffer, (spectrum_bins + FIR_LOCAL_SIZE - 1u) / FIR_LOCAL_SIZE, 1, 1);
    }
    if (submit_commands(&scratch, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
      goto cleanup;
  }
  output_copy.srcOffset = 0;
  output_copy.dstOffset = 0;
  output_copy.size = output_size;

  /* Phase 4: return the spectral product to working, inverse-transform it and
   * download only combined_count values, excluding FFT padding. */
  if (begin_commands(&scratch) != SOX_SUCCESS)
    goto cleanup;
  memory_barrier(
      scratch.command_buffer,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBuffer(scratch.command_buffer, accumulated.buffer, scratch.working.buffer, 1, &full_copy);
  memory_barrier(
      scratch.command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  if (lsx_vulkan_fft_append(fft, scratch.command_buffer, sox_true) != SOX_SUCCESS)
    goto cleanup;
  memory_barrier(
      scratch.command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBuffer(scratch.command_buffer, scratch.working.buffer, download.buffer, 1, &output_copy);
  if (submit_commands(&scratch, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
    goto cleanup;

  /* Phase 5: transfer ownership through newly allocated high/low arrays. */
  *result_highs = lsx_malloc(
      combined_count * sizeof(**result_highs));
  *result_lows = lsx_malloc(
      combined_count * sizeof(**result_lows));
  for (set_index = 0; set_index < combined_count; ++set_index) {
    double const *pair = (double const *)download.mapped + 2u * set_index;

    (*result_highs)[set_index] = pair[0];
    (*result_lows)[set_index] = pair[1];
  }
  *result_count = combined_count;
  status = SOX_SUCCESS;
  lsx_report(
      "Vulkan REFERENCE spectral fusion: %lu filters, "
      "%lu taps, %u-point FP64x2 setup FFT",
      (unsigned long)set_count, (unsigned long)combined_count,
      fft_size);

/* Every failure and the success path converge here.  Result arrays survive
 * only success; Vulkan setup resources never escape this function. */
cleanup: if (status != SOX_SUCCESS) {
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
    vkFreeCommandBuffers(vulkan->device, vulkan->command_pool, 1, &scratch.command_buffer);
  return status;
}

/* Build the descriptor set, layout and pipeline for the accumulation shader.
 *
 * Each strategy needs a different set of buffers, so the binding count varies
 * and the buffers are assigned to bindings in a different order: the plain
 * routes bind working, history and kernels; precise binds six, its extra
 * scratch, twiddles and separate output reflecting that it runs the transform
 * as well as the accumulation.
 * The shaders declare matching binding numbers, which is what the buffer_info
 * ordering below has to agree with -- nothing checks it at compile time. */
static int create_partition_pipeline(lsx_fir_vulkan_t *context)
{
  uint32_t binding_count = context->precise_fp32 ? 6u : 3u;
  uint32_t const *partition_spirv;
  size_t partition_size;
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
      context->precise_fp32 ? context->working_scratch.buffer : context->history.buffer,
      0,
      context->precise_fp32 ? context->working_scratch.size : context->history.size
    },
    {
      context->precise_fp32 ? context->history.buffer : context->kernels.buffer,
      0,
      context->precise_fp32 ? context->history.size : context->kernels.size
    },
    {
      context->kernels.buffer,
      0,
      context->kernels.size
    },
    {context->twiddles.buffer, 0, context->twiddles.size},
    {context->precise_output.buffer, 0, context->precise_output.size}
  };
  VkWriteDescriptorSet writes[6];
  VkPushConstantRange push_range = {
    VK_SHADER_STAGE_COMPUTE_BIT,
    0,
    context->precise_fp32 ? sizeof(precise_fp32_parameters_t) : sizeof(partition_parameters_t)
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
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(context->vulkan->device, binding_count, writes, 0, NULL);
  pipeline_layout_info.pSetLayouts = &context->descriptor_layout;
  /* Pick the partition kernel once, so the SPIR-V blob and its size can never
   * disagree.  Selection order matters: reference_dd and precise_fp64 both
   * imply double_precision, and precise_fp64 is also set for reference, so the
   * reference test has to come first. */
  if (context->double_precision) {
    if (context->reference_dd)
      partition_spirv = fir_partition_reference_dd_spv, partition_size = sizeof(fir_partition_reference_dd_spv);
    else if (context->precise_fp64)
      partition_spirv = fir_partition_precise_f64_spv, partition_size = sizeof(fir_partition_precise_f64_spv);
    else
      partition_spirv = fir_partition_f64_spv, partition_size = sizeof(fir_partition_f64_spv);
  } else {
    if (context->precise_fp32)
      partition_spirv = fir_partition_precise_f32_spv, partition_size = sizeof(fir_partition_precise_f32_spv);
    else
      partition_spirv = fir_partition_f32_spv, partition_size = sizeof(fir_partition_f32_spv);
  }
  if (vk_result(vkCreatePipelineLayout(
      context->vulkan->device, &pipeline_layout_info, NULL,
      &context->pipeline_layout),
      "vkCreatePipelineLayout") != SOX_SUCCESS ||
      lsx_vulkan_create_compute_pipeline(
      context->vulkan, partition_spirv, partition_size,
      context->pipeline_layout,
      &context->pipeline) != SOX_SUCCESS)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int initialize_fft(lsx_fir_vulkan_t *context)
{
  /* precise on a device without shaderFloat64 is the one route that cannot
   * use VkFFT: the library's precision ladder is half, float, double and
   * double-double, with no FP32x2 mode, and doublePrecisionFloatMemory still
   * needs FP64 in the shader.  On Apple Silicon, where Metal reports
   * shaderFloat64 = false, VkFFT can therefore only offer the FP32 that
   * precise exists to beat, so the profile runs its own double-single
   * transform with precomputed split twiddles instead.  Every other route,
   * precise on an FP64 device included, goes through VkFFT. */
  if (context->precise_fp32)
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

/* Split a double into an unevaluated sum of two floats.  The high word is the
 * value rounded to single precision and the low word the remainder, which is
 * itself exactly representable: together they carry about 48 bits of
 * significand where one float carries 24.  This is what lets the precise
 * profile exceed FP32 on a device with no double precision at all. */
static void store_double_single(float *target, size_t index, double value)
{
  float high = (float)value;

  target[index] = high;
  target[index + 1u] = (float)(value - (double)high);
}

/* Precompute the transform's twiddle factors as split-precision pairs.
 *
 * The precise profile's shader has no double precision to compute them with,
 * and recomputing a cosine in float would put an error into every butterfly
 * that no amount of careful accumulation afterwards could remove.  They are
 * therefore evaluated once here in host double precision and stored as pairs,
 * four floats per factor: high and low of the cosine, then of the sine. */
static int initialize_precise_fp32_twiddles(lsx_fir_vulkan_t *context)
{
  float *upload = context->upload.mapped;
  VkBufferCopy copy = {
    0, 0,
    (VkDeviceSize)(FIR_FFT_SIZE / 2u) * 4u * sizeof(*upload)
  };
  uint32_t index;

  memset(upload, 0, (size_t)copy.size);
  for (index = 0; index < FIR_FFT_SIZE / 2u; ++index) {
    double angle = 2.0 * acos(-1.0) * (double)index / (double)FIR_FFT_SIZE;

    store_double_single(upload, (size_t)index * 4u, cos(angle));
    store_double_single(upload, (size_t)index * 4u + 2u, sin(angle));
  }
  if (begin_commands(context) != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdCopyBuffer(context->command_buffer, context->upload.buffer, context->twiddles.buffer, 1, &copy);
  return submit_commands(context, lsx_vulkan_wait_fir_setup);
}

/* The response for one channel, collapsing the shared and per-channel cases:
 * a single set is used for every channel.  Returning the same pointer for
 * every channel is also what lets the kernel setup below notice that a
 * spectrum has already been computed and copy it instead. */
static double const *channel_coefficients(
    double const *const *coefficients,
    uint32_t coefficient_channels, uint32_t channel)
{
  return coefficients[coefficient_channels == 1u ? 0u : channel];
}

/* Transform each partition of the response and upload it as the kernel bank.
 *
 * The spectra are computed on the host, in double precision, with SoX's own
 * real transform: this happens once at setup, so its cost does not matter,
 * and doing it in double and splitting afterwards gives coefficients as
 * accurate as the pair format can hold -- which is not what running the
 * device's own FP32 transform over them would give.
 *
 * The kernels are stored over the full transform length rather than as a half
 * spectrum, because the precise shader's own transform works on the whole
 * range: the missing half is filled by conjugate symmetry, mirroring the bin
 * index and negating the imaginary part.  The sign of the imaginary part is
 * flipped once for the transform's own convention and again for the mirrored
 * half, which is why it is negated in two places.
 *
 * One partition at a time, through the single staging buffer, since the whole
 * bank is far larger than staging and setup has no need to overlap. */
static int initialize_precise_fp32_kernels(
    lsx_fir_vulkan_t *context,
    double const *const *coefficients,
    uint32_t coefficient_channels, size_t taps)
{
  double *spectra = lsx_calloc((size_t)context->channels * FIR_FFT_SIZE, sizeof(*spectra));
  uint32_t partition;

  if (initialize_precise_fp32_twiddles(context) != SOX_SUCCESS) {
    free(spectra);
    return SOX_EOF;
  }
  for (partition = 0; partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min((size_t)FIR_BLOCK_FRAMES, taps - first);
    float *upload = context->upload.mapped;
    VkBufferCopy copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };
    uint32_t channel;

    memset(upload, 0, (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      double const *source = channel_coefficients(coefficients, coefficient_channels, channel);
      double *spectrum = spectra + (size_t)channel * FIR_FFT_SIZE;
      uint32_t bin;

      /* Channels sharing a response share a spectrum, so the transform is
       * done once rather than per channel. */
      if (channel && source == channel_coefficients(coefficients, coefficient_channels, channel - 1u))
        memcpy(spectrum, spectrum - FIR_FFT_SIZE, FIR_FFT_SIZE * sizeof(*spectrum));
      else {
        memset(spectrum, 0, FIR_FFT_SIZE * sizeof(*spectrum));
        memcpy(spectrum, source + first, length * sizeof(*spectrum));
        lsx_safe_rdft(FIR_FFT_SIZE, 1, spectrum);
      }
      for (bin = 0; bin < FIR_FFT_SIZE; ++bin) {
        uint32_t source_bin = bin <= FIR_FFT_SIZE / 2u ? bin : FIR_FFT_SIZE - bin;
        double real =
            source_bin == 0u ? spectrum[0] :
            source_bin == FIR_FFT_SIZE / 2u ?
            spectrum[1] : spectrum[2u * source_bin];
        double imaginary = source_bin == 0u || source_bin == FIR_FFT_SIZE / 2u ? 0. : -spectrum[2u * source_bin + 1u];
        size_t target = ((size_t)channel * FIR_FFT_SIZE + bin) * 4u;

        if (bin > FIR_FFT_SIZE / 2u)
          imaginary = -imaginary;
        store_double_single(upload, target, real);
        store_double_single(upload, target + 2u, imaginary);
      }
    }
    if (begin_commands(context) != SOX_SUCCESS) {
      free(spectra);
      return SOX_EOF;
    }
    vkCmdCopyBuffer(context->command_buffer, context->upload.buffer, context->kernels.buffer, 1, &copy);
    if (submit_commands(context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS) {
      free(spectra);
      return SOX_EOF;
    }
  }
  free(spectra);
  return SOX_SUCCESS;
}

/* Kernels for the FP64 profiles that want the coefficient spectra computed on
 * the host rather than by a device transform.  The host's transform is the
 * authority: it is the same one the CPU path uses, so the two agree bin for
 * bin, and any difference between the CPU and Vulkan outputs is then down to
 * the convolution rather than to two different spectra of the same response. */
static int initialize_precise_f64_kernels(
    lsx_fir_vulkan_t *context,
    double const *const *coefficients,
    uint32_t coefficient_channels, size_t taps)
{
  double *spectra = lsx_calloc((size_t)context->channels * FIR_FFT_SIZE, sizeof(*spectra));
  uint32_t partition;

  for (partition = 0; partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min((size_t)FIR_BLOCK_FRAMES, taps - first);
    VkBufferCopy copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };
    uint32_t channel;

    memset(context->working_host, 0, (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      double const *source = channel_coefficients(coefficients, coefficient_channels, channel);
      double *spectrum = spectra + (size_t)channel * FIR_FFT_SIZE;
      double *output = (double *)context->working_host + (size_t)channel * (FIR_FFT_SIZE + 2u);
      uint32_t bin;

      /* Channels sharing a response share a spectrum, so the transform is
       * done once rather than per channel. */
      if (channel && source == channel_coefficients(coefficients, coefficient_channels, channel - 1u))
        memcpy(spectrum, spectrum - FIR_FFT_SIZE, FIR_FFT_SIZE * sizeof(*spectrum));
      else {
        memset(spectrum, 0, FIR_FFT_SIZE * sizeof(*spectrum));
        memcpy(spectrum, source + first, length * sizeof(*spectrum));
        lsx_safe_rdft(FIR_FFT_SIZE, 1, spectrum);
      }
      for (bin = 0; bin <= FIR_FFT_SIZE / 2u; ++bin) {
        output[2u * bin] = bin == 0 ? spectrum[0] : bin == FIR_FFT_SIZE / 2u ? spectrum[1] : spectrum[2u * bin];
        output[2u * bin + 1u] = bin == 0 || bin == FIR_FFT_SIZE / 2u ? 0. : -spectrum[2u * bin + 1u];
      }
    }
    memcpy(context->upload.mapped, context->working_host, (size_t)context->working.size);
    if (begin_commands(context) != SOX_SUCCESS) {
      free(spectra);
      return SOX_EOF;
    }
    vkCmdCopyBuffer(context->command_buffer, context->upload.buffer, context->kernels.buffer, 1, &copy);
    if (submit_commands(context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS) {
      free(spectra);
      return SOX_EOF;
    }
  }
  free(spectra);
  return SOX_SUCCESS;
}

/* Fill the kernel bank, dispatching to the strategy-specific routines above
 * or, failing those, transforming each partition on the device.
 *
 * The device route uploads a partition's coefficients as a time-domain signal
 * into working, runs the forward transform there, and copies the resulting
 * spectrum into its slot in the bank -- reusing the same working buffer and
 * plan the steady state will use, which is why this must run after both
 * exist.  It is the only route for the plain FP32 and FP64 profiles, and the
 * only one available at all for the reference profile, whose double-double
 * transform has no host equivalent. */
static int initialize_kernels(
    lsx_fir_vulkan_t *context,
    double const *const *coefficients,
    double const *const *coefficient_lows,
    uint32_t coefficient_channels, size_t taps)
{
  VkBufferCopy upload_copy = {
    0, 0, context->working.size
  };
  uint32_t partition;
  uint32_t channel;

  if (context->precise_fp32)
    return initialize_precise_fp32_kernels(context, coefficients, coefficient_channels, taps);
  if (context->authoritative_fp64_kernels)
    return initialize_precise_f64_kernels(context, coefficients, coefficient_channels, taps);
  for (partition = 0; partition < context->partitions; ++partition) {
    size_t first = (size_t)partition * FIR_BLOCK_FRAMES;
    size_t length = min((size_t)FIR_BLOCK_FRAMES, taps - first);
    VkBufferCopy kernel_copy = {
      0,
      (VkDeviceSize)partition * context->working.size,
      context->working.size
    };

    memset(context->working_host, 0, (size_t)context->working.size);
    for (channel = 0; channel < context->channels; ++channel) {
      double const *channel_highs = channel_coefficients(coefficients, coefficient_channels, channel);
      double const *channel_lows = coefficient_lows ?
          channel_coefficients(
              coefficient_lows, coefficient_channels, channel) : NULL;
      size_t index;

      for (index = 0; index < length; ++index) {
        double coefficient = channel_highs[first + index];
        size_t target_index = (size_t)channel * (FIR_FFT_SIZE + 2u) + index;

        if (context->reference_dd) {
          ((double *)context->working_host)[2u * target_index] = coefficient;
          ((double *)context->working_host)[2u * target_index + 1u] = channel_lows ? channel_lows[first + index] : 0.;
        }
        else if (context->double_precision)
          ((double *)context->working_host)[target_index] = coefficient;
        else
          ((float *)context->working_host)[target_index] = (float)coefficient;
      }
    }
    memcpy(context->upload.mapped, context->working_host, (size_t)context->working.size);
    if (begin_commands(context) != SOX_SUCCESS)
      return SOX_EOF;
    vkCmdCopyBuffer(context->command_buffer, context->upload.buffer, context->working.buffer, 1, &upload_copy);
    memory_barrier(
        context->command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (lsx_vulkan_fft_append(context->fft, context->command_buffer, sox_false) != SOX_SUCCESS) {
      lsx_fail("VkFFT FIR kernel transform failed");
      return SOX_EOF;
    }
    memory_barrier(
        context->command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBuffer(context->command_buffer, context->working.buffer, context->kernels.buffer, 1, &kernel_copy);
    if (submit_commands(context, lsx_vulkan_wait_fir_setup) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Zero the history ring, so the first blocks convolve against silence rather
 * than against whatever the allocation happened to contain.  This is what
 * makes the filter's start-up transient the correct one. */
static int clear_history(lsx_fir_vulkan_t *context)
{
  if (begin_commands(context) != SOX_SUCCESS)
    return SOX_EOF;
  vkCmdFillBuffer(context->command_buffer, context->history.buffer, 0, context->history.size, 0);
  return submit_commands(context, lsx_vulkan_wait_fir_setup);
}

/* Record the accumulation dispatch: one invocation per spectrum bin per
 * channel, each summing that bin's product across every partition.  The whole
 * partition loop is inside the shader, so this is a single dispatch however
 * long the response is; current_slot tells it where the newest input block
 * sits in the ring, which is the only thing that changes between blocks. */
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

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(
      command_buffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline_layout, 0, 1,
      &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(
      command_buffer, context->pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(parameters), &parameters);
  vkCmdDispatch(command_buffer, count / FIR_LOCAL_SIZE + (count % FIR_LOCAL_SIZE != 0), 1, 1);
}

/* One dispatch of the precise FP32 shader, which does several different jobs
 * selected by its push constants: a transform stage, the spectrum multiply,
 * or the output pass.  count is the number of invocations the caller wants,
 * which differs between them. */
static void append_precise_fp32_dispatch(
    lsx_fir_vulkan_t *context,
    VkCommandBuffer command_buffer,
    precise_fp32_parameters_t const *parameters,
    uint32_t count)
{
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, context->pipeline);
  vkCmdBindDescriptorSets(
      command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      context->pipeline_layout, 0, 1,
      &context->descriptor_set, 0, NULL);
  vkCmdPushConstants(
      command_buffer, context->pipeline_layout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(*parameters), parameters);
  vkCmdDispatch(command_buffer, count / FIR_LOCAL_SIZE + (count % FIR_LOCAL_SIZE != 0), 1, 1);
}

/* Record the whole steady-state command bank for the precise FP32 profile.
 *
 * There is one command buffer per (history slot, staging buffer) pair, since
 * a recorded buffer names both, and each contains the entire block: upload,
 * the fifteen butterfly stages of the forward transform, the spectrum
 * multiply and accumulation, the fifteen inverse stages, and the output pass.
 * Recording it once and replaying it means the steady state issues no Vulkan
 * calls at all beyond a submit.
 *
 * Fifteen stages is log2 of the default transform size, the only size this
 * profile uses.  Each stage reads what the last wrote, hence a barrier
 * between every pair, and the two buffers alternate as source and
 * destination, which is what source_is_a tracks: a butterfly cannot be done
 * in place because its two outputs depend on both its inputs.
 *
 * download_output selects the synchronous variant, which ends with a copy to
 * host-visible memory; the resident variant stops at the output buffer and
 * reads from the per-bank staging buffer instead of the shared one. */
static int record_precise_fp32_command_bank(
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
  VkDeviceSize channel_spectrum_size = (VkDeviceSize)FIR_FFT_SIZE * 4u * sizeof(float);
  VkCommandBuffer *process_commands = lsx_calloc(context->partitions * bank_depth, sizeof(*process_commands));
  uint32_t command_index;

  *commands = NULL;
  if (vk_result(vkAllocateCommandBuffers(
      context->vulkan->device, &allocation,
      process_commands),
      "vkAllocateCommandBuffers FIR precise FP32 process") !=
      SOX_SUCCESS) {
    free(process_commands);
    return SOX_EOF;
  }
  for (command_index = 0; command_index < context->partitions * bank_depth; ++command_index) {
    uint32_t current_slot = command_index % context->partitions;
    uint32_t bank_index = command_index / context->partitions;
    VkCommandBuffer command_buffer = process_commands[command_index];
    VkBuffer upload_buffer = download_output ? context->upload.buffer : context->resident_upload[bank_index].buffer;
    VkBufferCopy upload_copy = {
      0, 0, context->working.size
    };
    precise_fp32_parameters_t parameters = {
      0u, 0u, context->partitions,
      context->channels, current_slot,
      0u, 1u
    };
    uint32_t stage;
    uint32_t channel;

    if (vk_result(vkBeginCommandBuffer(
        command_buffer, &begin),
        "vkBeginCommandBuffer FIR precise FP32 process") !=
        SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_begin(
        context->vulkan, command_buffer,
        download_output ?
        "FIR precise FP32 process and download" :
        "FIR precise FP32 resident process");
    vkCmdCopyBuffer(command_buffer, upload_buffer, context->working.buffer, 1, &upload_copy);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR precise FP32 forward double-single FFT");
    for (stage = 0; stage < 15u; ++stage) {
      parameters.operation = 0u;
      parameters.stage = stage;
      parameters.inverse = 0u;
      parameters.source_is_a = stage % 2u == 0u;
      append_precise_fp32_dispatch(context, command_buffer, &parameters, context->channels * FIR_FFT_SIZE / 2u);
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
    /* File the new block's spectrum into the ring, one copy per channel: the
     * spectra are contiguous per channel in the source but strided by the
     * partition count in the ring, so they cannot go across in one copy.
     * This must happen after the transform and before the multiply, since
     * the multiply reads the ring including this slot. */
    for (channel = 0; channel < context->channels; ++channel) {
      VkBufferCopy history_copy = {
        (VkDeviceSize)channel * channel_spectrum_size,
        ((VkDeviceSize)channel * context->partitions + current_slot) * channel_spectrum_size,
        channel_spectrum_size
      };

      vkCmdCopyBuffer(command_buffer, context->working_scratch.buffer, context->history.buffer, 1, &history_copy);
    }
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    parameters.operation = 1u;
    parameters.inverse = 0u;
    parameters.source_is_a = 0u;
    append_precise_fp32_dispatch(context, command_buffer, &parameters, context->channels * FIR_FFT_SIZE);
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR precise FP32 inverse double-single FFT");
    for (stage = 0; stage < 15u; ++stage) {
      parameters.operation = 0u;
      parameters.stage = stage;
      parameters.inverse = 1u;
      parameters.source_is_a = stage % 2u == 0u;
      append_precise_fp32_dispatch(context, command_buffer, &parameters, context->channels * FIR_FFT_SIZE / 2u);
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
    append_precise_fp32_dispatch(context, command_buffer, &parameters, context->channels * FIR_BLOCK_FRAMES);
    if (download_output) {
      VkBufferCopy output_copy = {
        0, 0, context->precise_output.size
      };

      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBuffer(command_buffer, context->precise_output.buffer, context->download.buffer, 1, &output_copy);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer FIR precise FP32 process") != SOX_SUCCESS)
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

/* Record the steady-state command bank for every profile but precise FP32.
 *
 * Each command buffer is one whole block: upload, forward transform, file the
 * spectrum into the history ring, accumulate the products across all
 * partitions, inverse transform, and -- for the synchronous variant -- copy
 * the second half of the result to host-visible memory.  Barriers separate
 * every pair of steps, since each reads what the last wrote.
 *
 * Recording is by (slot, bank) exactly as in the precise variant: the slot
 * decides which ring position this block occupies, the bank which staging
 * buffer it arrives through, and a command buffer names both. */
static int record_process_command_bank(lsx_fir_vulkan_t *context, VkCommandBuffer **commands, sox_bool download_output, uint32_t bank_depth)
{
  VkDeviceSize complex_size = 2u * context->element_size;
  VkDeviceSize spectrum_size = (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize real_stride = (VkDeviceSize)(FIR_FFT_SIZE + 2u) * context->element_size;
  VkDeviceSize block_size = (VkDeviceSize)FIR_BLOCK_FRAMES * context->element_size;
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

  if (context->precise_fp32)
    return record_precise_fp32_command_bank(context, commands, download_output, bank_depth);
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

    if (vk_result(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer FIR process") != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_begin(context->vulkan, command_buffer, download_output ? "FIR process and download" : "FIR resident process");
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR input upload");
    vkCmdCopyBuffer(command_buffer, upload_buffer, context->working.buffer, 1, &upload_copy);
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    memory_barrier(
        command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR forward FFT");
    if (lsx_vulkan_fft_append(context->fft, command_buffer, sox_false) != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR history update");
    memory_barrier(
        command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* File the new spectrum into its ring slot, one copy per channel: the
     * source has the channels contiguous, the ring strides them by the
     * partition count, so they cannot go across together. */
    for (channel = 0; channel < context->channels; ++channel) {
      VkBufferCopy history_copy = {
        (VkDeviceSize)channel * spectrum_size,
        ((VkDeviceSize)channel * context->partitions + current_slot) * spectrum_size,
        spectrum_size
      };

      vkCmdCopyBuffer(command_buffer, context->working.buffer, context->history.buffer, 1, &history_copy);
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
    lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR inverse FFT");
    if (lsx_vulkan_fft_append(context->fft, command_buffer, sox_true) != SOX_SUCCESS)
      goto error;
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (download_output) {
      lsx_vulkan_label_begin(context->vulkan, command_buffer, "FIR output download");
      memory_barrier(
          command_buffer, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      /* Copy only the second half of each channel's transform: the first
       * half is where the circular convolution's wrap-around lands and is
       * discarded, which is the whole of what overlap-save does.  Hence the
       * source offset of one block into each channel's stride, and hence a
       * copy per channel rather than one over the lot. */
      for (channel = 0; channel < context->channels; ++channel) {
        VkBufferCopy output_copy = {
          (VkDeviceSize)channel * real_stride + block_size,
          (VkDeviceSize)channel * block_size,
          block_size
        };

        vkCmdCopyBuffer(command_buffer, context->working.buffer, context->download.buffer, 1, &output_copy);
      }
      lsx_vulkan_label_end(context->vulkan, command_buffer);
    }
    lsx_vulkan_label_end(context->vulkan, command_buffer);
    if (vk_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer FIR process") != SOX_SUCCESS)
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

/* Allocate everything the steady state needs.
 *
 * The spectrum of a real transform of FIR_FFT_SIZE points has half that many
 * bins plus one, each complex, which is what working holds per channel; the
 * two extra reals are why the per-channel stride is FIR_FFT_SIZE + 2.  The
 * history and kernel banks are that spectrum times the partition count, and
 * the two are checked against maxStorageBufferRange because a long response
 * at high channel counts is what actually runs out first -- a device limit
 * rather than an allocation failure, so it has to be tested explicitly.
 *
 * Precise FP32 departs from this: it keeps its own transform's data as four
 * floats per point over the full transform length, not a half-spectrum, and
 * its output is a pair per sample. */
static int create_buffers(lsx_fir_vulkan_t *context)
{
  VkDeviceSize complex_size = 2u * context->element_size;
  VkDeviceSize spectrum_size = (VkDeviceSize)(FIR_FFT_SIZE / 2u + 1u) * complex_size;
  VkDeviceSize working_size = context->channels * spectrum_size;
  VkDeviceSize bank_size;
  VkDeviceSize download_size = (VkDeviceSize)context->channels * FIR_BLOCK_FRAMES * context->element_size;
  VkPhysicalDeviceLimits const *limits = &context->vulkan->properties.limits;

  if (context->precise_fp32) {
    working_size = (VkDeviceSize)context->channels * FIR_FFT_SIZE * 4u * sizeof(float);
    download_size = (VkDeviceSize)context->channels * FIR_BLOCK_FRAMES * 2u * sizeof(float);
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
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (create_buffer(
      context, &context->history, bank_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (create_buffer(
      context, &context->kernels, bank_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (create_buffer(
      context, &context->upload, working_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (create_buffer(
      context, &context->download, download_size,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != SOX_SUCCESS)
    return SOX_EOF;
  if (context->precise_fp32 &&
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
       context, &context->precise_output, download_size,
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != SOX_SUCCESS))
    return SOX_EOF;
  {
    uint32_t index;
    for (index = 0; index < LSX_VULKAN_RESIDENT_BATCH_DEPTH; ++index)
      if (create_buffer(context, &context->resident_upload[index], working_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != SOX_SUCCESS)
        return SOX_EOF;
  }
  context->working_host = lsx_calloc(1, (size_t)working_size);
  context->previous = lsx_calloc((size_t)context->channels * FIR_BLOCK_FRAMES, sizeof(*context->previous));
  context->output = lsx_calloc((size_t)context->channels * FIR_BLOCK_FRAMES, sizeof(*context->output));
  return SOX_SUCCESS;
}

/* The one constructor the public forms funnel into.  coefficient_channels is
 * 1 for a shared response and channels for per-channel ones, which is the
 * only difference between them; coefficient_lows is non-NULL only for the
 * reference profile.
 *
 * The order below is a dependency chain, not a preference: the buffers need
 * the sizes the strategy flags imply, the FFT plan binds to the buffers, the
 * pipeline needs the descriptor set the buffers are written into, the kernels
 * are transformed through that pipeline, and the command buffers can only be
 * recorded once every handle they reference exists.  Any failure unwinds
 * through the destructor, which tolerates a partly built context. */
static lsx_fir_vulkan_t *create_fir(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients,
    double const *const *coefficient_lows,
    uint32_t coefficient_channels,
    size_t taps, uint32_t channels)
{
  lsx_fir_vulkan_t *context;
  double started = monotonic_seconds();
  uint32_t channel;

  if (!vulkan || !coefficients || !taps || !channels ||
      (coefficient_channels != 1u &&
       coefficient_channels != channels) ||
      taps > UINT32_MAX - FIR_FAST_BLOCK_FRAMES) {
    lsx_fail("invalid Vulkan FIR configuration");
    return NULL;
  }
  for (channel = 0; channel < coefficient_channels; ++channel) {
    if (!coefficients[channel] || (coefficient_lows && !coefficient_lows[channel])) {
      lsx_fail("invalid Vulkan FIR channel coefficients");
      return NULL;
    }
  }
  if (!vulkan->shader_float64 &&
      vulkan->profile != sox_vulkan_profile_fast &&
      vulkan->profile != sox_vulkan_profile_precise) {
    lsx_fail(
        "Vulkan FIR profile %s is not implemented for the FP32 "
        "emulated numerical family",
        lsx_vulkan_profile_name(vulkan->profile));
    return NULL;
  }
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->fft_size = vulkan->profile == sox_vulkan_profile_fast ? FIR_FAST_FFT_SIZE : FIR_DEFAULT_FFT_SIZE;
  context->block_frames = context->fft_size / 2u;
  context->double_precision = vulkan->use_float64;
  context->precise_fp32 = !context->double_precision && vulkan->profile == sox_vulkan_profile_precise;
  context->precise_fp64 =
      context->double_precision &&
      (vulkan->profile == sox_vulkan_profile_precise ||
       vulkan->profile == sox_vulkan_profile_reference);
  context->reference_dd = context->double_precision && vulkan->profile == sox_vulkan_profile_reference;
  /* Not for the reference profile: its coefficients are double-double, and a
   * host transform would have to collapse them to plain doubles first, which
   * is exactly the loss the profile exists to avoid.  It transforms its
   * kernels on the device instead. */
  context->authoritative_fp64_kernels =
      context->double_precision &&
      vulkan->profile == sox_vulkan_profile_precise;
  context->taps = (uint32_t)taps;
  context->element_size = context->reference_dd ?
      2u * sizeof(double) :
      context->double_precision ? sizeof(double) : sizeof(float);
  context->channels = channels;
  /* Rounded up: a final short partition is zero-padded rather than treated
   * specially, so every partition is the same size and one dispatch covers
   * them all. */
  context->partitions = (uint32_t)((taps + FIR_BLOCK_FRAMES - 1u) / FIR_BLOCK_FRAMES);
  if (create_commands(context) != SOX_SUCCESS)
    goto error;
  if (create_buffers(context) != SOX_SUCCESS)
    goto error;
  if (initialize_fft(context) != SOX_SUCCESS)
    goto error;
  if (create_partition_pipeline(context) != SOX_SUCCESS)
    goto error;
  if (initialize_kernels(context, coefficients, coefficient_lows, coefficient_channels, taps) != SOX_SUCCESS)
    goto error;
  if (clear_history(context) != SOX_SUCCESS)
    goto error;
  if (record_process_commands(context) != SOX_SUCCESS)
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
  lsx_report("Vulkan FIR strategy: %s", strategy_name(context));
  return context;

error: lsx_fir_vulkan_destroy(context);
  return NULL;
}

/* Publish what only the backend knows about this effect.  It is the same
 * material the -V3 lines above carry, emitted a second time as keys rather
 * than measured again, which is why nothing here computes anything. */
void lsx_fir_vulkan_diagnostics(lsx_fir_vulkan_t const *context, sox_effect_t const *effp)
{
  if (!context || !lsx_diagnostics_on)
    return;
  lsx_diagnostics_effect_setf(effp, "precision", "%s",
      context->reference_dd ? "FP64x2" :
      context->double_precision ? "FP64" : "FP32");
  lsx_diagnostics_effect_setf(effp, "strategy", "%s", strategy_name(context));
  lsx_diagnostics_effect_setf(effp, "taps", "%u", context->taps);
  lsx_diagnostics_effect_setf(effp, "partitions", "%u", context->partitions);
  lsx_diagnostics_effect_setf(effp, "fft_size", "%u", context->fft_size);
  lsx_diagnostics_effect_setf(effp, "block_frames", "%u", context->block_frames);
  lsx_diagnostics_effect_setf(effp, "startup_s", "%.6f", context->startup_seconds);
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create(
    lsx_vulkan_context_t *vulkan, double const *coefficients,
    size_t taps, uint32_t channels)
{
  double const *channel_coefficients[] = {coefficients};

  return create_fir(vulkan, channel_coefficients, NULL, 1u, taps, channels);
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficients,
    size_t taps, uint32_t channels)
{
  return create_fir(vulkan, coefficients, NULL, channels, taps, channels);
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd(
    lsx_vulkan_context_t *vulkan,
    double const *coefficient_highs,
    double const *coefficient_lows,
    size_t taps, uint32_t channels)
{
  double const *channel_highs[] = {coefficient_highs};
  double const *channel_lows[] = {coefficient_lows};

  if (!coefficient_lows || !vulkan || vulkan->profile != sox_vulkan_profile_reference)
    return NULL;
  return create_fir(vulkan, channel_highs, channel_lows, 1u, taps, channels);
}

lsx_fir_vulkan_t *lsx_fir_vulkan_create_reference_dd_channels(
    lsx_vulkan_context_t *vulkan,
    double const *const *coefficient_highs,
    double const *const *coefficient_lows,
    size_t taps, uint32_t channels)
{
  if (!coefficient_lows || !vulkan || vulkan->profile != sox_vulkan_profile_reference)
    return NULL;
  return create_fir(vulkan, coefficient_highs, coefficient_lows, channels, taps, channels);
}

void lsx_fir_vulkan_destroy(lsx_fir_vulkan_t *context)
{
  if (!context)
    return;
  /* Blocks may still be queued and unwaited on the resident path, so
   * everything below waits for the device first: freeing a buffer or a
   * pipeline a command buffer still references is undefined. */
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
    vkFreeCommandBuffers(context->vulkan->device, context->vulkan->command_pool, 1, &context->command_buffer);
  if (context->pipeline)
    vkDestroyPipeline(context->vulkan->device, context->pipeline, NULL);
  if (context->pipeline_layout)
    vkDestroyPipelineLayout(context->vulkan->device, context->pipeline_layout, NULL);
  if (context->descriptor_pool)
    vkDestroyDescriptorPool(context->vulkan->device, context->descriptor_pool, NULL);
  if (context->descriptor_layout)
    vkDestroyDescriptorSetLayout(context->vulkan->device, context->descriptor_layout, NULL);
  lsx_vulkan_fft_destroy(context->fft);
  if (context->fence)
    vkDestroyFence(context->vulkan->device, context->fence, NULL);
  destroy_buffer(context, &context->download);
  destroy_buffer(context, &context->upload);
  destroy_buffer(context, &context->precise_output);
  destroy_buffer(context, &context->twiddles);
  destroy_buffer(context, &context->kernels);
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

size_t lsx_fir_vulkan_block_frames_for(lsx_vulkan_context_t const *context)
{
  return context && context->profile == sox_vulkan_profile_fast ? FIR_FAST_BLOCK_FRAMES : FIR_DEFAULT_BLOCK_FRAMES;
}

/* The stride is the transform length plus the two extra reals a half-spectrum
 * needs; precise FP32 has no such padding, its buffers holding the full
 * transform length. */
size_t lsx_fir_vulkan_prepared_stride(lsx_fir_vulkan_t const *context)
{
  if (!context)
    return 0;
  return context->precise_fp32 ? FIR_FFT_SIZE : FIR_FFT_SIZE + 2u;
}

lsx_vulkan_buffer_t *lsx_fir_vulkan_prepared_input_buffer(lsx_fir_vulkan_t *context)
{
  return context ? &context->resident_upload[context->resident_bank_index] : NULL;
}

/* Reverse the low 15 bits of an index.  A decimation-in-time transform reads
 * its input in bit-reversed order, and the precise FP32 shader implements the
 * transform itself rather than calling VkFFT, so the host does the permutation
 * while it is laying the block out anyway.  Fifteen bits is log2 of the
 * default transform size, which is the only size the precise profile uses. */
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

/* Lay one interleaved block out as the transform input and copy it to staging.
 *
 * This is where overlap-save happens on the host: each channel's transform
 * input is the previous block followed by the current one, so the first half
 * comes from context->previous and the second from input, and input then
 * becomes previous for the next call.  The transform is twice a block long
 * precisely so that the two fit.
 *
 * The layout is planar -- channels run consecutively, not interleaved -- so
 * the transform sees one contiguous signal per batch.  Each strategy stores
 * its own element form: a bit-reversed pair of floats for precise FP32,
 * because that shader does its own transform, an explicit pair with a zero
 * low word for the double-double formats, and a plain value otherwise.
 *
 * The scratch is zeroed each time because the two extra reals per channel and
 * any tail past the block must not carry over from the previous call. */
static void prepare_process_input(lsx_fir_vulkan_t *context, double const *input, buffer_t *upload)
{
  uint32_t channel;
  size_t frame;

  memset(context->working_host, 0, (size_t)context->working.size);
  for (channel = 0; channel < context->channels; ++channel)
    for (frame = 0; frame < FIR_BLOCK_FRAMES; ++frame) {
      double value = input[frame * context->channels + channel];
      size_t previous_index = (size_t)channel * FIR_BLOCK_FRAMES + frame;
      size_t first_index = (size_t)channel * (FIR_FFT_SIZE + 2u) + frame;
      size_t second_index = first_index + FIR_BLOCK_FRAMES;

      if (context->precise_fp32) {
        float *prepared = context->working_host;
        uint32_t first_reversed = reverse_fft_index((uint32_t)frame);
        uint32_t second_reversed = reverse_fft_index((uint32_t)frame + FIR_BLOCK_FRAMES);
        size_t channel_base = (size_t)channel * FIR_FFT_SIZE;

        store_double_single(prepared, (channel_base + first_reversed) * 4u, context->previous[previous_index]);
        store_double_single(prepared, (channel_base + second_reversed) * 4u, value);
      }
      else if (context->reference_dd) {
        double *prepared = context->working_host;

        prepared[2u * first_index] = context->previous[previous_index];
        prepared[2u * first_index + 1u] = 0.;
        prepared[2u * second_index] = value;
        prepared[2u * second_index + 1u] = 0.;
      }
      else if (context->double_precision) {
        ((double *)context->working_host)[first_index] = context->previous[previous_index];
        ((double *)context->working_host)[second_index] = value;
      }
      else {
        float previous_high = (float)context->previous[previous_index];
        float value_high = (float)value;

        ((float *)context->working_host)[first_index] = previous_high;
        ((float *)context->working_host)[second_index] = value_high;
      }
      context->previous[previous_index] = value;
    }
  memcpy(upload->mapped, context->working_host, (size_t)context->working.size);
}

/* The synchronous path: prepare, submit the pre-recorded command buffer for
 * the current slot, wait, and de-interleave the result.  The download holds
 * only the second half of the transform -- the part overlap-save keeps -- so
 * what comes back is already one block of output per channel.
 *
 * The slot advances afterwards, so that the recorded command buffer for the
 * next block reads the history ring one position further on. */
int lsx_fir_vulkan_process(lsx_fir_vulkan_t *context, double const *input, double const **output)
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
      size_t index = (size_t)channel * FIR_BLOCK_FRAMES + frame;

      if (context->precise_fp32) {
        float const *double_single = (float const *)download + index * 2u;

        context->output[frame * context->channels + channel] = (double)double_single[0] + (double)double_single[1];
      }
      else if (context->reference_dd) {
        double const *double_double = (double const *)download + index * 2u;

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
  context->current_slot = (context->current_slot + 1u) % context->partitions;
  context->process_seconds += monotonic_seconds() - started;
  ++context->process_calls;
  *output = context->output;
  return SOX_SUCCESS;
}

/* Describe where this block's output sits, for a consumer to read in place.
 *
 * The offset is what makes overlap-save visible in the description: the
 * inverse transform fills a whole transform's worth, of which only the second
 * half is the answer, so the region starts one block in.  Precise FP32 writes
 * to a separate output buffer that already holds just that half, hence its
 * offset of zero and its different channel stride.
 *
 * The layout is planar with the same per-channel stride the transform uses,
 * so nothing is rearranged for the consumer's benefit; validation is what
 * ensures the description and the memory agree. */
static int describe_resident_output(lsx_fir_vulkan_t *context, sox_rate_t rate, uint64_t frame_offset, lsx_vulkan_resident_state_t state, lsx_vulkan_resident_buffer_t *resident)
{
  memset(resident, 0, sizeof(*resident));
  resident->buffer = context->precise_fp32 ? &context->precise_output : &context->working;
  resident->owner = context;
  resident->offset = context->precise_fp32 ? 0 : (VkDeviceSize)FIR_BLOCK_FRAMES * context->element_size;
  resident->producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  resident->producer_access = VK_ACCESS_SHADER_WRITE_BIT;
  resident->capacity_elements = FIR_BLOCK_FRAMES;
  resident->valid_elements = FIR_BLOCK_FRAMES;
  resident->frame_stride_elements = 1u;
  resident->channel_stride_elements = FIR_FFT_SIZE + 2u;
  if (context->precise_fp32)
    resident->channel_stride_elements = FIR_BLOCK_FRAMES;
  resident->frame_offset = frame_offset;
  resident->rate = rate;
  resident->channels = context->channels;
  resident->frames_per_element = 1u;
  resident->format = resident_format(context);
  resident->domain = lsx_vulkan_resident_domain_sox_sample;
  resident->layout = lsx_vulkan_resident_layout_planar;
  resident->state = state;
  return lsx_vulkan_resident_buffer_validate(resident);
}

/* The resident path.  Nothing is submitted and nothing is waited for: the
 * command buffer is queued and will go out with whatever the chain submits
 * next, which is the whole point -- a resident chain pays one submission for
 * a batch of blocks instead of one per block per effect.
 *
 * The bank is recorded on first use, and the command buffer is chosen by two
 * independent rotations: the slot, which says where in the history ring this
 * block lands, and the bank index, which says which staging buffer it was
 * uploaded through.  A recorded command buffer names both, so the product of
 * the two needs its own recording, which is why the bank is that many
 * command buffers and the index is this arithmetic. */
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
  context->current_slot = (context->current_slot + 1u) % context->partitions;
  /* The bank wraps at the configured depth, which may be below the compiled
   * maximum; the buffers above it are simply unused for this run. */
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
