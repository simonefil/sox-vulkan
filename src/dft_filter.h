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
  struct lsx_fir_vulkan * vulkan;
  struct lsx_rate_vulkan * vulkan_resident;
  fifo_t     vulkan_input_fifo, vulkan_output_fifo;
  double     * vulkan_drain_block;
  size_t     vulkan_skip_samples;
  uint32_t   vulkan_resident_pending;
#endif
} dft_filter_priv_t;

void lsx_set_dft_filter(dft_filter_t * f, double * h, int n, int post_peak);
