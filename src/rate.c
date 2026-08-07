/* Effect: change sample rate  Copyright (c) 2008,12 robs@users.sourceforge.net
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

/* Inspired by, and builds upon some of the ideas presented in:
 * `The Quest For The Perfect Resampler' by Laurent De Soras;
 * http://ldesoras.free.fr/doc/articles/resampler-en.pdf */

#ifdef NDEBUG /* Enable assert always. */
#undef NDEBUG /* Must undef above assert.h or other that might include it. */
#endif

#define _GNU_SOURCE
#include "sox_i.h"
#include "fft4g.h"
#include "dft_filter.h"
#include "diagnostics.h"
#include <assert.h>
#include <string.h>

#if HAVE_VULKAN
#include "rate_cubic_vulkan.h"
#include "rate_dsd_vulkan.h"
#include "rate_polyphase_vulkan.h"
#include "rate_vulkan.h"
#include "vulkan_effect_chain.h"

static int flow_vulkan_resident_endpoint(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
static int drain_vulkan_resident_endpoint(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);
static int transform_vulkan_resident_endpoint(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *active);
static int drain_transform_vulkan_resident_endpoint(sox_effect_t *effp, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *done);

/*
 * The effect-level resident interface.  The chain reaches it through the
 * lsx_vulkan_effect_endpoint_t vtable above, so these names stay inside this
 * file; the wrappers that go into the vtable are the four declared above.
 * effp is always a rate effect; calling these on any other is a programming
 * error.
 */

/* Whether this effect's chain is a lone DFT or a chain of resident stages, which is what the engine tunes its batch depth against. */
static lsx_vulkan_resident_topology_t lsx_rate_effect_resident_topology(sox_effect_t const *effp);

/* Resident equivalents of the effect's flow and drain.  *produced says whether a resident block came out, and *done, on drain, whether the effect has finished flushing. */
static int lsx_rate_effect_flow_resident(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
static int lsx_rate_effect_drain_resident(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);

/* What this effect's configuration can take part in.  The three are
 * independent: an effect may publish resident output without accepting
 * resident input, and the transform being supported is a further condition
 * still, so a caller has to ask the one it means. */
static sox_bool lsx_rate_effect_resident_supported(sox_effect_t const *effp);
static sox_bool lsx_rate_effect_resident_input_supported(sox_effect_t const *effp);
static sox_bool lsx_rate_effect_resident_transform_supported(sox_effect_t const *effp);

/* Whether the effect can take a resident block right now, its stream having room for one. */
static sox_bool lsx_rate_effect_resident_input_ready(sox_effect_t const *effp);

/* Clips counted while quantizing input from another effect, moved out of the effect so the chain can add them to its own total.  Reading them clears the count. */
static uint64_t lsx_rate_effect_external_input_clips(sox_effect_t *effp);

static lsx_vulkan_effect_endpoint_t const vulkan_resident_producer_endpoint = {
  flow_vulkan_resident_endpoint,
  drain_vulkan_resident_endpoint,
  NULL,
  NULL,
  NULL
};

static lsx_vulkan_effect_endpoint_t const vulkan_resident_transform_endpoint = {
  flow_vulkan_resident_endpoint,
  drain_vulkan_resident_endpoint,
  NULL,
  transform_vulkan_resident_endpoint,
  drain_transform_vulkan_resident_endpoint
};

#endif

#define calloc     lsx_calloc
#define malloc     lsx_malloc
#define raw_coef_t double

#if 0 /* For float32 version, as used in foobar */
  #define sample_t   float
  #define num_coefs4 ((num_coefs + 3) & ~3) /* align coefs for SSE */
  #define coefs4_check(i) ((i) < num_coefs)
#else
  #define sample_t   double
  #define num_coefs4 num_coefs
  #define coefs4_check(i) 1
#endif

#if defined M_PIl
  #define hi_prec_clock_t long double /* __float128 is also a (slow) option */
#else
  #define hi_prec_clock_t double
#endif

#define coef(coef_p, interp_order, fir_len, phase_num, coef_interp_num, fir_coef_num) coef_p[(fir_len) * ((interp_order) + 1) * (phase_num) + ((interp_order) + 1) * (fir_coef_num) + (interp_order - coef_interp_num)]

static sample_t * prepare_coefs(raw_coef_t const * coefs, int num_coefs,
    int num_phases, int interp_order, int multiplier)
{
  int i, j, length = num_coefs4 * num_phases;
  sample_t * result = malloc(length * (interp_order + 1) * sizeof(*result));
  double fm1 = coefs[0], f1 = 0, f2 = 0;

  for (i = num_coefs4 - 1; i >= 0; --i)
    for (j = num_phases - 1; j >= 0; --j) {
      double f0 = fm1, b = 0, c = 0, d = 0; /* = 0 to kill compiler warning */
      int pos = i * num_phases + j - 1;
      fm1 = coefs4_check(i) && pos > 0 ? coefs[pos - 1] * multiplier : 0;
      switch (interp_order) {
        case 1: b = f1 - f0; break;
        case 2: b = f1 - (.5 * (f2+f0) - f1) - f0; c = .5 * (f2+f0) - f1; break;
        case 3: c=.5*(f1+fm1)-f0;d=(1/6.)*(f2-f1+fm1-f0-4*c);b=f1-f0-d-c; break;
        default: if (interp_order) assert(0);
      }
      #define coef_coef(x) \
        coef(result, interp_order, num_coefs4, j, x, num_coefs4 - 1 - i)
      coef_coef(0) = f0;
      if (interp_order > 0) coef_coef(1) = b;
      if (interp_order > 1) coef_coef(2) = c;
      if (interp_order > 2) coef_coef(3) = d;
      #undef coef_coef
      f2 = f1, f1 = f0;
    }
  return result;
}

typedef struct { /* So generated filter coefs may be shared between channels */
  sample_t   * poly_fir_coefs;
  dft_filter_t dft_filter[2];
  /* The fused DSD response, when the plan has one.  Kept in host doubles
   * rather than sample_t: it is designed once and handed to the device, and
   * no CPU stage ever convolves with it. */
  double     * dsd_taps;
  int          dsd_num_taps;
  int          dsd_post_peak;
} rate_shared_t;

typedef enum {
  rate_stage_half_fir,
  rate_stage_dft,
  rate_stage_cubic,
  rate_stage_poly_fir,
  rate_stage_dsd_fir
} rate_stage_kind_t;

static char const *rate_stage_name(rate_stage_kind_t kind)
{
  static char const *names[] = {"half_fir", "dft", "cubic", "poly_fir", "dsd_fir"};

  return (unsigned)kind < array_length(names) ? names[kind] : "unknown";
}

struct stage;
typedef void (* stage_fn_t)(struct stage * input, fifo_t * output);
typedef struct stage {
  /* Common to all stage types: */
  rate_stage_kind_t kind;
  stage_fn_t fn;
  fifo_t     fifo;
  int        pre;       /* Number of past samples to store */
  int        pre_post;  /* pre + number of future samples to store */
  int        preload;   /* Number of zero samples to pre-load the fifo */
  double     out_in_ratio; /* For buffer management. */

  /* For a stage with variable (run-time generated) filter coefs: */
  rate_shared_t * shared;
  int        dft_filter_num; /* Which, if any, of the 2 DFT filters to use */

  /* For a stage with variable L/M: */
  union {               /* 32bit.32bit fixed point arithmetic */
    #if defined(WORDS_BIGENDIAN)
    struct {int32_t integer; uint32_t fraction;} parts;
    #else
    struct {uint32_t fraction; int32_t integer;} parts;
    #endif
    int64_t all;
    #define MULT32 (65536. * 65536.)

    hi_prec_clock_t hi_prec_clock;
  } at, step;
  sox_bool   use_hi_prec_clock;
  int        L, M, remL, remM;
  int        n, phase_bits, phase_count, interp_order;
  sample_t const * static_coefs;
} stage_t;

#define stage_read_p(s) ((sample_t *)fifo_read_ptr(&(s)->fifo) + (s)->pre)

static int stage_occupancy(stage_t * s)
{
  size_t const occupancy = fifo_occupancy(&s->fifo);

  if (occupancy <= (size_t)s->pre_post)
    return 0;
  assert(occupancy - (size_t)s->pre_post <= INT_MAX);
  return (int)(occupancy - (size_t)s->pre_post);
}

static void cubic_stage_fn(stage_t * p, fifo_t * output_fifo)
{
  int i, num_in = stage_occupancy(p), max_num_out = 1 + num_in*p->out_in_ratio;
  sample_t const * input = stage_read_p(p);
  sample_t * output = fifo_reserve(output_fifo, max_num_out);

  for (i = 0; p->at.parts.integer < num_in; ++i, p->at.all += p->step.all) {
    sample_t const * s = input + p->at.parts.integer;
    sample_t x = p->at.parts.fraction * (1 / MULT32);
    sample_t b = .5*(s[1]+s[-1])-*s, a = (1/6.)*(s[2]-s[1]+s[-1]-*s-4*b);
    sample_t c = s[1]-*s-a-b;
    output[i] = ((a*x + b)*x + c)*x + *s;
  }
  assert(max_num_out - i >= 0);
  fifo_trim_by(output_fifo, max_num_out - i);
  fifo_read(&p->fifo, p->at.parts.integer, NULL);
  p->at.parts.integer = 0;
}

/* The frequency-domain interpolation below replicates the input spectrum L
 * times.  That construction always starts at phase zero, so it cannot carry a
 * sub-L offset from one block to the next, and it is exact only when a block
 * consumes a whole number of input frames.  Both hold when the filter length
 * minus one is a multiple of L, which lsx_design_lpf arranges for a linear
 * phase filter.  lsx_fir_to_phase does not preserve it: for any other phase
 * the block then consumes L*ceil((dft_length - overlap)/L) samples while
 * emitting dft_length - overlap of them, and the output drifts by the
 * difference on every block.  Where the geometry does not hold, stuff the
 * zeros in the time domain instead; it is slower, but it tracks the offset in
 * remL. */
static sox_bool dft_stage_f_domain_exact(dft_filter_t const * f, int L, int remL)
{
  return lsx_is_power_of_2(L) && !remL && (f->dft_length - (f->num_taps - 1)) % L == 0;
}

static void dft_stage_fn(stage_t * p, fifo_t * output_fifo)
{
  sample_t * output, tmp;
  int i, j, num_in = max(0, fifo_occupancy(&p->fifo));
  rate_shared_t const * s = p->shared;
  dft_filter_t const * f = &s->dft_filter[p->dft_filter_num];
  int const overlap = f->num_taps - 1;

  while (p->remL + p->L * num_in >= f->dft_length) {
    div_t divd = div(f->dft_length - overlap - p->remL + p->L - 1, p->L);
    sample_t const * input = fifo_read_ptr(&p->fifo);
    fifo_read(&p->fifo, divd.quot, NULL);
    num_in -= divd.quot;

    output = fifo_reserve(output_fifo, f->dft_length);
    if (dft_stage_f_domain_exact(f, p->L, p->remL)) { /* F-domain */
      int portion = f->dft_length / p->L;
      memcpy(output, input, (unsigned)portion * sizeof(*output));
      lsx_safe_rdft(portion, 1, output);
      for (i = portion + 2; i < (portion << 1); i += 2)
        output[i] = output[(portion << 1) - i],
        output[i+1] = -output[(portion << 1) - i + 1];
      output[portion] = output[1];
      output[portion + 1] = 0;
      output[1] = output[0];
      for (portion <<= 1; i < f->dft_length; i += portion, portion <<= 1) {
        memcpy(output + i, output, portion * sizeof(*output));
        output[i + 1] = 0;
      }
    } else {
      if (p->L == 1)
        memcpy(output, input, f->dft_length * sizeof(*output));
      else {
        memset(output, 0, f->dft_length * sizeof(*output));
        for (j = 0, i = p->remL; i < f->dft_length; ++j, i += p->L)
          output[i] = input[j];
        p->remL = p->L - 1 - divd.rem;
      }
      lsx_safe_rdft(f->dft_length, 1, output);
    }
    output[0] *= f->coefs[0];
    if (p->step.parts.integer > 0) {
      output[1] *= f->coefs[1];
      for (i = 2; i < f->dft_length; i += 2) {
        tmp = output[i];
        output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
        output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
      }
      lsx_safe_rdft(f->dft_length, -1, output);
      if (p->step.parts.integer != 1) {
        for (j = 0, i = p->remM; i < f->dft_length - overlap; ++j,
            i += p->step.parts.integer)
          output[j] = output[i];
        p->remM = i - (f->dft_length - overlap);
        fifo_trim_by(output_fifo, f->dft_length - j);
      }
      else fifo_trim_by(output_fifo, overlap);
    }
    else { /* F-domain */
      int m = -p->step.parts.integer;
      for (i = 2; i < (f->dft_length >> m); i += 2) {
        tmp = output[i];
        output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
        output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
      }
      output[1] = f->coefs[i] * output[i] - f->coefs[i+1] * output[i+1];
      lsx_safe_rdft(f->dft_length >> m, -1, output);
      fifo_trim_by(output_fifo, (((1 << m) - 1) * f->dft_length + overlap) >>m);
    }
  }
}

static void dft_stage_init(
    unsigned instance, double Fp, double Fs, double Fn, double att,
    double phase, stage_t * stage, int L, int M)
{
  dft_filter_t * f = &stage->shared->dft_filter[instance];
  
  if (!f->num_taps) {
    int num_taps = 0, dft_length, i;
    int k = phase == 50 && lsx_is_power_of_2(L) && Fn == L? L << 1 : 4;
    double * h = lsx_design_lpf(Fp, Fs, Fn, att, &num_taps, -k, -1.);

    if (phase != 50)
      lsx_fir_to_phase(&h, &num_taps, &f->post_peak, phase);
    else f->post_peak = num_taps / 2;

    dft_length = lsx_set_dft_length(num_taps);
    f->coefs = calloc(dft_length, sizeof(*f->coefs));
    for (i = 0; i < num_taps; ++i)
      f->coefs[(i + dft_length - num_taps + 1) & (dft_length - 1)]
        = h[i] / dft_length * 2 * L;
    f->taps = h;
    f->num_taps = num_taps;
    f->dft_length = dft_length;
    lsx_safe_rdft(dft_length, 1, f->coefs);
    lsx_debug("fir_len=%i dft_length=%i Fp=%g Fs=%g Fn=%g att=%g %i/%i",
        num_taps, dft_length, Fp, Fs, Fn, att, L, M);
  }
  stage->kind = rate_stage_dft;
  stage->fn = dft_stage_fn;
  stage->preload = f->post_peak / L;
  stage->remL    = f->post_peak % L;
  stage->L = L;
  stage->M = M;
  stage->step.parts.integer = abs(3-M) == 1 && Fs == 1? -M/2 : M;
  stage->dft_filter_num = instance;
}

#include "rate_filters.h"

typedef struct {
  double     factor;
  int        num_stages;
  stage_t    * stages;
} rate_plan_t;

typedef struct {
  rate_plan_t plan;
  uint64_t    samples_in, samples_out;
} rate_t;

/* head is the number of stages before the pre stage: the half-band cascade,
 * or the single fused DSD stage that stands in for it. */
#define pre_stage       p->stages[head]
#define arb_stage       p->stages[head + have_pre_stage]
#define post_stage      p->stages[head + have_pre_stage + have_arb_stage]
#define have_pre_stage  (preM  * preL  != 1)
#define have_arb_stage  (arbM  * arbL  != 1)
#define have_post_stage (postM * postL != 1)

#define TO_3dB(a)       ((1.6e-6*a-7.5e-4)*a+.646)
#define LOW_Q_BW0_PC    (67 + 5 / 8.)
#define RATE_MIN_INPUT_CHUNK 4096

typedef enum {
  rolloff_none, rolloff_small /* <= 0.01 dB */, rolloff_medium /* <= 0.35 dB */
} rolloff_t;

/* How much of a DSD stream is worth decoding, in Hz.
 *
 * A DSD recording carries its quantisation noise above the audio band, rising
 * steeply, and the higher the DSD rate the further up that rise begins.  Above
 * these frequencies there is no signal to recover, only shaped noise, so a
 * decimation filter that reaches higher buys nothing and costs taps in
 * proportion.  The ceiling is stated in absolute Hz because that is what it
 * is: a property of the recording, not of the rate being decoded to. */
static double rate_dsd_band_ceiling(double dsd_rate)
{
  double multiple = dsd_rate / 2822400.;

  return multiple < 1.5 ? 20000. : multiple < 3 ? 30000. :
      multiple < 6 ? 60000. : 90000.;
}

/* The 0.01 dB pass-band edge of each half_firs[] entry, as a fraction of the
 * Nyquist frequency of the rate that entry decimates to.  Measured from the
 * coefficients themselves, and listed in the same order.
 *
 * It is here because the fused stage replaces a cascade of these, and where
 * the cascade rather than the DSD ceiling is what limits the band, the fused
 * response has to stop where the cascade would have: otherwise decoding the
 * same file with and without the fused stage would not give the same band. */
static double const half_fir_pass_edge[] = {
  .624118, .641940, .657584, .671423, .683762, .694842
};

/* Design the one response that replaces the whole half-band cascade.
 *
 * Two limits meet here and the tighter wins, which is the whole of the rule.
 * The cascade's own limit is the Nyquist frequency of what it decimates to,
 * with the pass-band edge the half-band filters would have had; the DSD
 * ceiling is absolute.  When the ceiling bites (which for DSD64 it does at
 * every output rate) the transition is set to 2.5% of it rather than taken
 * from the cascade, so the audible band stays flat to 19.5 kHz instead of
 * retreating to 62% of 20 kHz for the sake of a band that holds only noise.
 *
 * The attenuation is the request, capped by what the device's arithmetic can
 * actually deliver: designing a stop-band deeper than the accumulator's own
 * floor costs taps in proportion and rejects nothing further. */
static double *rate_dsd_design(
    double dsd_rate, int shift, int half_fir, double att, double phase,
    int *num_taps, int *post_peak)
{
  double mid_nyquist = dsd_rate / (double)(1 << shift) * .5;
  double Fs = min(mid_nyquist, rate_dsd_band_ceiling(dsd_rate));
  double Fp = min(mid_nyquist * half_fir_pass_edge[half_fir], .975 * Fs);
  double *h;

  *num_taps = 0;
  h = lsx_design_lpf(Fp, Fs, dsd_rate * .5, att, num_taps, -2, -1.);
  if (phase != 50)
    lsx_fir_to_phase(&h, num_taps, post_peak, phase);
  else
    *post_peak = *num_taps / 2;
  lsx_debug("dsd_fir taps=%i Fp=%g Fs=%g Fn=%g att=%g 1/%i post_peak=%i",
      *num_taps, Fp, Fs, dsd_rate * .5, att, 1 << shift, *post_peak);
  return h;
}

static void rate_plan_create(
  /* Private work areas (to be supplied by the client):                       */
  rate_plan_t * p,           /* Per audio channel.                            */
  rate_shared_t * shared,    /* Between channels (undergoing same rate change)*/
                            
  /* Public parameters:                                             Typically */
  double factor,             /* Input rate divided by output rate.            */
  double bits,               /* Required bit-accuracy (pass + stop)  16|20|28 */
  double phase,              /* Linear/minimum etc. filter phase.       50    */
  double bw_pc,              /* Pass-band % (0dB pt.) to preserve.   91.3|98.4*/
  double anti_aliasing_pc,   /* % bandwidth without aliasing            100   */
  rolloff_t rolloff,         /* Pass-band roll-off                    small   */
  sox_bool maintain_3dB_pt,  /*                                        true   */
                            
  /* Primarily for test/development purposes:                                 */
  sox_bool use_hi_prec_clock,/* Increase irrational ratio accuracy.   false   */
  int interpolator,          /* Force a particular coef interpolator.   -1    */
  int max_coefs_size,        /* k bytes of coefs to try to keep below.  400   */
  sox_bool noSmallIntOpt,    /* Disable small integer optimisations.  false   */

  /* Packed DSD input, which replaces the half-band cascade with one fused
   * stage.  Zero for every other input, and then nothing below changes.     */
  double dsd_rate,           /* The DSD frame rate, in Hz.              0     */
  double dsd_att_ceiling)    /* What the device's arithmetic can hold.  0     */
{
  double att_single = (bits + 1) * linear_to_dB(2.);       /* Before splitting */
  double att = att_single, attArb = att;                     /* pass + stop */
  double tbw0 = 1 - bw_pc / 100, Fs_a = 2 - anti_aliasing_pc / 100;
  double arbM = factor, tbw_tighten = 1;
  int n = 0, i, preL = 1, preM = 1, shift = 0, arbL = 1, postL = 1, postM = 1;
  int head;
  sox_bool fuse_dsd;
  sox_bool upsample = sox_false, rational = sox_false, iOpt = !noSmallIntOpt;
  int mode = rolloff > rolloff_small? factor > 1 || bw_pc > LOW_Q_BW0_PC :
    ceil(2 + (bits - 17) / 4);
  stage_t * s;

  assert(factor > 0);
  assert(!bits || (15 <= bits && bits <= 33));
  assert(0 <= phase && phase <= 100);
  assert(53 <= bw_pc && bw_pc <= 100);
  assert(85 <= anti_aliasing_pc && anti_aliasing_pc <= 100);

  p->factor = factor;
  if (bits) while (!n++) {                               /* Determine stages: */
    int try, L, M, x, maxL = interpolator > 0? 1 : mode? 2048 :
      ceil(max_coefs_size * 1000. / (U100_l * sizeof(sample_t)));
    double d, epsilon = 0, frac;
    upsample = arbM < 1;
    for (i = arbM * .5, shift = 0; i >>= 1; arbM *= .5, ++shift);
    preM = upsample || (arbM > 1.5 && arbM < 2);
    postM = 1 + (arbM > 1 && preM), arbM /= postM;
    preL = 1 + (!preM && arbM < 2) + (upsample && mode), arbM *= preL;
    if ((frac = arbM - (int)arbM))
      epsilon = fabs((uint32_t)(frac * MULT32 + .5) / (frac * MULT32) - 1);
    for (i = 1, rational = !frac; i <= maxL && !rational; ++i) {
      d = frac * i, try = d + .5;
      if ((rational = fabs(try / d - 1) <= epsilon)) {    /* No long doubles! */
        if (try == i)
          arbM = ceil(arbM), shift += arbM > 2, arbM /= 1 + (arbM > 2);
        else arbM = i * (int)arbM + try, arbL = i;
      }
    }
    L = preL * arbL, M = arbM * postM, x = (L|M)&1, L >>= !x, M >>= !x;
    if (iOpt && postL == 1 && (d = preL * arbL / arbM) > 4 && d != 5) {
      for (postL = 4, i = d / 16; i >>= 1; postL <<= 1);
      arbM = arbM * postL / arbL / preL, arbL = 1, n = 0;
    } else if (rational && (max(L, M) < 3 + 2 * iOpt || L * M < 6 * iOpt))
      preL = L, preM = M, arbM = arbL = postM = 1;
    if (!mode && (!rational || !n))
      ++mode, n = 0;
  }

  /* The fused stage stands in for the whole cascade, so it exists only when
   * there is a cascade to stand in for.  A plan that does not halve at all
   * has nothing to fuse and is left as the planner made it. */
  fuse_dsd = dsd_rate > 0 && shift > 0;
  head = fuse_dsd ? 1 : shift;
  p->num_stages = head + have_pre_stage + have_arb_stage + have_post_stage;

  if (!p->num_stages)
    return;

  p->stages = calloc(p->num_stages + 1, sizeof(*p->stages));
  for (i = 0; i < p->num_stages; ++i)
    p->stages[i].shared = shared;

  if ((n = p->num_stages) > 1) {                              /* Att. budget: */
    if (have_arb_stage)
      att += linear_to_dB(2.), attArb = att, --n; 
    att += linear_to_dB((double)n);
  }

  for (n = 0; n + 1u < array_length(half_firs) && att > half_firs[n].att; ++n);
  if (fuse_dsd) {
    /* One stage, so the whole budget is its own: the split above exists to
     * share a stop-band between several filters in series, and there is only
     * this one.  The device's arithmetic caps it from the other side. */
    double dsd_att = dsd_att_ceiling > 0 ?
        min(att_single, dsd_att_ceiling) : att_single;
    stage_t *stage = p->stages;

    if (!stage->shared->dsd_taps)
      stage->shared->dsd_taps = rate_dsd_design(
          dsd_rate, shift, n, dsd_att, phase,
          &stage->shared->dsd_num_taps, &stage->shared->dsd_post_peak);
    stage->kind = rate_stage_dsd_fir;
    stage->L = 1;
    stage->M = 1 << shift;
    stage->n = stage->shared->dsd_num_taps;
    stage->pre_post = stage->shared->dsd_num_taps - 1;
    stage->preload = stage->pre = stage->shared->dsd_post_peak;
  }
  else for (i = 0, s = p->stages; i < shift; ++i, ++s) {
    s->kind = rate_stage_half_fir;
    s->fn = half_firs[n].fn;
    s->static_coefs = half_firs[n].coefs;
    s->pre_post = 4 * half_firs[n].num_coefs;
    s->preload = s->pre = s->pre_post >> 1;
  }

  if (have_pre_stage) {
    if (maintain_3dB_pt && have_post_stage) {    /* Trans. bands overlapping. */
      double tbw3 = tbw0 * TO_3dB(att);               /* TODO: consider Fs_a. */
      double x = ((2.1429e-4 - 5.2083e-7 * att) * att - .015863) * att + 3.95;
      x = att * pow((tbw0 - tbw3) / (postM / (factor * postL) - 1 + tbw0), x);
      if (x > .035) {
        tbw_tighten = ((4.3074e-3 - 3.9121e-4 * x) * x - .040009) * x + 1.0014;
        lsx_debug("x=%g tbw_tighten=%g", x, tbw_tighten);
      }
    }
    dft_stage_init(0, 1 - tbw0 * tbw_tighten, Fs_a, preM? max(preL, preM) :
        arbM / arbL, att, phase, &pre_stage, preL, max(preM, 1));
  }

  if (!bits) {                                  /* Quick and dirty arb stage: */
    arb_stage.kind = rate_stage_cubic;
    arb_stage.fn = cubic_stage_fn;
    arb_stage.step.all = arbM * MULT32 + .5;
    arb_stage.pre_post = max(3, arb_stage.step.parts.integer);
    arb_stage.preload = arb_stage.pre = 1;
    arb_stage.out_in_ratio = MULT32 * arbL / arb_stage.step.all;
  }
  else if (have_arb_stage) {                     /* Higher quality arb stage: */
    poly_fir_t const * f = &poly_firs[6*(upsample + !!preM) + mode - !upsample];
    int order, num_coefs = f->interp[0].scalar, phase_bits, phases, coefs_size;
    double x = .5, at, Fp, Fs, Fn, mult = upsample? 1 : arbL / arbM;
    poly_fir1_t const * f1;

    Fn = !upsample && preM? x = arbM / arbL : 1;
    Fp = !preM? mult : mode? .5 : 1;
    Fs = 2 - Fp;           /* Ignore Fs_a; it would have little benefit here. */
    Fp *= 1 - tbw0;
    if (rolloff > rolloff_small && mode)
      Fp = !preM? mult * .5 - .125 : mult * .05 + .1;
    else if (rolloff == rolloff_small)
      Fp = Fs - (Fs - .148 * x - Fp * .852) * (.00813 * bits + .973);

    i = (interpolator < 0? !rational : max(interpolator, !rational)) - 1;
    do {
      f1 = &f->interp[++i];
      assert(f1->fn);
      if (i)
        arbM /= arbL, arbL = 1, rational = sox_false;
      phase_bits = ceil(f1->scalar + log(mult)/log(2.));
      phases = !rational? (1 << phase_bits) : arbL;
      if (!f->interp[0].scalar) {
        int phases0 = max(phases, 19), n0 = 0;
        lsx_design_lpf(Fp, Fs, -Fn, attArb, &n0, phases0, f->beta);
        num_coefs = n0 / phases0 + 1, num_coefs += num_coefs & !preM;
      }
      if ((num_coefs & 1) && rational && (arbL & 1))
        phases <<= 1, arbL <<= 1, arbM *= 2;
      at = arbL * .5 * (num_coefs & 1);
      order = i + (i && mode > 4);
      coefs_size = num_coefs4 * phases * (order + 1) * sizeof(sample_t);
    } while (interpolator < 0 && i < 2 && f->interp[i+1].fn &&
        coefs_size / 1000 > max_coefs_size);

    if (!arb_stage.shared->poly_fir_coefs) {
      int num_taps = num_coefs * phases - 1;
      raw_coef_t * coefs = lsx_design_lpf(
          Fp, Fs, Fn, attArb, &num_taps, phases, f->beta);
      arb_stage.shared->poly_fir_coefs = prepare_coefs(
          coefs, num_coefs, phases, order, 1);
      lsx_debug("fir_len=%i phases=%i coef_interp=%i size=%s",
          num_coefs, phases, order, lsx_sigfigs3((double)coefs_size));
      free(coefs);
    }
    arb_stage.kind = rate_stage_poly_fir;
    arb_stage.fn = f1->fn;
    arb_stage.pre_post = num_coefs4 - 1;
    arb_stage.preload = (num_coefs - 1) >> 1;
    arb_stage.n = num_coefs4;
    arb_stage.phase_bits = phase_bits;
    arb_stage.phase_count = phases;
    arb_stage.interp_order = order;
    arb_stage.L = arbL;
    arb_stage.M = (int)arbM;
    arb_stage.use_hi_prec_clock = mode > 1 && use_hi_prec_clock && !rational;
    if (arb_stage.use_hi_prec_clock) {
      arb_stage.at.hi_prec_clock = at;
      arb_stage.step.hi_prec_clock = arbM;
      arb_stage.out_in_ratio = arbL / arb_stage.step.hi_prec_clock;
    } else {
      arb_stage.at.all = at * MULT32 + .5;
      arb_stage.step.all = arbM * MULT32 + .5;
      arb_stage.out_in_ratio = MULT32 * arbL / arb_stage.step.all;
    }
  }

  if (have_post_stage)
    dft_stage_init(1, 1 - (1 - (1 - tbw0) *
        (upsample? factor * postL / postM : 1)) * tbw_tighten, Fs_a,
        (double)max(postL, postM), att, phase, &post_stage, postL, postM);
}

static void rate_cpu_start(rate_t * p)
{
  rate_plan_t * plan = &p->plan;
  stage_t * s;
  int i;

  for (i = 0, s = plan->stages; i < plan->num_stages; ++i, ++s) {
    fifo_create(&s->fifo, (int)sizeof(sample_t));
    memset(fifo_reserve(&s->fifo, s->preload), 0, sizeof(sample_t) * s->preload);
    lsx_debug("%5i|%-5i preload=%i remL=%i",
        s->pre, s->pre_post - s->pre, s->preload, s->remL);
  }
  fifo_create(&s->fifo, (int)sizeof(sample_t));
}

static void rate_plan_destroy(rate_plan_t * p)
{
  rate_shared_t *shared;

  if (!p->num_stages)
    return;

  shared = p->stages[0].shared;
  free(shared->dft_filter[0].coefs);
  free(shared->dft_filter[0].taps);
  free(shared->dft_filter[1].coefs);
  free(shared->dft_filter[1].taps);
  free(shared->poly_fir_coefs);
  free(shared->dsd_taps);
  memset(shared, 0, sizeof(*shared));
  free(p->stages);
  memset(p, 0, sizeof(*p));
}

static void rate_cpu_process(rate_t * p)
{
  rate_plan_t * plan = &p->plan;
  stage_t * stage = plan->stages;
  int i;

  for (i = 0; i < plan->num_stages; ++i, ++stage)
    stage->fn(stage, &(stage+1)->fifo);
}

static sample_t * rate_cpu_input(rate_t * p, sample_t const * samples, size_t n)
{
  p->samples_in += n;
  return fifo_write(&p->plan.stages[0].fifo, n, samples);
}

static sample_t const * rate_cpu_output(rate_t * p, sample_t * samples, size_t * n)
{
  fifo_t * fifo = &p->plan.stages[p->plan.num_stages].fifo;

  p->samples_out += *n = min(*n, (size_t)fifo_occupancy(fifo));
  return fifo_read(fifo, *n, samples);
}

static void rate_cpu_flush(rate_t * p)
{
  fifo_t * fifo = &p->plan.stages[p->plan.num_stages].fifo;
  uint64_t samples_out = p->samples_in / p->plan.factor + .5;
  size_t remaining = samples_out > p->samples_out ?
      (size_t)(samples_out - p->samples_out) : 0;
  sample_t * buff = calloc(1024, sizeof(*buff));

  if (remaining > 0) {
    while ((size_t)fifo_occupancy(fifo) < remaining) {
      rate_cpu_input(p, buff, (size_t) 1024);
      rate_cpu_process(p);
    }
    fifo_trim_to(fifo, remaining);
    p->samples_in = 0;
  }
  free(buff);
}

static void rate_cpu_stop(rate_t * p)
{
  rate_plan_t * plan = &p->plan;
  int i;

  if (!plan->num_stages)
    return;

  for (i = 0; i <= plan->num_stages; ++i)
    fifo_delete(&plan->stages[i].fifo);
  rate_plan_destroy(plan);
}

/*------------------------------- SoX Wrapper --------------------------------*/

typedef struct {
  sox_rate_t      out_rate;
  int             rolloff, coef_interp, max_coefs_size;
  double          bit_depth, phase, bw_0dB_pc, anti_aliasing_pc;
  sox_bool        use_hi_prec_clock, noIOpt, given_0dB_pt, vulkan_eligible;
  rate_t          rate;
  rate_shared_t   shared, * shared_ptr;
#if HAVE_VULKAN
  /* One executor per plan stage, and one FIFO more than that: the FIFOs sit
   * between the stages, the first holding this effect's input and the last
   * its output. */
  struct rate_vulkan_stage_executor *vulkan_stages;
  fifo_t          *vulkan_fifos;
  size_t          vulkan_stage_count;
  sox_bool        vulkan_final_stream_active;

  /* Packed DSD input, when the plan begins with the fused stage.  One queue
   * per channel, of 32-frame words: the file's own layout is channel major
   * and so is what the stage reads, so nothing is interleaved on the way
   * through.  These stand in for vulkan_fifos[0], which a fused plan leaves
   * unused. */
  fifo_t          *vulkan_dsd_fifos;
  uint32_t const  **vulkan_dsd_runs;
  sox_bool        vulkan_dsd_input;
  sox_bool        vulkan_dsd_flushing;

  /* A resident block produced but not yet accepted downstream.  A stage may
   * be unable to take what the one before it produced -- its stream may be
   * full -- so the block is held here and retried, rather than the producer
   * being made to wait for it. */
  lsx_vulkan_resident_buffer_t vulkan_deferred_append;
  sox_bool        vulkan_deferred_append_valid;

  /* Set when the effect is being driven as a link of someone else's resident
   * chain rather than from host samples. */
  sox_bool        vulkan_external_input_active;
  sox_bool        vulkan_external_output_normalize;

  /* Output produced but not yet handed to the caller, which takes one block
   * at a time. */
  sox_bool        vulkan_pending_output_valid;
  lsx_vulkan_resident_buffer_t vulkan_pending_output;

  /* Input accepted but not yet consumed, held at the two points where a
   * stage can only take part of what it was given: the direct entry into the
   * final stage, and each link of a polyphase chain. */
  sox_bool        vulkan_pending_direct_input_valid;
  lsx_vulkan_resident_buffer_t vulkan_pending_direct_input;
  sox_bool        *vulkan_pending_chain_input_valid;
  lsx_vulkan_resident_buffer_t *vulkan_pending_chain_input;

  /* Blocks queued since the last flush, counted so the device's lag stays
   * within the batch depth. */
  uint32_t        vulkan_polyphase_chain_pending;
  uint32_t        vulkan_external_input_pending;
#endif
} priv_t;

#if HAVE_VULKAN
/*
 * The Vulkan side of the rate effect mirrors the CPU plan stage for stage:
 * rate_plan_create decides what the resampling chain should be, and each of
 * its stages is then given an executor that runs that stage on the device.
 * The plan is therefore the same either way, and only the arithmetic moves.
 *
 * Exactly one backend pointer is set per executor, chosen from the stage's
 * kind.  A DFT stage is the exception: it may be run either as a partitioned
 * transform or, when its response is short enough for the direct form to be
 * cheaper, as a polyphase stage -- hence dft_polyphase, which says which of
 * the two a DFT stage actually became.
 */
typedef struct rate_vulkan_stage_executor {
  rate_stage_kind_t kind;
  lsx_rate_vulkan_t *dft;
  lsx_rate_cubic_vulkan_t *cubic;
  lsx_rate_dsd_vulkan_t *dsd;
  lsx_rate_polyphase_vulkan_t *polyphase;
  sox_bool dft_polyphase;
  size_t polyphase_block_frames;
  sox_rate_t output_rate;
} rate_vulkan_stage_executor_t;

/* Whether this stage runs as a transform rather than in the direct form.  It
 * matters beyond the choice of backend: only the transform stage exchanges
 * resident buffers with its neighbours and insists on exact blocks. */
static sox_bool rate_vulkan_executor_is_fft(rate_vulkan_stage_executor_t const *executor)
{
  return executor->kind == rate_stage_dft && !executor->dft_polyphase;
}

/* Feed a resident block through a polyphase stage and write the result to a
 * host FIFO.
 *
 * The block is cut into slices no larger than the stage accepts per call --
 * a transform stage's block is much the larger of the two -- and the
 * description is advanced across the block rather than anything being
 * copied.  A final block cut into several slices has all but its last marked
 * draining, since only the last is really the end of the stream and marking
 * them all final would start the stage's drain early. */
static int process_vulkan_resident_polyphase_to_host(
    rate_vulkan_stage_executor_t *polyphase,
    lsx_vulkan_resident_buffer_t const *resident,
    fifo_t *output_fifo, size_t channels)
{
  lsx_vulkan_resident_buffer_t remaining;
  size_t element_size;

  if (!polyphase || !polyphase->polyphase || !resident || !output_fifo || !channels)
    return SOX_EINVAL;
  element_size = lsx_vulkan_resident_element_size(resident->format);
  if (!element_size)
    return SOX_EINVAL;
  remaining = *resident;
  while (remaining.valid_elements) {
    lsx_vulkan_resident_buffer_t slice = remaining;
    double const *output;
    size_t output_frames;
    size_t slice_frames = min(remaining.valid_elements, lsx_rate_polyphase_vulkan_block_frames());
    VkDeviceSize consumed_bytes = (VkDeviceSize)slice_frames * remaining.frame_stride_elements * element_size;

    slice.capacity_elements = slice_frames;
    slice.valid_elements = slice_frames;
    if (remaining.valid_elements > slice_frames && slice.state == lsx_vulkan_resident_final)
      slice.state = lsx_vulkan_resident_draining;
    if (lsx_rate_polyphase_vulkan_process_resident_input(
            polyphase->polyphase, &slice, &output,
            &output_frames, 0, slice.state, NULL) !=
        SOX_SUCCESS) {
      lsx_fail(
          "resident Vulkan DFT to polyphase slice failed: "
          "%lu pending frames, %lu-frame slice",
          (unsigned long)remaining.valid_elements,
          (unsigned long)slice_frames);
      return SOX_EINVAL;
    }
    if (output_frames)
      fifo_write(output_fifo, output_frames * channels, output);
    remaining.offset += consumed_bytes;
    remaining.capacity_elements -= slice_frames;
    remaining.valid_elements -= slice_frames;
  }
  return SOX_SUCCESS;
}

/* Advance the held input by one slice through the final stage, producing one
 * resident output block.
 *
 * The effect can only hand out one block at a time, so this does nothing
 * while an output is still waiting to be taken; the caller alternates between
 * taking the output and calling here again.  As above, a final block cut into
 * slices marks all but its last as draining. */
static int process_vulkan_pending_direct_input(sox_effect_t *effp, rate_vulkan_stage_executor_t *final)
{
  priv_t *p = (priv_t *)effp->priv;
  lsx_vulkan_resident_buffer_t input;
  size_t input_frames;
  size_t output_frames;
  VkDeviceSize consumed_bytes;

  if (!p->vulkan_pending_direct_input_valid || p->vulkan_pending_output_valid || !final->dft_polyphase)
    return SOX_EOF;
  input = p->vulkan_pending_direct_input;
  input_frames = min(input.valid_elements, lsx_rate_polyphase_vulkan_block_frames());
  input.valid_elements = input_frames;
  input.capacity_elements = input_frames;
  if (p->vulkan_pending_direct_input.valid_elements > input_frames && input.state == lsx_vulkan_resident_final)
    input.state = lsx_vulkan_resident_draining;
  if (lsx_rate_polyphase_vulkan_process_resident_input_normalized(
          final->polyphase, &input, NULL, &output_frames,
          final->output_rate, input.state, sox_true,
          &p->vulkan_pending_output) != SOX_SUCCESS) {
    lsx_fail(
        "resident Vulkan final direct polyphase slice failed: "
        "%lu pending frames, %lu-frame slice",
        (unsigned long)
            p->vulkan_pending_direct_input.valid_elements,
        (unsigned long)input_frames);
    return SOX_EOF;
  }
  p->vulkan_pending_output.frame_offset = p->rate.samples_out / effp->in_signal.channels;
  p->vulkan_pending_output_valid = sox_true;
  consumed_bytes =
      (VkDeviceSize)input_frames *
      p->vulkan_pending_direct_input.frame_stride_elements *
      lsx_vulkan_resident_element_size(
          p->vulkan_pending_direct_input.format);
  p->vulkan_pending_direct_input.offset += consumed_bytes;
  p->vulkan_pending_direct_input.capacity_elements -= input_frames;
  p->vulkan_pending_direct_input.valid_elements -= input_frames;
  if (!p->vulkan_pending_direct_input.valid_elements) {
    memset(&p->vulkan_pending_direct_input, 0, sizeof(p->vulkan_pending_direct_input));
    p->vulkan_pending_direct_input_valid = sox_false;
  }
  return SOX_SUCCESS;
}

/* Move a resident description past frames already consumed.  Nothing is
 * copied: the samples stay on the device and only the window into them
 * moves. */
static void advance_vulkan_resident_input(lsx_vulkan_resident_buffer_t *input, size_t frames)
{
  VkDeviceSize consumed_bytes =
      (VkDeviceSize)frames * input->frame_stride_elements *
      lsx_vulkan_resident_element_size(input->format);

  input->offset += consumed_bytes;
  input->capacity_elements -= frames;
  input->valid_elements -= frames;
}

/*
 * The final DFT stage reads a stream of bounded capacity, so a slice that
 * does not fit waits here instead of being dropped or forced.  The caller
 * takes the stage's output and comes back.
 */
static int append_vulkan_final_stream(
    priv_t *p, size_t final_index,
    lsx_vulkan_resident_buffer_t const *output,
    sox_bool *appended)
{
  rate_vulkan_stage_executor_t *final = &p->vulkan_stages[final_index];

  *appended = sox_false;
  if (output->valid_elements > lsx_rate_vulkan_resident_stream_room(final->dft))
    return SOX_SUCCESS;
  if (lsx_rate_vulkan_append_resident_stream(final->dft, output) != SOX_SUCCESS) {
    lsx_fail(
        "resident Vulkan polyphase slice to DFT stream failed: "
        "stage %lu, %lu frames, format %d, domain %d, state %d",
        (unsigned long)final_index,
        (unsigned long)output->valid_elements,
        (int)output->format, (int)output->domain,
        (int)output->state);
    return SOX_EOF;
  }
  *appended = sox_true;
  p->vulkan_final_stream_active = sox_true;
  if (++p->vulkan_polyphase_chain_pending == lsx_rate_vulkan_resident_batch_depth(final->dft)) {
    if (lsx_rate_vulkan_flush_resident(final->dft) != SOX_SUCCESS)
      return SOX_EOF;
    p->vulkan_polyphase_chain_pending = 0;
  }
  return SOX_SUCCESS;
}

/* Advance the resident chain by one step, doing whatever is furthest along
 * that can be done.
 *
 * The order below is deliberate and is what keeps the chain from deadlocking:
 * a deferred append is retried first, since everything behind it is blocked
 * on the final stream having room; then the final stage's own held input; and
 * only then the intermediate stages, scanned from the last backwards.
 * Working from the end forwards means a block is always moved out of the way
 * before another is produced to sit behind it.
 *
 * One step per call, not a loop to exhaustion: the caller has to be given the
 * chance to take the output between steps. */
static int process_vulkan_pending_polyphase_chain(sox_effect_t *effp, size_t final_index)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t index;

  if (p->vulkan_deferred_append_valid) {
    sox_bool appended;

    if (append_vulkan_final_stream(p, final_index, &p->vulkan_deferred_append, &appended) != SOX_SUCCESS)
      return SOX_EOF;
    if (!appended)
      return SOX_SUCCESS;
    p->vulkan_deferred_append_valid = sox_false;
  }
  if (p->vulkan_pending_direct_input_valid)
    return process_vulkan_pending_direct_input(effp, &p->vulkan_stages[final_index]);
  for (index = final_index; index-- > 0;) {
    if (p->vulkan_pending_chain_input_valid[index])
      break;
  }
  if (index >= final_index || !p->vulkan_pending_chain_input_valid[index])
    return SOX_EOF;
  for (;;) {
    rate_vulkan_stage_executor_t *polyphase = &p->vulkan_stages[index];
    lsx_vulkan_resident_buffer_t *pending = &p->vulkan_pending_chain_input[index];
    lsx_vulkan_resident_buffer_t slice = *pending;
    lsx_vulkan_resident_buffer_t output;
    size_t output_frames;
    size_t slice_frames = min(pending->valid_elements, lsx_rate_polyphase_vulkan_block_frames());

    /*
     * When the chain ends in a resident stream, the slice is also bounded
     * by the room left in it: a slice whose output does not fit could
     * never be appended, and the walker would spin on it.
     */
    if (!p->vulkan_stages[final_index].dft_polyphase && index + 1u == final_index) {
      stage_t const *stage = &p->rate.plan.stages[index];
      size_t room = lsx_rate_vulkan_resident_stream_room(p->vulkan_stages[final_index].dft);
      size_t room_frames = stage->L > 0 && stage->M > 0 ? room * (size_t)stage->M / (size_t)stage->L : room;

      if (!room_frames)
        return SOX_SUCCESS;
      slice_frames = min(slice_frames, room_frames);
    }

    slice.capacity_elements = slice_frames;
    slice.valid_elements = slice_frames;
    if (pending->valid_elements > slice_frames && slice.state == lsx_vulkan_resident_final)
      slice.state = lsx_vulkan_resident_draining;
    if (!slice_frames) {
      /*
       * A stage can hand on an empty buffer when it has nothing left to
       * give.  There is no work in it and the stream state travels with
       * the drain, so it is dropped here rather than filtered or appended.
       */
      memset(pending, 0, sizeof(*pending));
      p->vulkan_pending_chain_input_valid[index] = sox_false;
      return SOX_SUCCESS;
    }
    if (lsx_rate_polyphase_vulkan_process_resident_input(
            polyphase->polyphase, &slice, NULL,
            &output_frames, polyphase->output_rate,
            slice.state, &output) != SOX_SUCCESS) {
      lsx_fail(
          "resident Vulkan chained polyphase slice failed: "
          "stage %lu, %lu pending frames, %lu-frame slice",
          (unsigned long)(index + 1u),
          (unsigned long)pending->valid_elements,
          (unsigned long)slice_frames);
      return SOX_EOF;
    }
    advance_vulkan_resident_input(pending, slice_frames);
    if (!pending->valid_elements) {
      memset(pending, 0, sizeof(*pending));
      p->vulkan_pending_chain_input_valid[index] = sox_false;
    }
    if (++index == final_index) {
      if (!p->vulkan_stages[final_index].dft_polyphase) {
        sox_bool appended;

        if (append_vulkan_final_stream(p, final_index, &output, &appended) != SOX_SUCCESS)
          return SOX_EOF;
        if (!appended) {
          p->vulkan_deferred_append = output;
          p->vulkan_deferred_append_valid = sox_true;
          return SOX_SUCCESS;
        }
        for (index = final_index; index-- > 0;)
          if (p->vulkan_pending_chain_input_valid[index])
            break;
        if (index >= final_index || !p->vulkan_pending_chain_input_valid[index])
          return SOX_SUCCESS;
        continue;
      }
      p->vulkan_pending_direct_input = output;
      p->vulkan_pending_direct_input_valid = sox_true;
      return process_vulkan_pending_direct_input(effp, &p->vulkan_stages[final_index]);
    }
    if (p->vulkan_pending_chain_input_valid[index]) {
      lsx_fail("resident Vulkan polyphase chain state collision");
      return SOX_EOF;
    }
    p->vulkan_pending_chain_input[index] = output;
    p->vulkan_pending_chain_input_valid[index] = sox_true;
  }
}

static sox_bool vulkan_pending_polyphase_chain(priv_t const *p)
{
  size_t index;

  if (p->vulkan_deferred_append_valid)
    return sox_true;
  for (index = 0; index + 1u < p->vulkan_stage_count; ++index)
    if (p->vulkan_pending_chain_input_valid[index])
      return sox_true;
  return sox_false;
}

static size_t rate_vulkan_executor_input_frames(rate_vulkan_stage_executor_t const *executor)
{
  if (executor->kind == rate_stage_dsd_fir)
    return lsx_rate_dsd_vulkan_block_frames();
  if (rate_vulkan_executor_is_fft(executor))
    return lsx_rate_vulkan_input_frames(executor->dft);
  if (executor->kind == rate_stage_cubic)
    return lsx_rate_cubic_vulkan_block_frames();
  if (executor->dft_polyphase)
    return executor->polyphase_block_frames;
  return min((size_t)16384, lsx_rate_polyphase_vulkan_block_frames());
}

static sox_bool rate_vulkan_dft_fft_supported(stage_t const *stage)
{
  /* The executor streams a continuous L:1 zero-stuffed convolution, so it
   * carries no per-block offset and needs no relation between post_peak and L:
   * the whole alignment is the initial output skip that create_rate applies. */
  return stage->L >= 1 && stage->M >= 1 && lsx_fir_vulkan_block_frames() % stage->L == 0;
}

/* A pure interpolation can be run as a polyphase stage instead of a
 * transform.  This includes a power-of-two final stage: a preceding chain of
 * half-band stages does not make that final DFT stage disappear.  M must be
 * 1, a direct stage here doing no decimation. */
static sox_bool rate_vulkan_dft_direct_supported(stage_t const *stage)
{
  return stage->L > 1 && stage->M == 1;
}

/* Whether every stage of a plan has a Vulkan executor.  All or nothing: a
 * plan with one unsupported stage falls back to the CPU entirely, rather than
 * splitting the chain and paying a round trip in the middle.
 *
 * The polyphase conditions are what the device kernel implements and the CPU
 * stage does not require: no coefficient interpolation, no high-precision
 * clock, one phase per interpolation step, and a starting phase inside the
 * first cycle. */
static sox_bool rate_vulkan_plan_supported(rate_plan_t const *plan)
{
  int index;

  if (plan->num_stages < 1)
    return sox_false;
  for (index = 0; index < plan->num_stages; ++index) {
    stage_t const *stage = &plan->stages[index];

    if (stage->kind == rate_stage_dft) {
      if (stage->L < 1 || stage->M < 1 ||
          (!rate_vulkan_dft_fft_supported(stage) &&
           !rate_vulkan_dft_direct_supported(stage)))
        return sox_false;
    }
    else if (stage->kind == rate_stage_poly_fir) {
      if (stage->interp_order != 0 || stage->use_hi_prec_clock || stage->phase_count != stage->L || stage->M < 1 || stage->at.parts.integer < 0 || stage->at.parts.integer >= stage->L)
        return sox_false;
    }
    else if (stage->kind == rate_stage_dsd_fir) {
      /* The fused stage is the head of its plan and nothing else: it consumes
       * packed words, which no other stage produces. */
      if (index || stage->L != 1 || stage->M < 2 || stage->n < stage->M)
        return sox_false;
    }
    else if (stage->kind != rate_stage_half_fir && stage->kind != rate_stage_cubic)
      return sox_false;
  }
  return sox_true;
}

/* Expand a half-band stage's coefficients into a full response the FIR
 * backend can take.
 *
 * A half-band filter stores only its odd-indexed taps, every even one but the
 * centre being zero and the response being symmetric.  The backend has no
 * such special case, so the full response is reconstructed: the centre is
 * one half, and each stored coefficient is placed at an odd offset either
 * side of it. */
static double *rate_vulkan_half_taps(stage_t const *stage)
{
  size_t taps = (size_t)stage->pre_post + 1u;
  size_t coefficients = (size_t)stage->pre / 2u;
  double *result = lsx_calloc(taps, sizeof(*result));
  size_t index;

  result[stage->pre] = .5;
  for (index = 0; index < coefficients; ++index) {
    size_t offset = 2u * index + 1u;

    result[stage->pre - offset] = stage->static_coefs[index];
    result[stage->pre + offset] = stage->static_coefs[index];
  }
  return result;
}

/* Split a response into the phase_count sub-filters a polyphase stage needs.
 *
 * Phase p takes every phase_count'th tap starting at p, which is what
 * interleaving the sub-filters back together would undo.  Each sub-filter is
 * also reversed, the direct form correlating rather than convolving, and
 * scaled by phase_count, which is the interpolation gain the zero stuffing
 * would otherwise cost.  A response that does not divide evenly is
 * zero-padded, the buffer being allocated cleared. */
static double *rate_vulkan_dft_polyphase_taps(
    dft_filter_t const *filter, uint32_t phase_count,
    uint32_t *taps_per_phase)
{
  double *result;
  uint32_t phase;
  uint32_t tap;

  *taps_per_phase =
      ((uint32_t)filter->num_taps + phase_count - 1u) / phase_count;
  result = lsx_calloc((size_t)phase_count * *taps_per_phase, sizeof(*result));
  for (phase = 0; phase < phase_count; ++phase)
    for (tap = 0; tap < *taps_per_phase; ++tap) {
      uint32_t reverse_tap = *taps_per_phase - 1u - tap;
      uint64_t source = phase + (uint64_t)reverse_tap * phase_count;

      if (source < (uint32_t)filter->num_taps)
        result[(size_t)phase * *taps_per_phase + tap] = filter->taps[source] * phase_count;
    }
  return result;
}

/* Write the plan's responses where a qualification oracle can read them.
 *
 * sox-benchmark measures the fused DSD stage in exact rational arithmetic, and
 * for that it has to convolve the coefficients sox actually built -- not a
 * Python restatement of the design.  A reimplementation that drifts measures
 * itself and reports the difference as a backend error.
 *
 * Every stage of the plan is written, not just the fused one: a DSD plan always
 * keeps one half-band behind the fused filter, so the response the file shows
 * is the composition of the two.  The oracle needs each stage's decimation to
 * compose them, which is why M and the peak position travel with the taps.
 *
 * Off unless SOX_DSD_COEFFICIENTS names a file; %.17g so every value arrives as
 * the same double; and a failure to write is silent, because a qualification
 * hook that can change the outcome of a decode is worse than no hook. */
static void rate_dsd_dump_plan(priv_t const *p)
{
  char const *path = getenv("SOX_DSD_COEFFICIENTS");
  FILE *file;
  int index;

  if (!path || !*path || !(file = fopen(path, "w")))
    return;
  for (index = 0; index < (int)p->rate.plan.num_stages; ++index) {
    stage_t const *stage = &p->rate.plan.stages[index];
    double const *taps = NULL;
    double *owned = NULL;
    int count = 0, post_peak = 0, L = 1, M = 1;

    if (stage->kind == rate_stage_dsd_fir) {
      taps = stage->shared->dsd_taps;
      count = stage->shared->dsd_num_taps;
      post_peak = stage->shared->dsd_post_peak;
      M = stage->M;
    }
    else if (stage->kind == rate_stage_dft) {
      dft_filter_t const *filter =
          &stage->shared->dft_filter[stage->dft_filter_num];

      taps = filter->taps;
      count = filter->num_taps;
      post_peak = filter->post_peak;
      L = stage->L;
      M = stage->M;
    }
    else if (stage->kind == rate_stage_half_fir) {
      owned = rate_vulkan_half_taps(stage);
      taps = owned;
      count = stage->pre_post + 1;
      post_peak = stage->pre_post / 2;
      M = 2;
    }
    if (!taps)
      continue;
    fprintf(file, "# stage %i %s\n# taps %i\n# interpolation %i\n"
        "# decimation %i\n# post_peak %i\n",
        index, rate_stage_name(stage->kind), count, L, M, post_peak);
    {
      int tap;

      for (tap = 0; tap < count; ++tap)
        fprintf(file, "%.17g\n", taps[tap]);
    }
    free(owned);
  }
  fclose(file);
}

/* Build a Vulkan executor for each stage of the plan the CPU path already
 * produced, and the FIFOs between them.
 *
 * The plan is not re-derived: taking exactly the stages rate_plan_create
 * chose is what makes the two paths comparable, so any difference between
 * their outputs is down to the arithmetic rather than to a different chain of
 * filters.  Each stage's output rate is accumulated as the loop goes, since
 * a resident block has to state the rate it carries.
 *
 * A DFT stage's response is split into polyphase sub-filters when the direct
 * form is the better fit for its ratio; otherwise it goes to the partitioned
 * transform.  High interpolation factors use the direct form automatically:
 * this is the same policy used by the resident PCM-to-DSD path before rate and
 * SDM became explicit adjacent effects. */
static int rate_vulkan_start(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  lsx_vulkan_context_t *vulkan = lsx_vulkan_context_get(effp->global_info);
  size_t channels = effp->in_signal.channels;
  sox_rate_t stage_rate = effp->in_signal.rate;
  size_t created_fifos = 0;
  size_t created_dsd_fifos = 0;
  size_t channel;
  int index;

  if (!vulkan)
    return SOX_EOF;
  p->vulkan_stage_count = (size_t)p->rate.plan.num_stages;
  p->vulkan_stages = lsx_calloc(p->vulkan_stage_count, sizeof(*p->vulkan_stages));
  p->vulkan_fifos = lsx_calloc(p->vulkan_stage_count + 1u, sizeof(*p->vulkan_fifos));
  p->vulkan_pending_chain_input_valid = lsx_calloc(p->vulkan_stage_count, sizeof(*p->vulkan_pending_chain_input_valid));
  p->vulkan_pending_chain_input = lsx_calloc(p->vulkan_stage_count, sizeof(*p->vulkan_pending_chain_input));
  for (index = 0; index <= p->rate.plan.num_stages; ++index) {
    fifo_create(&p->vulkan_fifos[index], sizeof(double));
    ++created_fifos;
  }
  for (index = 0; index < p->rate.plan.num_stages; ++index) {
    stage_t const *stage = &p->rate.plan.stages[index];
    rate_vulkan_stage_executor_t *executor = &p->vulkan_stages[index];

    executor->kind = stage->kind;
    stage_rate *= stage->kind == rate_stage_half_fir ? .5 : stage->L > 0 && stage->M > 0 ? (double)stage->L / stage->M : stage->out_in_ratio;
    executor->output_rate = stage_rate;
    if (stage->kind == rate_stage_dsd_fir) {
      uint32_t preload_words;

      executor->dsd = lsx_rate_dsd_vulkan_create(
          vulkan, stage->shared->dsd_taps,
          (uint32_t)stage->shared->dsd_num_taps, (uint32_t)stage->M,
          (uint32_t)channels, (uint32_t)stage->shared->dsd_post_peak);
      if (!executor->dsd)
        goto error;
      /* The response's own delay, as words of silence in front of the
       * stream, so that output frame zero lines up with input frame zero;
       * the same preload every other stage gets, in the units this one
       * reads. */
      preload_words = lsx_rate_dsd_vulkan_preload_words(executor->dsd);
      p->vulkan_dsd_input = sox_true;
      p->vulkan_dsd_fifos = lsx_calloc(channels, sizeof(*p->vulkan_dsd_fifos));
      p->vulkan_dsd_runs = lsx_calloc(channels, sizeof(*p->vulkan_dsd_runs));
      for (channel = 0; channel < channels; ++channel) {
        fifo_create(&p->vulkan_dsd_fifos[channel], sizeof(uint32_t));
        memset(fifo_reserve(&p->vulkan_dsd_fifos[channel], preload_words), 0,
            preload_words * sizeof(uint32_t));
      }
      created_dsd_fifos = channels;
    }
    else if (stage->kind == rate_stage_dft) {
      dft_filter_t const *filter = &stage->shared->dft_filter[stage->dft_filter_num];
      sox_bool use_polyphase =
          rate_vulkan_dft_direct_supported(stage) &&
          (!rate_vulkan_dft_fft_supported(stage) || stage->L >= 4);

      if (use_polyphase) {
        uint32_t taps_per_phase;
        uint32_t phase_start;
        uint32_t preload_frames = (uint32_t)stage->preload;
        double *coefficients = rate_vulkan_dft_polyphase_taps(filter, (uint32_t)stage->L, &taps_per_phase);

        /* remL is the sub-L part of the peak that stage->preload cannot
         * hold; the phase origin has to carry it, for every L. */
        {
          int phase_origin = (filter->num_taps - 1) % stage->L - stage->remL;

          if (phase_origin < 0) {
            phase_origin += stage->L;
            ++preload_frames;
          }
          phase_start = (uint32_t)phase_origin;
        }
        executor->polyphase =
            lsx_rate_polyphase_vulkan_create(
                vulkan, coefficients, taps_per_phase,
                (uint32_t)stage->L, (uint32_t)stage->M,
                phase_start, (uint32_t)channels,
                preload_frames, sox_false);
        free(coefficients);
        if (!executor->polyphase)
          goto error;
        executor->dft_polyphase = sox_true;
        executor->polyphase_block_frames = min(
            lsx_rate_polyphase_vulkan_block_frames(),
            max((size_t)1,
                (index + 1 <
                    p->rate.plan.num_stages ?
                    (size_t)16384 :
                    (size_t)2097152 / channels) *
                (size_t)stage->M /
                (size_t)stage->L));
        memset(
            fifo_reserve(
            &p->vulkan_fifos[index],
                preload_frames * channels),
            0, preload_frames * channels *
            sizeof(double));
        lsx_report(
            "Vulkan rate DFT strategy: direct polyphase "
            "%d/%d, %u taps/phase",
            stage->L, stage->M, taps_per_phase);
      }
      else {
        executor->dft = lsx_rate_vulkan_create(
            vulkan, filter->taps,
            (size_t)filter->num_taps,
            (size_t)filter->post_peak,
            (uint32_t)stage->L,
            (uint32_t)stage->M,
            (uint32_t)channels);
        if (!executor->dft)
          goto error;
      }
    }
    else if (stage->kind == rate_stage_cubic) {
      executor->cubic = lsx_rate_cubic_vulkan_create(
          vulkan, (uint64_t)stage->step.all,
          (uint32_t)stage->pre_post, (uint32_t)channels);
      if (!executor->cubic)
        goto error;
      memset(
          fifo_reserve(
              &p->vulkan_fifos[index],
              stage->preload * channels),
          0, stage->preload * channels *
          sizeof(double));
    }
    else {
      double *half_taps = stage->kind == rate_stage_half_fir ? rate_vulkan_half_taps(stage) : NULL;
      double const *coefficients = half_taps ? half_taps : stage->shared->poly_fir_coefs;
      uint32_t taps = half_taps ? (uint32_t)stage->pre_post + 1u : (uint32_t)stage->n;
      uint32_t phase_count = half_taps ? 1u : (uint32_t)stage->phase_count;
      uint32_t phase_step = half_taps ? 2u : (uint32_t)stage->M;
      uint32_t phase_start = half_taps ? 0u : (uint32_t)stage->at.parts.integer;

      executor->polyphase = lsx_rate_polyphase_vulkan_create(vulkan, coefficients, taps, phase_count, phase_step, phase_start, (uint32_t)channels, (uint32_t)stage->preload, half_taps != NULL);
      free(half_taps);
      if (!executor->polyphase)
        goto error;
      memset(fifo_reserve(&p->vulkan_fifos[index], stage->preload * channels), 0, stage->preload * channels * sizeof(double));
    }
  }
  effp->flows = 1;
  rate_dsd_dump_plan(p);
  lsx_report("Vulkan rate plan: %lu stages, %lu channel%s", (unsigned long)p->vulkan_stage_count, (unsigned long)channels, channels == 1u ? "" : "s");
  return SOX_SUCCESS;

error:
  for (index = 0; index < (int)p->vulkan_stage_count; ++index) {
    lsx_rate_vulkan_destroy(p->vulkan_stages[index].dft);
    lsx_rate_cubic_vulkan_destroy(p->vulkan_stages[index].cubic);
    lsx_rate_dsd_vulkan_destroy(p->vulkan_stages[index].dsd);
    lsx_rate_polyphase_vulkan_destroy(p->vulkan_stages[index].polyphase);
  }
  while (created_fifos)
    fifo_delete(&p->vulkan_fifos[--created_fifos]);
  while (created_dsd_fifos)
    fifo_delete(&p->vulkan_dsd_fifos[--created_dsd_fifos]);
  free(p->vulkan_stages);
  free(p->vulkan_fifos);
  free(p->vulkan_dsd_fifos);
  free(p->vulkan_dsd_runs);
  free(p->vulkan_pending_chain_input_valid);
  free(p->vulkan_pending_chain_input);
  p->vulkan_stages = NULL;
  p->vulkan_fifos = NULL;
  p->vulkan_dsd_fifos = NULL;
  p->vulkan_dsd_runs = NULL;
  p->vulkan_dsd_input = sox_false;
  p->vulkan_pending_chain_input_valid = NULL;
  p->vulkan_pending_chain_input = NULL;
  p->vulkan_stage_count = 0;
  return SOX_EOF;
}
#endif

static int create(sox_effect_t * effp, int argc, char **argv)
{
  priv_t * p = (priv_t *) effp->priv;
  int c, quality;
  char * dummy_p, * found_at;
  char const * opts = "+i:c:b:B:A:p:Q:R:d:MILafnost" "qlmghevu";
  char const * qopts = strchr(opts, 'q');
  double rej = 0, bw_3dB_pc = 0;
  sox_bool allow_aliasing = sox_false;
  lsx_getopt_t optstate;
  lsx_getopt_init(argc, argv, opts, NULL, lsx_getopt_flag_none, 1, &optstate);

  p->coef_interp = quality = -1;
  p->rolloff = rolloff_small;
  p->phase = 50;
  p->max_coefs_size = 400;
  p->shared_ptr = &p->shared;

  while ((c = lsx_getopt(&optstate)) != -1) switch (c) {
    GETOPT_NUMERIC(optstate, 'i', coef_interp, -1, 2)
    GETOPT_NUMERIC(optstate, 'c', max_coefs_size, 100, INT_MAX)
    GETOPT_NUMERIC(optstate, 'p', phase, 0, 100)
    GETOPT_NUMERIC(optstate, 'B', bw_0dB_pc, 53, 99.5)
    GETOPT_NUMERIC(optstate, 'A', anti_aliasing_pc, 85, 100)
    GETOPT_NUMERIC(optstate, 'd', bit_depth, 15, 33)
    GETOPT_LOCAL_NUMERIC(optstate, 'b', bw_3dB_pc, 74, 99.7)
    GETOPT_LOCAL_NUMERIC(optstate, 'R', rej, 90, 200)
    GETOPT_LOCAL_NUMERIC(optstate, 'Q', quality, 0, 7)
    case 'M': p->phase =  0; break;
    case 'I': p->phase = 25; break;
    case 'L': p->phase = 50; break;
    case 'a': allow_aliasing = sox_true; break;
    case 'f': p->rolloff = rolloff_none; break;
    case 'n': p->noIOpt = sox_true; break;
    case 's': bw_3dB_pc = 99; break;
    case 't': p->use_hi_prec_clock = sox_true; break;
    default:
      if ((found_at = strchr(qopts, c)))
        quality = found_at - qopts;
      else {
        lsx_fail("unknown option `-%c'", optstate.opt);
        return lsx_usage(effp);
      }
  }
  argc -= optstate.ind, argv += optstate.ind;

  if ((unsigned)quality < 2 && (p->bw_0dB_pc || bw_3dB_pc || p->phase != 50 ||
        allow_aliasing || rej || p->bit_depth || p->anti_aliasing_pc)) {
    lsx_fail("override options not allowed with this quality level");
    return SOX_EOF;
  }
#if HAVE_VULKAN
  /* -R and -d are the only options that leave quality unset, and an unset
   * quality is what excludes the effect from the device.  Rather than let the
   * backend silently disappear on an option that says nothing about where the
   * work should run, the option is dropped and said to be dropped: the same
   * treatment the Vulkan modulator gives sdm's CPU-only options. */
  if (sox_globals.vulkan_profile != sox_vulkan_profile_none && quality < 0 &&
      (rej || p->bit_depth)) {
    lsx_warn("Vulkan rate uses preset quality levels; -R and -d are ignored");
    rej = p->bit_depth = 0;
  }
#endif
  if (quality < 0 && rej == 0 && p->bit_depth == 0)
    quality = 4;
  if (rej)
    p->bit_depth = rej / linear_to_dB(2.);
  else {
    if (quality >= 0) {
      p->bit_depth = quality? 16 + 4 * max(quality - 3, 0) : 0;
      if (quality <= 2)
        p->rolloff = rolloff_medium;
    }
    rej = p->bit_depth * linear_to_dB(2.);
  }
  p->vulkan_eligible = quality >= 0;

  if (bw_3dB_pc && p->bw_0dB_pc) {
    lsx_fail("conflicting bandwidth options");
    return SOX_EOF;
  }
  allow_aliasing |= p->anti_aliasing_pc != 0;
  if (!bw_3dB_pc && !p->bw_0dB_pc)
    p->bw_0dB_pc = quality == 1? LOW_Q_BW0_PC : 100 - 5 / TO_3dB(rej);
  else if (bw_3dB_pc && bw_3dB_pc < 85 && allow_aliasing) {
    lsx_fail("minimum allowed 3dB bandwidth with aliasing is %g%%", 85.);
    return SOX_EOF;
  }
  else if (p->bw_0dB_pc && p->bw_0dB_pc < 74 && allow_aliasing) {
    lsx_fail("minimum allowed bandwidth with aliasing is %g%%", 74.);
    return SOX_EOF;
  }
  if (bw_3dB_pc)
    p->bw_0dB_pc = 100 - (100 - bw_3dB_pc) / TO_3dB(rej);
  else {
    bw_3dB_pc = 100 - (100 - p->bw_0dB_pc) * TO_3dB(rej);
    p->given_0dB_pt = sox_true;
  }
  p->anti_aliasing_pc = p->anti_aliasing_pc? p->anti_aliasing_pc :
    allow_aliasing? bw_3dB_pc : 100;

  if (argc) {
    if ((p->out_rate = lsx_parse_frequency(*argv, &dummy_p)) <= 0 || *dummy_p)
      return lsx_usage(effp);
    argc--; argv++;
    effp->out_signal.rate = p->out_rate;
  }
  return argc? lsx_usage(effp) : SOX_SUCCESS;
}

/* What the device's accumulator can still resolve, in dB, per profile.  A
 * stop-band designed below this is not rejection, it is the arithmetic's own
 * noise, and every dB of it costs taps.
 *
 * The three figures are the measured SNR of each profile, as listed in the
 * README: a single float, a split-float pair, and a double-double.  Only the
 * first can ever bite (the deepest stop-band any quality level asks for is
 * about 175 dB) but the other two are stated at their real values rather
 * than at some round number below them, so that raising the quality range one
 * day cannot silently truncate a design the arithmetic could have carried. */
static double rate_dsd_arithmetic_ceiling(void)
{
  switch (sox_globals.vulkan_profile) {
    case sox_vulkan_profile_fast:      return 140.;
    case sox_vulkan_profile_precise:   return 300.;
    case sox_vulkan_profile_reference: return 625.;
    default:                           return 0.;
  }
}

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;
  double out_rate = p->out_rate != 0 ? p->out_rate : effp->out_signal.rate;
  double dsd_rate = 0;

  if (effp->in_signal.packing) {
    /* Packed input reaches an effect only because the reader was told to
     * produce it, and it is told that only for a chain that ends in this
     * stage on the device.  Anything else here is a chain that was built one
     * way and run another, which is worth saying rather than working around. */
    if (effp->in_signal.packing != SOX_DSD_PACKING_WORD ||
        sox_globals.vulkan_profile == sox_vulkan_profile_none ||
        !p->vulkan_eligible) {
      lsx_fail("packed DSD input requires the Vulkan rate backend");
      return SOX_EOF;
    }
    dsd_rate = effp->in_signal.rate;
  }

  if (effp->in_signal.rate == out_rate)
    return SOX_EFF_NULL;

  if (effp->in_signal.mult)
    *effp->in_signal.mult *= .705; /* 1/(2/sinc(pi/3)-1); see De Soras 4.1.2 */

  effp->out_signal.channels = effp->in_signal.channels;
  effp->out_signal.rate = out_rate;
  effp->out_signal.packing = 0;      /* Whatever came in, PCM goes out. */
  rate_plan_create(&p->rate.plan, p->shared_ptr,
      effp->in_signal.rate / out_rate, p->bit_depth,
      p->phase, p->bw_0dB_pc, p->anti_aliasing_pc, p->rolloff,
      !p->given_0dB_pt, p->use_hi_prec_clock,
      p->coef_interp, p->max_coefs_size, p->noIOpt,
      dsd_rate, rate_dsd_arithmetic_ceiling());

  if (!p->rate.plan.num_stages) {
    lsx_warn("input and output rates too close, skipping resampling");
    return SOX_EFF_NULL;
  }
  if (dsd_rate > 0 && p->rate.plan.stages[0].kind != rate_stage_dsd_fir) {
    /* Only a plan that halves at all has a cascade to fuse, and only a fused
     * plan can read words.  An output rate above half the DSD rate leaves
     * nothing to collapse. */
    lsx_fail("packed DSD input needs an output rate below half %g Hz", dsd_rate);
    rate_plan_destroy(&p->rate.plan);
    return SOX_EOF;
  }
  {
    int stage_index;

    for (stage_index = 0; stage_index < p->rate.plan.num_stages; ++stage_index) {
      stage_t const *stage = &p->rate.plan.stages[stage_index];
      lsx_debug("rate plan stage %d/%d: %s L=%d M=%d phases=%d interpolation=%d taps=%d preload=%d remL=%d", stage_index + 1, p->rate.plan.num_stages, rate_stage_name(stage->kind), stage->L, stage->M, stage->phase_count, stage->interp_order, stage->n, stage->preload, stage->remL);
    }
    /* The CPU plan is written out for the same reason the Vulkan one is: an
     * oracle that scores this path has to convolve the cascade sox built, and
     * the cascade is where the two paths differ.  The Vulkan branch below
     * overwrites it with its own, so a Vulkan run still describes what ran. */
    rate_dsd_dump_plan(p);
    /* The plan, not the ratio: it is the plan that decides which code runs,
     * and whether the Vulkan backend will take the effect at all. */
    if (lsx_diagnostics_on) {
      lsx_diagnostics_effect_setf(effp, "stages", "%d", p->rate.plan.num_stages);
      lsx_diagnostics_effect_setf(effp, "rejection_db", "%.2f", p->bit_depth * linear_to_dB(2.));
      /* False when a rejection target was given instead of a preset, which is
       * the whole of what excludes the Vulkan backend at rate.c's own gate. */
      lsx_diagnostics_effect_setf(effp, "preset_eligible", "%d", p->vulkan_eligible ? 1 : 0);
      for (stage_index = 0; stage_index < p->rate.plan.num_stages; ++stage_index) {
        stage_t const *stage = &p->rate.plan.stages[stage_index];
        char leaf[32];

        sprintf(leaf, "stage.%d", stage_index);
        lsx_diagnostics_effect_setf(effp, leaf,
            "%s L=%d M=%d phases=%d interpolation=%d taps=%d",
            rate_stage_name(stage->kind), stage->L, stage->M,
            stage->phase_count, stage->interp_order, stage->n);
      }
    }
  }

#if HAVE_VULKAN
  if (sox_globals.vulkan_profile != sox_vulkan_profile_none) {
    if (!p->vulkan_eligible || !rate_vulkan_plan_supported(&p->rate.plan)) {
      lsx_fail("rate plan is not supported by the Vulkan backend");
      rate_plan_destroy(&p->rate.plan);
      return SOX_EOF;
    }
    if (rate_vulkan_start(effp) != SOX_SUCCESS) {
      rate_plan_destroy(&p->rate.plan);
      return SOX_EOF;
    }
    if (lsx_diagnostics_on)
      lsx_diagnostics_effect_setf(effp, "backend", "vulkan");
    if (lsx_rate_effect_resident_transform_supported(effp))
      effp->internal_chain_endpoint = &vulkan_resident_transform_endpoint;
    else if (lsx_rate_effect_resident_supported(effp))
      effp->internal_chain_endpoint = &vulkan_resident_producer_endpoint;
    /*
     * A downstream resident consumer reads whole batches of this effect's
     * output, so the batch depth follows this effect's own topology.
     */
    if (effp->internal_chain_endpoint &&
        lsx_vulkan_configure_resident_batch_depth(
        lsx_vulkan_context_get(effp->global_info),
        effp->in_signal.rate, effp->out_signal.rate,
        effp->in_signal.channels, effp->in_signal.length,
        lsx_rate_effect_resident_topology(effp)) != SOX_SUCCESS)
      return SOX_EOF;
    return SOX_SUCCESS;
  }
#endif
  rate_cpu_start(&p->rate);
  if (lsx_diagnostics_on) {
    lsx_diagnostics_effect_setf(effp, "backend", "cpu");
    lsx_diagnostics_effect_setf(effp, "precision", "FP64");
  }
  return SOX_SUCCESS;
}

