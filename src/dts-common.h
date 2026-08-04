/* Shared DTS and DTS-HD elementary stream format implementation.
 *
 * (c) Simone Filippini <info@simonefilippini.it> 2026
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
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
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
