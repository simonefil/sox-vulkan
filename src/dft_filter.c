/* Abstract effect: dft filter     Copyright (c) 2008 robs@users.sourceforge.net
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "sox_i.h"
#include "fft4g.h"
#include "dft_filter.h"
#include <string.h>

#if HAVE_VULKAN
#include "fir_vulkan.h"
#include "rate_vulkan.h"
#include "vulkan_engine.h"
#include "vulkan_effect_chain.h"
#endif

typedef dft_filter_t filter_t;
typedef dft_filter_priv_t priv_t;

#if HAVE_VULKAN
static int flow_vulkan_resident_producer(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
static int drain_vulkan_resident_producer(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);
static int consume_vulkan_resident(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, sox_sample_t *obuf, size_t *osamp, sox_bool *active);
static int transform_vulkan_resident(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *active);
static int drain_transform_vulkan_resident(sox_effect_t *effp, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *done);

/* Rebuild the Vulkan backend around a new response, taking ownership of the
 * arrays passed in.  Called when the effect's response changes after start --
 * which is what fusion does -- and each form matches one of the four ways a
 * response can be given: shared or per channel, plain or double-double. */
static int lsx_dft_filter_restart_vulkan(sox_effect_t *effp, double *taps, int num_taps, int post_peak);
static int lsx_dft_filter_restart_vulkan_reference_dd(sox_effect_t *effp, double *tap_highs, double *tap_lows, int num_taps, int post_peak);
static int lsx_dft_filter_restart_vulkan_channels(sox_effect_t *effp, double **taps, uint32_t channels, int num_taps, int post_peak);
static int lsx_dft_filter_restart_vulkan_reference_dd_channels(sox_effect_t *effp, double **tap_highs, double **tap_lows, uint32_t channels, int num_taps, int post_peak);

static lsx_vulkan_effect_endpoint_t const vulkan_resident_endpoint = {
  flow_vulkan_resident_producer,
  drain_vulkan_resident_producer,
  consume_vulkan_resident,
  transform_vulkan_resident,
  drain_transform_vulkan_resident
};

static lsx_vulkan_effect_endpoint_t const vulkan_resident_producer_endpoint = {
  flow_vulkan_resident_producer,
  drain_vulkan_resident_producer,
  NULL,
  NULL,
  NULL
};

/* Build the fused response, if one is owed, and rebuild the backend around
 * it.  Called at the top of every entry point, and does nothing once the
 * fusion has been performed.
 *
 * It is deferred to here rather than done when each response is offered
 * because neighbours may go on offering: fusing eagerly would rebuild the
 * whole backend once per offer, and only the last of those rebuilds would
 * survive.  By the time the first block arrives no further offers can come,
 * the chain having started.
 *
 * The four cases below are the product of two independent choices: whether
 * the response is per channel, and whether any sources were offered at all --
 * with none, the configured response is simply copied and reinstated, which
 * is what a restart needs. */
static int ensure_vulkan_fusion(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  double *taps;
  double *tap_lows = NULL;
  size_t fused_taps = 0;
  int num_taps;
  int post_peak;

  if (!p->vulkan_fusion_pending)
    return SOX_SUCCESS;
  if (p->vulkan_channels) {
    double **channel_taps = lsx_calloc(p->vulkan_channel_count, sizeof(*channel_taps));
    double **channel_lows = p->vulkan_fusion_source_count ?
        lsx_calloc(p->vulkan_channel_count, sizeof(*channel_lows)) : NULL;
    uint32_t channel;

    num_taps = p->vulkan_source_num_taps;
    for (channel = 0; channel < p->vulkan_channel_count; ++channel) {
      if (p->vulkan_fusion_source_count) {
        size_t channel_fused_taps = 0;

        if (lsx_fir_vulkan_fuse_reference_coefficients(p->vulkan_context,
            (double const *const *)p->vulkan_channels[channel].fusion_sources,
            p->vulkan_channels[channel].fusion_source_taps, p->vulkan_fusion_source_count,
            &channel_taps[channel], &channel_lows[channel], &channel_fused_taps) != SOX_SUCCESS ||
            channel_fused_taps > INT_MAX ||
            (channel && channel_fused_taps != fused_taps))
          goto channel_error;
        fused_taps = channel_fused_taps;
        num_taps = (int)fused_taps;
      }
      else
        channel_taps[channel] = lsx_memdup(p->vulkan_channels[channel].source_taps,
            (size_t)num_taps * sizeof(**channel_taps));
    }
    post_peak = p->vulkan_source_post_peak;
    if ((channel_lows ?
        lsx_dft_filter_restart_vulkan_reference_dd_channels(
            effp, channel_taps, channel_lows, p->vulkan_channel_count, num_taps, post_peak) :
        lsx_dft_filter_restart_vulkan_channels(
            effp, channel_taps, p->vulkan_channel_count, num_taps, post_peak)) != SOX_SUCCESS)
      return SOX_EOF;
    p->vulkan_fusion_pending = sox_false;
    return SOX_SUCCESS;

channel_error:
    for (channel = 0; channel < p->vulkan_channel_count; ++channel) {
      free(channel_taps[channel]);
      if (channel_lows)
        free(channel_lows[channel]);
    }
    free(channel_lows);
    free(channel_taps);
    return SOX_EOF;
  }
  if (p->vulkan_fusion_source_count) {
    if (lsx_fir_vulkan_fuse_reference_coefficients(p->vulkan_context,
        (double const *const *)p->vulkan_fusion_sources, p->vulkan_fusion_source_taps,
        p->vulkan_fusion_source_count, &taps, &tap_lows, &fused_taps) != SOX_SUCCESS ||
        fused_taps > INT_MAX)
      return SOX_EOF;
    num_taps = (int)fused_taps;
  }
  else {
    taps = lsx_memdup(p->vulkan_source_taps, (size_t)p->vulkan_source_num_taps * sizeof(*p->vulkan_source_taps));
    num_taps = p->vulkan_source_num_taps;
  }
  post_peak = p->vulkan_source_post_peak;
  if ((tap_lows ?
      lsx_dft_filter_restart_vulkan_reference_dd(effp, taps, tap_lows, num_taps, post_peak) :
      lsx_dft_filter_restart_vulkan(effp, taps, num_taps, post_peak)) != SOX_SUCCESS)
    return SOX_EOF;
  p->vulkan_fusion_pending = sox_false;
  return SOX_SUCCESS;
}
#endif

