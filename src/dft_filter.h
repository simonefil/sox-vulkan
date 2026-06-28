#include "fft4g.h"
#include "fifo.h"

/* One FIR response, ready for overlap-save convolution.
 *
 * taps is the response itself; coefs is its transform, of dft_length points,
 * precomputed once so that each block costs one forward and one inverse
 * transform rather than three.  post_peak is how many taps follow the
 * response's peak, which is what fixes the effect's latency.
 */
typedef struct {
  int        dft_length, num_taps, post_peak;
  double     * coefs, * taps;
} dft_filter_t;

#if HAVE_VULKAN
/* Per-channel coefficients, for effects whose response differs by channel.
 * source_taps is what was configured, kept apart from the response actually
 * in use because a later effect may fuse into it and the original is then
 * still needed.  The fusion arrays hold the responses of the effects being
 * cascaded, in order; eight is the deepest cascade fusion accepts.
 */
typedef struct {
  double     * source_taps;
  double     * reference_low_taps; /* Low halves, reference profile only. */
  double     * fusion_sources[8];
  size_t     fusion_source_taps[8];
} dft_filter_vulkan_channel_t;
#endif

/* State shared by every effect built on this filter: fir, the biquad-derived
 * effects that use a DFT response, and anything else that convolves.  The
 * effect's own private struct begins with one of these, so the code here can
 * be reached from any of them.
 */
typedef struct {
  uint64_t   samples_in, samples_out;
  fifo_t     input_fifo, output_fifo;
  /* filter is the response this effect owns; filter_ptr is what is actually
   * used, which for a shared response points elsewhere. */
  dft_filter_t   filter, * filter_ptr;
#if HAVE_VULKAN
  /* The response as configured, kept so that a restart -- after a fusion, or
   * when the channel count is finally known -- can rebuild from the original
   * rather than from whatever the last rebuild produced. */
  double     * vulkan_source_taps;
  int        vulkan_source_num_taps;
  int        vulkan_source_post_peak;
  double     * vulkan_reference_low_taps;

  /* Per-channel responses, when the effect has them; NULL means one shared
   * response for every channel. */
  dft_filter_vulkan_channel_t * vulkan_channels;
  uint32_t   vulkan_channel_count;

  /* Responses of the effects being fused into this one.  Fusion is deferred:
   * the sources are collected as neighbouring effects offer them and the
   * combined response is built once, at the first block, since each further
   * fusion would otherwise rebuild everything again. */
  double     * vulkan_fusion_sources[8];
  size_t     vulkan_fusion_source_taps[8];
  uint32_t   vulkan_fusion_source_count;
  sox_bool   vulkan_fusion_pending;

  struct lsx_fir_vulkan * vulkan;          /* The convolution backend. */
  /* A rate stage at ratio 1:1, used only as the resident path's plumbing --
   * it owns the resident stream and the interpolate/decimate helpers, which
   * a filter with no rate change still needs in order to exchange resident
   * blocks with its neighbours. */
  struct lsx_rate_vulkan * vulkan_resident;
  sox_bool   vulkan_resident_enabled;
  struct lsx_vulkan_context * vulkan_context;

  /* Re-blocking either side of the backend, which insists on exactly one
   * block per call while the effect chain hands over whatever it has. */
  fifo_t     vulkan_input_fifo, vulkan_output_fifo;
  double     * vulkan_drain_block;         /* Zero block used to flush the tail. */
  /* Output samples still to be discarded for the filter's latency. */
  size_t     vulkan_skip_samples;
  uint32_t   vulkan_resident_pending;

  /* Consumer-side state: whether this effect is acting as the tail of a
   * resident chain, whether its producer has sent its final block, and
   * whether its own drain has finished. */
  sox_bool   vulkan_consumer_transform_active;
  sox_bool   vulkan_consumer_final_received;
  sox_bool   vulkan_consumer_done;
#endif
} dft_filter_priv_t;

/* Prepare a response for use: takes ownership of h, chooses the transform
 * length and precomputes the coefficient spectrum. */
void lsx_set_dft_filter(dft_filter_t * f, double * h, int n, int post_peak);

#if HAVE_VULKAN
/* Record per-channel responses, copying them.  All must be the same length,
 * the transform size and partitioning being shared. */
int lsx_set_dft_filter_vulkan_channels(
    dft_filter_priv_t *p, double const *const *taps,
    int num_taps, int post_peak, uint32_t channels);

/* Release them; safe to call when none were recorded. */
void lsx_clear_dft_filter_vulkan_channels(dft_filter_priv_t *p);

/* Offer second's response to first for fusion, so that a cascade of filters
 * becomes one convolution instead of several.  Returns SOX_SUCCESS if first
 * accepted it, in which case second must not also apply it.  Only the
 * response is taken; nothing is computed here, the combining being deferred
 * until the first block. */
int lsx_fir_vulkan_try_fuse(sox_effect_t *first, sox_effect_t const *second);
#endif
