/* Shared VkFFT executor for Vulkan SoX effects.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "vulkan_fft.h"
#include "vulkan_fft_cache.h"
#include "vulkan_fft_dd.h"

#include <glslang/Include/glslang_c_interface.h>
#include <vkFFT.h>

#include <stdlib.h>

struct lsx_vulkan_fft {
  lsx_vulkan_context_t *vulkan;
  VkFFTApplication application;
  uint64_t buffer_size;
  sox_bool initialized;
  sox_bool compiler_acquired;
  void *double_double;
};

static unsigned compiler_users;

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

/* Both instantiations build their key here so that the two agree on what a
 * plan depends on; the double-double one is handed the finished key. */
static void cache_key_init(
    lsx_vulkan_fft_cache_key_t *key, lsx_vulkan_context_t const *vulkan,
    uint64_t buffer_size, uint32_t length, uint32_t batches,
    sox_bool double_precision,
    sox_bool double_double_precision, sox_bool use_lut,
    sox_bool real_to_complex, sox_bool normalize_inverse)
{
  memset(key, 0, sizeof(*key));
  key->buffer_size = buffer_size;
  key->vkfft_version = (uint32_t)VkFFTGetVersion();
  key->vendor_id = vulkan->properties.vendorID;
  key->device_id = vulkan->properties.deviceID;
  key->length = length;
  key->batches = batches;
  key->double_precision = double_precision ? 1u : 0u;
  key->double_double_precision = double_double_precision ? 1u : 0u;
  key->real_to_complex = real_to_complex ? 1u : 0u;
  key->normalize_inverse = normalize_inverse ? 1u : 0u;
  key->use_lut = use_lut ? 1u : 0u;
}

lsx_vulkan_fft_t *lsx_vulkan_fft_create(
    lsx_vulkan_context_t *vulkan, lsx_vulkan_buffer_t *buffer,
    uint32_t length, uint32_t batches, sox_bool double_precision,
    sox_bool double_double_precision,
    sox_bool real_to_complex, sox_bool normalize_inverse,
    VkFence *fence)
{
  lsx_vulkan_fft_t *context;
  VkFFTConfiguration configuration = VKFFT_ZERO_INIT;
  lsx_vulkan_fft_cache_key_t key;
  void const *cached;
  VkFFTResult result;

  if (!vulkan || !buffer || !buffer->buffer || !buffer->size ||
      !length || !batches || !fence)
    return NULL;
  context = lsx_calloc(1, sizeof(*context));
  context->vulkan = vulkan;
  context->buffer_size = buffer->size;
  if (compiler_acquire() != SOX_SUCCESS)
    goto error;
  context->compiler_acquired = sox_true;
  if (lsx_vulkan_result(
      vkResetFences(vulkan->device, 1, fence),
      "vkResetFences before initializeVkFFT") != SOX_SUCCESS)
    goto error;
  if (double_double_precision) {
    int dd_result = 0;

    /* The double-double instantiation always uses the LUT. */
    cache_key_init(
        &key, vulkan, context->buffer_size, length, batches, sox_false,
        sox_true, sox_true, real_to_complex, normalize_inverse);
    context->double_double = lsx_vulkan_fft_dd_create(
        &vulkan->device, &vulkan->physical_device, &vulkan->queue,
        &vulkan->command_pool, &buffer->buffer, context->buffer_size,
        length, batches, real_to_complex ? 1 : 0,
        normalize_inverse ? 1 : 0, fence, &key, &dd_result);
    if (!context->double_double) {
      lsx_fail(
          "double-double initializeVkFFT failed with result %d",
          dd_result);
      goto error;
    }
    return context;
  }
  configuration.FFTdim = 1;
  configuration.size[0] = length;
  configuration.numberBatches = batches;
  configuration.doublePrecision = double_precision ? 1u : 0u;
  configuration.useLUT = !double_precision ? 1 : 0;
  configuration.performR2C = real_to_complex ? 1u : 0u;
  configuration.normalize = normalize_inverse ? 1u : 0u;
  configuration.device = &vulkan->device;
  configuration.physicalDevice = &vulkan->physical_device;
  configuration.queue = &vulkan->queue;
  configuration.commandPool = &vulkan->command_pool;
  configuration.fence = fence;
  configuration.buffer = &buffer->buffer;
  configuration.bufferSize = &context->buffer_size;
  configuration.isCompilerInitialized = 1;
  cache_key_init(
      &key, vulkan, context->buffer_size, length, batches,
      double_precision, sox_false, configuration.useLUT != 0,
      real_to_complex, normalize_inverse);
  cached = lsx_vulkan_fft_cache_lookup(&key, NULL);
  if (cached) {
    configuration.loadApplicationFromString = 1;
    configuration.loadApplicationString = (void *)(uintptr_t)cached;
  } else if (lsx_vulkan_fft_cache_enabled())
    configuration.saveApplicationToString = 1;
  result = initializeVkFFT(&context->application, configuration);
  if (result != VKFFT_SUCCESS && cached) {
    /* A blob that VkFFT refuses is not worth failing over: drop back to
     * compiling.  Cannot happen while the cache lives in the process, but
     * the same path serves the on-disk cache, where a stale or truncated
     * file has to degrade into a slow start rather than an error.  VkFFT
     * requires a zeroed application struct, and rejects a reused one. */
    lsx_warn(
        "cached VkFFT kernels rejected with result %d, recompiling",
        (int)result);
    memset(&context->application, 0, sizeof(context->application));
    configuration.loadApplicationFromString = 0;
    configuration.loadApplicationString = NULL;
    configuration.saveApplicationToString = 1;
    cached = NULL;
    result = initializeVkFFT(&context->application, configuration);
  }
  if (result != VKFFT_SUCCESS) {
    lsx_fail("initializeVkFFT failed with result %d", (int)result);
    goto error;
  }
  context->initialized = sox_true;
  if (!cached)
    lsx_vulkan_fft_cache_store(
        &key, context->application.saveApplicationString,
        context->application.applicationStringSize);
  return context;

error:
  lsx_vulkan_fft_destroy(context);
  return NULL;
}

void lsx_vulkan_fft_destroy(lsx_vulkan_fft_t *context)
{
  if (!context)
    return;
  if (context->double_double)
    lsx_vulkan_fft_dd_destroy(context->double_double);
  if (context->initialized)
    deleteVkFFT(&context->application);
  if (context->compiler_acquired)
    compiler_release();
  free(context);
}

int lsx_vulkan_fft_append(
    lsx_vulkan_fft_t *context, VkCommandBuffer command_buffer,
    sox_bool inverse)
{
  VkFFTLaunchParams launch = VKFFT_ZERO_INIT;
  VkFFTResult result;

  if (!context || !command_buffer)
    return SOX_EOF;
  if (context->double_double) {
    result = (VkFFTResult)lsx_vulkan_fft_dd_append(
        context->double_double, command_buffer, inverse ? 1 : 0);
    if (result != VKFFT_SUCCESS) {
      lsx_fail(
          "double-double VkFFT %s command recording failed with "
          "result %d", inverse ? "inverse" : "forward", (int)result);
      return SOX_EOF;
    }
    return SOX_SUCCESS;
  }
  if (!context->initialized)
    return SOX_EOF;
  launch.commandBuffer = &command_buffer;
  result = VkFFTAppend(
      &context->application, inverse ? 1 : -1, &launch);
  if (result != VKFFT_SUCCESS) {
    lsx_fail(
        "VkFFT %s command recording failed with result %d",
        inverse ? "inverse" : "forward", (int)result);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}