void lsx_set_dft_filter(dft_filter_t *f, double *h, int n, int post_peak)
{
  f->taps = h;
  f->num_taps = n;
  f->post_peak = post_peak;
}

#if HAVE_VULKAN
static void free_vulkan_channels(priv_t *p)
{
  uint32_t channel;

  for (channel = 0; channel < p->vulkan_channel_count; ++channel) {
    uint32_t source;

    free(p->vulkan_channels[channel].source_taps);
    free(p->vulkan_channels[channel].reference_low_taps);
    for (source = 0; source < p->vulkan_fusion_source_count; ++source)
      free(p->vulkan_channels[channel].fusion_sources[source]);
  }
  free(p->vulkan_channels);
  p->vulkan_channels = NULL;
  p->vulkan_channel_count = 0;
}

void lsx_clear_dft_filter_vulkan_channels(dft_filter_priv_t *p)
{
  if (!p)
    return;
  free_vulkan_channels(p);
}

int lsx_set_dft_filter_vulkan_channels(
    dft_filter_priv_t *p, double const *const *taps,
    int num_taps, int post_peak, uint32_t channels)
{
  uint32_t channel;

  if (!p || !taps || num_taps < 1 || post_peak < 0 || post_peak >= num_taps || !channels || p->vulkan_channels)
    return SOX_EOF;
  p->vulkan_channels = lsx_calloc(channels, sizeof(*p->vulkan_channels));
  p->vulkan_channel_count = channels;
  for (channel = 0; channel < channels; ++channel) {
    if (!taps[channel]) {
      free_vulkan_channels(p);
      return SOX_EOF;
    }
    p->vulkan_channels[channel].source_taps = lsx_memdup(taps[channel], (size_t)num_taps * sizeof(**taps));
  }
  p->vulkan_source_num_taps = num_taps;
  p->vulkan_source_post_peak = post_peak;
  return SOX_SUCCESS;
}
#endif

/* Transform the response into the coefficient spectrum the CPU path
 * convolves against, and release the time-domain taps.
 *
 * Two things happen while the taps are copied in.  They are scaled by 2 over
 * the transform length, which is the normalisation this real transform needs
 * on its inverse and which is applied here so the per-block path does not
 * have to.  And they are placed with a rotation, wrapping modulo the
 * transform length: the response is centred so that a circular convolution
 * agrees with a linear one over the part of each block that is kept. */
static void prepare_cpu_filter(filter_t *f)
{
  int i;

  f->dft_length = lsx_set_dft_length(f->num_taps);
  f->coefs = lsx_calloc(f->dft_length, sizeof(*f->coefs));
  for (i = 0; i < f->num_taps; ++i)
    f->coefs[(i + f->dft_length - f->num_taps + 1) & (f->dft_length - 1)] = f->taps[i] / f->dft_length * 2;
  lsx_safe_rdft(f->dft_length, 1, f->coefs);
  free(f->taps);
  f->taps = NULL;
}

/* Set up either the Vulkan path or the CPU one; the two share nothing beyond
 * the response they start from.
 *
 * The Vulkan path forces a single flow, because the backend keeps its own
 * per-channel state and running the effect once per channel would give each
 * flow a separate device context.  The CPU path instead primes its input FIFO
 * with post_peak zeros, which is how its latency is accounted for -- the
 * Vulkan path does the same thing at the other end, by discarding output. */
static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;
  filter_t * f = p->filter_ptr;

#if HAVE_VULKAN
  if (sox_globals.vulkan_profile != sox_vulkan_profile_none) {
    lsx_vulkan_context_t *vulkan = lsx_vulkan_context_get(effp->global_info);
    sox_bool enable_resident = vulkan && !getenv("SOX_VULKAN_DISABLE_RESIDENT_DFT_CONSUMER");
    size_t block_samples;

    effp->flows = 1;
    if (!vulkan) {
      free(f->taps);
      f->taps = NULL;
      return SOX_EOF;
    }
    if (!p->vulkan_source_taps && !p->vulkan_channels) {
      p->vulkan_source_taps = lsx_memdup(f->taps, (size_t)f->num_taps * sizeof(*f->taps));
      p->vulkan_source_num_taps = f->num_taps;
      p->vulkan_source_post_peak = f->post_peak;
    }
    /* Neither FIR context is created here.  An effect registers both the
     * producer and the transform callbacks, because which of the two it will
     * play is not settled until the whole chain has started and the resident
     * segment is laid out -- the first Vulkan effect feeds the segment, the
     * rest transform inside it.  Creating both up front therefore built two
     * contexts per effect and used one, sixteen for eight chained effects.
     * Each is now built on its first call, from the coefficients that
     * vulkan_source_taps already keeps for the fusion path. */
    p->vulkan_context = vulkan;
    p->vulkan_resident_enabled = enable_resident;
    free(f->taps);
    f->taps = NULL;
    block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * effp->in_signal.channels;
    fifo_create(&p->vulkan_input_fifo, sizeof(double));
    fifo_create(&p->vulkan_output_fifo, sizeof(double));
    p->vulkan_drain_block = lsx_calloc(block_samples, sizeof(*p->vulkan_drain_block));
    p->vulkan_skip_samples = (size_t)(f->num_taps - 1 - f->post_peak) * effp->in_signal.channels;
    effp->internal_chain_endpoint = enable_resident ? &vulkan_resident_endpoint : &vulkan_resident_producer_endpoint;
    return SOX_SUCCESS;
  }
