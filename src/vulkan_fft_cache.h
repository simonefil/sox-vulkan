/* In-process cache of compiled VkFFT kernel binaries.
 *
 * VkFFT generates its own GLSL and compiles it with glslang every time a
 * context is created, which measured 1.487 s out of 1.517 s of startup on an
 * RTX 3080 -- 98% -- and is paid sixteen times for eight chained effects.
 * saveApplicationToString / loadApplicationFromString exist precisely for
 * this: one context compiles, the rest load the SPIR-V it produced.
 *
 * Deliberately free of SoX headers, because the double-double instantiation
 * in vulkan_fft_dd.cpp has to reach it from C++ without pulling sox_i.h into
 * a translation unit that redefines the host scalar type.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_VULKAN_FFT_CACHE_H
#define LSX_VULKAN_FFT_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything a VkFFT plan depends on, and nothing else.  Measured on the
 * target machine: the number of taps does not enter (8,192 / 131,072 /
 * 1,048,576 taps all compiled to the same 120 ms per context) and neither do
 * the coefficients, which are data uploaded into buffers rather than compiled
 * into kernels.  The channel count does, because VkFFT emits numberBatches as
 * a literal in the generated GLSL.  Entries are therefore (profile) x
 * (channel count), at most 32 for four profiles and SoX's eight channels.
 *
 * The buffer size is in the key because VkFFT derives its descriptor
 * ranges, and with them specialization constants that reach the generated
 * code, from it.  In practice it follows from the transform, so keying on it
 * costs no extra entries and removes the question.
 *
 * pipelineCacheUUID is deliberately absent: it identifies driver pipeline
 * binaries, whereas the blob here is SPIR-V and independent of the driver.
 * Including it would discard the cache on every driver update for nothing.
 */
typedef struct {
  uint64_t buffer_size;
  uint32_t vkfft_version;
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t length;
  uint32_t batches;
  uint8_t double_precision;
  uint8_t double_double_precision;
  uint8_t real_to_complex;
  uint8_t normalize_inverse;
  uint8_t use_lut;
} lsx_vulkan_fft_cache_key_t;

/* Nonzero unless SOX_VULKAN_FFT_CACHE is set to 0, in which case every
 * context compiles its own kernels as before.  Two further variables affect
 * only the on-disk half: SOX_VULKAN_FFT_DISK_CACHE=0 keeps the cache in the
 * process, and SOX_VULKAN_FFT_CACHE_DIR replaces the default location
 * (%LOCALAPPDATA%\sox\vkfft-cache, ~/Library/Caches/sox/vkfft-cache, or
 * $XDG_CACHE_HOME/sox/vkfft-cache). */
int lsx_vulkan_fft_cache_enabled(void);

/* Borrowed pointer into the cache, valid until lsx_vulkan_fft_cache_clear();
 * NULL on a miss.  The caller hands it straight to loadApplicationString,
 * which only reads from it.  A miss consults the on-disk cache before
 * reporting one, and a file found there is kept in memory too. */
void const *lsx_vulkan_fft_cache_lookup(lsx_vulkan_fft_cache_key_t const *key, uint64_t *size);

/* Takes a private copy: VkFFT frees saveApplicationString in deleteVkFFT. */
void lsx_vulkan_fft_cache_store(lsx_vulkan_fft_cache_key_t const *key, void const *blob, uint64_t size);

/* Called once when the Vulkan context goes away.  Reports hits and misses. */
void lsx_vulkan_fft_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif
