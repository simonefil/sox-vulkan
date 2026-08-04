/* Shared MLP and Dolby TrueHD elementary stream format implementation.
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

#ifndef LSX_MLP_COMMON_H
#define LSX_MLP_COMMON_H

#include "sox.h"

/* The two handlers mlp.c and truehd.c register.  Both live in mlp-common.c
 * because TrueHD is MLP extended: same access unit framing, same major sync,
 * and only a stream type byte and a codec id to tell them apart.  Each
 * returns a pointer to static data that outlives any file. */
sox_format_handler_t const * lsx_mlp_format_handler(void);
sox_format_handler_t const * lsx_truehd_format_handler(void);

#endif
