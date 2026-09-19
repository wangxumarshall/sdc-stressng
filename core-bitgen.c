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
#include "stress-ng.h"
#include "core-bitgen.h"
#include "core-mwc.h"

/*
 *  Boundary dictionary (research lever D3: data-dependent SDC
 *  clusters around value-space boundaries).  64 entries are plenty:
 *  the point is dense sampling in the neighbourhoods, done by the
 *  ±1..4 LSB jitter in stress_bitgen_edge64().
 */
static const uint64_t bitgen_edge_dict[] = {
	0x0000000000000000ULL,				/* zero */
	0x0000000000000001ULL,				/* one */
	0x0000000000000002ULL,
	0x0000000000000003ULL,
	0x7fffffffffffffffULL,				/* INT64_MAX */
	0x8000000000000000ULL,				/* INT64_MIN / sign bit */
	0x8000000000000001ULL,				/* sign bit + 1 */
	0xfffffffffffffffeULL,				/* -2 */
	0xffffffffffffffffULL,				/* all ones */
	/* 2^k - 1 / 2^k / 2^k + 1 for carry-chain boundaries */
	0x00000000000000ffULL, 0x0000000000000100ULL, 0x0000000000000101ULL,
	0x000000000000ffffULL, 0x0000000000010000ULL, 0x0000000000010001ULL,
	0x00000000ffffffffULL, 0x0000000100000000ULL, 0x0000000100000001ULL,
	0x0000ffffffffffffULL, 0x0001000000000000ULL, 0x0001000000000001ULL,
	0x00ffffffffffffffULL, 0x0100000000000000ULL, 0x0100000000000001ULL,
	/* double-precision boundary encodings */
	0x7fefffffffffffffULL,				/* DBL_MAX */
	0x0010000000000000ULL,				/* DBL_MIN (smallest normal) */
	0x0000000000000001ULL,				/* smallest subnormal */
	0x7ff0000000000000ULL,				/* +inf */
	0x7ff8000000000000ULL,				/* quiet NaN */
	0x7feffffffffffffeULL,				/* DBL_MAX - 1 ulp */
	0x0010000000000001ULL,				/* DBL_MIN + 1 ulp */
	0x4197d78400000000ULL,				/* ~1.1^53 neighbourhood (Meta core 59) */
	0x4197d783ffffffffULL,				/* 1 ulp below it */
	0x4340000000000000ULL,				/* 2^53 (int-exact boundary) */
	0x433fffffffffffffULL,				/* 2^53 - 1 ulp */
	/* single-precision boundaries widened */
	0x47efffffe0000000ULL,				/* ~FLT_MAX as double */
	0x36a0000000000000ULL,				/* ~FLT_MIN normal as double */
	/* byte/nibble transition patterns */
	0x00ff00ff00ff00ffULL,
	0xff00ff00ff00ff00ULL,
	0x0f0f0f0f0f0f0f0fULL,
	0xf0f0f0f0f0f0f0f0ULL,
	0x00000000ffffffffULL,				/* dup by design: jitter makes them distinct */
	/* power-of-two spread for exponent-like coverage */
	0x0000000000000400ULL,				/* 1024 */
	0x0000000000000800ULL,
	0x0000000000001000ULL,
	0x0000000000010000ULL,
	0x0000000000100000ULL,
	0x0000000001000000ULL,
	0x0000000010000000ULL,
	0x0000000100000000ULL,				/* dup by design */
	0x0000001000000000ULL,
	0x0000010000000000ULL,
	0x0000100000000000ULL,
	0x0001000000000000ULL,				/* dup by design */
	0x0010000000000000ULL,				/* dup by design */
	0x0100000000000000ULL,				/* dup by design */
	0x1000000000000000ULL,
};

#define EDGE_DICT_SIZE	(SIZEOF_ARRAY(bitgen_edge_dict))

/*  density → probability that a band bit is set */
static const uint32_t density_ones[5] = { 0, 1, 2, 3, 4 };	/* 0/25/50/75/100% */
#define DENSITY_MAX	4

/*
 *  bg_mwc32()
 *	private MWC step — identical algorithm to stress_mwc32() but on
 *	the handle's state, so the global stream is never consumed here.
 */
