#ifndef SOX_VULKAN_QUALITY_PROBE_H
#define SOX_VULKAN_QUALITY_PROBE_H

#include <stddef.h>

typedef void (*lsx_save_samples_observer_t)(
    double const *samples, size_t count, double normalization_scale,
    void *client_data);

void lsx_set_save_samples_observer(
    lsx_save_samples_observer_t observer, void *client_data);

#endif
