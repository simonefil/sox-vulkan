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
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Machine-readable diagnostics for one run of SoX.
 *
 * --diagnostics DIR turns this on.  It writes DIR/run.txt, one key=value per
 * line with the hierarchy spelled out by dots, plus whatever sample captures
 * the run could take.  It emits numbers the run already computes; it takes
 * no measurements of its own.
 *
 * The files carry no version field.  Keys keep their meaning and are never
 * removed; new ones are only added.  A reader takes an absent key as "not
 * measured", neither an error nor zero, so an old binary produces fewer keys
 * a new reader can handle, and a new binary produces keys an old reader
 * ignores.
 *
 * Invariants the rest of the tree has to preserve:
 *
 *   Neutrality.  The audio is byte-identical with the flag and without it.
 *                Nothing here may feed back into a sample.
 *   Zero cost when off.  The taps and setters test lsx_diagnostics_on
 *                first, and that test is all a run without the flag pays.
 *                No file is opened and nothing is allocated.
 *   Explicit failure.  A metric that was asked for and cannot be produced
 *                stops the run with a message.  A key is never left empty to
 *                stand in for one.
 */

#ifndef LSX_DIAGNOSTICS_H
#define LSX_DIAGNOSTICS_H

#include "sox.h"

#ifdef __GNUC__
#define LSX_DIAG_PRINTF23 __attribute__ ((format (printf, 2, 3)))
#define LSX_DIAG_PRINTF34 __attribute__ ((format (printf, 3, 4)))
#else
#define LSX_DIAG_PRINTF23
#define LSX_DIAG_PRINTF34
#endif

/* Non-zero from lsx_diagnostics_open() until lsx_diagnostics_close().  Read
 * directly by the taps so that the off case is one predictable branch. */
extern int lsx_diagnostics_on;

/* Create DIR if needed and begin collecting.  Only the files this run writes
 * are replaced -- run.txt and the captures -- so a mistyped path costs those
 * names and leaves the rest of the directory intact.  Anything that stops the
 * mode from working is fatal: a diagnostics run is complete or it stops. */
void lsx_diagnostics_open(char const *dir);

/* Record a key.  A key set twice keeps its original position and takes the
 * new value, so a late correction reads as one fact rather than two. */
void lsx_diagnostics_setf(char const *key, char const *fmt, ...) LSX_DIAG_PRINTF23;

/* Whether a key has already been recorded, for a caller that must not
 * overwrite an earlier one. */
int lsx_diagnostics_have(char const *key);

/* The same, under effect.N. for the effect's position in the chain.  Effects
 * use it to publish what only they know: taps, stages, strategy, latency.
 * A key from an effect that is not yet in the chain being diagnosed is held
 * as pending; see below. */
void lsx_diagnostics_effect_setf(sox_effect_t const *effp, char const *leaf,
    char const *fmt, ...) LSX_DIAG_PRINTF34;

/* An effect publishes its keys from its start function, which runs before the
 * effect is in the chain and therefore before it has a number.  Those keys
 * are held aside until sox_add_effect() says which position it took, and
 * dropped if it never took one -- an effect that reported SOX_EFF_NULL must
 * not leave keys behind under the number the next effect will get. */
void lsx_diagnostics_effect_pending_clear(void);
void lsx_diagnostics_effect_pending_flush(size_t index);

/* The chain about to be run: the source of the effect.N. numbering and of the
 * generic per-effect keys written by lsx_diagnostics_close(). */
void lsx_diagnostics_chain(sox_effects_chain_t const *chain);

/* The chain has finished: write its per-effect keys while its effects are
 * still alive, and stop pointing at it.  A run with several chains -- files
 * processed in sequence -- leaves the keys of the last one and the captures
 * of all of them, which is what the output file holds too. */
void lsx_diagnostics_chain_done(void);

/* Frames handed to and produced by effect n, accumulated over the run. */
void lsx_diagnostics_effect_frames(size_t n, size_t in, size_t out);

/* Arm the sample taps around one effect's flow or drain, and disarm after.
 * Only the last effect before the chain's sink is armed: its samples are the
 * chain's output, so each capture file holds one signal.  The effect's flow
 * count has to be known before any flow starts, since each writes its own
 * part file and the slots for the parts must exist before threads reach
 * them. */
void lsx_diagnostics_tap_begin(sox_effect_t const *effp);
void lsx_diagnostics_tap_flow(size_t flow);
void lsx_diagnostics_tap_end(void);

/* Non-zero while a tapped effect is running.  lsx_vulkan_collapse_pair()
 * tests it before paying for a capture call. */
int lsx_diagnostics_tap_armed(void);

/* The universal tap: n doubles as an effect hands them to lsx_save_samples(),
 * which copies them into the chain's sox_sample_t buffer.  Covers every
 * backend and profile, and measures at the file's floor of about -186 dB. */
void lsx_diagnostics_capture_f64(double const *samples, size_t n);

/* The precision tap: one double-double, high then low, taken where both
 * halves still exist, in lsx_vulkan_collapse_pair().  It makes the reference
 * profile measurable from a single run. */
void lsx_diagnostics_capture_dd(double high, double low);
/* The fused DSD stage's output, as pairs, interleaved by channel. */
void lsx_diagnostics_capture_dsd_dd(double const *pairs, size_t samples);

/* Capture the packed one-bit stream handed to a DSD writer.  Byte-packed
 * input is group-interleaved and carries its valid-bit count; word-packed
 * input is channel-major within each call.  The capture normalises both to
 * channel-interleaved bytes with the earliest bit in bit zero. */
void lsx_diagnostics_capture_dsd(sox_sample_t const *samples, size_t n,
    unsigned channels, unsigned packing);

/* Stop the run with a message, recorded as result.status=error before the
 * files are closed.  Never returns. */
void lsx_diagnostics_fail(char const *fmt, ...) LSX_PRINTF12;

/* Write run.txt, assemble the captures from their parts and stop.  status is
 * the run's own exit status, so a failed run still leaves a readable file
 * that says why. */
void lsx_diagnostics_close(int status);

#endif