#endif
  if (!f->coefs)
    prepare_cpu_filter(f);
  fifo_create(&p->input_fifo, sizeof(double));
  memset(fifo_reserve(&p->input_fifo,
        f->post_peak), 0, sizeof(double) * f->post_peak);
  fifo_create(&p->output_fifo, sizeof(double));
  return SOX_SUCCESS;
}

/* The CPU convolution: overlap-save over whatever whole transforms the input
 * FIFO holds.
 *
 * Each pass reads a full transform's worth but consumes only the part beyond
 * the overlap, so the next pass sees the last num_taps - 1 samples again --
 * that overlap is what makes the circular convolution equal a linear one.
 * The output FIFO is correspondingly trimmed, discarding the leading overlap
 * of each result, which is where the wrap-around lands.
 *
 * The spectrum multiply is written out rather than looped over complex pairs
 * because the transform packs the two real-valued end bins into slots 0 and
 * 1, which are therefore multiplied directly rather than as a complex pair. */
static void filter(priv_t * p)
{
  int i, num_in = max(0, fifo_occupancy(&p->input_fifo));
  filter_t const * f = p->filter_ptr;
  int const overlap = f->num_taps - 1;
  double * output;

  while (num_in >= f->dft_length) {
    double const * input = fifo_read_ptr(&p->input_fifo);
    fifo_read(&p->input_fifo, f->dft_length - overlap, NULL);
    num_in -= f->dft_length - overlap;

    output = fifo_reserve(&p->output_fifo, f->dft_length);
    fifo_trim_by(&p->output_fifo, overlap);
    memcpy(output, input, f->dft_length * sizeof(*output));

    lsx_safe_rdft(f->dft_length, 1, output);
    output[0] *= f->coefs[0];
    output[1] *= f->coefs[1];
    for (i = 2; i < f->dft_length; i += 2) {
      double tmp = output[i];
      output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
      output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
    }
    lsx_safe_rdft(f->dft_length, -1, output);
  }
}

#if HAVE_VULKAN
/* Built on first use; see the note in start().  Both read the coefficients
 * from vulkan_source_taps, which outlives f->taps for exactly this reason,
 * and both are idempotent. */
static int ensure_vulkan_producer(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  double const **highs = NULL;
  double const **lows = NULL;
  uint32_t channel;

  if (p->vulkan)
    return SOX_SUCCESS;
  if (!p->vulkan_context || (!p->vulkan_source_taps && !p->vulkan_channels))
    return SOX_EOF;
  if (p->vulkan_channels) {
    highs = lsx_malloc(p->vulkan_channel_count * sizeof(*highs));
    if (p->vulkan_channels[0].reference_low_taps)
      lows = lsx_malloc(p->vulkan_channel_count * sizeof(*lows));
    for (channel = 0; channel < p->vulkan_channel_count; ++channel) {
      highs[channel] = p->vulkan_channels[channel].source_taps;
      if (lows)
        lows[channel] = p->vulkan_channels[channel].reference_low_taps;
    }
    if (lows)
      p->vulkan = lsx_fir_vulkan_create_reference_dd_channels(p->vulkan_context, highs, lows, (size_t)p->vulkan_source_num_taps, p->vulkan_channel_count);
    else
      p->vulkan = lsx_fir_vulkan_create_channels(p->vulkan_context, highs, (size_t)p->vulkan_source_num_taps, p->vulkan_channel_count);
    free(lows);
    free(highs);
  }
  else
    if (p->vulkan_reference_low_taps)
      p->vulkan = lsx_fir_vulkan_create_reference_dd(p->vulkan_context, p->vulkan_source_taps, p->vulkan_reference_low_taps, (size_t)p->vulkan_source_num_taps, (uint32_t)effp->in_signal.channels);
    else
      p->vulkan = lsx_fir_vulkan_create(p->vulkan_context, p->vulkan_source_taps, (size_t)p->vulkan_source_num_taps, (uint32_t)effp->in_signal.channels);
  return p->vulkan ? SOX_SUCCESS : SOX_EOF;
}

static int ensure_vulkan_resident(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  double const **highs = NULL;
  double const **lows = NULL;
  uint32_t channel;

  if (p->vulkan_resident)
    return SOX_SUCCESS;
  if (!p->vulkan_context || (!p->vulkan_source_taps && !p->vulkan_channels) || !p->vulkan_resident_enabled)
    return SOX_EOF;
  if (p->vulkan_channels) {
    highs = lsx_malloc(p->vulkan_channel_count * sizeof(*highs));
    if (p->vulkan_channels[0].reference_low_taps)
      lows = lsx_malloc(p->vulkan_channel_count * sizeof(*lows));
    for (channel = 0; channel < p->vulkan_channel_count; ++channel) {
      highs[channel] = p->vulkan_channels[channel].source_taps;
      if (lows)
        lows[channel] = p->vulkan_channels[channel].reference_low_taps;
    }
    if (lows)
      p->vulkan_resident = lsx_rate_vulkan_create_reference_dd_channels(p->vulkan_context, highs, lows, (size_t)p->vulkan_source_num_taps, (size_t)p->vulkan_source_post_peak, 1u, 1u, p->vulkan_channel_count);
    else
      p->vulkan_resident = lsx_rate_vulkan_create_channels(p->vulkan_context, highs, (size_t)p->vulkan_source_num_taps, (size_t)p->vulkan_source_post_peak, 1u, 1u, p->vulkan_channel_count);
    free(lows);
    free(highs);
  }
  else
    if (p->vulkan_reference_low_taps)
      p->vulkan_resident = lsx_rate_vulkan_create_reference_dd(p->vulkan_context, p->vulkan_source_taps, p->vulkan_reference_low_taps, (size_t)p->vulkan_source_num_taps, (size_t)p->vulkan_source_post_peak, 1u, 1u, (uint32_t)effp->in_signal.channels);
    else
      p->vulkan_resident = lsx_rate_vulkan_create(p->vulkan_context, p->vulkan_source_taps, (size_t)p->vulkan_source_num_taps, (size_t)p->vulkan_source_post_peak, 1u, 1u, (uint32_t)effp->in_signal.channels);
  return p->vulkan_resident ? SOX_SUCCESS : SOX_EOF;
}

