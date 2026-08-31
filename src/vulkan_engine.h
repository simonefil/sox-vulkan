/* Shared Vulkan execution core for SoX effects.
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

/*
 * One Vulkan device, shared by every Vulkan-capable effect in a chain, plus
 * the vocabulary those effects use to hand audio to one another without
 * going back through host memory.
 *
 * The central idea is residency.  An effect that can produce its output on
 * the device publishes it as an lsx_vulkan_resident_buffer_t rather than
 * downloading it; the next effect, if it can consume one, reads that buffer
 * directly.  A chain of such effects therefore uploads once at its head and
 * downloads once at its tail, whatever happens in between.  An effect that
 * cannot take part simply does not publish, and the previous one downloads as
 * it always would, so the two paths coexist and no effect has to know what
 * follows it.
 */

#ifndef LSX_VULKAN_ENGINE_H
#define LSX_VULKAN_ENGINE_H

#include "sox.h"

#include <vulkan/vulkan.h>

/* A device buffer and the allocation behind it, kept together because the two
 * are always created and destroyed as one.  mapped is non-NULL only for
 * host-visible memory, where the mapping is made once at creation and held
 * for the buffer's lifetime rather than being made and dropped per access. */
typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  VkBufferUsageFlags usage;
  VkMemoryPropertyFlags memory_flags;
  void *mapped;
} lsx_vulkan_buffer_t;

/* How one element of a resident buffer is stored.  The x2 forms are
 * unevaluated sums of two floats: the high part and a correction, which
 * together carry roughly twice the precision of the underlying type.  They
 * exist because a GPU has no wider float than double, so the precise and
 * reference profiles reach beyond it by pairing.  dsd_u32 is 32 one-bit DSD
 * frames packed into a word, not a number. */
typedef enum {
  lsx_vulkan_resident_format_f32,
  lsx_vulkan_resident_format_f32x2,
  lsx_vulkan_resident_format_f64,
  lsx_vulkan_resident_format_f64x2,
  lsx_vulkan_resident_format_dsd_u32
} lsx_vulkan_resident_format_t;

/* What the stored numbers mean.  sox_sample is whatever scale the chain's own
 * samples are on, which is what most of the pipeline passes around; normalized
 * is that signal scaled to +/-1; dsd is bits, not amplitudes.  A consumer must
 * check this, since the same format can carry either scaling.
 *
 * The two amplitude domains used to differ by 2^31, the chain carrying
 * full-scale integers.  It now carries the normalised value itself, so they
 * coincide -- lsx_sample_values_are_normalized() is what says so, and every
 * producer and consumer here asks it rather than assuming either scale. */
typedef enum {
  lsx_vulkan_resident_domain_sox_sample,
  lsx_vulkan_resident_domain_normalized,
  lsx_vulkan_resident_domain_dsd
} lsx_vulkan_resident_domain_t;

/* Whether channels alternate within a frame or occupy separate runs.  Both
 * occur: interleaved matches what the host hands over, planar is what a
 * per-channel kernel wants, and the strides below say which is in use without
 * a consumer having to branch on this. */
typedef enum {
  lsx_vulkan_resident_layout_interleaved,
  lsx_vulkan_resident_layout_planar
} lsx_vulkan_resident_layout_t;

/* Where a resident buffer stands in the stream.  empty carries nothing yet;
 * ready is ordinary mid-stream data; draining means the producer has seen the
 * end of its input and is flushing what its own latency still holds; final
 * marks the last block, after which nothing more will be published.  A
 * consumer needs the distinction because its own drain can only begin once
 * its producer has finished flushing. */
typedef enum {
  lsx_vulkan_resident_empty,
  lsx_vulkan_resident_ready,
  lsx_vulkan_resident_draining,
  lsx_vulkan_resident_final
} lsx_vulkan_resident_state_t;

/* Why the host had to stop and wait for the device.  Counted per reason so
 * that -V3 can show which stage forced the stalls: a resident chain is
 * supposed to wait only at its setup and its flushes, and a count rising
 * against a synchronous reason means work is round-tripping per block. */
