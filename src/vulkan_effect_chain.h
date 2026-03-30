/* Private GPU-resident contracts for consecutive libSoX effects. */

#ifndef LSX_VULKAN_EFFECT_CHAIN_H
#define LSX_VULKAN_EFFECT_CHAIN_H

#include "vulkan_engine.h"

typedef struct {
  int (*flow_producer)(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);
  int (*drain_producer)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);
  int (*consume)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, sox_sample_t *obuf, size_t *osamp, sox_bool *active);
  /*
   * A transform consumes at most one resident input slice and can produce at
   * most one resident output slice per call.  The input remains owned by its
   * producer until input_consumed becomes true.  When active remains true,
   * the scheduler calls the transform again with input == NULL after the
   * current output has moved downstream.  After consuming a final input, the
   * scheduler uses drain_transform until it produces the final output slice.
   */
  int (*transform)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *active);
  int (*drain_transform)(sox_effect_t *effp, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *done);
} lsx_vulkan_effect_endpoint_t;

#endif