#if HAVE_VULKAN
/* Take one buffer of packed words into the per-channel queues.
 *
 * The reader hands over channel-major runs sized to the buffer it filled, so
 * a buffer has to be taken whole or not at all: consuming part of one would
 * leave the rest with a stride the next call has no way to know.  Every path
 * that calls this consumes all of what it was given, which is what makes the
 * layout safe to rely on. */
static void write_vulkan_dsd_input(sox_effect_t *effp, sox_sample_t const *ibuf, size_t samples)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t groups = samples / channels;
  size_t channel;

  for (channel = 0; channel < channels; ++channel) {
    uint32_t *target = fifo_write(&p->vulkan_dsd_fifos[channel], (int)groups, NULL);
    size_t index;

    for (index = 0; index < groups; ++index)
      target[index] = (uint32_t)ibuf[channel * groups + index];
  }
  p->rate.samples_in += groups * 32u * channels;
}

/* Run the fused DSD stage over everything the word queues hold.
 *
 * One call per block until the stage says it has no complete window left; the
 * words it did not consume stay queued, since the response reads far behind
 * each output it produces.  While flushing there is no such condition
 * so exactly one block is produced per call and the caller decides when it has enough. */
static int process_vulkan_dsd_stage(
    sox_effect_t *effp, rate_vulkan_stage_executor_t *executor,
    fifo_t *output_fifo)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t window = lsx_rate_dsd_vulkan_window_words(executor->dsd);

  for (;;) {
    size_t available = min((size_t)fifo_occupancy(&p->vulkan_dsd_fifos[0]), window);
    double const *output;
    size_t output_frames;
    size_t consumed;
    size_t channel;

    for (channel = 0; channel < channels; ++channel)
      p->vulkan_dsd_runs[channel] = fifo_read_ptr(&p->vulkan_dsd_fifos[channel]);
    if (lsx_rate_dsd_vulkan_process(
            executor->dsd, p->vulkan_dsd_runs, available,
            p->vulkan_dsd_flushing, &output, &output_frames,
            &consumed) != SOX_SUCCESS)
      return SOX_EOF;
    if (output_frames)
      fifo_write(output_fifo, output_frames * channels, output);
    for (channel = 0; channel < channels; ++channel)
      fifo_read(&p->vulkan_dsd_fifos[channel], (int)consumed, NULL);
    if (!output_frames || p->vulkan_dsd_flushing)
      return SOX_SUCCESS;
  }
}

/* Push whatever the FIFOs hold through the plan's stages, in order.
 *
 * Held work comes first, for the same reason as in the chain walker: a block
 * that is already part-way through must be moved on before more is produced
 * behind it.  Only then are the stages run, each consuming from the FIFO
 * before it and writing to the one after.
 *
 * resident_chain says whether this effect is publishing resident output.  It
 * changes what happens at the boundary between a transform stage and what
 * follows: with it, a stage's output stays on the device and is handed
 * onwards as a resident block; without it, everything lands in the host FIFOs
 * and the stages are simply run one after another.
 *
 * A transform stage takes exact blocks, so it runs only while its FIFO holds
 * a whole one; the other kinds take what they are given up to their own
 * maximum.  That difference is why the loop body is not uniform across the
 * stage kinds. */
static int process_vulkan_stages(sox_effect_t *effp, size_t stage_count, sox_bool resident_chain)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t index;

  if (resident_chain && p->vulkan_pending_direct_input_valid)
    return process_vulkan_pending_direct_input(effp, &p->vulkan_stages[p->vulkan_stage_count - 1u]);
  if (resident_chain && vulkan_pending_polyphase_chain(p))
    return process_vulkan_pending_polyphase_chain(effp, p->vulkan_stage_count - 1u);
  for (index = 0; index < stage_count; ++index) {
    rate_vulkan_stage_executor_t *executor = &p->vulkan_stages[index];
    fifo_t *input_fifo = &p->vulkan_fifos[index];
    fifo_t *output_fifo = &p->vulkan_fifos[index + 1u];

    if (executor->kind == rate_stage_dsd_fir) {
      if (process_vulkan_dsd_stage(effp, executor, output_fifo) != SOX_SUCCESS)
        return SOX_EOF;
    }
    else if (rate_vulkan_executor_is_fft(executor)) {
      size_t block_samples = lsx_rate_vulkan_input_frames(executor->dft) * channels;
      sox_bool resident_pair = index + 1u < stage_count && !rate_vulkan_executor_is_fft(&p->vulkan_stages[index + 1u]);
      size_t final_stream_index = index + 1u;
      sox_bool final_stream_chain;

      while (final_stream_index < p->vulkan_stage_count && p->vulkan_stages[final_stream_index].kind != rate_stage_dft)
        ++final_stream_index;
      final_stream_chain =
          resident_chain &&
          final_stream_index == p->vulkan_stage_count - 1u &&
          (final_stream_index > index + 1u ||
          p->vulkan_stages[
              final_stream_index].dft_polyphase);

      while ((size_t)fifo_occupancy(input_fifo) >= block_samples) {
        double const *input = fifo_read(input_fifo, block_samples, NULL);
        double const *output;
        size_t output_frames;

        if (final_stream_chain) {
          lsx_vulkan_resident_buffer_t resident;

          if (lsx_rate_vulkan_process_resident(executor->dft, input, executor->output_rate, 0, lsx_vulkan_resident_ready, sox_false, &resident) != SOX_SUCCESS) {
            lsx_fail("resident Vulkan chained DFT stage failed");
            return SOX_EOF;
          }
          if (index + 1u < final_stream_index) {
            /*
             * Every polyphase stage in the chain has a block limit, so the
             * DFT output travels through the sliced walker whatever the
             * final stage consumes.
             */
            p->vulkan_pending_chain_input[index + 1u] = resident;
            p->vulkan_pending_chain_input_valid[index + 1u] = sox_true;
            return process_vulkan_pending_polyphase_chain(effp, final_stream_index);
          }
          if (p->vulkan_stages[final_stream_index].dft_polyphase) {
            p->vulkan_pending_direct_input = resident;
            p->vulkan_pending_direct_input_valid = sox_true;
            if (process_vulkan_pending_direct_input(effp, &p->vulkan_stages[final_stream_index]) != SOX_SUCCESS)
              return SOX_EOF;
            return SOX_SUCCESS;
          }
          if (lsx_rate_vulkan_append_resident_stream(p->vulkan_stages[final_stream_index].dft, &resident) != SOX_SUCCESS) {
            lsx_fail("resident Vulkan polyphase chain to DFT stream failed");
            return SOX_EOF;
          }
          p->vulkan_final_stream_active = sox_true;
          if (++p->vulkan_polyphase_chain_pending == lsx_rate_vulkan_resident_batch_depth(p->vulkan_stages[final_stream_index].dft)) {
            if (lsx_rate_vulkan_flush_resident(p->vulkan_stages[final_stream_index].dft) != SOX_SUCCESS)
              return SOX_EOF;
            p->vulkan_polyphase_chain_pending = 0;
          }
          return SOX_SUCCESS;
        }
        if (resident_pair) {
          rate_vulkan_stage_executor_t *polyphase = &p->vulkan_stages[index + 1u];
          lsx_vulkan_resident_buffer_t resident;

          if (lsx_rate_vulkan_process_resident(executor->dft, input, executor->output_rate, 0, lsx_vulkan_resident_ready, sox_false, &resident) != SOX_SUCCESS) {
            lsx_fail("resident Vulkan DFT stage failed");
            return SOX_EOF;
          }
          if (process_vulkan_resident_polyphase_to_host(
                  polyphase, &resident,
                  &p->vulkan_fifos[index + 2u],
                  channels) != SOX_SUCCESS)
            return SOX_EINVAL;
          continue;
        }
        if (lsx_rate_vulkan_process(executor->dft, input, &output, &output_frames) != SOX_SUCCESS)
          return SOX_EOF;
        if (output_frames)
          fifo_write(output_fifo, output_frames * channels, output);
      }
      if (resident_pair)
        ++index;
    }
    else if (executor->kind == rate_stage_cubic) {
      size_t pre_post = lsx_rate_cubic_vulkan_pre_post(executor->cubic);
      size_t occupancy_frames = (size_t)fifo_occupancy(input_fifo) / channels;

      while (occupancy_frames > pre_post) {
        double const *input = fifo_read_ptr(input_fifo);
        double const *output;
        size_t output_frames;
        size_t consumed_frames;

        if (lsx_rate_cubic_vulkan_process(
                executor->cubic, input, occupancy_frames,
                &output, &output_frames,
                &consumed_frames) != SOX_SUCCESS)
          return SOX_EOF;
        fifo_read(input_fifo, consumed_frames * channels, NULL);
        if (output_frames)
          fifo_write(output_fifo, output_frames * channels, output);
        occupancy_frames = (size_t)fifo_occupancy(input_fifo) / channels;
      }
    }
    else {
      size_t taps = lsx_rate_polyphase_vulkan_taps(executor->polyphase);
      size_t block_frames = rate_vulkan_executor_input_frames(executor);
      size_t occupancy_frames = (size_t)fifo_occupancy(input_fifo) / channels;
      size_t final_stream_index = index + 1u;
      sox_bool final_stream_chain;

      while (final_stream_index < p->vulkan_stage_count && p->vulkan_stages[final_stream_index].kind != rate_stage_dft)
        ++final_stream_index;
      final_stream_chain = resident_chain &&
          final_stream_index == p->vulkan_stage_count - 1u;

      while (occupancy_frames > taps - 1u) {
        size_t processable_frames = min(block_frames, occupancy_frames - (taps - 1u));
        double const *input = fifo_read_ptr(input_fifo);
        double const *output;
        size_t output_frames;
        size_t consumed_frames;

        if (final_stream_chain) {
          lsx_vulkan_resident_buffer_t resident;

          if (lsx_rate_polyphase_vulkan_process_resident(executor->polyphase, input, processable_frames, &output_frames, &consumed_frames, executor->output_rate, lsx_vulkan_resident_ready, &resident) != SOX_SUCCESS)
            return SOX_EOF;
          fifo_read(input_fifo, consumed_frames * channels, NULL);
          if (index + 1u < final_stream_index) {
            p->vulkan_pending_chain_input[index + 1u] = resident;
            p->vulkan_pending_chain_input_valid[index + 1u] = sox_true;
            return process_vulkan_pending_polyphase_chain(effp, final_stream_index);
          }
          if (p->vulkan_stages[final_stream_index].dft_polyphase) {
            p->vulkan_pending_direct_input = resident;
            p->vulkan_pending_direct_input_valid = sox_true;
            return process_vulkan_pending_direct_input(effp, &p->vulkan_stages[final_stream_index]);
          }
          if (lsx_rate_vulkan_append_resident_stream(p->vulkan_stages[final_stream_index].dft, &resident) != SOX_SUCCESS)
            return SOX_EOF;
          p->vulkan_final_stream_active = sox_true;
          if (++p->vulkan_polyphase_chain_pending == lsx_rate_vulkan_resident_batch_depth(p->vulkan_stages[final_stream_index].dft)) {
            if (lsx_rate_vulkan_flush_resident(p->vulkan_stages[final_stream_index].dft) != SOX_SUCCESS)
              return SOX_EOF;
            p->vulkan_polyphase_chain_pending = 0;
          }
          return SOX_SUCCESS;
        }
        if (lsx_rate_polyphase_vulkan_process(executor->polyphase, input, processable_frames, &output, &output_frames, &consumed_frames) != SOX_SUCCESS)
          return SOX_EOF;
        fifo_read(input_fifo, consumed_frames * channels, NULL);
        if (output_frames)
          fifo_write(output_fifo, output_frames * channels, output);
        occupancy_frames = (size_t)fifo_occupancy(input_fifo) / channels;
      }
    }
  }
  return SOX_SUCCESS;
}

static int flow_vulkan(sox_effect_t *effp, sox_sample_t const *ibuf, sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  fifo_t *input_fifo = &p->vulkan_fifos[0];
  fifo_t *output_fifo = &p->vulkan_fifos[p->vulkan_stage_count];
  size_t odone = min(*osamp, (size_t)fifo_occupancy(output_fifo));
  double const *output;

  odone -= odone % channels;
  output = fifo_read(output_fifo, odone, NULL);
  if (odone) {
    lsx_save_samples(obuf, output, odone, &effp->clips);
    p->rate.samples_out += odone;
  }
  if (*isamp && odone < *osamp) {
    size_t idone = *isamp - *isamp % channels;
    int status;

    if (p->vulkan_dsd_input)
      write_vulkan_dsd_input(effp, ibuf, idone);
    else {
      double *input = fifo_write(input_fifo, idone, NULL);

      lsx_load_samples(input, ibuf, idone);
      p->rate.samples_in += idone;
    }
    *isamp = idone;
    status = process_vulkan_stages(effp, p->vulkan_stage_count, sox_false);
    if (status != SOX_SUCCESS) {
      *osamp = odone;
      return SOX_EINVAL;
    }
  }
  else
    *isamp = 0;
  *osamp = odone;
  return SOX_SUCCESS;
}

