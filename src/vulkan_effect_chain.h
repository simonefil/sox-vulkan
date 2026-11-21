/* (c) Simone Filippini <info@simonefilippini.it> 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

/* Private GPU-resident contracts for consecutive libSoX effects. */

#ifndef LSX_VULKAN_EFFECT_CHAIN_H
#define LSX_VULKAN_EFFECT_CHAIN_H

#include "vulkan_engine.h"

typedef struct {
  /* Consume host samples at the head of a resident segment.  *isamp enters as
   * the available sample count and returns how many were consumed.  A true
   * *produced publishes exactly one resident slice in *resident; its storage
   * remains owned by the effect until the downstream endpoint consumes it. */
  int (*flow_producer)(sox_effect_t *effp, sox_sample_t const *ibuf, size_t *isamp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced);

  /* Flush the producer after its host input ends.  Each call may publish one
   * slice.  *done becomes true only when no further call is owed; a slice
   * produced by that call must carry the final state. */
  int (*drain_producer)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t *resident, sox_bool *produced, sox_bool *done);

  /* Terminate a resident segment by converting at most one input slice to
   * host samples.  *osamp is capacity on entry and samples written on return.
   * *input_consumed releases the producer's storage, *input_clips transfers
   * clipping charged to it, and *active requests calls with input == NULL
   * while buffered output or internal work remains.  An input larger than
   * the consumer can hold is taken in parts over several calls, as a classic
   * flow takes what fits of its input: *input_consumed comes with the last
   * part, the scheduler presents the same input until then, and *active
   * stays true meanwhile. */
  int (*consume)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, sox_sample_t *obuf, size_t *osamp, sox_bool *active);
  /*
   * A transform consumes at most one resident input slice and can produce at
   * most one resident output slice per call.  The input remains owned by its
   * producer until input_consumed becomes true; an input larger than the
   * transform can hold is taken in parts over several calls, the same input
   * being presented until the last, and only that part carries the input's
   * state.  When active remains true,
   * the scheduler calls the transform again with input == NULL after the
   * current output has moved downstream.  *input_clips transfers clipping
   * charged to the upstream slice.  After consuming a final input, the
   * scheduler uses drain_transform until it produces the final output slice;
   * before then transform must not mark an output final.
   */
  int (*transform)(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *active);

  /* Flush a transform after it consumed its final input.  One call publishes
   * at most one slice.  *done may become true only together with a produced
   * final slice; until then any produced slice is draining, and false asks
   * the scheduler to call again after that output has been consumed. */
  int (*drain_transform)(sox_effect_t *effp, uint64_t *input_clips, lsx_vulkan_resident_buffer_t *output, sox_bool *output_produced, sox_bool *done);
} lsx_vulkan_effect_endpoint_t;

#endif
