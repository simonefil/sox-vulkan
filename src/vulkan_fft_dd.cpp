/* Double-double instantiation of the VkFFT executor.
 *
 * VkFFT reaches true double-double precision only when its host scalar type
 * holds more than 53 bits, which on MSVC requires the shim in vulkan_dd/.
 * That shim changes pfLD for the whole translation unit, so it must not be
 * visible to the FP32 and FP64 profiles: every VkFFT entry point is static
 * inline, hence internal to its translation unit, and this file is a second,
 * independent instantiation used only by the reference profile. The plain C
 * executor in vulkan_fft.c is left untouched.
 *
 * Only Vulkan and VkFFT are included here; the SoX headers stay on the C
 * side, so this file needs no C++ compatibility from them.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "vulkan_fft_dd.h"

#include <glslang/Include/glslang_c_interface.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <vector>

/* VkFFT emits its double-double arithmetic as generated GLSL and never marks
 * any of it precise: the whole library contains no occurrence of the keyword.
 * Every compensation term in its butterflies is therefore algebraically zero
 * and free to be folded, which is exactly the defect that was found in our own
 * shaders -- and it is fatal here, because a folded compensation term leaves
 * the low word of every transformed value at exactly zero. Measured on an
 * RTX 3080: the forward transform returned pairs whose low half was zero on
 * every bin, so the double-double route was carrying a second word that never
 * held anything.
 *
 * Rather than edit VkFFT, its call to vkCreateShaderModule is redirected here
 * and the SPIR-V it produced is decorated with NoContraction on every
 * arithmetic result, which is what precise would have generated in the first
 * place. The redirection is a macro, so it reaches only the code textually
 * included below -- this translation unit is the double-double instantiation
 * alone, and the FP32 and FP64 executors in vulkan_fft.c are untouched.
 */
namespace {

/* SPIR-V physical layout: the magic and four more header words, then a stream
 * of instructions whose first word packs the word count and the opcode. */
const uint32_t spirv_magic = 0x07230203u;
const size_t spirv_header_words = 5u;

enum : uint32_t {
  op_decorate = 71u,
  op_f_negate = 127u,
  op_f_add = 129u,
  op_f_sub = 131u,
  op_f_mul = 133u,
  op_f_div = 136u,
  op_vector_times_scalar = 142u,
  op_dot = 148u,
  decoration_no_contraction = 42u
};

bool is_arithmetic(uint32_t opcode)
{
  return opcode == op_f_negate || opcode == op_f_add ||
      opcode == op_f_sub || opcode == op_f_mul ||
      opcode == op_f_div || opcode == op_vector_times_scalar ||
      opcode == op_dot;
}

/* Annotations must precede the first type declaration, so that is where the
 * new decorations go; opcodes 19 to 39 are the OpType family. */
bool starts_types(uint32_t opcode)
{
  return opcode >= 19u && opcode <= 39u;
}

std::vector<uint32_t> decorate_no_contraction(
    uint32_t const *words, size_t word_count)
{
  std::vector<uint32_t> result(words, words + word_count);
  std::vector<uint32_t> targets;
  std::vector<uint32_t> decorated;
  size_t insertion = word_count;
  size_t index = spirv_header_words;

  if (word_count < spirv_header_words || words[0] != spirv_magic)
    return result;
  while (index < word_count) {
    uint32_t opcode = words[index] & 0xffffu;
    uint32_t length = words[index] >> 16u;

    if (!length || index + length > word_count)
      return result;
    if (insertion == word_count && starts_types(opcode))
      insertion = index;
    if (opcode == op_decorate && length >= 3u &&
        words[index + 2u] == decoration_no_contraction)
      decorated.push_back(words[index + 1u]);
    /* The result id of an arithmetic instruction is its second operand, the
     * first being the result type. */
    if (is_arithmetic(opcode) && length >= 4u)
      targets.push_back(words[index + 2u]);
    index += length;
  }
  if (insertion == word_count)
    return result;
  {
    std::vector<uint32_t> additions;

    for (size_t target = 0; target < targets.size(); ++target) {
      bool present = false;

      for (size_t seen = 0; seen < decorated.size() && !present; ++seen)
        present = decorated[seen] == targets[target];
      for (size_t seen = 0; seen < additions.size() && !present; seen += 3u)
        present = additions[seen + 1u] == targets[target];
      if (present)
        continue;
      additions.push_back((3u << 16u) | op_decorate);
      additions.push_back(targets[target]);
      additions.push_back(decoration_no_contraction);
    }
    result.insert(
        result.begin() + (long)insertion,
        additions.begin(), additions.end());
  }
  return result;
}

VkResult create_decorated_shader_module(
    VkDevice device, VkShaderModuleCreateInfo const *create_info,
    VkAllocationCallbacks const *allocator, VkShaderModule *module)
{
  std::vector<uint32_t> patched = decorate_no_contraction(
      create_info->pCode, create_info->codeSize / sizeof(uint32_t));
  VkShaderModuleCreateInfo decorated = *create_info;

  decorated.pCode = patched.data();
  decorated.codeSize = patched.size() * sizeof(uint32_t);
  return vkCreateShaderModule(device, &decorated, allocator, module);
}

}