/* Produce one resident output block from the last stage, if it can.
 *
 * A block already held from an earlier call is handed out first.  Otherwise
 * the last stage is asked to make one, in whichever of its two forms it took;
 * *produced false with success means it has not enough input yet, which is
 * the normal answer rather than a failure. */
static int take_vulkan_resident_output(sox_effect_t *effp, lsx_vulkan_resident_state_t state, sox_bool normalize, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t last_index;
  rate_vulkan_stage_executor_t *last;
  fifo_t *input_fifo;
  size_t block_samples;
  double const *input;

  *produced = sox_false;
  if (!p->vulkan_stage_count)
    return SOX_EOF;
  if (p->vulkan_pending_output_valid) {
    *resident = p->vulkan_pending_output;
    memset(&p->vulkan_pending_output, 0, sizeof(p->vulkan_pending_output));
    p->vulkan_pending_output_valid = sox_false;
    *produced = sox_true;
    return SOX_SUCCESS;
  }
  last_index = p->vulkan_stage_count - 1u;
  last = &p->vulkan_stages[last_index];
  if (last->kind != rate_stage_dft) {
    lsx_fail("resident Vulkan rate output requires a final DFT stage");
    return SOX_EOF;
  }
  if (last->dft_polyphase) {
    size_t taps = lsx_rate_polyphase_vulkan_taps(last->polyphase);
    size_t occupancy_frames = (size_t)fifo_occupancy(&p->vulkan_fifos[last_index]) / channels;
    size_t processable_frames;
    size_t consumed_frames;
    size_t output_frames;

    if (occupancy_frames <= taps - 1u)
      return SOX_SUCCESS;
    processable_frames = min(last->polyphase_block_frames, occupancy_frames - (taps - 1u));
    input = fifo_read_ptr(&p->vulkan_fifos[last_index]);
    if (lsx_rate_polyphase_vulkan_process_resident_normalized(
        last->polyphase, input, processable_frames,
        &output_frames, &consumed_frames,
        effp->out_signal.rate, state, normalize,
        resident) != SOX_SUCCESS)
      return SOX_EOF;
    fifo_read(&p->vulkan_fifos[last_index], consumed_frames * channels, NULL);
    resident->frame_offset = p->rate.samples_out / channels;
    *produced = sox_true;
    return SOX_SUCCESS;
  }
  if (p->vulkan_final_stream_active)
    return lsx_rate_vulkan_process_resident_stream(last->dft, effp->out_signal.rate, p->rate.samples_out / channels, state, normalize, resident, produced);
  input_fifo = &p->vulkan_fifos[last_index];
  block_samples = lsx_rate_vulkan_input_frames(last->dft) * channels;
  if ((size_t)fifo_occupancy(input_fifo) < block_samples)
    return SOX_SUCCESS;
  input = fifo_read(input_fifo, block_samples, NULL);
  if (lsx_rate_vulkan_process_resident(last->dft, input, effp->out_signal.rate, p->rate.samples_out / channels, state, normalize, resident) != SOX_SUCCESS)
    return SOX_EOF;
  *produced = sox_true;
  return SOX_SUCCESS;
}