/* Queue a finished block for the host, dropping whatever of it is still the
 * filter's start-up latency.  Counting the skip down here rather than
 * priming the input, as the CPU path does, keeps the backend's blocks whole:
 * it insists on exactly one block per call. */
static void append_vulkan_output(sox_effect_t *effp, double const *output)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * effp->in_signal.channels;
  size_t skip = min(p->vulkan_skip_samples, block_samples);
  size_t count = block_samples - skip;

  p->vulkan_skip_samples -= skip;
  if (count)
    fifo_write(&p->vulkan_output_fifo, count, output + skip);
}

/* Run whole blocks out of the input FIFO through the backend, leaving any
 * remainder for the next call.  This is the non-resident path: the effect
 * chain hands over arbitrary amounts, the backend takes exact blocks, and the
 * two FIFOs either side are what reconciles them. */
static int process_vulkan_input(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * effp->in_signal.channels;

  if (ensure_vulkan_producer(effp) != SOX_SUCCESS)
    return SOX_EOF;
  while ((size_t)fifo_occupancy(&p->vulkan_input_fifo) >= block_samples) {
    double const *input = fifo_read(&p->vulkan_input_fifo, block_samples, NULL);
    double const *output;

    if (lsx_fir_vulkan_process(p->vulkan, input, &output) != SOX_SUCCESS)
      return SOX_EOF;
    append_vulkan_output(effp, output);
  }
  return SOX_SUCCESS;
}

/* Drop the start-up latency from a resident block by moving its description
 * forward, rather than by copying anything: the samples stay where they are
 * on the device and the consumer is simply pointed past them.  The offset
 * moves by whole frames, the layout being planar with a frame stride of one
 * element, so this cannot straddle a channel boundary. */
static void trim_vulkan_resident_output(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t skip_frames = min(p->vulkan_skip_samples / channels, resident->valid_elements);
  size_t element_size = (size_t)lsx_vulkan_resident_element_size(resident->format);

  p->vulkan_skip_samples -= skip_frames * channels;
  resident->offset += (VkDeviceSize)skip_frames * element_size;
  resident->capacity_elements -= skip_frames;
  resident->valid_elements -= skip_frames;
}

/* Act as the head of a resident chain: take host samples, and publish one
 * resident block per call when a whole block has accumulated.
 *
 * At most one block is published per call even if the FIFO holds several,
 * because the consumer takes one at a time; the loop only continues when a
 * block produced nothing, which happens while the start-up latency is still
 * being discarded.
 *
 * A partial sample frame is never taken, so the input FIFO always holds whole
 * frames and the channel interleave cannot drift.
 *
 * The flush every batch-depth blocks is what bounds how far the device runs
 * behind: past that depth the staging buffers would be reused under work that
 * has not yet executed. */