#define vkCreateShaderModule create_decorated_shader_module
#include <vkFFT.h>
#undef vkCreateShaderModule

namespace {

/* Allocated with calloc and released with free, so every member starts zeroed.
 * Default member initialisers would be dead code here: the constructor is
 * never run. */
struct dd_fft {
  VkFFTApplication application;
  uint64_t buffer_size;
  bool initialized;
};

}

extern "C" void *lsx_vulkan_fft_dd_create(
    VkDevice *device, VkPhysicalDevice *physical_device,
    VkQueue *queue, VkCommandPool *command_pool,
    VkBuffer *buffer, uint64_t buffer_size,
    uint32_t length, uint32_t batches,
    int real_to_complex, int normalize_inverse,
    VkFence *fence, lsx_vulkan_fft_cache_key_t const *key,
    int *result_code)
{
  VkFFTConfiguration configuration = VKFFT_ZERO_INIT;
  dd_fft *context;
  void const *cached = key ?
      lsx_vulkan_fft_cache_lookup(key, NULL) : NULL;
  VkFFTResult result;

  if (result_code)
    *result_code = 0;
  context = (dd_fft *)calloc(1, sizeof(*context));
  if (!context)
    return NULL;
  context->buffer_size = buffer_size;
  configuration.FFTdim = 1;
  configuration.size[0] = length;
  configuration.numberBatches = batches;
  configuration.doublePrecision = 0u;
  configuration.quadDoubleDoublePrecision = 1u;
  configuration.useLUT = 1;
  configuration.performR2C = real_to_complex ? 1u : 0u;
  configuration.normalize = normalize_inverse ? 1u : 0u;
  configuration.device = device;
  configuration.physicalDevice = physical_device;
  configuration.queue = queue;
  configuration.commandPool = command_pool;
  configuration.fence = fence;
  configuration.buffer = buffer;
  configuration.bufferSize = &context->buffer_size;
  configuration.isCompilerInitialized = 1;
  /* The cached blob is VkFFT's own SPIR-V, and on load it goes back through
   * the same vkCreateShaderModule that is redirected above, so the
   * NoContraction decoration is reapplied to it exactly as if it had just
   * been compiled.  What is cached is codegen plus glslang, not the
   * decoration, and the reference profile's arithmetic is unaffected. */
  if (cached) {
    configuration.loadApplicationFromString = 1;
    configuration.loadApplicationString = const_cast<void *>(cached);
  } else if (key && lsx_vulkan_fft_cache_enabled())
    configuration.saveApplicationToString = 1;
  result = initializeVkFFT(&context->application, configuration);
  if (result != VKFFT_SUCCESS && cached) {
    memset(&context->application, 0, sizeof(context->application));
    configuration.loadApplicationFromString = 0;
    configuration.loadApplicationString = 0;
    configuration.saveApplicationToString = 1;
    cached = NULL;
    result = initializeVkFFT(&context->application, configuration);
  }
  if (result != VKFFT_SUCCESS) {
    if (result_code)
      *result_code = (int)result;
    free(context);
    return NULL;
  }
  context->initialized = true;
  if (!cached)
    lsx_vulkan_fft_cache_store(
        key, context->application.saveApplicationString,
        context->application.applicationStringSize);
  return context;
}

extern "C" void lsx_vulkan_fft_dd_destroy(void *handle)
{
  dd_fft *context = (dd_fft *)handle;

  if (!context)
    return;
  if (context->initialized)
    deleteVkFFT(&context->application);
  free(context);
}

extern "C" int lsx_vulkan_fft_dd_append(
    void *handle, VkCommandBuffer command_buffer, int inverse)
{
  dd_fft *context = (dd_fft *)handle;
  VkFFTLaunchParams launch = VKFFT_ZERO_INIT;

  if (!context || !context->initialized || !command_buffer)
    return -1;
  launch.commandBuffer = &command_buffer;
  return (int)VkFFTAppend(
      &context->application, inverse ? 1 : -1, &launch);
}