/* Resident output requires the plan to end in a DFT stage: that is the only
 * kind whose output stays on the device in a form a consumer can be given. */
static sox_bool lsx_rate_effect_resident_supported(sox_effect_t const *effp)
{
  priv_t const *p;

  if (!effp)
    return sox_false;
  p = (priv_t const *)effp->priv;
  return p->vulkan_stage_count && p->vulkan_stages[p->vulkan_stage_count - 1u].kind == rate_stage_dft;
}

static lsx_vulkan_resident_topology_t lsx_rate_effect_resident_topology(sox_effect_t const *effp)
{
  priv_t const *p;
  size_t index;

  if (!effp)
    return lsx_vulkan_resident_topology_chained;
  p = (priv_t const *)effp->priv;
  for (index = 0; index < p->vulkan_stage_count; ++index)
    if (!rate_vulkan_executor_is_fft(&p->vulkan_stages[index]))
      return lsx_vulkan_resident_topology_chained;
  return lsx_vulkan_resident_topology_dft_only;
}

/* Resident input requires a plan the chain machinery can actually drive.
 *
 * The first stage must be a transform, since that is what accepts a resident
 * block; and any stage after the first must not be, because two transform
 * stages in the middle of a chain would each want exact blocks of a different
 * size, which nothing here reconciles.  A single transform stage is therefore
 * fine, as is a transform followed by polyphase stages ending in either kind
 * -- and nothing else. */
