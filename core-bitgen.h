/*
 * Copyright (C) 2026 wangxumarshall
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 *
 */

/*
 *  core-bitgen.h — SDC-directed bit-pattern generation
 *
 *  Uniformly random operands sample the 2^64 value space evenly, so
 *  boundary-adjacent patterns (where data-dependent silicon defects
 *  live — e.g. Meta's core-59 `Int(1.1^53) == 0` failing while
 *  `Int(1.1^52)` is correct) are hit with probability ~2^-40 and in
 *  practice never.  Fixed patterns (memrate's 0xaa everywhere) cover
 *  exactly one point.  This generator mixes both: pattern
 *  dictionaries swept by cursors plus per-call random jitter,
 *  following the mutation levers from the SDC frontier research
 *  (docs/superpowers/research/2026-09-16-r2-sdc-frontier-research.md,
 *  items D2/D3/D4/D5).
 *
 *  All generators keep private state (a handle) and never touch the
 *  global stress_mwc_* stream, so callers' seed save/replay flows
 *  (e.g. the vm stressor's) are unaffected.
 */
#ifndef CORE_BITGEN_H
#define CORE_BITGEN_H

#include "stress-ng.h"

typedef struct {
	/* private MWC state, seeded from the global stream at creation
	 * time so runs differ, but advanced independently afterwards */
	uint32_t w, z;
	/* bandwalk cursor: which bit-band window we are sweeping and
	 * the offset inside it */
	uint32_t band_lo;	/* band start bit position */
	uint32_t band_width;	/* 6..20 bits */
	uint32_t band_step;	/* calls until the window advances */
	uint32_t band_count;
	uint32_t density;	/* 0..4 -> 0/25/50/75/100% ones in band */
	/* edge dictionary cursor */
	uint32_t edge_idx;
	/* mode mixing: which sub-generator the next call serves */
	uint32_t mix;
} stress_bitgen_t;

/*  stress_bitgen_init()
 *	initialise a generator handle with a seed drawn from the
 *	global MWC stream (i.e. varies per run/worker; use --seed for
 *	reproducibility as usual).
 */
void stress_bitgen_init(stress_bitgen_t *bg);

/*  stress_bitgen_seed()
 *	re-initialise with an explicit seed (for verify replay).
 */
void stress_bitgen_seed(stress_bitgen_t *bg, const uint64_t seed);

/*  stress_bitgen_bandwalk64/32()
 *	bit-band sweeping integers: a window of band_width bits walks
 *	through the word; inside the window a 0/25/50/75/100%-ones
 *	pattern is emitted (random where the density is partial).
 *	The window advances every few calls, so long runs sweep the
 *	mantissa/middle-bit regions where bit-flip-sensitive defects
 *	concentrate (research lever D4).
 */
uint64_t stress_bitgen_bandwalk64(stress_bitgen_t *bg);
uint32_t stress_bitgen_bandwalk32(stress_bitgen_t *bg);

/*  stress_bitgen_edge64()
 *	boundary dictionary value with ±1..4 LSB jitter:
 *	  0, 1, 2^k-1, 2^k, 2^k+1 (k = 1..63), all-ones,
 *	  sign-bit transitions, and the float/double boundary
 *	  encodings (0x7ff0.., 0x000f.., DBL_MAX/MIN shapes) that
 *	  Meta's data-dependent core failures cluster around (D3).
 */
uint64_t stress_bitgen_edge64(stress_bitgen_t *bg);

/*  stress_bitgen_fp64_bits()
 *	synthesise a double's raw bits directly — exponent field and
 *	mantissa field filled independently (bandwalk on the mantissa,
 *	swept exponent including subnormal/all-ones regions).  Unlike
 *	arithmetic scaling ((double)i + r/2^38) the two fields are
 *	uncorrelated, so boundary exponents with boundary mantissas
 *	are reachable.
 */
uint64_t stress_bitgen_fp64_bits(stress_bitgen_t *bg);

/*  stress_bitgen_fp32_bits()
 *	same for a 32-bit float.
 */
uint32_t stress_bitgen_fp32_bits(stress_bitgen_t *bg);

/*  stress_bitgen_complement_pair64()
 *	(a, b) with b = ~a: alternating them toggles every trace each
 *	beat (P = alpha*C*V^2*f maximised, targeted ageing >7x,
 *	research lever D2).  a is drawn from bandwalk/edge mixtures so
 *	the pair is not always the classic 0x55/0xaa.
 */
void stress_bitgen_complement_pair64(stress_bitgen_t *bg, uint64_t *a, uint64_t *b);

/*  stress_bitgen_hamming64()
 *	word with exactly target_weight bits set (target_weight capped
 *	at 32) — Hamming-directed operand for switching-power control.
 */
uint64_t stress_bitgen_hamming64(stress_bitgen_t *bg, const unsigned int target_weight);

/*  stress_bitgen_u64()
 *	mode-mixed operand: internally rotates between bandwalk, edge
 *	and jittered-uniform so a plain consumer gets SDC-directed
 *	variety without picking a mode.
 */
uint64_t stress_bitgen_u64(stress_bitgen_t *bg);

#endif /* CORE_BITGEN_H */
