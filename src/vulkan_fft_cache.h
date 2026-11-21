/* (c) Simone Filippini <info@simonefilippini.it> 2026
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

/* Cache of compiled VkFFT kernel binaries, in process and on disk.
 *
 * VkFFT generates its own GLSL and compiles it with glslang every time a
 * context is created, which measured 1.487 s out of 1.517 s of startup on an
 * RTX 3080 (98%).  With saveApplicationToString / loadApplicationFromString
 * one context compiles and the rest load the SPIR-V it produced.
 *
 * This header includes no SoX headers, because the double-double
 * instantiation in vulkan_fft_dd.cpp reaches it from C++ without pulling
 * sox_i.h into a translation unit that redefines the host scalar type. */

#ifndef LSX_VULKAN_FFT_CACHE_H
#define LSX_VULKAN_FFT_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything a VkFFT plan depends on, and nothing else.  The coefficients do
 * not enter: they are data uploaded into buffers, and measured on the target
 * machine 8,192, 131,072 and 1,048,576 taps all compiled to the same 120 ms
 * per context.  The channel count does, because VkFFT emits numberBatches as
 * a literal in the generated GLSL.
 *
 * The buffer size is in the key because VkFFT derives its descriptor
 * ranges, and with them specialization constants that reach the generated
 * code, from it.  In practice it follows from the transform, so keying on it
 * costs no extra entries.
 *
 * pipelineCacheUUID is not in the key: it identifies driver pipeline
 * binaries, and the blob here is driver-independent SPIR-V, so the cache
 * survives a driver update.
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
  uint8_t use_lut;
} lsx_vulkan_fft_cache_key_t;

/* The cache is always active.  Its on-disk location follows the platform:
 * %LOCALAPPDATA%\sox\vkfft-cache, ~/Library/Caches/sox/vkfft-cache, or
 * $XDG_CACHE_HOME/sox/vkfft-cache (~/.cache/sox/vkfft-cache when
 * XDG_CACHE_HOME is unset or empty). */

/* Borrowed pointer into the cache, valid until lsx_vulkan_fft_cache_clear();
 * NULL on a miss.  The caller hands it straight to loadApplicationString,
 * which only reads from it.  A miss consults the on-disk cache before
 * reporting one, and a file found there is kept in memory too. */
void const *lsx_vulkan_fft_cache_lookup(lsx_vulkan_fft_cache_key_t const *key);

/* Takes a private copy: VkFFT frees saveApplicationString in deleteVkFFT. */
void lsx_vulkan_fft_cache_store(lsx_vulkan_fft_cache_key_t const *key, void const *blob, uint64_t size);

/* Called once when the Vulkan context goes away.  Reports hits and misses. */
void lsx_vulkan_fft_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif
