/* Machine-readable diagnostics for one run of SoX.  See the header.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define diagnostics_mkdir(path) _mkdir(path)
#define DIAGNOSTICS_PATH_SEPARATOR '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define diagnostics_mkdir(path) mkdir((path), 0777)
#define DIAGNOSTICS_PATH_SEPARATOR '/'
#endif

/* Each flow of a tapped effect writes its own part file and the parts are
 * interleaved when the run ends.  The alternative -- one file written by
 * whichever flow is running -- would interleave the channels in thread
 * arrival order, which is not an order at all. */
#if defined _MSC_VER
#define DIAGNOSTICS_THREAD_LOCAL __declspec(thread)
#elif defined __GNUC__
#define DIAGNOSTICS_THREAD_LOCAL __thread
#else
#define DIAGNOSTICS_THREAD_LOCAL
#endif

int lsx_diagnostics_on;

typedef struct {
  char *key;
  char *value;
} entry_t;

/* One flow's share of a capture.  The file is opened by the thread that first
 * writes to it and touched by no other, so the slots can be handed out before
 * the flows start and then used without any lock. */
typedef struct {
  FILE *file;
  char *path;
  uint64_t samples;
} slot_t;

typedef struct {
  char const *name;             /* File name inside the directory. */
  char const *point;            /* Where in the pipeline it was taken. */
  char const *element;          /* f64 or dd. */
  unsigned doubles;             /* Doubles per sample. */
  slot_t *slots;
  size_t slot_count;
  int used;
} capture_t;

static char *directory;
static entry_t *entries;
static size_t entry_count;
static size_t entry_size;
/* Keys published by an effect that has no number yet; see the header. */
static entry_t *pending;
static size_t pending_count;
static size_t pending_size;
static sox_effects_chain_t const *diagnostics_chain;
static size_t chain_count;
static size_t *frames_in;
static size_t *frames_out;
static size_t frame_counts;

/* Both taps, in the order they are numbered in run.txt. */
static capture_t captures[2];
#define CAPTURE_F64 0
#define CAPTURE_DD  1

static int tap_armed;
static unsigned tap_channels;
static char const *tap_effect;
static DIAGNOSTICS_THREAD_LOCAL size_t tap_flow;

/* Packed DSD reaches the writer without passing through lsx_save_samples(),
 * so it has its own passive capture at the common writer input. */
static FILE *dsd_file;
static char *dsd_path;
static uint64_t dsd_frames;
static unsigned dsd_channels;
static uint8_t *dsd_buffer;
static size_t dsd_buffer_capacity;

static char *format_string(char const *fmt, va_list args)
{
  va_list copy;
  char *text;
  int length;

  va_copy(copy, args);
  length = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (length < 0)
    lsx_diagnostics_fail("diagnostics could not format a value");
  text = lsx_malloc((size_t)length + 1);
  vsnprintf(text, (size_t)length + 1, fmt, args);
  return text;
}

static char *join_path(char const *dir, char const *leaf)
{
  size_t length = strlen(dir) + 1 + strlen(leaf) + 1;
  char *path = lsx_malloc(length);

  sprintf(path, "%s%c%s", dir, DIAGNOSTICS_PATH_SEPARATOR, leaf);
  return path;
}

static void set_value(char const *key, char *value)
{
  size_t index;

  for (index = 0; index < entry_count; ++index)
    if (!strcmp(entries[index].key, key)) {
      free(entries[index].value);
      entries[index].value = value;
      return;
    }
  if (entry_count == entry_size) {
    entry_size = entry_size ? entry_size * 2 : 64;
    lsx_revalloc(entries, entry_size);
  }
  entries[entry_count].key = lsx_strdup(key);
  entries[entry_count].value = value;
  ++entry_count;
}

void lsx_diagnostics_setf(char const *key, char const *fmt, ...)
{
  va_list args;
  char *value;

  if (!lsx_diagnostics_on)
    return;
  va_start(args, fmt);
  value = format_string(fmt, args);
  va_end(args);
  set_value(key, value);
}

int lsx_diagnostics_have(char const *key)
{
  size_t index;

  for (index = 0; index < entry_count; ++index)
    if (!strcmp(entries[index].key, key))
      return 1;
  return 0;
}

/* The chain position of the effect, or the chain length if it is not in the
 * chain.  effp may be any of an effect's flows, which are one array. */
static size_t effect_index(sox_effect_t const *effp)
{
  size_t index;

  if (!diagnostics_chain || !effp)
    return 0;
  for (index = 0; index < diagnostics_chain->length; ++index) {
    sox_effect_t const *base = diagnostics_chain->effects[index];

    if (effp >= base && effp < base + base->flows)
      return index;
  }
  return diagnostics_chain->length;
}

void lsx_diagnostics_effect_setf(sox_effect_t const *effp, char const *leaf,
    char const *fmt, ...)
{
  va_list args;
  char key[64];
  size_t index;
  char *value;

  if (!lsx_diagnostics_on)
    return;
  index = diagnostics_chain ? effect_index(effp) : 0;
  va_start(args, fmt);
  value = format_string(fmt, args);
  va_end(args);
  /* Before the effect is in the chain it has no number yet, so the key waits
   * for one instead of being written under a guess. */
  if (!diagnostics_chain || index >= diagnostics_chain->length) {
    if (pending_count == pending_size) {
      pending_size = pending_size ? pending_size * 2 : 16;
      lsx_revalloc(pending, pending_size);
    }
    pending[pending_count].key = lsx_strdup(leaf);
    pending[pending_count].value = value;
    ++pending_count;
    return;
  }
  if ((size_t)snprintf(key, sizeof(key), "effect.%" PRIuPTR ".%s", index, leaf) >= sizeof(key))
    lsx_diagnostics_fail("diagnostics key for effect %" PRIuPTR " is too long: %s", index, leaf);
  set_value(key, value);
}

void lsx_diagnostics_effect_pending_clear(void)
{
  size_t index;

  for (index = 0; index < pending_count; ++index) {
    free(pending[index].key);
    free(pending[index].value);
  }
  pending_count = 0;
}

void lsx_diagnostics_effect_pending_flush(size_t index)
{
  size_t held;

  for (held = 0; held < pending_count; ++held) {
    char key[64];

    if ((size_t)snprintf(key, sizeof(key), "effect.%" PRIuPTR ".%s", index,
        pending[held].key) >= sizeof(key))
      lsx_diagnostics_fail("diagnostics key for effect %" PRIuPTR " is too long: %s",
          index, pending[held].key);
    set_value(key, pending[held].value);
    free(pending[held].key);
  }
  pending_count = 0;
}

void lsx_diagnostics_chain(sox_effects_chain_t const *chain)
{
  if (!lsx_diagnostics_on)
    return;
  diagnostics_chain = chain;
  lsx_diagnostics_setf("chain.count", "%" PRIuPTR, ++chain_count);
  free(frames_in);
  free(frames_out);
  frame_counts = chain ? chain->length : 0;
  frames_in = frame_counts ? lsx_calloc(frame_counts, sizeof(*frames_in)) : NULL;
  frames_out = frame_counts ? lsx_calloc(frame_counts, sizeof(*frames_out)) : NULL;
}

static void write_effects(void);

void lsx_diagnostics_chain_done(void)
{
  if (!lsx_diagnostics_on || !diagnostics_chain)
    return;
  write_effects();
  diagnostics_chain = NULL;
}

void lsx_diagnostics_effect_frames(size_t n, size_t in, size_t out)
{
  if (!lsx_diagnostics_on || n >= frame_counts)
    return;
  frames_in[n] += in;
  frames_out[n] += out;
}

void lsx_diagnostics_open(char const *dir)
{
  if (!dir || !*dir) {
    lsx_fail("--diagnostics needs a directory");
    exit(1);
  }
  if (diagnostics_mkdir(dir) != 0 && errno != EEXIST) {
    lsx_fail("cannot create the diagnostics directory `%s': %s", dir, strerror(errno));
    exit(1);
  }
  directory = lsx_strdup(dir);
  captures[CAPTURE_F64].name = "chain-out.f64";
  captures[CAPTURE_F64].point = "pre-quantisation";
  captures[CAPTURE_F64].element = "f64";
  captures[CAPTURE_F64].doubles = 1;
  captures[CAPTURE_DD].name = "chain-out.dd";
  captures[CAPTURE_DD].point = "pair-collapse";
  captures[CAPTURE_DD].element = "dd";
  captures[CAPTURE_DD].doubles = 2;
  lsx_diagnostics_on = 1;
}

/* Both taps have the same shape, so they share one emitter: the difference
 * between them is how many doubles a sample is and where the call comes
 * from. */
static void capture_write(capture_t *capture, double const *values, size_t samples)
{
  slot_t *slot;

  if (!tap_armed || tap_flow >= capture->slot_count || !samples)
    return;
  slot = &capture->slots[tap_flow];
  if (!slot->file) {
    char leaf[64];

    snprintf(leaf, sizeof(leaf), "%s.part%" PRIuPTR, capture->name, tap_flow);
    slot->path = join_path(directory, leaf);
    slot->file = fopen(slot->path, "wb");
    if (!slot->file)
      lsx_diagnostics_fail("cannot write the capture `%s': %s", slot->path, strerror(errno));
    capture->used = 1;
  }
  if (fwrite(values, sizeof(*values) * capture->doubles, samples, slot->file) != samples)
    lsx_diagnostics_fail("cannot write the capture `%s': %s", slot->path, strerror(errno));
  slot->samples += samples;
}

void lsx_diagnostics_capture_f64(double const *samples, size_t n)
{
  if (!lsx_diagnostics_on || !tap_armed)
    return;
  capture_write(&captures[CAPTURE_F64], samples, n);
}

void lsx_diagnostics_capture_dd(double high, double low)
{
  double pair[2];

  if (!lsx_diagnostics_on || !tap_armed)
    return;
  pair[0] = high;
  pair[1] = low;
  capture_write(&captures[CAPTURE_DD], pair, 1);
}