static int flow_vulkan_resident_producer(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * channels;
  size_t idone = *isamp - *isamp % channels;

  if (ensure_vulkan_fusion(effp) != SOX_SUCCESS)
    return SOX_EOF;
  memset(resident, 0, sizeof(*resident));
  *produced = sox_false;
  if (idone) {
    double *input = fifo_write(&p->vulkan_input_fifo, idone, NULL);

    lsx_load_samples(input, ibuf, idone);
    p->samples_in += idone;
  }
  *isamp = idone;
  while ((size_t)fifo_occupancy(&p->vulkan_input_fifo) >= block_samples) {
    double const *input = fifo_read(&p->vulkan_input_fifo, block_samples, NULL);

    /* Here, not at the top: the scheduler calls this on every effect in the
     * segment, and only the one that actually reaches a full block is the
     * producer.  Deciding earlier would build a context for all of them. */
    if (ensure_vulkan_producer(effp) != SOX_SUCCESS)
      return SOX_EOF;
    if (lsx_fir_vulkan_process_resident(p->vulkan, input, effp->out_signal.rate, p->samples_out / channels, lsx_vulkan_resident_ready, resident) != SOX_SUCCESS)
      return SOX_EOF;
    trim_vulkan_resident_output(effp, resident);
    if (resident->valid_elements) {
      p->samples_out += resident->valid_elements * channels;
      *produced = sox_true;
      ++p->vulkan_resident_pending;
      if (p->vulkan_resident_pending >= lsx_vulkan_resident_batch_depth(p->vulkan_context)) {
        p->vulkan_resident_pending = 0;
        if (lsx_fir_vulkan_flush_resident(p->vulkan) != SOX_SUCCESS)
          return SOX_EOF;
      }
      return SOX_SUCCESS;
    }
    if (lsx_fir_vulkan_flush_resident(p->vulkan) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Flush the tail once the input has ended.
 *
 * The filter still owes samples_in - samples_out samples, held inside its own
 * latency, so blocks of zeros are pushed through until they have all come
 * out.  The final block is truncated to exactly what is owed -- the backend
 * always returns a whole block, and the extra frames are the response
 * continuing past the end of the signal -- and marked final, which is what
 * tells the consumer to begin its own drain. */
static int drain_vulkan_resident_producer(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * channels;
  uint64_t remaining = p->samples_in > p->samples_out ? p->samples_in - p->samples_out : 0;

  if (ensure_vulkan_fusion(effp) != SOX_SUCCESS)
    return SOX_EOF;
  memset(resident, 0, sizeof(*resident));
  *produced = sox_false;
  *done = remaining == 0;
  while (remaining) {
    size_t pending = min(block_samples, (size_t)fifo_occupancy(&p->vulkan_input_fifo));

    if (ensure_vulkan_producer(effp) != SOX_SUCCESS)
      return SOX_EOF;

    memset(p->vulkan_drain_block, 0, block_samples * sizeof(*p->vulkan_drain_block));
    if (pending)
      fifo_read(&p->vulkan_input_fifo, pending, p->vulkan_drain_block);
    if (lsx_fir_vulkan_process_resident(p->vulkan, p->vulkan_drain_block, effp->out_signal.rate, p->samples_out / channels, lsx_vulkan_resident_draining, resident) != SOX_SUCCESS)
      return SOX_EOF;
    trim_vulkan_resident_output(effp, resident);
    if (resident->valid_elements) {
      size_t remaining_frames = (size_t)(remaining / channels);

      resident->valid_elements = min(resident->valid_elements, remaining_frames);
      p->samples_out += resident->valid_elements * channels;
      *produced = sox_true;
      ++p->vulkan_resident_pending;
      if (p->vulkan_resident_pending >= lsx_vulkan_resident_batch_depth(p->vulkan_context)) {
        p->vulkan_resident_pending = 0;
        if (lsx_fir_vulkan_flush_resident(p->vulkan) != SOX_SUCCESS)
          return SOX_EOF;
      }
      *done = p->samples_out == p->samples_in;
      if (*done)
        resident->state = lsx_vulkan_resident_final;
      return SOX_SUCCESS;
    }
    if (lsx_fir_vulkan_flush_resident(p->vulkan) != SOX_SUCCESS)
      return SOX_EOF;
    remaining = p->samples_in > p->samples_out ? p->samples_in - p->samples_out : 0;
  }
  *done = sox_true;
  return SOX_SUCCESS;
}

static int take_vulkan_resident_transform_output(
    sox_effect_t *effp, lsx_vulkan_resident_state_t state,
    lsx_vulkan_resident_buffer_t *output,
    sox_bool *output_produced)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;

  if (lsx_rate_vulkan_process_resident_stream(
      p->vulkan_resident, effp->out_signal.rate,
      p->samples_out / channels, state, sox_false,
      output, output_produced) != SOX_SUCCESS)
    return SOX_EOF;
  if (*output_produced)
    p->samples_out += output->valid_elements * channels;
  return SOX_SUCCESS;
}

/* Act as a middle link of a resident chain: take a resident block and publish
 * one, without either end touching host memory.
 *
 * Output is attempted before the input is looked at, because the stream may
 * already hold a whole block from an earlier call and the caller can only be
 * handed one block at a time.  A NULL input means "produce what you can", so
 * the same function serves both a call with new data and a call that is only
 * draining the stream.  *active reports whether another block is available
 * without more input, which is how the caller knows to come back.
 *
 * The clip count is read twice on purpose: once for what has already
 * completed, and again after a flush, since the counters are written by the
 * device and only mean anything once its work has run. */
static int transform_vulkan_resident(
    sox_effect_t *effp,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, uint64_t *input_clips,
    lsx_vulkan_resident_buffer_t *output,
    sox_bool *output_produced, sox_bool *active)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;

  if (ensure_vulkan_resident(effp) != SOX_SUCCESS ||
      !input_consumed || !input_clips ||
      !output || !output_produced || !active)
    return SOX_EOF;
  memset(output, 0, sizeof(*output));
  *input_consumed = sox_false;
  *input_clips = lsx_rate_vulkan_resident_stream_clips(
      p->vulkan_resident);
  *output_produced = sox_false;
  if (take_vulkan_resident_transform_output(effp, lsx_vulkan_resident_ready, output, output_produced) != SOX_SUCCESS)
    return SOX_EOF;
  if (*output_produced) {
    *active = lsx_rate_vulkan_resident_stream_ready(
        p->vulkan_resident);
    return SOX_SUCCESS;
  }
  if (!input) {
    *active = sox_false;
    return SOX_SUCCESS;
  }
  if (input->rate != effp->in_signal.rate ||
      input->channels != channels ||
      lsx_rate_vulkan_append_resident_stream(
          p->vulkan_resident, input) != SOX_SUCCESS)
    return SOX_EOF;
  p->samples_in += input->valid_elements * channels;
  *input_consumed = sox_true;
  ++p->vulkan_resident_pending;
  if (take_vulkan_resident_transform_output(effp, lsx_vulkan_resident_ready, output, output_produced) != SOX_SUCCESS)
    return SOX_EOF;
  if (!*output_produced && p->vulkan_resident_pending >= lsx_rate_vulkan_resident_batch_depth(p->vulkan_resident)) {
    p->vulkan_resident_pending = 0;
    if (lsx_rate_vulkan_flush_resident(p->vulkan_resident) != SOX_SUCCESS)
      return SOX_EOF;
    *input_clips +=
        lsx_rate_vulkan_resident_stream_clips_completed(p->vulkan_resident);
  }
  *active = lsx_rate_vulkan_resident_stream_ready(
      p->vulkan_resident);
  return SOX_SUCCESS;
}

static size_t emit_vulkan_consumer_output(fifo_t *fifo, sox_sample_t *output, size_t capacity, uint64_t *clips)
{
  size_t count = min(capacity, (size_t)fifo_occupancy(fifo));
  double const *samples = fifo_read(fifo, count, NULL);

  if (count)
    lsx_save_samples(output, samples, count, clips);
  return count;
}

/* Bring a resident block back to the host and queue it for the effect chain.
 * This is the one download of a resident chain, at its tail; the drain block
 * is reused as the landing buffer, being idle on this path and already the
 * right size. */
static int queue_vulkan_consumer_output(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *resident)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t samples;

  if (!resident ||
      resident->domain != lsx_vulkan_resident_domain_sox_sample ||
      resident->channels != effp->out_signal.channels)
    return SOX_EOF;
  samples = resident->valid_elements * resident->channels;
  if (resident->channels && samples / resident->channels != resident->valid_elements)
    return SOX_EOF;
  if (lsx_vulkan_download_resident_pcm(
      p->vulkan_context, resident, p->vulkan_drain_block,
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      resident->channels) !=
      SOX_SUCCESS)
    return SOX_EOF;
  fifo_write(&p->vulkan_output_fifo, samples, p->vulkan_drain_block);
  return SOX_SUCCESS;
}

