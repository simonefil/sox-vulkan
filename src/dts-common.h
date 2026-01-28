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

sox_format_handler_t const * lsx_dts_format_handler(void);
sox_format_handler_t const * lsx_dtshd_format_handler(void);

#endif