typedef enum {
  lsx_vulkan_wait_fir_setup,
  lsx_vulkan_wait_fir_synchronous,
  lsx_vulkan_wait_fir_resident_flush,
  lsx_vulkan_wait_rate_synchronous,
  lsx_vulkan_wait_sdm_setup,
  lsx_vulkan_wait_sdm_synchronous,
  lsx_vulkan_wait_sdm_resident_flush,
  lsx_vulkan_wait_packed_output,
  lsx_vulkan_wait_resident_output,
  lsx_vulkan_wait_reason_count
} lsx_vulkan_wait_reason_t;

/* The shape of the resident chain being run, which is what the batch depth
 * below is chosen against: a lone DFT effect, or several effects feeding one
 * another on the device. */
typedef enum {
  lsx_vulkan_resident_topology_dft_only,
  lsx_vulkan_resident_topology_chained
} lsx_vulkan_resident_topology_t;

/* Which arithmetic the shaders will use.  fp64 is native double, chosen only
 * when the device supports it and the profile asks for that much precision;
 * fp32_emulated is single precision, with the paired formats above standing
 * in where more is needed.  This is a property of the whole context, so every
 * effect in a chain agrees on it. */
typedef enum {
  lsx_vulkan_numerical_family_fp32_emulated,
  lsx_vulkan_numerical_family_fp64
} lsx_vulkan_numerical_family_t;

/*
 * Non-owning description of a stream region that remains in Vulkan memory.
 * The producer identified by owner retains allocation lifetime responsibility.
 * producer_stage and producer_access describe the dependency required before a
 * consumer reads the region.  capacity_elements and valid_elements are per
 * channel.  block_elements is the largest slice this producer will ever hand
 * over, so a consumer can size itself once instead of guessing from the first
 * slice; zero means the producer does not declare one.  domain distinguishes
 * raw SoX sample units from normalized PCM; packed DSD words contain 32
 * frames.
 */
typedef struct {
  /* The buffer and the region within it.  Neither is owned: the producer
   * keeps them alive at least until the consumer has been given a chance to
   * read this block, which in practice means until it publishes the next. */
  lsx_vulkan_buffer_t *buffer;
  void *owner;                  /* Producing effect, for identity checks only. */
  VkDeviceSize offset;

  /* What the consumer must wait on before reading.  A buffer barrier from
   * this stage and access to the consumer's own is the whole synchronisation
   * contract between the two effects; nothing else is guaranteed. */
  VkPipelineStageFlags producer_stage;
  VkAccessFlags producer_access;

  /* All three are per channel.  block_elements is the largest slice this
   * producer will ever publish, so a consumer can size itself once instead
   * of growing when a later block turns out bigger; zero means the producer
   * declares no bound. */
  size_t capacity_elements;
  size_t valid_elements;
  size_t block_elements;

  /* Distances in elements, so that one addressing expression serves both
   * layouts and a consumer never has to branch on layout to find a sample. */
  size_t frame_stride_elements;
  size_t channel_stride_elements;

  uint64_t frame_offset;        /* Position of this block in the stream. */
  sox_rate_t rate;
  uint32_t channels;
  /* 32 for packed DSD, 1 for everything else; the factor between elements
   * and frames, which is what makes valid_elements meaningful. */
  uint32_t frames_per_element;

  lsx_vulkan_resident_format_t format;
  lsx_vulkan_resident_domain_t domain;
  lsx_vulkan_resident_layout_t layout;
  lsx_vulkan_resident_state_t state;
} lsx_vulkan_resident_buffer_t;

/* The shared device context.  One per effects chain, created on first use and
 * destroyed with the chain, so that several Vulkan effects in one chain share
 * a device, a queue and a pipeline cache rather than each paying to open its
 * own. */