/* Act as the tail of a resident chain: take resident blocks and emit ordinary
 * host samples.
 *
 * The three sources of a block are tried in a fixed order -- what the stream
 * already holds, then new input, then the drain once the producer's final
 * block has been seen -- so that nothing is left behind when the stream ends.
 * The loop is bounded rather than run to exhaustion because the caller's
 * output buffer is finite and whatever does not fit stays in the FIFO for the
 * next call; two attempts is enough to fill any buffer this effect is given,
 * a block being larger than one call's capacity.
 *
 * *active tells the caller whether anything is still pending here, which is
 * what keeps it calling after its own input has ended. */
static int consume_vulkan_resident(
    sox_effect_t *effp,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, uint64_t *input_clips,
    sox_sample_t *obuf, size_t *osamp, sox_bool *active)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t capacity;
  size_t emitted;
  unsigned attempt;

  if (ensure_vulkan_resident(effp) != SOX_SUCCESS || !input_consumed || !input_clips || !obuf || !osamp || !active)
    return SOX_EOF;
  capacity = *osamp;
  *osamp = 0;
  *input_consumed = sox_false;
  *input_clips = lsx_rate_vulkan_resident_stream_clips(
      p->vulkan_resident);
  emitted = emit_vulkan_consumer_output(&p->vulkan_output_fifo, obuf, capacity, input_clips);
  *osamp = emitted;
  for (attempt = 0; emitted < capacity && attempt < 2u; ++attempt) {
    lsx_vulkan_resident_buffer_t resident;
    sox_bool produced = sox_false;
    sox_bool consumed = sox_false;
    int status;

    if (p->vulkan_consumer_transform_active)
      status = transform_vulkan_resident(
          effp, NULL, &consumed, input_clips, &resident,
          &produced, &p->vulkan_consumer_transform_active);
    else if (input && !*input_consumed) {
      status = transform_vulkan_resident(
          effp, input, &consumed, input_clips, &resident,
          &produced, &p->vulkan_consumer_transform_active);
      *input_consumed = consumed;
      if (consumed && input->state == lsx_vulkan_resident_final)
        p->vulkan_consumer_final_received = sox_true;
    }
    else if (p->vulkan_consumer_final_received && !p->vulkan_consumer_done)
      status = drain_transform_vulkan_resident(effp, input_clips, &resident, &produced, &p->vulkan_consumer_done);
    else
      break;
    if (status != SOX_SUCCESS)
      return status;
    if (produced) {
      size_t count;

      if (queue_vulkan_consumer_output(effp, &resident) != SOX_SUCCESS)
        return SOX_EOF;
      count = emit_vulkan_consumer_output(&p->vulkan_output_fifo, obuf + emitted, capacity - emitted, input_clips);
      emitted += count;
      *osamp = emitted;
    }
    if (!produced && !consumed &&
        !p->vulkan_consumer_transform_active &&
        !(p->vulkan_consumer_final_received &&
          !p->vulkan_consumer_done))
      break;
  }
  *active = fifo_occupancy(&p->vulkan_output_fifo) ||
      p->vulkan_consumer_transform_active || (p->vulkan_consumer_final_received && !p->vulkan_consumer_done);
  return SOX_SUCCESS;
}

/* Flush a middle link's own latency once its producer has finished.
 *
 * The stream is zero-padded to a whole block if it is short of one, since the
 * stage only produces on whole blocks and the tail would otherwise stay
 * buffered.  Blocks are marked draining until the last, which is marked
 * final; done says the effect owes nothing further. */
static int drain_transform_vulkan_resident(
    sox_effect_t *effp, uint64_t *input_clips,
    lsx_vulkan_resident_buffer_t *output,
    sox_bool *output_produced, sox_bool *done)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  uint64_t remaining = p->samples_in > p->samples_out ? p->samples_in - p->samples_out : 0;

  if (ensure_vulkan_resident(effp) != SOX_SUCCESS || !input_clips || !output || !output_produced || !done)
    return SOX_EOF;
  memset(output, 0, sizeof(*output));
  *input_clips = lsx_rate_vulkan_resident_stream_clips(
      p->vulkan_resident);
  *output_produced = sox_false;
  *done = sox_false;
  if (!lsx_rate_vulkan_resident_stream_ready(
      p->vulkan_resident) &&
      lsx_rate_vulkan_pad_resident_stream(
          p->vulkan_resident) != SOX_SUCCESS)
    return SOX_EOF;
  if (take_vulkan_resident_transform_output(
      effp, lsx_vulkan_resident_draining, output,
      output_produced) != SOX_SUCCESS ||
      !*output_produced)
    return SOX_EOF;
  {
    size_t produced_frames = output->valid_elements;

    output->valid_elements = min(produced_frames, (size_t)(remaining / channels));
    p->samples_out -= (produced_frames - output->valid_elements) * channels;
  }
  *done = p->samples_out >= p->samples_in;
  if (*done) {
    p->samples_out = p->samples_in;
    output->state = lsx_vulkan_resident_final;
  }
  return SOX_SUCCESS;
}