static sox_bool lsx_rate_effect_resident_input_supported(sox_effect_t const *effp)
{
  priv_t const *p;
  rate_vulkan_stage_executor_t const *last;
  size_t index;

  if (!effp)
    return sox_false;
  p = (priv_t const *)effp->priv;
  if (!p->vulkan_stage_count || !rate_vulkan_executor_is_fft(&p->vulkan_stages[0]))
    return sox_false;
  if (p->vulkan_stage_count == 1u)
    return sox_true;
  if (p->vulkan_stage_count < 3u)
    return sox_false;
  last = &p->vulkan_stages[p->vulkan_stage_count - 1u];
  if (!rate_vulkan_executor_is_fft(last) && !last->dft_polyphase)
    return sox_false;
  for (index = 1u; index + 1u < p->vulkan_stage_count; ++index) {
    if (rate_vulkan_executor_is_fft(&p->vulkan_stages[index]))
      return sox_false;
  }
  return sox_true;
}

/* Acting as a middle link needs both ends: resident input, and a final
 * transform stage so the output is resident too. */
static sox_bool lsx_rate_effect_resident_transform_supported(sox_effect_t const *effp)
{
  priv_t const *p;

  if (!lsx_rate_effect_resident_input_supported(effp))
    return sox_false;
  p = (priv_t const *)effp->priv;
  return rate_vulkan_executor_is_fft(&p->vulkan_stages[p->vulkan_stage_count - 1u]);
}

static sox_bool lsx_rate_effect_resident_input_ready(sox_effect_t const *effp)
{
  priv_t const *p;

  if (!lsx_rate_effect_resident_input_supported(effp))
    return sox_false;
  p = (priv_t const *)effp->priv;
  return p->vulkan_pending_output_valid ||
      p->vulkan_pending_direct_input_valid ||
      vulkan_pending_polyphase_chain(p) ||
      lsx_rate_vulkan_resident_stream_ready(
          p->vulkan_stages[0].dft) ||
      (rate_vulkan_executor_is_fft(
          &p->vulkan_stages[
              p->vulkan_stage_count - 1u]) &&
       lsx_rate_vulkan_resident_stream_ready(
          p->vulkan_stages[
              p->vulkan_stage_count - 1u].dft));
}

static uint64_t lsx_rate_effect_external_input_clips(sox_effect_t *effp)
{
  priv_t *p;

  if (!lsx_rate_effect_resident_input_supported(effp))
    return 0;
  p = (priv_t *)effp->priv;
  return lsx_rate_vulkan_resident_stream_clips(p->vulkan_stages[0].dft);
}

/* Move a resident-input plan forward by one step: take a block out of the
 * first stage's stream, run it through the intermediate stages, and hand it
 * to the last.
 *
 * *advanced says whether anything happened, so a caller can tell "nothing to
 * do yet" from "made progress"; held work is dealt with first, as everywhere
 * in this file, before a new block is produced behind it. */
static int process_vulkan_resident_chain(sox_effect_t *effp, lsx_vulkan_resident_state_t state, sox_bool *advanced)
{
  priv_t *p = (priv_t *)effp->priv;
  rate_vulkan_stage_executor_t *first = &p->vulkan_stages[0];
  lsx_vulkan_resident_buffer_t current;
  sox_bool first_produced;

  *advanced = sox_false;
  /*
   * A single DFT stage is both the external-input stream and the final
   * output stream.  take_vulkan_resident_output() drains it directly; do
   * not feed its output back into itself as the multi-stage bridge does.
   */
  if (p->vulkan_stage_count == 1u)
    return SOX_SUCCESS;
  if (p->vulkan_pending_direct_input_valid) {
    if (process_vulkan_pending_direct_input(effp, &p->vulkan_stages[p->vulkan_stage_count - 1u]) != SOX_SUCCESS)
      return SOX_EOF;
    *advanced = sox_true;
    return SOX_SUCCESS;
  }
  if (vulkan_pending_polyphase_chain(p)) {
    if (process_vulkan_pending_polyphase_chain(effp, p->vulkan_stage_count - 1u) != SOX_SUCCESS)
      return SOX_EOF;
    *advanced = sox_true;
    return SOX_SUCCESS;
  }
  if (lsx_rate_vulkan_process_resident_stream(first->dft, first->output_rate, 0, state, sox_false, &current, &first_produced) != SOX_SUCCESS)
    return SOX_EOF;
  if (!first_produced)
    return SOX_SUCCESS;
  p->vulkan_pending_chain_input[1u] = current;
  p->vulkan_pending_chain_input_valid[1u] = sox_true;
  if (process_vulkan_pending_polyphase_chain(
          effp, p->vulkan_stage_count - 1u) != SOX_SUCCESS)
    return SOX_EOF;
  *advanced = sox_true;
  return SOX_SUCCESS;
}

/* Drive the effect as a link of someone else's resident chain: take a
 * resident block and publish one.
 *
 * The sequence is fixed and the same throughout this file -- hand out what is
 * ready, advance the chain, hand out again, and only then look at the new
 * input.  It means an input is never accepted while output is waiting, which
 * is what keeps the plan's held blocks from accumulating.
 *
 * Whether the output is normalized is settled by the first call and may not
 * change afterwards: the stages were built for one or the other, and
 * switching would put two scalings into one stream. */
static int flow_vulkan_resident_input_mode(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool normalize_output, sox_bool *input_consumed, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  priv_t *p;
  size_t channels;
  sox_bool advanced;

  if (!effp || !input_consumed || !resident || !produced || !lsx_rate_effect_resident_input_supported(effp))
    return SOX_EINVAL;
  p = (priv_t *)effp->priv;
  if (p->vulkan_external_input_active && p->vulkan_external_output_normalize != normalize_output)
    return SOX_EINVAL;
  if (!p->vulkan_external_input_active)
    p->vulkan_external_output_normalize = normalize_output;
  channels = effp->in_signal.channels;
  memset(resident, 0, sizeof(*resident));
  *input_consumed = sox_false;
  *produced = sox_false;
  if (take_vulkan_resident_output(effp, lsx_vulkan_resident_ready, normalize_output, resident, produced) != SOX_SUCCESS)
    return SOX_EINVAL;
  if (*produced)
    goto output_ready;
  if (process_vulkan_resident_chain(effp, lsx_vulkan_resident_ready, &advanced) != SOX_SUCCESS || take_vulkan_resident_output(effp, lsx_vulkan_resident_ready, normalize_output, resident, produced) != SOX_SUCCESS)
    return SOX_EINVAL;
  if (*produced)
    goto output_ready;
  if (advanced &&
      rate_vulkan_executor_is_fft(
          &p->vulkan_stages[
              p->vulkan_stage_count - 1u]) &&
      lsx_rate_vulkan_flush_resident(
          p->vulkan_stages[
              p->vulkan_stage_count - 1u].dft) !=
          SOX_SUCCESS)
    return SOX_EINVAL;
  if (!input)
    return SOX_SUCCESS;
  if (!input->valid_elements) {
    if (lsx_vulkan_resident_buffer_validate(input) !=
        SOX_SUCCESS ||
        input->state != lsx_vulkan_resident_final ||
        input->format != lsx_rate_vulkan_resident_format(
            p->vulkan_stages[0].dft) ||
        input->domain !=
            lsx_vulkan_resident_domain_sox_sample ||
        input->frames_per_element != 1u ||
        input->rate != effp->in_signal.rate ||
        input->channels != channels)
      return SOX_EINVAL;
    p->vulkan_external_input_active = sox_true;
    *input_consumed = sox_true;
    return SOX_SUCCESS;
  }
  if (input->rate != effp->in_signal.rate || input->channels != channels) {
    lsx_fail(
        "resident Vulkan rate transform input mismatch: "
        "%.0f/%.0f Hz, %u/%lu channels",
        input->rate, effp->in_signal.rate,
        input->channels, (unsigned long)channels);
    return SOX_EINVAL;
  }
  if (lsx_rate_vulkan_append_resident_stream_quantized(
          p->vulkan_stages[0].dft, input) != SOX_SUCCESS) {
    lsx_fail(
        "resident Vulkan rate transform input append failed: "
        "%lu frames, %lu frames available",
        (unsigned long)input->valid_elements,
        (unsigned long)lsx_rate_vulkan_resident_stream_room(
            p->vulkan_stages[0].dft));
    return SOX_EINVAL;
  }
  if (p->vulkan_stage_count == 1u)
    p->vulkan_final_stream_active = sox_true;
  ++p->vulkan_external_input_pending;
  p->rate.samples_in += input->valid_elements * channels;
  p->vulkan_external_input_active = sox_true;
  *input_consumed = sox_true;
  if (process_vulkan_resident_chain(effp, input->state, &advanced) != SOX_SUCCESS || take_vulkan_resident_output(effp, input->state, normalize_output, resident, produced) != SOX_SUCCESS)
    return SOX_EINVAL;
  if (!*produced &&
      rate_vulkan_executor_is_fft(
          &p->vulkan_stages[
              p->vulkan_stage_count - 1u]) &&
      (p->vulkan_stage_count != 1u ||
       p->vulkan_external_input_pending >=
           lsx_rate_vulkan_resident_batch_depth(
               p->vulkan_stages[0].dft)) &&
      (p->vulkan_external_input_pending = 0,
       lsx_rate_vulkan_flush_resident(
          p->vulkan_stages[
              p->vulkan_stage_count - 1u].dft)) !=
          SOX_SUCCESS)
    return SOX_EINVAL;
  if (!*produced)
    return SOX_SUCCESS;

output_ready:
  p->rate.samples_out += resident->valid_elements * channels;
  return SOX_SUCCESS;
}