typedef struct lsx_vulkan_context {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;                /* Single queue: all work is ordered on it. */
  uint32_t queue_family;
  uint32_t graphics_queue_family; /* UINT32_MAX if none; used only for capture. */
  uint32_t timestamp_valid_bits;
  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceMemoryProperties memory_properties;

  /* Numerics, fixed for the life of the context.  The profile is what the
   * user asked for and cannot change mid-chain, since effects choose their
   * shaders and buffer formats from it. */
  sox_vulkan_profile_t profile;
  lsx_vulkan_numerical_family_t numerical_family;
  sox_bool shader_float64;      /* What the device offers. */
  sox_bool use_float64;         /* What the profile then asked to use. */

  /* Optional instrumentation, all off unless both the extension and the
   * environment ask for it.  None of it affects results. */
  sox_bool debug_utils;
  sox_bool graphics_capture;
  sox_bool frame_boundary;
  PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_debug_utils_label;
  PFN_vkCmdEndDebugUtilsLabelEXT cmd_end_debug_utils_label;

  VkCommandPool command_pool;
  VkPipelineCache pipeline_cache; /* Shared, so repeated pipelines cost less. */

  /* Counters reported at teardown under -V3. */
  double startup_seconds;
  uint64_t submit_count;
  uint64_t host_wait_count;
  uint64_t frame_id;
  uint64_t submit_batch_counts[10]; /* Indexed by batch size, 9 meaning 9 or more. */
  uint64_t wait_reason_counts[lsx_vulkan_wait_reason_count];

  /* Command buffers recorded but not yet submitted.  Batching them means one
   * queue submission and one fence wait for a whole group of effects rather
   * than one each. */
  VkCommandBuffer pending_command_buffers[256];
  uint32_t pending_command_buffer_count;

  /* Staging for the one download at the end of a resident chain.  Kept on the
   * context and grown as needed, so a long stream allocates once. */
  lsx_vulkan_buffer_t resident_download;
  VkCommandBuffer resident_download_command;
  VkFence resident_download_fence;

  /* How many blocks a resident producer may run ahead of the host. */
  uint32_t resident_batch_depth;
  sox_bool resident_batch_depth_overridden;
} lsx_vulkan_context_t;

/* Default and maximum depth for the resident pipeline: the number of blocks
 * that may be in flight before the host waits.  Deeper hides more latency but
 * costs proportionally more device memory, since every block in flight needs
 * its own buffers. */
#define LSX_VULKAN_RESIDENT_BATCH_DEPTH 4u

/* The chain's context, created on first call.  Returns NULL if no usable
 * device exists or the requested profile cannot be honoured, which callers
 * treat as "fall back to the CPU path" rather than as a fatal error.  The
 * context belongs to the effects chain, not the caller. */
lsx_vulkan_context_t *lsx_vulkan_context_get(sox_effects_globals_t *effects_globals);

/* Destroy a context.  Takes void * because it is installed as the effects
 * chain's destructor, which knows nothing of Vulkan types.  Waits for the
 * device to go idle first, so outstanding work cannot outlive its buffers. */
void lsx_vulkan_context_destroy(void *opaque_context);

char const *lsx_vulkan_profile_name(sox_vulkan_profile_t profile);

/* Turn a VkResult into a SoX status, reporting the operation on failure.
 * Every Vulkan call in the backend goes through this, so that no failure is
 * silently ignored and the message always names what was attempted. */
int lsx_vulkan_result(VkResult result, char const *operation);

/* Bracket a region of a command buffer with a debug label, for a GPU trace.
 * Both are no-ops when the debug utils extension is absent. */
void lsx_vulkan_label_begin(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer, char const *name);
void lsx_vulkan_label_end(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer);

/* Create a buffer with its own allocation, mapping it if the requested
 * properties are host-visible.  On failure the buffer may hold a partial
 * allocation, so lsx_vulkan_buffer_destroy must still be called on it. */
int lsx_vulkan_buffer_create(
    lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties);

/* Unmap, destroy and clear a buffer.  Safe on a zeroed or partly created one,
 * and leaves it zeroed so it can be created into again. */
