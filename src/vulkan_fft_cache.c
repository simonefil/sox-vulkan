/* Cache of compiled VkFFT kernel binaries, in-process and on disk.
 * See the header.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "vulkan_fft_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define cache_mkdir(path) _mkdir(path)
#define cache_getpid() _getpid()
#define CACHE_PATH_SEPARATOR '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define cache_mkdir(path) mkdir((path), 0777)
#define cache_getpid() getpid()
#define CACHE_PATH_SEPARATOR '/'
#endif

typedef struct cache_entry {
  struct cache_entry *next;
  lsx_vulkan_fft_cache_key_t key;
  void *blob;
  uint64_t size;
} cache_entry_t;

static cache_entry_t *entries;
static unsigned hits;
static unsigned misses;
static unsigned disk_hits;
static unsigned disk_writes;

/* Compared field by field rather than with memcmp, which would read padding
 * bytes that a partially assigned key leaves indeterminate. */
static int key_equal(
    lsx_vulkan_fft_cache_key_t const *left,
    lsx_vulkan_fft_cache_key_t const *right)
{
  return left->buffer_size == right->buffer_size &&
      left->vkfft_version == right->vkfft_version &&
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

/* ---------------------------------------------------------------- on disk */

/* The in-process cache alone still pays a full compile once per invocation,
 * and the shipped product is a sox binary run once per file: that first
 * compile is the whole of what the user waits for.  The same blob therefore
 * goes to a file keyed on the same fields, so that the second invocation on
 * a machine starts with no compilation at all.
 *
 * The file is self-describing and fully verified on read -- format magic,
 * sox version, every key field, blob length and a hash of the blob -- so a
 * stale, truncated or foreign file reads as a miss rather than as kernels.
 * Nothing here can make a run incorrect; the worst case is a slow start. */

#define CACHE_FILE_MAGIC 0x564b4643u /* "VKFC" */
#define CACHE_FILE_FORMAT 1u

static int disk_enabled(void)
{
  char const *setting = getenv("SOX_VULKAN_FFT_DISK_CACHE");

  return lsx_vulkan_fft_cache_enabled() &&
      (!setting || strcmp(setting, "0") != 0);
}

static uint64_t blob_hash(void const *blob, uint64_t size)
{
  unsigned char const *bytes = blob;
  uint64_t hash = 14695981039346656037ULL;
  uint64_t index;

  for (index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static void put_u32(FILE *file, uint32_t value)
{
  unsigned char bytes[4];
  int index;

  for (index = 0; index < 4; ++index)
    bytes[index] = (unsigned char)(value >> (8 * index));
  fwrite(bytes, 1, sizeof(bytes), file);
}

static void put_u64(FILE *file, uint64_t value)
{
  unsigned char bytes[8];
  int index;

  for (index = 0; index < 8; ++index)
    bytes[index] = (unsigned char)(value >> (8 * index));
  fwrite(bytes, 1, sizeof(bytes), file);
}

static int get_u32(FILE *file, uint32_t *value)
{
  unsigned char bytes[4];
  int index;

  if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
    return 0;
  *value = 0;
  for (index = 0; index < 4; ++index)
    *value |= (uint32_t)bytes[index] << (8 * index);
  return 1;
}

static int get_u64(FILE *file, uint64_t *value)
{
  unsigned char bytes[8];
  int index;

  if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
    return 0;
  *value = 0;
  for (index = 0; index < 8; ++index)
    *value |= (uint64_t)bytes[index] << (8 * index);
  return 1;
}

/* Assembles path/component, creating each directory as it goes.  Returns
 * zero if any level could not be created, which disables the disk cache for
 * this run rather than failing the effect. */
static int append_directory(
    char *path, size_t size, char const *component)
{
  size_t used = strlen(path);

  if (used + strlen(component) + 2 > size)
    return 0;
  if (used)
    path[used++] = CACHE_PATH_SEPARATOR;
  strcpy(path + used, component);
  if (cache_mkdir(path) != 0 && errno != EEXIST)
    return 0;
  return 1;
}

/* %LOCALAPPDATA%\sox\vkfft-cache, ~/Library/Caches/sox/vkfft-cache, or
 * $XDG_CACHE_HOME/sox/vkfft-cache; SOX_VULKAN_FFT_CACHE_DIR overrides. */
static char const *cache_directory(void)
{
  static char path[1024];
  static int resolved;
  char const *override = getenv("SOX_VULKAN_FFT_CACHE_DIR");
  char const *base;

  if (resolved)
    return path[0] ? path : NULL;
  resolved = 1;
  if (override && *override) {
    if (strlen(override) + 1 > sizeof(path))
      return NULL;
    strcpy(path, override);
    if (cache_mkdir(path) != 0 && errno != EEXIST)
      path[0] = '\0';
    return path[0] ? path : NULL;
  }
  {
    /* Each platform's cache root, then the same sox/vkfft-cache below it. */
    char const *below[3];
    size_t levels = 0;
    size_t level;

#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
#elif defined(__APPLE__)
    base = getenv("HOME");
    below[levels++] = "Library";
    below[levels++] = "Caches";
#else
    base = getenv("XDG_CACHE_HOME");
    if (!base || !*base) {
      base = getenv("HOME");
      below[levels++] = ".cache";
    }
#endif
    if (!base || !*base || strlen(base) + 1 > sizeof(path))
      return NULL;
    strcpy(path, base);
    for (level = 0; level < levels; ++level)
      if (!append_directory(path, sizeof(path), below[level])) {
        path[0] = '\0';
        return NULL;
      }
  }
  if (!append_directory(path, sizeof(path), "sox") ||
      !append_directory(path, sizeof(path), "vkfft-cache"))
    path[0] = '\0';
  return path[0] ? path : NULL;
}

/* The sox version is verified from the file header rather than named here,
 * so that a new version overwrites the stale file instead of leaving it
 * behind to accumulate. */
static int entry_path(
    lsx_vulkan_fft_cache_key_t const *key, char *path, size_t size)
{
  char const *directory = cache_directory();
  unsigned flags;
  int written;

  if (!directory)
    return 0;
  flags = (unsigned)key->double_precision |
      ((unsigned)key->double_double_precision << 1) |
      ((unsigned)key->real_to_complex << 2) |
      ((unsigned)key->normalize_inverse << 3) |
      ((unsigned)key->use_lut << 4);
  written = snprintf(
      path, size, "%s%c%08x-%08x-%08x-%u-%u-%llx-%02x.vkfft",
      directory, CACHE_PATH_SEPARATOR, key->vendor_id, key->device_id,
      key->vkfft_version, key->length, key->batches,
      (unsigned long long)key->buffer_size, flags);
  return written > 0 && (size_t)written < size;
}

static void write_key(FILE *file, lsx_vulkan_fft_cache_key_t const *key)
{
  put_u64(file, key->buffer_size);
  put_u32(file, key->vkfft_version);
  put_u32(file, key->vendor_id);
  put_u32(file, key->device_id);
  put_u32(file, key->length);
  put_u32(file, key->batches);
  put_u32(file, key->double_precision);
  put_u32(file, key->double_double_precision);
  put_u32(file, key->real_to_complex);
  put_u32(file, key->normalize_inverse);
  put_u32(file, key->use_lut);
}

static int read_key_matches(
    FILE *file, lsx_vulkan_fft_cache_key_t const *key)
{
  lsx_vulkan_fft_cache_key_t stored;
  uint32_t fields[10];
  uint64_t buffer_size;
  size_t index;

  if (!get_u64(file, &buffer_size))
    return 0;
  for (index = 0; index < 10; ++index)
    if (!get_u32(file, &fields[index]))
      return 0;
  memset(&stored, 0, sizeof(stored));
  stored.buffer_size = buffer_size;
  stored.vkfft_version = fields[0];
  stored.vendor_id = fields[1];
  stored.device_id = fields[2];
  stored.length = fields[3];
  stored.batches = fields[4];
  stored.double_precision = (uint8_t)fields[5];
  stored.double_double_precision = (uint8_t)fields[6];
  stored.real_to_complex = (uint8_t)fields[7];
  stored.normalize_inverse = (uint8_t)fields[8];
  stored.use_lut = (uint8_t)fields[9];
  return key_equal(&stored, key);
}

static int version_matches(FILE *file)
{
  char const *version = sox_version();
  uint32_t length;
  char stored[64];

  if (!get_u32(file, &length) || length >= sizeof(stored))
    return 0;
  if (fread(stored, 1, length, file) != length)
    return 0;
  stored[length] = '\0';
  return strcmp(stored, version) == 0;
}

static void *disk_lookup(
    lsx_vulkan_fft_cache_key_t const *key, uint64_t *size)
{
  char path[1024];
  FILE *file;
  uint32_t magic = 0;
  uint32_t format = 0;
  uint64_t stored_size = 0;
  uint64_t stored_hash = 0;
  void *blob;

  if (!entry_path(key, path, sizeof(path)))
    return NULL;
  file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (!get_u32(file, &magic) || magic != CACHE_FILE_MAGIC ||
      !get_u32(file, &format) || format != CACHE_FILE_FORMAT ||
      !version_matches(file) || !read_key_matches(file, key) ||
      !get_u64(file, &stored_size) || !stored_size ||
      !get_u64(file, &stored_hash)) {
    fclose(file);
    return NULL;
  }
  /* A blob big enough to exhaust memory is a corrupt length field, not a
   * plan: the largest entry measured is a little over 300 KB. */
  if (stored_size > 64u * 1024u * 1024u) {
    fclose(file);
    return NULL;
  }
  blob = lsx_malloc((size_t)stored_size);
  if (fread(blob, 1, (size_t)stored_size, file) != stored_size ||
      blob_hash(blob, stored_size) != stored_hash) {
    free(blob);
    fclose(file);
    return NULL;
  }
  fclose(file);
  if (size)
    *size = stored_size;
  return blob;
}

/* Written to a temporary name and renamed, so that a reader never sees a
 * half-written file and two concurrent sox processes cannot interleave. */
static void disk_store(
    lsx_vulkan_fft_cache_key_t const *key, void const *blob, uint64_t size)
{
  char path[1024];
  char temporary[1024];
  char const *version = sox_version();
  FILE *file;
  int written;

  if (!entry_path(key, path, sizeof(path)))
    return;
  written = snprintf(
      temporary, sizeof(temporary), "%s.%d.tmp", path,
      (int)cache_getpid());
  if (written <= 0 || (size_t)written >= sizeof(temporary))
    return;
  file = fopen(temporary, "wb");
  if (!file)
    return;
  put_u32(file, CACHE_FILE_MAGIC);
  put_u32(file, CACHE_FILE_FORMAT);
  put_u32(file, (uint32_t)strlen(version));
  fwrite(version, 1, strlen(version), file);
  write_key(file, key);
  put_u64(file, size);
  put_u64(file, blob_hash(blob, size));
  fwrite(blob, 1, (size_t)size, file);
  if (ferror(file) || fclose(file) != 0) {
    remove(temporary);
    return;
  }
#ifdef _WIN32
  /* rename() will not replace an existing file on Windows; elsewhere it is
   * left to replace atomically, so a concurrent reader never sees a gap. */
  remove(path);
#endif
  if (rename(temporary, path) != 0) {
    remove(temporary);
    return;
  }
  ++disk_writes;
}

/* ------------------------------------------------------------- in process */

static void memory_store(
    lsx_vulkan_fft_cache_key_t const *key, void *blob, uint64_t size)
{
  cache_entry_t *entry = lsx_calloc(1, sizeof(*entry));

  entry->key = *key;
  entry->size = size;
  entry->blob = blob;
  entry->next = entries;
  entries = entry;
}

void const *lsx_vulkan_fft_cache_lookup(
    lsx_vulkan_fft_cache_key_t const *key, uint64_t *size)
{
  cache_entry_t *entry;
  void *blob;
  uint64_t blob_size = 0;

  if (!key || !lsx_vulkan_fft_cache_enabled())
    return NULL;
  for (entry = entries; entry; entry = entry->next)
    if (key_equal(&entry->key, key)) {
      ++hits;
      if (size)
        *size = entry->size;
      return entry->blob;
    }
  blob = disk_enabled() ? disk_lookup(key, &blob_size) : NULL;
  if (blob) {
    ++disk_hits;
    memory_store(key, blob, blob_size);
    if (size)
      *size = blob_size;
    return blob;
  }
  ++misses;
  return NULL;
}

void lsx_vulkan_fft_cache_store(
    lsx_vulkan_fft_cache_key_t const *key, void const *blob, uint64_t size)
{
  void *copy;

  if (!key || !blob || !size || !lsx_vulkan_fft_cache_enabled())
    return;
  /* VkFFT frees saveApplicationString in deleteVkFFT. */
  copy = lsx_malloc((size_t)size);
  memcpy(copy, blob, (size_t)size);
  memory_store(key, copy, size);
  if (disk_enabled())
    disk_store(key, blob, size);
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
  if (hits || misses || disk_hits)
    lsx_report(
        "VkFFT kernel cache: %u compiled, %u from disk, %u in process, "
        "%u entries, %llu bytes, %u written",
        misses, disk_hits, hits, count, (unsigned long long)total,
        disk_writes);
  hits = 0;
  misses = 0;
  disk_hits = 0;
  disk_writes = 0;
}