/* Act as the head of a resident chain: take host samples, publish resident
 * blocks.  Output is offered before input is taken, and *isamp is set to zero
 * when a block is produced -- the caller keeps its samples and offers them
 * again next time, so nothing is buffered that has not been paid for. */
static int lsx_rate_effect_flow_resident(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  priv_t *p;
  size_t channels;
  size_t idone = 0;

  if (!effp || !isamp || !resident || !produced)
    return SOX_EINVAL;
  p = (priv_t *)effp->priv;
  channels = effp->in_signal.channels;
  memset(resident, 0, sizeof(*resident));
  if (!p->vulkan_stage_count)
    return SOX_EINVAL;
  if (take_vulkan_resident_output(effp, lsx_vulkan_resident_ready, sox_false, resident, produced) != SOX_SUCCESS)
    return SOX_EINVAL;
  if (*produced) {
    p->rate.samples_out += resident->valid_elements * channels;
    *isamp = 0;
    return SOX_SUCCESS;
  }
  if (process_vulkan_stages(effp, p->vulkan_stage_count - 1u, sox_true) != SOX_SUCCESS || take_vulkan_resident_output(effp, lsx_vulkan_resident_ready, sox_false, resident, produced) != SOX_SUCCESS)
    return SOX_EINVAL;
  if (*produced) {
    p->rate.samples_out += resident->valid_elements * channels;
    *isamp = 0;
    return SOX_SUCCESS;
  }
  if (*isamp) {
    fifo_t *input_fifo = &p->vulkan_fifos[0];

    idone = *isamp - *isamp % channels;
    if (p->vulkan_dsd_input)
      write_vulkan_dsd_input(effp, ibuf, idone);
    else {
      double *input = fifo_write(input_fifo, idone, NULL);

      lsx_load_samples(input, ibuf, idone);
      p->rate.samples_in += idone;
    }
    if (process_vulkan_stages(effp, p->vulkan_stage_count - 1u, sox_true) != SOX_SUCCESS ||
        take_vulkan_resident_output(
        effp, lsx_vulkan_resident_ready, sox_false,
        resident, produced) != SOX_SUCCESS) {
      lsx_fail("resident Vulkan rate flow failed");
      return SOX_EINVAL;
    }
    if (*produced)
      p->rate.samples_out += resident->valid_elements * channels;
  }
  *isamp = idone;
  return SOX_SUCCESS;
}

/* How many output samples the effect still owes.
 *
 * Computed from the input count and the plan's ratio rather than accumulated,
 * so that rounding cannot drift over a long stream: the target is recomputed
 * from scratch each time and rounded once. */
static size_t vulkan_remaining_samples(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  uint64_t input_frames = p->rate.samples_in / channels;
  uint64_t target_samples = (uint64_t)(input_frames / p->rate.plan.factor + .5) * channels;

  return target_samples > p->rate.samples_out ? (size_t)(target_samples - p->rate.samples_out) : 0;
}

static int drain_vulkan_resident_external_input(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  size_t remaining = vulkan_remaining_samples(effp);

  memset(resident, 0, sizeof(*resident));
  *produced = sox_false;
  *done = remaining == 0;
  while (!*done && !*produced) {
    sox_bool advanced;

    if (take_vulkan_resident_output(effp, lsx_vulkan_resident_draining, p->vulkan_external_output_normalize, resident, produced) != SOX_SUCCESS)
      return SOX_EINVAL;
    if (*produced)
      break;
    if (!lsx_rate_vulkan_resident_stream_ready(p->vulkan_stages[0].dft) && lsx_rate_vulkan_pad_resident_stream(p->vulkan_stages[0].dft) != SOX_SUCCESS)
      return SOX_EINVAL;
    if (process_vulkan_resident_chain(effp, lsx_vulkan_resident_draining, &advanced) != SOX_SUCCESS || take_vulkan_resident_output(effp, lsx_vulkan_resident_draining, p->vulkan_external_output_normalize, resident, produced) != SOX_SUCCESS)
      return SOX_EINVAL;
    if (!*produced &&
        rate_vulkan_executor_is_fft(
            &p->vulkan_stages[
                p->vulkan_stage_count - 1u]) &&
        lsx_rate_vulkan_flush_resident(
            p->vulkan_stages[
                p->vulkan_stage_count - 1u].dft) !=
            SOX_SUCCESS)
      return SOX_EINVAL;
  }
  if (!*produced)
    return SOX_SUCCESS;
  remaining /= channels;
  resident->valid_elements = min(resident->valid_elements, remaining);
  p->rate.samples_out += resident->valid_elements * channels;
  *done = resident->valid_elements == remaining;
  if (*done)
    resident->state = lsx_vulkan_resident_final;
  return SOX_SUCCESS;
}

/* Flush the resident path once the input has ended.
 *
 * How much is still owed is computed from the ratio rather than from what the
 * stages hold, so the effect emits exactly the number of output samples the
 * resampling implies.  Zero blocks are pushed in until that many have come
 * out; done says nothing further is owed. */
static int lsx_rate_effect_drain_resident(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done)
{
  priv_t *p;
  size_t channels;
  size_t remaining;
  rate_vulkan_stage_executor_t *first;
  fifo_t *input_fifo;
  size_t block_frames;
  size_t block_samples;

  if (!effp || !resident || !produced || !done)
    return SOX_EINVAL;
  p = (priv_t *)effp->priv;
  if (p->vulkan_external_input_active)
    return drain_vulkan_resident_external_input(effp, resident, produced, done);
  channels = effp->in_signal.channels;
  memset(resident, 0, sizeof(*resident));
  *produced = sox_false;
  remaining = vulkan_remaining_samples(effp);
  *done = remaining == 0;
  if (*done)
    return SOX_SUCCESS;
  first = &p->vulkan_stages[0];
  input_fifo = &p->vulkan_fifos[0];
  block_frames = rate_vulkan_executor_input_frames(first);
  block_samples = block_frames * channels;
  while (!*produced) {
    if (take_vulkan_resident_output(effp, lsx_vulkan_resident_draining, sox_false, resident, produced) != SOX_SUCCESS) {
      lsx_fail("resident Vulkan rate drain output failed");
      return SOX_EINVAL;
    }
    if (*produced)
      break;
    if (process_vulkan_stages(effp, p->vulkan_stage_count - 1u, sox_true) != SOX_SUCCESS) {
      lsx_fail("resident Vulkan rate drain stages failed");
      return SOX_EINVAL;
    }
    if (take_vulkan_resident_output(effp, lsx_vulkan_resident_draining, sox_false, resident, produced) != SOX_SUCCESS) {
      lsx_fail("resident Vulkan rate drain final output failed");
      return SOX_EINVAL;
    }
    if (*produced)
      break;
    if (p->vulkan_dsd_input)
      p->vulkan_dsd_flushing = sox_true;
    else
      memset(fifo_write(input_fifo, block_samples, NULL), 0, block_samples * sizeof(double));
  }
  remaining /= channels;
  if (resident->valid_elements > remaining)
    resident->valid_elements = remaining;
  if (resident->valid_elements == remaining) {
    resident->state = lsx_vulkan_resident_final;
    *done = sox_true;
  }
  p->rate.samples_out += resident->valid_elements * channels;
  return SOX_SUCCESS;
}

static int flow_vulkan_resident_endpoint(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced)
{
  return lsx_rate_effect_flow_resident(effp, ibuf, isamp, resident, produced);
}

static int drain_vulkan_resident_endpoint(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done)
{
  return lsx_rate_effect_drain_resident(effp, resident, produced, done);
}

static int transform_vulkan_resident_endpoint(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *active)
{
  int status = flow_vulkan_resident_input_mode(effp, input, sox_false, input_consumed, output, output_produced);

  *input_clips = lsx_rate_effect_external_input_clips(effp);
  *active = lsx_rate_effect_resident_input_ready(effp);
  return status;
}

static int drain_transform_vulkan_resident_endpoint(sox_effect_t *effp, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *done)
{
  int status = lsx_rate_effect_drain_resident(effp, output, output_produced, done);

  *input_clips = lsx_rate_effect_external_input_clips(effp);
  return status;
}

/* Flush the non-resident Vulkan path: push silence through until the output
 * FIFO holds everything the ratio says is owed, then trim the surplus the
 * filters add beyond the end of the signal. */
static int flush_vulkan(sox_effect_t *effp)
{
  priv_t *p = (priv_t *)effp->priv;
  size_t channels = effp->in_signal.channels;
  rate_vulkan_stage_executor_t *first = &p->vulkan_stages[0];
  fifo_t *input_fifo = &p->vulkan_fifos[0];
  fifo_t *output_fifo = &p->vulkan_fifos[p->vulkan_stage_count];
  size_t block_frames = rate_vulkan_executor_input_frames(first);
  size_t block_samples = block_frames * channels;
  size_t remaining = vulkan_remaining_samples(effp);

  /* The fused stage is not padded with silent input: it is told the stream
   * has ended and reads the frames past it as zero samples, which is the same
   * thing without a bit pattern having to stand for silence. */
  p->vulkan_dsd_flushing = p->vulkan_dsd_input;
  while ((size_t)fifo_occupancy(output_fifo) < remaining) {
    int status;

    if (!p->vulkan_dsd_input) {
      double *zeros = fifo_write(input_fifo, block_samples, NULL);

      memset(zeros, 0, block_samples * sizeof(*zeros));
    }
    status = process_vulkan_stages(effp, p->vulkan_stage_count, sox_false);
    if (status != SOX_SUCCESS)
      return SOX_EINVAL;
  }
  if ((size_t)fifo_occupancy(output_fifo) > remaining)
    fifo_trim_to(output_fifo, remaining);
  return SOX_SUCCESS;
}
#endif

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
                sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t odone = *osamp;

#if HAVE_VULKAN
  if (p->vulkan_stage_count)
    return flow_vulkan(effp, ibuf, obuf, isamp, osamp);
#endif
  sample_t const * s = rate_cpu_output(&p->rate, NULL, &odone);
  lsx_save_samples(obuf, s, odone, &effp->clips);

  if (*isamp && odone < *osamp) {
    size_t output_room = *osamp - odone;
    double input_limit = ceil(output_room * p->rate.plan.factor);
    size_t idone = input_limit >= (double)*isamp ?
        *isamp : min(*isamp, max((size_t)RATE_MIN_INPUT_CHUNK,
            (size_t)input_limit));
    sample_t * t = rate_cpu_input(&p->rate, NULL, idone);
    lsx_load_samples(t, ibuf, idone);
    rate_cpu_process(&p->rate);
    *isamp = idone;

    {
      size_t more = *osamp - odone;
      s = rate_cpu_output(&p->rate, NULL, &more);
      lsx_save_samples(obuf + odone, s, more, &effp->clips);
      odone += more;
    }
  }
  else *isamp = 0;
  *osamp = odone;
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  static size_t isamp = 0;

#if HAVE_VULKAN
  if (p->vulkan_stage_count) {
    if (flush_vulkan(effp) != SOX_SUCCESS) {
      *osamp = 0;
      return SOX_EINVAL;
    }
    return flow_vulkan(effp, NULL, obuf, &isamp, osamp);
  }
#endif
  rate_cpu_flush(&p->rate);
  return flow(effp, 0, obuf, &isamp, osamp);
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;

#if HAVE_VULKAN
  if (p->vulkan_stage_count) {
    size_t index;

    for (index = 0; index < p->vulkan_stage_count; ++index) {
      lsx_rate_vulkan_destroy(p->vulkan_stages[index].dft);
      lsx_rate_cubic_vulkan_destroy(p->vulkan_stages[index].cubic);
      lsx_rate_dsd_vulkan_destroy(p->vulkan_stages[index].dsd);
      lsx_rate_polyphase_vulkan_destroy(p->vulkan_stages[index].polyphase);
      fifo_delete(&p->vulkan_fifos[index]);
    }
    fifo_delete(&p->vulkan_fifos[p->vulkan_stage_count]);
    if (p->vulkan_dsd_fifos) {
      size_t channel;

      for (channel = 0; channel < effp->in_signal.channels; ++channel)
        fifo_delete(&p->vulkan_dsd_fifos[channel]);
    }
    free(p->vulkan_dsd_fifos);
    free(p->vulkan_dsd_runs);
    p->vulkan_dsd_fifos = NULL;
    p->vulkan_dsd_runs = NULL;
    p->vulkan_dsd_input = sox_false;
    p->vulkan_dsd_flushing = sox_false;
    free(p->vulkan_stages);
    free(p->vulkan_fifos);
    free(p->vulkan_pending_chain_input_valid);
    free(p->vulkan_pending_chain_input);
    p->vulkan_stages = NULL;
    p->vulkan_fifos = NULL;
    p->vulkan_pending_chain_input_valid = NULL;
    p->vulkan_pending_chain_input = NULL;
    p->vulkan_stage_count = 0;
    effp->internal_chain_endpoint = NULL;
    rate_plan_destroy(&p->rate.plan);
    return SOX_SUCCESS;
  }
#endif
  rate_cpu_stop(&p->rate);
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_rate_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "rate", 0, SOX_EFF_RATE, create, start, flow, drain, stop, 0, sizeof(priv_t)
  };
  static char const * lines[] = {
    "[-q|-l|-m|-h|-v] [override-options] RATE[k]",
    "                    BAND-",
    "     QUALITY        WIDTH  REJ dB   TYPICAL USE",
    " -q  quick          n/a  ~30 @ Fs/4 playback on ancient hardware",
    " -l  low            80%     100     playback on old hardware",
    " -m  medium         95%     100     audio playback",
    " -h  high (default) 95%     125     16-bit mastering (use with dither)",
    " -v  very high      95%     175     24-bit mastering",
    "              OVERRIDE OPTIONS (only with -m, -h, -v)",
    " -M/-I/-L     Phase response = minimum/intermediate/linear(default)",
    " -s           Steep filter (band-width = 99%)",
    " -a           Allow aliasing above the pass-band",
    " -b 74-99.7   Any band-width %",
    " -p 0-100     Any phase response (0 = minimum, 25 = intermediate,",
    "              50 = linear, 100 = maximum)",
  };
  static char * usage;
  handler.usage = lsx_usage_lines(&usage, lines, array_length(lines));
  return &handler;
}