static uint8_t reverse_byte(uint8_t value)
{
  value = (uint8_t)((value >> 4) | (value << 4));
  value = (uint8_t)(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
  return (uint8_t)(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
}

void lsx_diagnostics_capture_dsd(sox_sample_t const *samples, size_t n,
    unsigned channels, unsigned packing)
{
  size_t groups;
  size_t group;
  size_t bytes;

  if (!lsx_diagnostics_on || !samples || !n)
    return;
  if (!channels || n % channels ||
      (packing != SOX_DSD_PACKING_BYTE && packing != SOX_DSD_PACKING_WORD))
    lsx_diagnostics_fail("invalid packed DSD capture shape");
  if (dsd_channels && dsd_channels != channels)
    lsx_diagnostics_fail("packed DSD capture changed channel count");
  if (!dsd_file) {
    dsd_path = join_path(directory, "chain-out.dsd-u8");
    dsd_file = fopen(dsd_path, "wb");
    if (!dsd_file)
      lsx_diagnostics_fail("cannot write the capture `%s': %s", dsd_path, strerror(errno));
    dsd_channels = channels;
  }

  groups = n / channels;
  if (packing == SOX_DSD_PACKING_WORD && n > SIZE_MAX / 4u)
    lsx_diagnostics_fail("packed DSD capture is too large");
  bytes = packing == SOX_DSD_PACKING_BYTE ? n : n * 4u;
  if (bytes > dsd_buffer_capacity) {
    dsd_buffer = lsx_realloc(dsd_buffer, bytes);
    dsd_buffer_capacity = bytes;
  }
  if (packing == SOX_DSD_PACKING_BYTE) {
    for (group = 0; group < groups; ++group) {
      unsigned valid = SOX_DSD_PACKED_VALID_BITS(samples[group * channels]);
      unsigned channel;

      if (!valid || valid > 8)
        lsx_diagnostics_fail("invalid packed DSD byte capture");
      for (channel = 0; channel < channels; ++channel) {
        sox_sample_t sample = samples[group * channels + channel];
        uint8_t value;

        if (SOX_DSD_PACKED_VALID_BITS(sample) != valid)
          lsx_diagnostics_fail("packed DSD capture channels are out of step");
        value = reverse_byte(SOX_DSD_PACKED_DATA(sample));
        dsd_buffer[group * channels + channel] = value;
      }
      dsd_frames += valid;
    }
  } else {
    for (group = 0; group < groups; ++group) {
      unsigned byte;

      /* A word buffer is channel-major, but the canonical capture is
       * interleaved at every eight DSD frames, just like the byte path. */
      for (byte = 0; byte < 4; ++byte) {
        unsigned channel;

        for (channel = 0; channel < channels; ++channel) {
          uint32_t word = (uint32_t)samples[channel * groups + group];
          uint8_t value = (uint8_t)(word >> (8 * byte));

          dsd_buffer[(group * 4u + byte) * channels + channel] = value;
        }
      }
    }
    dsd_frames += groups * 32u;
  }
  if (fwrite(dsd_buffer, 1, bytes, dsd_file) != bytes)
    lsx_diagnostics_fail("cannot write the capture `%s': %s", dsd_path, strerror(errno));
}

int lsx_diagnostics_tap_armed(void)
{
  return tap_armed;
}

static void slots_reserve(capture_t *capture, size_t flows)
{
  if (flows <= capture->slot_count)
    return;
  lsx_revalloc(capture->slots, flows);
  memset(capture->slots + capture->slot_count, 0,
      (flows - capture->slot_count) * sizeof(*capture->slots));
  capture->slot_count = flows;
}

void lsx_diagnostics_tap_begin(sox_effect_t const *effp, size_t flows)
{
  if (!lsx_diagnostics_on || !effp || !flows)
    return;
  /* One effect is tapped for the whole run, so a second one with a different
   * flow count would mean two signals in one file. */
  if (tap_effect && strcmp(tap_effect, effp->handler.name))
    lsx_diagnostics_fail("two effects claimed the sample tap: %s and %s",
        tap_effect, effp->handler.name);
  if (tap_channels && tap_channels != effp->out_signal.channels)
    lsx_diagnostics_fail("the tapped effect changed channel count mid-run");
  if (flows > 1 && flows != effp->out_signal.channels)
    lsx_diagnostics_fail(
        "the tapped effect runs %" PRIuPTR " flows for %u channels, "
        "which is not a capture this can interleave",
        flows, effp->out_signal.channels);
  tap_effect = effp->handler.name;
  tap_channels = effp->out_signal.channels;
  slots_reserve(&captures[CAPTURE_F64], flows);
  slots_reserve(&captures[CAPTURE_DD], flows);
  tap_armed = 1;
}

void lsx_diagnostics_tap_flow(size_t flow)
{
  tap_flow = flow;
}

void lsx_diagnostics_tap_end(void)
{
  tap_armed = 0;
}

/* Interleave the part files into the capture's own, one frame at a time
 * across the flows.  With a single flow the effect already produced its
 * channels interleaved, so the part is the capture and only its name
 * changes. */
static uint64_t capture_assemble(capture_t *capture)
{
  char *path;
  FILE *output;
  uint64_t frames;
  size_t slot;
  double *block;
  size_t block_frames = 4096;
  uint64_t written = 0;

  for (slot = 0; slot < capture->slot_count; ++slot)
    if (capture->slots[slot].file) {
      fclose(capture->slots[slot].file);
      capture->slots[slot].file = NULL;
    }
  path = join_path(directory, capture->name);
  if (capture->slot_count == 1) {
    frames = tap_channels ? capture->slots[0].samples / tap_channels : 0;
    remove(path);
    if (rename(capture->slots[0].path, path) != 0)
      lsx_diagnostics_fail("cannot name the capture `%s': %s", path, strerror(errno));
    free(path);
    return frames;
  }
  frames = capture->slots[0].samples;
  for (slot = 1; slot < capture->slot_count; ++slot)
    if (capture->slots[slot].samples != frames)
      lsx_diagnostics_fail(
          "the tapped effect captured %llu samples on flow 0 and %llu on flow %" PRIuPTR,
          (unsigned long long)frames,
          (unsigned long long)capture->slots[slot].samples, slot);
  output = fopen(path, "wb");
  if (!output)
    lsx_diagnostics_fail("cannot write the capture `%s': %s", path, strerror(errno));
  for (slot = 0; slot < capture->slot_count; ++slot) {
    capture->slots[slot].file = fopen(capture->slots[slot].path, "rb");
    if (!capture->slots[slot].file)
      lsx_diagnostics_fail("cannot read back the capture `%s': %s",
          capture->slots[slot].path, strerror(errno));
  }
  block = lsx_malloc(block_frames * capture->slot_count * capture->doubles * sizeof(*block));
  while (written < frames) {
    size_t count = (size_t)min((uint64_t)block_frames, frames - written);
    size_t frame;

    for (slot = 0; slot < capture->slot_count; ++slot)
      if (fread(block + slot * block_frames * capture->doubles,
          sizeof(*block) * capture->doubles, count,
          capture->slots[slot].file) != count)
        lsx_diagnostics_fail("cannot read back the capture `%s'",
            capture->slots[slot].path);
    for (frame = 0; frame < count; ++frame)
      for (slot = 0; slot < capture->slot_count; ++slot)
        if (fwrite(block + (slot * block_frames + frame) * capture->doubles,
            sizeof(*block) * capture->doubles, 1, output) != 1)
          lsx_diagnostics_fail("cannot write the capture `%s': %s", path, strerror(errno));
    written += count;
  }
  free(block);
  for (slot = 0; slot < capture->slot_count; ++slot) {
    fclose(capture->slots[slot].file);
    capture->slots[slot].file = NULL;
  }
  fclose(output);
  free(path);
  return frames;
}

static void write_captures(void)
{
  unsigned index = 0;
  size_t which;

  for (which = 0; which < 2; ++which) {
    capture_t *capture = &captures[which];
    char key[64];
    uint64_t frames;
    size_t slot;

    if (!capture->used)
      continue;
    frames = capture_assemble(capture);
    for (slot = 0; slot < capture->slot_count; ++slot)
      if (capture->slots[slot].path) {
        /* The single-flow part became the capture itself, so removing it by
         * name would delete what was just assembled. */
        if (capture->slot_count > 1)
          remove(capture->slots[slot].path);
        free(capture->slots[slot].path);
        capture->slots[slot].path = NULL;
      }
    snprintf(key, sizeof(key), "capture.%u.file", index);
    lsx_diagnostics_setf(key, "%s", capture->name);
    snprintf(key, sizeof(key), "capture.%u.point", index);
    lsx_diagnostics_setf(key, "%s", capture->point);
    snprintf(key, sizeof(key), "capture.%u.effect", index);
    lsx_diagnostics_setf(key, "%s", tap_effect ? tap_effect : "unknown");
    snprintf(key, sizeof(key), "capture.%u.element", index);
    lsx_diagnostics_setf(key, "%s", capture->element);
    snprintf(key, sizeof(key), "capture.%u.channels", index);
    lsx_diagnostics_setf(key, "%u", tap_channels);
    snprintf(key, sizeof(key), "capture.%u.interleave", index);
    lsx_diagnostics_setf(key, "1");
    /* Nothing is normalised or scaled on the way into a capture, so the
     * reader has to be told which of SoX's two sample domains the build uses.
     * The two differ by exactly 2^31, a power of two: dividing by 2^31 - 1 to
     * reach a normalised oracle would put a rounding floor back inside the
     * measurement. */
    snprintf(key, sizeof(key), "capture.%u.domain", index);
    lsx_diagnostics_setf(key, "%s",
        lsx_sample_values_are_normalized() ? "normalized" : "sox-sample");
    snprintf(key, sizeof(key), "capture.%u.full_scale", index);
    lsx_diagnostics_setf(key, "%s",
        lsx_sample_values_are_normalized() ? "1" : "2147483648");
    snprintf(key, sizeof(key), "capture.%u.frames", index);
    lsx_diagnostics_setf(key, "%llu", (unsigned long long)frames);
    ++index;
  }
  if (dsd_file) {
    char key[64];

    if (fclose(dsd_file) != 0)
      lsx_diagnostics_fail("cannot write the capture `%s': %s", dsd_path, strerror(errno));
    dsd_file = NULL;
    snprintf(key, sizeof(key), "capture.%u.file", index);
    lsx_diagnostics_setf(key, "%s", "chain-out.dsd-u8");
    snprintf(key, sizeof(key), "capture.%u.point", index);
    lsx_diagnostics_setf(key, "%s", "packed-dsd-writer-input");
    snprintf(key, sizeof(key), "capture.%u.effect", index);
    lsx_diagnostics_setf(key, "%s", "sdm");
    snprintf(key, sizeof(key), "capture.%u.element", index);
    lsx_diagnostics_setf(key, "%s", "dsd-u8-lsb");
    snprintf(key, sizeof(key), "capture.%u.channels", index);
    lsx_diagnostics_setf(key, "%u", dsd_channels);
    snprintf(key, sizeof(key), "capture.%u.interleave", index);
    lsx_diagnostics_setf(key, "%s", "1");
    snprintf(key, sizeof(key), "capture.%u.domain", index);
    lsx_diagnostics_setf(key, "%s", "one-bit");
    snprintf(key, sizeof(key), "capture.%u.full_scale", index);
    lsx_diagnostics_setf(key, "%s", "1");
    snprintf(key, sizeof(key), "capture.%u.frames", index);
    lsx_diagnostics_setf(key, "%llu", (unsigned long long)dsd_frames);
    free(dsd_path);
    dsd_path = NULL;
  }
  free(dsd_buffer);
  dsd_buffer = NULL;
  dsd_buffer_capacity = 0;
}

static void write_effects(void)
{
  size_t index;

  if (!diagnostics_chain)
    return;
  for (index = 0; index < diagnostics_chain->length; ++index) {
    sox_effect_t const *effp = diagnostics_chain->effects[index];
    sox_uint64_t clips = 0;
    size_t flow;

    lsx_diagnostics_effect_setf(effp, "name", "%s", effp->handler.name);
    lsx_diagnostics_effect_setf(effp, "flows", "%" PRIuPTR, effp->flows);
    lsx_diagnostics_effect_setf(effp, "channels_in", "%u", effp->in_signal.channels);
    lsx_diagnostics_effect_setf(effp, "channels_out", "%u", effp->out_signal.channels);
    lsx_diagnostics_effect_setf(effp, "rate_in", "%g", effp->in_signal.rate);
    lsx_diagnostics_effect_setf(effp, "rate_out", "%g", effp->out_signal.rate);
    for (flow = 0; flow < effp->flows; ++flow)
      clips += effp[flow].clips;
    lsx_diagnostics_effect_setf(effp, "clips", "%llu", (unsigned long long)clips);
    if (index < frame_counts) {
      lsx_diagnostics_effect_setf(effp, "frames_in", "%" PRIuPTR, frames_in[index]);
      lsx_diagnostics_effect_setf(effp, "frames_out", "%" PRIuPTR, frames_out[index]);
    }
  }
}

/* Sorted by key, so that two runs of the same case diff cleanly whatever
 * order the chain happened to emit them in. */
static int compare_entries(void const *left, void const *right)
{
  return strcmp(((entry_t const *)left)->key, ((entry_t const *)right)->key);
}

static void write_run_txt(void)
{
  char *path = join_path(directory, "run.txt");
  FILE *file = fopen(path, "wb");
  size_t index;

  if (!file) {
    lsx_fail("cannot write `%s': %s", path, strerror(errno));
    exit(1);
  }
  qsort(entries, entry_count, sizeof(*entries), compare_entries);
  for (index = 0; index < entry_count; ++index)
    fprintf(file, "%s=%s\n", entries[index].key, entries[index].value);
  if (fclose(file) != 0) {
    lsx_fail("cannot write `%s': %s", path, strerror(errno));
    exit(1);
  }
  free(path);
}

/* Called from inside a failure, so it may not fail again: everything it does
 * is best effort, and run.txt says what went wrong even if a capture could
 * not be assembled. */
void lsx_diagnostics_fail(char const *fmt, ...)
{
  va_list args;
  char *message;

  va_start(args, fmt);
  message = format_string(fmt, args);
  va_end(args);
  lsx_fail("%s", message);
  if (lsx_diagnostics_on) {
    /* Off from here on, so that anything still holding a tap writes nothing
     * more into files that are about to be abandoned. */
    lsx_diagnostics_on = 0;
    tap_armed = 0;
    set_value("result.status", lsx_strdup("error"));
    set_value("result.message", message);
    write_run_txt();
  }
  exit(1);
}

void lsx_diagnostics_close(int status)
{
  if (!lsx_diagnostics_on)
    return;
  lsx_diagnostics_chain_done();
  write_captures();
  lsx_diagnostics_setf("result.status", "%s", status == SOX_SUCCESS ? "ok" : "error");
  lsx_diagnostics_on = 0;
  write_run_txt();
}