static inline uint32_t bg_mwc32(stress_bitgen_t *bg)
{
	bg->z = 36969 * (bg->z & 65535) + (bg->z >> 16);
	bg->w = 18000 * (bg->w & 65535) + (bg->w >> 16);

	return (((uint32_t)bg->z & 0xffff) << 16) + bg->w;
}

static inline uint64_t bg_mwc64(stress_bitgen_t *bg)
{
	return (((uint64_t)bg_mwc32(bg)) << 32) | bg_mwc32(bg);
}

/*
 *  stress_bitgen_init()
 *	seed from the global stream: runs/workers differ, --seed
 *	still controls everything (it seeds the global stream first).
 */
void stress_bitgen_init(stress_bitgen_t *bg)
{
	const uint64_t seed = stress_mwc64();

	stress_bitgen_seed(bg, seed);
}

void stress_bitgen_seed(stress_bitgen_t *bg, const uint64_t seed)
{
	bg->w = (uint32_t)(seed & 0xffffffff);
	bg->z = (uint32_t)(seed >> 32);
	/* avoid the all-zero fixed point */
	if (!bg->w && !bg->z)
		bg->w = 0x13572468;
	/* decorrelate the first outputs from the seed itself */
	(void)bg_mwc64(bg);
	(void)bg_mwc64(bg);

	bg->band_lo = bg_mwc32(bg) & 63;
	bg->band_width = 6 + (bg_mwc32(bg) % 15);		/* 6..20 */
	bg->band_step = 1 + (bg_mwc32(bg) % 7);
	bg->band_count = 0;
	bg->density = bg_mwc32(bg) % (DENSITY_MAX + 1);
	bg->edge_idx = bg_mwc32(bg) % EDGE_DICT_SIZE;
	bg->mix = bg_mwc32(bg);
}

/*
 *  band_fill()
 *	emit one word: bits outside [lo, lo+width) come from jittered
 *	uniform randomness (so adjacent bits still toggle), bits inside
 *	the band follow the density selector.
 */
static uint64_t band_fill(stress_bitgen_t *bg)
{
	uint64_t v = bg_mwc64(bg);
	uint64_t band = 0;
	const uint32_t width = bg->band_width;
	uint32_t i;

	for (i = 0; i < width; i++) {
		switch (density_ones[bg->density]) {
		default:
		case 0:
			break;				/* 0% ones */
		case 4:
			band |= 1ULL << i;		/* 100% ones */
			break;
		case 1:
			if ((bg_mwc32(bg) & 3) == 0)
				band |= 1ULL << i;	/* 25% */
			break;
		case 2:
			if ((bg_mwc32(bg) & 1) == 0)
				band |= 1ULL << i;	/* 50% */
			break;
		case 3:
			if ((bg_mwc32(bg) & 3) != 0)
				band |= 1ULL << i;	/* 75% */
			break;
		}
	}

	/* clear the band region then place the band pattern */
	const uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
	const uint32_t lo = bg->band_lo;
	const uint64_t shifted = (lo + width > 64) ?
		((band << lo) | (band >> (64 - lo))) :	/* wrap around */
		(band << lo);

	v &= ~(mask << lo);				/* clear band bits (no wrap case first) */
	if (lo + width > 64) {
		/* wrapped band: clear the low tail too */
		v &= ~(mask >> (64 - lo));
	}
	v |= shifted;

	/* advance the window every band_step calls */
	if (++bg->band_count >= bg->band_step) {
		bg->band_count = 0;
		bg->band_lo = (bg->band_lo + 1) & 63;
		if (bg->band_lo == 0) {
			/* full sweep done: re-roll width/density */
			bg->band_width = 6 + (bg_mwc32(bg) % 15);
			bg->density = bg_mwc32(bg) % (DENSITY_MAX + 1);
			bg->band_step = 1 + (bg_mwc32(bg) % 7);
		}
	}
	return v;
}

uint64_t stress_bitgen_bandwalk64(stress_bitgen_t *bg)
{
	return band_fill(bg);
}

uint32_t stress_bitgen_bandwalk32(stress_bitgen_t *bg)
{
	return (uint32_t)(band_fill(bg) >> 32);
}

/*
 *  stress_bitgen_edge64()
 *	dictionary value with ±1..4 LSB jitter (so the boundary itself
 *	AND its near neighbours are densely sampled).
 */
