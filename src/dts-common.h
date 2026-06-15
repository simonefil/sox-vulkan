/* Shared DTS and DTS-HD elementary stream format implementation.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef LSX_DTS_COMMON_H
#define LSX_DTS_COMMON_H

#include "sox.h"

/* The two handlers dts.c and dtshd.c register.  Both live in dts-common.c
 * because a DTS-HD stream is a DTS core with extension substreams appended,
 * so one decoder reads both and only a profile check separates them.  Each
 * returns a pointer to static data that outlives any file. */
sox_format_handler_t const * lsx_dts_format_handler(void);
sox_format_handler_t const * lsx_dtshd_format_handler(void);

#endif