static int flow_vulkan(sox_effect_t *effp, sox_sample_t const *ibuf, sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t odone = min(*osamp, (size_t)fifo_occupancy(&p->vulkan_output_fifo));
  double const *output;

  if (ensure_vulkan_fusion(effp) != SOX_SUCCESS) {
    *isamp = *osamp = 0;
    return SOX_EOF;
  }

  odone -= odone % channels;
  output = fifo_read(&p->vulkan_output_fifo, odone, NULL);
  if (odone) {
    lsx_save_samples(obuf, output, odone, &effp->clips);
    p->samples_out += odone;
  }
  if (*isamp && odone < *osamp) {
    double *input = fifo_write(&p->vulkan_input_fifo, *isamp, NULL);

    lsx_load_samples(input, ibuf, *isamp);
    p->samples_in += *isamp;
    if (process_vulkan_input(effp) != SOX_SUCCESS) {
      *osamp = odone;
      return SOX_EOF;
    }
  }
  else
    *isamp = 0;
  *osamp = odone;
  return SOX_SUCCESS;
}

/* Flush the non-resident Vulkan path.  Zero blocks are pushed through until
 * the output FIFO holds everything the filter still owes, and the surplus --
 * the response continuing past the end of the signal -- is trimmed away, so
 * the effect emits exactly as many samples as it was given. */
static int drain_vulkan(sox_effect_t *effp, sox_sample_t *obuf, size_t *osamp)
{
  priv_t *p = (priv_t *)effp->priv;
  static size_t isamp;
  size_t block_samples = lsx_fir_vulkan_block_frames_for(p->vulkan_context) * effp->in_signal.channels;
  uint64_t remaining = p->samples_in > p->samples_out ? p->samples_in - p->samples_out : 0;

  if (ensure_vulkan_producer(effp) != SOX_SUCCESS) {
    *osamp = 0;
    return SOX_EOF;
  }
  while ((uint64_t)fifo_occupancy(&p->vulkan_output_fifo) < remaining) {
    size_t pending = min(block_samples, (size_t)fifo_occupancy(&p->vulkan_input_fifo));
    double const *output;

    memset(p->vulkan_drain_block, 0, block_samples * sizeof(*p->vulkan_drain_block));
    if (pending)
      fifo_read(&p->vulkan_input_fifo, pending, p->vulkan_drain_block);
    if (lsx_fir_vulkan_process(p->vulkan, p->vulkan_drain_block, &output) != SOX_SUCCESS) {
      *osamp = 0;
      return SOX_EOF;
    }
    append_vulkan_output(effp, output);
  }
  if ((uint64_t)fifo_occupancy(&p->vulkan_output_fifo) > remaining)
    fifo_trim_to(&p->vulkan_output_fifo, (size_t)remaining);
  return flow_vulkan(effp, NULL, obuf, &isamp, osamp);
}
#endif

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
                sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t odone;
  double const * s;

#if HAVE_VULKAN
  /* The context, not the FIR: the FIR is built lazily and is still null
   * before the first block reaches it. */
  if (p->vulkan_context)
    return flow_vulkan(effp, ibuf, obuf, isamp, osamp);
#endif
  odone = min(*osamp, (size_t)fifo_occupancy(&p->output_fifo));
  s = fifo_read(&p->output_fifo, odone, NULL);
  lsx_save_samples(obuf, s, odone, &effp->clips);
  p->samples_out += odone;

  if (*isamp && odone < *osamp) {
    double * t = fifo_write(&p->input_fifo, *isamp, NULL);
    p->samples_in += *isamp;
    lsx_load_samples(t, ibuf, *isamp);
    filter(p);
  }
  else *isamp = 0;
  *osamp = odone;
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  static size_t isamp = 0;
  size_t remaining = p->samples_in > p->samples_out ?
      (size_t)(p->samples_in - p->samples_out) : 0;
  double * buff;

#if HAVE_VULKAN
  if (p->vulkan_context)
    return drain_vulkan(effp, obuf, osamp);
#endif
  /* Same idea as the Vulkan drain: push silence through until the filter has
   * given back everything it still owes, then trim the tail the response
   * adds beyond the end of the signal. */
  buff = lsx_calloc(1024, sizeof(*buff));
  if (remaining > 0) {
    while ((size_t)fifo_occupancy(&p->output_fifo) < remaining) {
      fifo_write(&p->input_fifo, 1024, buff);
      p->samples_in += 1024;
      filter(p);
    }
    fifo_trim_to(&p->output_fifo, remaining);
    p->samples_in = 0;
  }
  free(buff);
  return flow(effp, 0, obuf, &isamp, osamp);
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;

#if HAVE_VULKAN
  if (p->vulkan_context) {
    uint32_t fusion_index;

    lsx_rate_vulkan_destroy(p->vulkan_resident);
    p->vulkan_resident = NULL;
    lsx_fir_vulkan_destroy(p->vulkan);
    fifo_delete(&p->vulkan_input_fifo);
    fifo_delete(&p->vulkan_output_fifo);
    free(p->vulkan_drain_block);
    p->vulkan = NULL;
    p->vulkan_context = NULL;
    p->vulkan_resident_enabled = sox_false;
    p->vulkan_drain_block = NULL;
    effp->internal_chain_endpoint = NULL;
    memset(p->filter_ptr, 0, sizeof(*p->filter_ptr));
    free(p->vulkan_source_taps);
    free(p->vulkan_reference_low_taps);
    free_vulkan_channels(p);
    p->vulkan_source_taps = NULL;
    p->vulkan_reference_low_taps = NULL;
    p->vulkan_source_num_taps = 0;
    p->vulkan_source_post_peak = 0;
    for (fusion_index = 0; fusion_index < p->vulkan_fusion_source_count; ++fusion_index) {
      free(p->vulkan_fusion_sources[fusion_index]);
      p->vulkan_fusion_sources[fusion_index] = NULL;
      p->vulkan_fusion_source_taps[fusion_index] = 0;
    }
    p->vulkan_fusion_source_count = 0;
    p->vulkan_fusion_pending = sox_false;
    return SOX_SUCCESS;
  }
