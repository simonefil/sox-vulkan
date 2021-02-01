/* Sigma-Delta modulator
 * Copyright (c) 2015 Mans Rullgard <mans@mansr.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 Damien Plisson <damien78@audirvana.com> Apr 20th, 2016
 Added 64bit sample => 8bit packets SDM processing functions
 */


#ifndef SOX_SDM_H
#define SOX_SDM_H

#include "sox_i.h"

#if _MSC_VER && defined(SOX_IMPORT)
#define SOX_EXPORT __declspec(dllimport)
#elif _MSC_VER && defined(_DLL)
#define SOX_EXPORT __declspec(dllexport)
#else
#define SOX_EXPORT
#endif


#define SDM_TRELLIS_MAX_ORDER 32
#define SDM_TRELLIS_MAX_NUM   32
#define SDM_TRELLIS_MAX_LAT   2048

typedef struct sdm sdm_t;

SOX_EXPORT
sdm_t *sdm_init(const char *filter_name,
                unsigned freq,
                unsigned trellis_order,
                unsigned trellis_num,
                unsigned trellis_latency);

SOX_EXPORT
int sdm_process(sdm_t *s, const sox_sample_t *ibuf, sox_sample_t *obuf,
                size_t *ilen, size_t *olen);

SOX_EXPORT
int sdm_drain(sdm_t *s, sox_sample_t *obuf, size_t *olen);

///Process input in 64bit format into a 8bit packet of 1bit samples
/// - parameter p: the sdm private structure
/// - parameter inSamples: input buffer of 64bit (double) samples
/// - parameter outPackets: output buffer, in 8bit packets of 8 1bit samples
/// - parameter inLength: number of samples in input buffer
/// - important: outPackets size must be at least inLength / 8
/// - returns: number of packets in outPackets
SOX_EXPORT
size_t sdm_packet_process(sdm_t *p, const double *inSamples, uint8_t *outPackets, size_t inLength);

///Drain filter in 8bit packets
/// - parameter outBufSize: size of the outPackets buffer
/// - returns: number of packets in outPackets
SOX_EXPORT
size_t sdm_packet_drain(sdm_t *p, uint8_t *outPackets, size_t outBufSize);

SOX_EXPORT
void sdm_close(sdm_t *s);

#endif