uint64_t stress_bitgen_edge64(stress_bitgen_t *bg)
{
	uint64_t v = bitgen_edge_dict[bg->edge_idx];

	bg->edge_idx++;
	if (bg->edge_idx >= EDGE_DICT_SIZE)
		bg->edge_idx = 0;

	/* ±1..4 LSB jitter: flip between add/subtract, magnitude 1..4 */
	const uint32_t r = bg_mwc32(bg);
	const uint64_t delta = (uint64_t)1 + (r & 3);

	if (r & 4)
		v += delta;
	else
		v -= delta;
	return v;
}

/*
 *  stress_bitgen_fp64_bits()
 *	raw double bits with independent exponent/mantissa:
 *	  sign:     random
 *	  exponent: 0 (subnormals), 1..2046 swept by cursor, 2047 (inf/nan)
 *	  mantissa: bandwalk 52-bit field
 *	The exponent cursor advances every 64 calls so every exponent
 *	value gets sustained traffic (levers D3/D4).
 */
uint64_t stress_bitgen_fp64_bits(stress_bitgen_t *bg)
{
	const uint64_t mant = band_fill(bg) & 0x000fffffffffffffULL;
	uint64_t expn;
	const uint32_t r = bg_mwc32(bg);

	switch (r & 7) {
	case 0:
		expn = 0;				/* subnormal region */
		break;
	case 7:
		expn = 2047;				/* inf / nan encodings */
		break;
	default:
		/* swept exponent 1..2046, advancing with the band cursor */
		expn = 1 + ((bg->band_lo * 32 + (r >> 8)) % 2046);
		break;
	}
	return ((uint64_t)(r & 0x80000000) << 32) | (expn << 52) | mant;
}

uint32_t stress_bitgen_fp32_bits(stress_bitgen_t *bg)
{
	const uint32_t mant = stress_bitgen_bandwalk32(bg) & 0x007fffff;
	const uint32_t r = bg_mwc32(bg);
	uint32_t expn;

	switch (r & 7) {
	case 0:
		expn = 0;
		break;
	case 7:
		expn = 255;
		break;
	default:
		expn = 1 + ((bg->band_lo * 4 + (r >> 10)) % 254);
		break;
	}
	return (r & 0x80000000) | (expn << 23) | mant;
}

void stress_bitgen_complement_pair64(stress_bitgen_t *bg, uint64_t *a, uint64_t *b)
{
	uint64_t v;

	/* mix edge values and bandwalk so the pair is sometimes a
	 * dictionary boundary and its full complement — the maximal
	 * carry-chain inversion — and sometimes a dense random word */
	if (bg_mwc32(bg) & 1)
		v = stress_bitgen_edge64(bg);
	else
		v = stress_bitgen_bandwalk64(bg);
	*a = v;
	*b = ~v;
}

uint64_t stress_bitgen_hamming64(stress_bitgen_t *bg, const unsigned int target_weight)
{
	unsigned int weight = target_weight & 63;
	uint64_t v = 0;

	if (weight > 32)
		weight = 32;
	while (weight--) {
		/* set a random unset bit; expected work is bounded by
		 * 64/ (remaining zeros) which is fine for weight <= 32 */
		uint32_t bit;
		do {
			bit = bg_mwc32(bg) & 63;
		} while (v & (1ULL << bit));
		v |= 1ULL << bit;
	}
	return v;
}

/*
 *  stress_bitgen_skip()
 *	consume len bytes from the stream (same call sequence the fill
 *	would make) discarding the output.
 */
void stress_bitgen_skip(stress_bitgen_t *bg, const size_t len)
{
	size_t i;
	const size_t words = (len + 7) / 8;

	/* fill_pattern() draws one u64 per 8 bytes; mirror that exactly
	 * so the streams stay aligned */
	for (i = 0; i < words; i++)
		(void)stress_bitgen_u64(bg);
}

/*
 *  stress_bitgen_u64()
 *	mode-mixed default operand: 1/3 bandwalk, 1/3 edge, 1/3
 *	jittered uniform (the uniform third keeps wide coverage so the
 *	dictionaries never become the only thing the silicon sees).
 */
uint64_t stress_bitgen_u64(stress_bitgen_t *bg)
{
	bg->mix++;
	switch (bg->mix % 3) {
	case 0:
		return stress_bitgen_bandwalk64(bg);
	case 1:
		return stress_bitgen_edge64(bg);
	default:
		return bg_mwc64(bg) ^ (uint64_t)bg_mwc32(bg);
	}
}
