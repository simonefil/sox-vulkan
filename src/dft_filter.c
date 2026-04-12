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

static int ensure_vulkan_fusion(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  double *taps;
  int num_taps;
  int post_peak;

  if (!p->vulkan_fusion_pending)
    return SOX_SUCCESS;
  taps = lsx_memdup(
      p->vulkan_source_taps,
      (size_t)p->vulkan_source_num_taps *
      sizeof(*p->vulkan_source_taps));
  num_taps = p->vulkan_source_num_taps;
  post_peak = p->vulkan_source_post_peak;
  if (lsx_dft_filter_restart_vulkan(
      effp, taps, num_taps, post_peak) != SOX_SUCCESS)
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

static void prepare_cpu_filter(filter_t *f)
{
  int i;

  f->dft_length = lsx_set_dft_length(f->num_taps);
  f->coefs = lsx_calloc(f->dft_length, sizeof(*f->coefs));
  for (i = 0; i < f->num_taps; ++i)
    f->coefs[(i + f->dft_length - f->num_taps + 1) &
        (f->dft_length - 1)] =
        f->taps[i] / f->dft_length * 2;
  lsx_safe_rdft(f->dft_length, 1, f->coefs);
  free(f->taps);
  f->taps = NULL;
}

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;
  filter_t * f = p->filter_ptr;

#if HAVE_VULKAN
  if (sox_globals.vulkan_profile != sox_vulkan_profile_none) {
    lsx_vulkan_context_t *vulkan =
        lsx_vulkan_context_get(effp->global_info);
    sox_bool enable_resident =
        vulkan &&
        !getenv("SOX_VULKAN_DISABLE_RESIDENT_DFT_CONSUMER");
    size_t block_samples;

    effp->flows = 1;
    if (!vulkan) {
      free(f->taps);
      f->taps = NULL;
      return SOX_EOF;
    }
    if (!p->vulkan_source_taps) {
      p->vulkan_source_taps = lsx_memdup(
          f->taps,
          (size_t)f->num_taps * sizeof(*f->taps));
      p->vulkan_source_num_taps = f->num_taps;
      p->vulkan_source_post_peak = f->post_peak;
    }
    p->vulkan = lsx_fir_vulkan_create(
        vulkan, f->taps, (size_t)f->num_taps,
        (uint32_t)effp->in_signal.channels);
    p->vulkan_context = vulkan;
    if (enable_resident)
      p->vulkan_resident = lsx_rate_vulkan_create(
          vulkan, f->taps, (size_t)f->num_taps,
          (size_t)f->post_peak, 1u, 1u,
          (uint32_t)effp->in_signal.channels);
    free(f->taps);
    f->taps = NULL;
    if (!p->vulkan || (enable_resident && !p->vulkan_resident))
      return SOX_EOF;
    block_samples =
        lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
        effp->in_signal.channels;
    fifo_create(&p->vulkan_input_fifo, sizeof(double));
    fifo_create(&p->vulkan_output_fifo, sizeof(double));
    p->vulkan_drain_block =
        lsx_calloc(block_samples, sizeof(*p->vulkan_drain_block));
    p->vulkan_skip_samples =
        (size_t)(f->num_taps - 1 - f->post_peak) *
        effp->in_signal.channels;
    effp->internal_chain_endpoint = p->vulkan_resident ?
        &vulkan_resident_endpoint :
        &vulkan_resident_producer_endpoint;
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
static void append_vulkan_output(
    sox_effect_t *effp, double const *output)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t block_samples =
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      effp->in_signal.channels;
  size_t skip = min(p->vulkan_skip_samples, block_samples);
  size_t count = block_samples - skip;

  p->vulkan_skip_samples -= skip;
  if (count)
    fifo_write(&p->vulkan_output_fifo, count, output + skip);
}

static int process_vulkan_input(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t block_samples =
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      effp->in_signal.channels;

  while ((size_t)fifo_occupancy(
      &p->vulkan_input_fifo) >= block_samples) {
    double const *input = fifo_read(
        &p->vulkan_input_fifo, block_samples, NULL);
    double const *output;

    if (lsx_fir_vulkan_process(
        p->vulkan, input, &output) != SOX_SUCCESS)
      return SOX_EOF;
    append_vulkan_output(effp, output);
  }
  return SOX_SUCCESS;
}

static void trim_vulkan_resident_output(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t skip_frames = min(p->vulkan_skip_samples / channels, resident->valid_elements);
  size_t element_size =
      resident->format == lsx_vulkan_resident_format_f32 ?
      sizeof(float) :
      resident->format == lsx_vulkan_resident_format_f32x2 ?
      2u * sizeof(float) : sizeof(double);

  p->vulkan_skip_samples -= skip_frames * channels;
  resident->offset += (VkDeviceSize)skip_frames * element_size;
  resident->capacity_elements -= skip_frames;
  resident->valid_elements -= skip_frames;
}

static int flow_vulkan_resident_producer(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t block_samples =
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      channels;
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

    if (lsx_fir_vulkan_process_resident(p->vulkan, input, effp->out_signal.rate, p->samples_out / channels, lsx_vulkan_resident_ready, resident) != SOX_SUCCESS)
      return SOX_EOF;
    trim_vulkan_resident_output(effp, resident);
    if (resident->valid_elements) {
      p->samples_out += resident->valid_elements * channels;
      *produced = sox_true;
      ++p->vulkan_resident_pending;
      if (p->vulkan_resident_pending >=
          lsx_rate_vulkan_resident_batch_depth(
              p->vulkan_resident)) {
        p->vulkan_resident_pending = 0;
        if (lsx_fir_vulkan_flush_resident(p->vulkan) !=
            SOX_SUCCESS)
          return SOX_EOF;
      }
      return SOX_SUCCESS;
    }
    if (lsx_fir_vulkan_flush_resident(p->vulkan) != SOX_SUCCESS)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int drain_vulkan_resident_producer(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t block_samples =
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      channels;
  uint64_t remaining = p->samples_in > p->samples_out ? p->samples_in - p->samples_out : 0;

  if (ensure_vulkan_fusion(effp) != SOX_SUCCESS)
    return SOX_EOF;
  memset(resident, 0, sizeof(*resident));
  *produced = sox_false;
  *done = remaining == 0;
  while (remaining) {
    size_t pending = min(block_samples, (size_t)fifo_occupancy(&p->vulkan_input_fifo));

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
      if (p->vulkan_resident_pending >=
          lsx_rate_vulkan_resident_batch_depth(
              p->vulkan_resident)) {
        p->vulkan_resident_pending = 0;
        if (lsx_fir_vulkan_flush_resident(p->vulkan) !=
            SOX_SUCCESS)
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

static int transform_vulkan_resident(
    sox_effect_t *effp,
    lsx_vulkan_resident_buffer_t const *input,
    sox_bool *input_consumed, uint64_t *input_clips,
    lsx_vulkan_resident_buffer_t *output,
    sox_bool *output_produced, sox_bool *active)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;

  if (!p->vulkan_resident || !input_consumed || !input_clips ||
      !output || !output_produced || !active)
    return SOX_EOF;
  memset(output, 0, sizeof(*output));
  *input_consumed = sox_false;
  *input_clips = lsx_rate_vulkan_resident_stream_clips(
      p->vulkan_resident);
  *output_produced = sox_false;
  if (take_vulkan_resident_transform_output(
      effp, lsx_vulkan_resident_ready, output,
      output_produced) != SOX_SUCCESS)
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
  if (take_vulkan_resident_transform_output(
      effp, lsx_vulkan_resident_ready, output,
      output_produced) != SOX_SUCCESS)
    return SOX_EOF;
  if (!*output_produced &&
      p->vulkan_resident_pending >=
          lsx_rate_vulkan_resident_batch_depth(
              p->vulkan_resident)) {
    p->vulkan_resident_pending = 0;
    if (lsx_rate_vulkan_flush_resident(
        p->vulkan_resident) != SOX_SUCCESS)
      return SOX_EOF;
    *input_clips +=
        lsx_rate_vulkan_resident_stream_clips_completed(
            p->vulkan_resident);
  }
  *active = lsx_rate_vulkan_resident_stream_ready(
      p->vulkan_resident);
  return SOX_SUCCESS;
}

static size_t emit_vulkan_consumer_output(
    fifo_t *fifo, sox_sample_t *output, size_t capacity,
    uint64_t *clips)
{
  size_t count = min(capacity, (size_t)fifo_occupancy(fifo));
  double const *samples = fifo_read(fifo, count, NULL);

  if (count)
    lsx_save_samples(output, samples, count, clips);
  return count;
}

static int queue_vulkan_consumer_output(
    sox_effect_t *effp,
    lsx_vulkan_resident_buffer_t const *resident)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t samples;

  if (!resident ||
      resident->domain != lsx_vulkan_resident_domain_sox_sample ||
      resident->channels != effp->out_signal.channels)
    return SOX_EOF;
  samples = resident->valid_elements * resident->channels;
  if (resident->channels &&
      samples / resident->channels != resident->valid_elements)
    return SOX_EOF;
  if (lsx_vulkan_download_resident_pcm(
      p->vulkan_context, resident, p->vulkan_drain_block,
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      resident->channels) !=
      SOX_SUCCESS)
    return SOX_EOF;
  fifo_write(
      &p->vulkan_output_fifo, samples, p->vulkan_drain_block);
  return SOX_SUCCESS;
}

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

  if (!p->vulkan_resident || !input_consumed || !input_clips ||
      !obuf || !osamp || !active)
    return SOX_EOF;
  capacity = *osamp;
  *osamp = 0;
  *input_consumed = sox_false;
  *input_clips = lsx_rate_vulkan_resident_stream_clips(
      p->vulkan_resident);
  emitted = emit_vulkan_consumer_output(
      &p->vulkan_output_fifo, obuf, capacity, input_clips);
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
    else if (p->vulkan_consumer_final_received &&
             !p->vulkan_consumer_done)
      status = drain_transform_vulkan_resident(
          effp, input_clips, &resident, &produced,
          &p->vulkan_consumer_done);
    else
      break;
    if (status != SOX_SUCCESS)
      return status;
    if (produced) {
      size_t count;

      if (queue_vulkan_consumer_output(effp, &resident) !=
          SOX_SUCCESS)
        return SOX_EOF;
      count = emit_vulkan_consumer_output(
          &p->vulkan_output_fifo, obuf + emitted,
          capacity - emitted, input_clips);
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
      p->vulkan_consumer_transform_active ||
      (p->vulkan_consumer_final_received &&
       !p->vulkan_consumer_done);
  return SOX_SUCCESS;
}

static int drain_transform_vulkan_resident(
    sox_effect_t *effp, uint64_t *input_clips,
    lsx_vulkan_resident_buffer_t *output,
    sox_bool *output_produced, sox_bool *done)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  uint64_t remaining = p->samples_in > p->samples_out ?
      p->samples_in - p->samples_out : 0;

  if (!p->vulkan_resident || !input_clips || !output ||
      !output_produced || !done)
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

    output->valid_elements = min(
        produced_frames, (size_t)(remaining / channels));
    p->samples_out -=
        (produced_frames - output->valid_elements) * channels;
  }
  *done = p->samples_out >= p->samples_in;
  if (*done) {
    p->samples_out = p->samples_in;
    output->state = lsx_vulkan_resident_final;
  }
  return SOX_SUCCESS;
}

static int flow_vulkan(
    sox_effect_t *effp, sox_sample_t const *ibuf,
    sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t odone = min(
      *osamp,
      (size_t)fifo_occupancy(&p->vulkan_output_fifo));
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
    double *input = fifo_write(
        &p->vulkan_input_fifo, *isamp, NULL);

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

static int drain_vulkan(
    sox_effect_t *effp, sox_sample_t *obuf, size_t *osamp)
{
  priv_t *p = (priv_t *)effp->priv;
  static size_t isamp;
  size_t block_samples =
      lsx_fir_vulkan_block_frames_for(p->vulkan_context) *
      effp->in_signal.channels;
  uint64_t remaining =
      p->samples_in > p->samples_out ?
      p->samples_in - p->samples_out : 0;

  while ((uint64_t)fifo_occupancy(
      &p->vulkan_output_fifo) < remaining) {
    size_t pending = min(
        block_samples,
        (size_t)fifo_occupancy(&p->vulkan_input_fifo));
    double const *output;

    memset(
        p->vulkan_drain_block, 0,
        block_samples * sizeof(*p->vulkan_drain_block));
    if (pending)
      fifo_read(
          &p->vulkan_input_fifo, pending,
          p->vulkan_drain_block);
    if (lsx_fir_vulkan_process(
        p->vulkan, p->vulkan_drain_block,
        &output) != SOX_SUCCESS) {
      *osamp = 0;
      return SOX_EOF;
    }
    append_vulkan_output(effp, output);
  }
  if ((uint64_t)fifo_occupancy(
      &p->vulkan_output_fifo) > remaining)
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
  if (p->vulkan)
    return flow_vulkan(effp, ibuf, obuf, isamp, osamp);
#endif
  odone = min(
      *osamp, (size_t)fifo_occupancy(&p->output_fifo));
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
  if (p->vulkan)
    return drain_vulkan(effp, obuf, osamp);
#endif
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
  if (p->vulkan) {
    lsx_rate_vulkan_destroy(p->vulkan_resident);
    p->vulkan_resident = NULL;
    lsx_fir_vulkan_destroy(p->vulkan);
    fifo_delete(&p->vulkan_input_fifo);
    fifo_delete(&p->vulkan_output_fifo);
    free(p->vulkan_drain_block);
    p->vulkan = NULL;
    p->vulkan_context = NULL;
    p->vulkan_drain_block = NULL;
    effp->internal_chain_endpoint = NULL;
    memset(p->filter_ptr, 0, sizeof(*p->filter_ptr));
    free(p->vulkan_source_taps);
    p->vulkan_source_taps = NULL;
    p->vulkan_source_num_taps = 0;
    p->vulkan_source_post_peak = 0;
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

#if HAVE_VULKAN
int lsx_dft_filter_restart_vulkan(
    sox_effect_t *effp, double *taps,
    int num_taps, int post_peak)
{
  priv_t *p;

  if (!effp || !taps || num_taps < 1 ||
      post_peak < 0 || post_peak >= num_taps)
    return SOX_EOF;
  p = (priv_t *)effp->priv;
  if (!p || !p->vulkan || stop(effp) != SOX_SUCCESS) {
    free(taps);
    return SOX_EOF;
  }
  p->vulkan_source_taps = lsx_memdup(
      taps, (size_t)num_taps * sizeof(*taps));
  p->vulkan_source_num_taps = num_taps;
  p->vulkan_source_post_peak = post_peak;
  lsx_set_dft_filter(
      p->filter_ptr, taps, num_taps, post_peak);
  return start(effp);
}
#endif

sox_effect_handler_t const * lsx_dft_filter_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    NULL, NULL, SOX_EFF_GAIN, NULL, start, flow, drain, stop, NULL, 0
  };
  return &handler;
}