void lsx_vulkan_buffer_destroy(lsx_vulkan_context_t *context, lsx_vulkan_buffer_t *buffer);
/* Bytes one element of a resident buffer occupies.  Returns zero for a format
 * the build does not know, so callers can reject it rather than compute a
 * stride from a guess. */
VkDeviceSize lsx_vulkan_resident_element_size(lsx_vulkan_resident_format_t format);
/* Bytes the described region spans, from its first element to its last
 * inclusive, so that a planar buffer is measured across its whole extent and
 * not just one channel.  Returns 0 if the description is inconsistent or the
 * arithmetic would overflow, which callers treat as invalid. */
VkDeviceSize lsx_vulkan_resident_buffer_size(lsx_vulkan_resident_buffer_t const *resident);

/* Check a resident buffer against every invariant the contract states -- the
 * strides and counts agree, the region fits inside its buffer, the format
 * matches the domain, a barrier has been declared -- and report why not.
 * Every consumer calls it before reading, since a description that is wrong
 * in device memory is a fault with no diagnosis at all. */
int lsx_vulkan_resident_buffer_validate(lsx_vulkan_resident_buffer_t const *resident);

/* Copy a resident PCM block to host memory as interleaved doubles, waiting
 * for the device.  This is the one download at the tail of a resident chain,
 * so it barriers from the producer's stage and collapses the paired formats
 * to a single double on the way out.  output must have room for
 * valid_elements * channels doubles.  Rejects a DSD buffer: bits are not
 * samples and leave by the packed path instead. */
int lsx_vulkan_download_resident_pcm(
    lsx_vulkan_context_t *context,
    lsx_vulkan_resident_buffer_t const *resident,
    double *output, size_t output_samples);
/* Every double-double resident sample has to become a single double before it
 * can leave the engine, and that collapse is itself the measurement floor of
 * the reference profile.  Every f64x2 collapse in the engine must go through
 * here: it is the one place where both halves of the pair still exist, and
 * therefore the only place a measurement can see the profile's full
 * precision.  Under --diagnostics it captures the pair on the way past; the
 * value it returns is the same either way. */
double lsx_vulkan_collapse_pair(double high, double low);

/* Build a compute pipeline from embedded SPIR-V.  The shader module is
 * temporary -- the pipeline holds what it needs -- and the context's pipeline
 * cache is used, so a shader built twice in one chain compiles once. */
int lsx_vulkan_create_compute_pipeline(
    lsx_vulkan_context_t *context, uint32_t const *spirv,
    size_t spirv_size, VkPipelineLayout layout, VkPipeline *pipeline);

/* Hold a recorded command buffer back for the next submission instead of
 * submitting it now.  The caller must keep it alive and unrecorded until
 * lsx_vulkan_submit_and_wait has run, since that is when it is actually
 * executed. */
int lsx_vulkan_enqueue(lsx_vulkan_context_t *context, VkCommandBuffer command_buffer);

/* Submit everything enqueued plus command_buffer, in that order, and block
 * until the fence signals.  Ordering matters: the enqueued buffers are the
 * work whose results the final one depends on.  reason records why the host
 * had to wait; the queue is cleared even if the wait fails. */
int lsx_vulkan_submit_and_wait(
    lsx_vulkan_context_t *context, VkCommandBuffer command_buffer,
    VkFence fence, lsx_vulkan_wait_reason_t reason);

/* The batch depth in force, falling back to the default for a context that
 * has not been configured. */
uint32_t lsx_vulkan_resident_batch_depth(lsx_vulkan_context_t const *context);

/* Choose the batch depth for a chain from its shape and the work ahead of it,
 * and report the choice.  An environment override, if given, wins and is
 * left untouched.  Advisory: it tunes how far the pipeline runs ahead, and
 * changes no result. */
int lsx_vulkan_configure_resident_batch_depth(
    lsx_vulkan_context_t *context, sox_rate_t input_rate,
    sox_rate_t output_rate, uint32_t channels, uint64_t input_samples,
    lsx_vulkan_resident_topology_t topology);

#endif