#endif
  fifo_delete(&p->input_fifo);
  fifo_delete(&p->output_fifo);
  free(p->filter_ptr->coefs);
  memset(p->filter_ptr, 0, sizeof(*p->filter_ptr));
  return SOX_SUCCESS;
}

/* Rebuild the backend around a new response, by stopping and starting the
 * effect: everything downstream of the response -- the FIR context, the
 * FIFOs, the latency counter -- depends on it, so reinstating it wholesale is
 * both simpler and safer than patching each piece.
 *
 * The taps are taken over on every path, the failing ones included, since the
 * caller has already handed them across.  Only valid before any samples have
 * flowed; the state a restart discards includes the filter's history. */
#if HAVE_VULKAN
static int lsx_dft_filter_restart_vulkan(sox_effect_t *effp, double *taps, int num_taps, int post_peak)
{
  priv_t *p;

  if (!effp || !taps || num_taps < 1 || post_peak < 0 || post_peak >= num_taps)
    return SOX_EOF;
  p = (priv_t *)effp->priv;
  if (!p || !p->vulkan_context || stop(effp) != SOX_SUCCESS) {
    free(taps);
    return SOX_EOF;
  }
  p->vulkan_source_taps = lsx_memdup(taps, (size_t)num_taps * sizeof(*taps));
  p->vulkan_source_num_taps = num_taps;
  p->vulkan_source_post_peak = post_peak;
  lsx_set_dft_filter(p->filter_ptr, taps, num_taps, post_peak);
  return start(effp);
}

static int lsx_dft_filter_restart_vulkan_reference_dd(sox_effect_t *effp, double *tap_highs, double *tap_lows, int num_taps, int post_peak)
{
  priv_t *p;

  if (!effp || !tap_highs || !tap_lows || num_taps < 1 || post_peak < 0 || post_peak >= num_taps) {
    free(tap_lows);
    free(tap_highs);
    return SOX_EOF;
  }
  p = (priv_t *)effp->priv;
  if (!p || !p->vulkan_context || stop(effp) != SOX_SUCCESS) {
    free(tap_lows);
    free(tap_highs);
    return SOX_EOF;
  }
  p->vulkan_source_taps = lsx_memdup(tap_highs, (size_t)num_taps * sizeof(*tap_highs));
  p->vulkan_source_num_taps = num_taps;
  p->vulkan_source_post_peak = post_peak;
  p->vulkan_reference_low_taps = tap_lows;
  lsx_set_dft_filter(p->filter_ptr, tap_highs, num_taps, post_peak);
  return start(effp);
}

static void free_restart_channel_taps(double **tap_highs, double **tap_lows, uint32_t channels)
{
  uint32_t channel;

  for (channel = 0; channel < channels; ++channel) {
    free(tap_highs ? tap_highs[channel] : NULL);
    free(tap_lows ? tap_lows[channel] : NULL);
  }
  free(tap_lows);
  free(tap_highs);
}

static int restart_vulkan_channels(
    sox_effect_t *effp, double **tap_highs,
    double **tap_lows, uint32_t channels,
    int num_taps, int post_peak)
{
  priv_t *p;
  uint32_t channel;

  if (!effp || !tap_highs || !channels ||
      channels != effp->in_signal.channels ||
      num_taps < 1 || post_peak < 0 || post_peak >= num_taps) {
    free_restart_channel_taps(tap_highs, tap_lows, channels);
    return SOX_EOF;
  }
  for (channel = 0; channel < channels; ++channel) {
    if (!tap_highs[channel] || (tap_lows && !tap_lows[channel])) {
      free_restart_channel_taps(tap_highs, tap_lows, channels);
      return SOX_EOF;
    }
  }
  p = (priv_t *)effp->priv;
  if (!p || !p->vulkan_context || stop(effp) != SOX_SUCCESS) {
    free_restart_channel_taps(tap_highs, tap_lows, channels);
    return SOX_EOF;
  }
  p->vulkan_channels = lsx_calloc(channels, sizeof(*p->vulkan_channels));
  p->vulkan_channel_count = channels;
  p->vulkan_source_num_taps = num_taps;
  p->vulkan_source_post_peak = post_peak;
  for (channel = 0; channel < channels; ++channel) {
    p->vulkan_channels[channel].source_taps = tap_highs[channel];
    if (tap_lows)
      p->vulkan_channels[channel].reference_low_taps = tap_lows[channel];
  }
  lsx_set_dft_filter(
      p->filter_ptr,
      lsx_memdup(
          tap_highs[0], (size_t)num_taps * sizeof(**tap_highs)),
      num_taps, post_peak);
  free(tap_lows);
  free(tap_highs);
  return start(effp);
}

static int lsx_dft_filter_restart_vulkan_channels(sox_effect_t *effp, double **taps, uint32_t channels, int num_taps, int post_peak)
{
  return restart_vulkan_channels(effp, taps, NULL, channels, num_taps, post_peak);
}

static int lsx_dft_filter_restart_vulkan_reference_dd_channels(sox_effect_t *effp, double **tap_highs, double **tap_lows, uint32_t channels, int num_taps, int post_peak)
{
  return restart_vulkan_channels(effp, tap_highs, tap_lows, channels, num_taps, post_peak);
}
#endif

sox_effect_handler_t const * lsx_dft_filter_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    NULL, NULL, SOX_EFF_GAIN, NULL, start, flow, drain, stop, NULL, 0
  };
  return &handler;
}
