#include "fft4g.h"
#include "fifo.h"

typedef struct {
  int        dft_length, num_taps, post_peak;
  double     * coefs, * taps;
} dft_filter_t;

typedef struct {
  uint64_t   samples_in, samples_out;
  fifo_t     input_fifo, output_fifo;
  dft_filter_t   filter, * filter_ptr;
#if HAVE_VULKAN
  double     * vulkan_source_taps;
  int        vulkan_source_num_taps;
  int        vulkan_source_post_peak;
  sox_bool   vulkan_fusion_pending;
  struct lsx_fir_vulkan * vulkan;
  struct lsx_rate_vulkan * vulkan_resident;
  struct lsx_vulkan_context * vulkan_context;
  fifo_t     vulkan_input_fifo, vulkan_output_fifo;
  double     * vulkan_drain_block;
  size_t     vulkan_skip_samples;
  uint32_t   vulkan_resident_pending;
  sox_bool   vulkan_consumer_transform_active;
  sox_bool   vulkan_consumer_final_received;
  sox_bool   vulkan_consumer_done;
#endif
} dft_filter_priv_t;

void lsx_set_dft_filter(dft_filter_t * f, double * h, int n, int post_peak);
#if HAVE_VULKAN
int lsx_dft_filter_restart_vulkan(
    sox_effect_t *effp, double *taps,
    int num_taps, int post_peak);
int lsx_fir_vulkan_try_fuse(
    sox_effect_t *first, sox_effect_t const *second);
#endif
