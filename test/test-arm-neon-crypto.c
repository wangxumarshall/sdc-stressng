/*
 * Copyright (C) 2026 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

/*
 *  Probe for aarch64 NEON cryptographic extension intrinsics.
 *  The intrinsics (vaeseq_u8, vsha256hq_u32, vmull_p64, ...)
 *  are emitted when the function is annotated with the
 *  __attribute__((target("+crypto"))) attribute, so no global
 *  -march change is required for the probe to compile on a
 *  default (plain armv8-a) toolchain.
 *
 *  Note the probe only checks that the toolchain can emit the
 *  instructions; the stressor run-time path is additionally
 *  gated by HWCAP feature bits, so the produced binary runs
 *  (and skips honestly) on hardware with or without each
 *  crypto extension.
 */
#if defined(__aarch64__)

#include <arm_neon.h>

__attribute__((target("+crypto")))
static uint32_t crypto_round(void)
{
	uint8x16_t d = vdupq_n_u8(0), k = vdupq_n_u8(1);
	uint32x4_t a = vdupq_n_u32(1), b = vdupq_n_u32(2), c = vdupq_n_u32(3);
	uint64x2_t x = vdupq_n_u64(5), y = vdupq_n_u64(6);
	poly128_t p;

	/* FEAT_AES + FEAT_SHA1 + FEAT_SHA256 + FEAT_PMULL */
	d = vaesmcq_u8(vaeseq_u8(d, k));
	a = vsha1cq_u32(a, 7, b);
	b = vsha256hq_u32(a, b, c);
	c = vsha256su0q_u32(a, b);
	p = vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u8(d), 0),
		      vgetq_lane_p64(vreinterpretq_p64_u8(k), 0));

	return vaddvq_u32(a) + vaddvq_u32(b) + vaddvq_u32(c) +
	       (uint32_t)vaddvq_u64(x) + (uint32_t)vaddvq_u64(y) +
	       (uint32_t)(p & 0xff);
}

int main(void)
{
	return (int)(crypto_round() & 1);
}
#else
#error not aarch64, no NEON crypto intrinsics
#endif
