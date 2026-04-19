/* In-process cache of compiled VkFFT kernel binaries.  See the header.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "vulkan_fft_cache.h"

#include <stdlib.h>
#include <string.h>

typedef struct cache_entry {
  struct cache_entry *next;
  lsx_vulkan_fft_cache_key_t key;
  void *blob;
  uint64_t size;
} cache_entry_t;

static cache_entry_t *entries;
static unsigned hits;
static unsigned misses;

/* Compared field by field rather than with memcmp, which would read padding
 * bytes that a partially assigned key leaves indeterminate. */
static int key_equal(
    lsx_vulkan_fft_cache_key_t const *left,
    lsx_vulkan_fft_cache_key_t const *right)
{
  return left->vkfft_version == right->vkfft_version &&
      left->vendor_id == right->vendor_id &&
      left->device_id == right->device_id &&
      left->length == right->length &&
      left->batches == right->batches &&
      left->double_precision == right->double_precision &&
      left->double_double_precision == right->double_double_precision &&
      left->real_to_complex == right->real_to_complex &&
      left->normalize_inverse == right->normalize_inverse &&
      left->use_lut == right->use_lut;
}

int lsx_vulkan_fft_cache_enabled(void)
{
  char const *setting = getenv("SOX_VULKAN_FFT_CACHE");

  return !setting || strcmp(setting, "0") != 0;
}

void const *lsx_vulkan_fft_cache_lookup(
    lsx_vulkan_fft_cache_key_t const *key, uint64_t *size)
{
  cache_entry_t *entry;

  if (!key || !lsx_vulkan_fft_cache_enabled())
    return NULL;
  for (entry = entries; entry; entry = entry->next)
    if (key_equal(&entry->key, key)) {
      ++hits;
      if (size)
        *size = entry->size;
      return entry->blob;
    }
  ++misses;
  return NULL;
}

void lsx_vulkan_fft_cache_store(
    lsx_vulkan_fft_cache_key_t const *key, void const *blob, uint64_t size)
{
  cache_entry_t *entry;

  if (!key || !blob || !size || !lsx_vulkan_fft_cache_enabled())
    return;
  entry = lsx_calloc(1, sizeof(*entry));
  entry->key = *key;
  entry->size = size;
  entry->blob = lsx_malloc((size_t)size);
  memcpy(entry->blob, blob, (size_t)size);
  entry->next = entries;
  entries = entry;
}

void lsx_vulkan_fft_cache_clear(void)
{
  cache_entry_t *entry = entries;
  uint64_t total = 0;
  unsigned count = 0;

  while (entry) {
    cache_entry_t *next = entry->next;

    total += entry->size;
    ++count;
    free(entry->blob);
    free(entry);
    entry = next;
  }
  entries = NULL;
  if (hits || misses)
    lsx_report(
        "VkFFT kernel cache: %u compiled, %u loaded, %u entries, "
        "%llu bytes",
        misses, hits, count, (unsigned long long)total);
  hits = 0;
  misses = 0;
}
