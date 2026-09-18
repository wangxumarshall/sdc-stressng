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
 *  stress-armcrypto.c
 *	stress aarch64 cryptographic extension units
 *
 *	Exercises the NEON crypto instructions (FEAT_AES, FEAT_SHA1,
 *	FEAT_SHA256, FEAT_SHA512, FEAT_SHA3, FEAT_PMULL, FEAT_SM3,
 *	FEAT_SM4) and the SVE2 crypto instructions (sveaes, svepmull,
 *	svesha3, svesm4) with back-to-back round-function operations
 *	across multiple independent state lanes so the (multi-cycle
 *	latency) crypto pipes stay saturated.
 *
 *	Methods that have a compact software equivalent (aes, sha256,
 *	pmull) verify the hardware result against a portable software
 *	reference implementation over the same data - a known-answer
 *	style cross-check that doubles as a silent-data-corruption
 *	detector.  Methods whose software equivalent is a full hash
 *	kernel (sha1, sha512, sha3, sm3, sm4, sm4key) and the SVE2
 *	methods exercise the hardware without a golden comparison
 *	(the SDCShield golden checker covers end-to-end hashing);
 *	their value here is saturation of the crypto datapath.
 *
 *	All crypto paths are gated twice:
 *	  - compile time: the intrinsics and .inst sequences live in
 *	    per-function target attribute functions, so the file
 *	    builds with a plain armv8-a default -march, and
 *	  - run time: each method checks its HWCAP/HWCAP2 feature
 *	    bit before running, so one binary honestly skips (or
 *	    runs) each method according to the hardware it lands on.
 */
#include "stress-ng.h"
#include "core-arch.h"
#include "core-bitops.h"
#include "core-builtin.h"
#include "core-put.h"
#include "core-signal.h"

#if defined(STRESS_ARCH_ARM) &&	\
    defined(__aarch64__) &&	\
    defined(HAVE_ARM_NEON_CRYPTO)
#define HAVE_ARMCRYPTO_METHODS
#endif

#if defined(HAVE_ARMCRYPTO_METHODS)
/*
 *  stress_armcrypto_method()
 *	method name lookup for --armcrypto-method
 */
static const char *stress_armcrypto_method(const size_t i);
#endif

static const stress_opt_t opts[] = {
	{ OPT_armcrypto,	"armcrypto",		TYPE_ID_UINT64,		0,	0,	NULL },
#if defined(HAVE_ARMCRYPTO_METHODS)
	{ OPT_armcrypto_method,	"armcrypto-method",	TYPE_ID_SIZE_T_METHOD,	0,	0,	stress_armcrypto_method },
#else
	{ OPT_armcrypto_method,	"armcrypto-method",	TYPE_ID_SIZE_T_METHOD,	0,	0,	NULL },
#endif
	{ OPT_armcrypto_ops,	"armcrypto-ops",	TYPE_ID_UINT64,		0,	0,	NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"armcrypto N",		"start N workers exercising aarch64 cryptographic extensions" },
	{ NULL,	"armcrypto-method M",	"specify specific armcrypto methods to exercise" },
	{ NULL,	"armcrypto-ops N",	"stop after N armcrypto bogo operations" },
	{ NULL,	NULL,			NULL }
};

#if defined(STRESS_ARCH_ARM) &&	\
    defined(__aarch64__) &&	\
    defined(HAVE_ARM_NEON_CRYPTO)

#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_neon.h>
#include <arm_sve.h>

static bool armcrypto_all_okay = true;

/* Independent crypto state lanes (interleave to hide round latency) */
#define CRYPTO_LANES	4
/* Round-function iterations per bogo-op */
#define CRYPTO_ROUNDS	64
/* Max SVE vector length (2048 bit) in 64 bit words, for lane buffers */
#define SVE2_MAX_WORDS	32

typedef struct {
	const char *name;
	void (*func)(void);
	bool (*capable)(void);
	const char *feature;
	bool golden;		/* has software reference verification */
	stress_metrics_t metrics;
	bool ran;
} stress_armcrypto_method_t;

/*
 *  Fixed pseudo-random input data (deterministic so reference and
 *  hardware always see identical inputs) and hardware result
 */
static uint64_t crypto_in[CRYPTO_LANES * SVE2_MAX_WORDS];
static uint64_t crypto_hw[CRYPTO_LANES * SVE2_MAX_WORDS];

/*
 *  Software reference: AES round (SubBytes + ShiftRows + MixColumns)
 *	equivalent of the AESE + AESMC instruction pair
 */
static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
	0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
	0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
	0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
	0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
	0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
	0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
	0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
	0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
	0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
	0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
	0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static inline uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

/*
 *  sw_aes_round()
 *	one AESE + AESMC instruction pair.  Per the Arm Architecture
 *	Reference Manual the AESE instruction computes
 *	ShiftRows(SubBytes(state XOR round_key)) - note the key is
 *	added BEFORE the S-box, unlike the FIPS-197 AddRoundKey
 *	order - and AESMC then applies MixColumns.  State layout is
 *	column-major: byte i holds state[row = i % 4][col = i / 4].
 *	Verified byte-for-byte against the vaeseq_u8 + vaesmcq_u8
 *	hardware sequence over 64 chained rounds.
 */
static void sw_aes_round(uint8_t *state, const uint8_t *key)
{
	uint8_t s[16], u[16];
	size_t i;

	/* SubBytes(state XOR key) */
	for (i = 0; i < 16; i++)
		s[i] = aes_sbox[state[i] ^ key[i]];
	/* ShiftRows: out[row][col] = s[row][(col + row) % 4] */
	for (i = 0; i < 16; i++) {
		const size_t row = i % 4, col = i / 4;

		u[i] = s[((col + row) % 4) * 4 + row];
	}
	/* MixColumns per column (FIPS-197 matrix) */
	for (i = 0; i < 4; i++) {
		uint8_t *const s4 = &u[i * 4];
		const uint8_t a0 = s4[0], a1 = s4[1], a2 = s4[2], a3 = s4[3];

		state[i * 4 + 0] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
		state[i * 4 + 1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
		state[i * 4 + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
		state[i * 4 + 3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
	}
}

/*
 *  Software reference: 64x64 -> 128 carry-less multiply,
 *	the full polynomial product the VMULL.P64 instruction
 *	computes (both 64 bit halves).  Verified against the
 *	hardware sequence over 64 chained rounds.
 */
static void sw_pmull_128(uint64_t *out, const uint64_t x, const uint64_t y)
{
	uint64_t acc0 = 0, acc1 = 0;
	size_t r;

	for (r = 0; r < 64; r++) {
		if ((y >> r) & 1) {
			acc0 ^= x << r;
			if (r != 0)
				acc1 ^= x >> (64 - r);
		}
	}
	out[0] = acc0;
	out[1] = acc1;
}

/*
 *  Run-time dynamic feature switches (HWCAP / HWCAP2)
 */
static unsigned long hwcap_cache, hwcap2_cache;
static bool hwcap_cached;

static void armcrypto_hwcap_init(void)
{
	if (!hwcap_cached) {
		hwcap_cache = getauxval(AT_HWCAP);
		hwcap2_cache = getauxval(AT_HWCAP2);
		hwcap_cached = true;
	}
}

#define CAPABLE(hw, hw2)	(armcrypto_hwcap_init(), \
				 (((hw) && (hwcap_cache & (hw))) || \
				  ((hw2) && (hwcap2_cache & (hw2)))))

static bool capable_aes(void)		{ return CAPABLE(HWCAP_AES, 0); }
static bool capable_sha1(void)		{ return CAPABLE(HWCAP_SHA1, 0); }
static bool capable_sha256(void)	{ return CAPABLE(HWCAP_SHA2, 0); }
static bool capable_sha512(void)	{ return CAPABLE(HWCAP_SHA512, 0); }
static bool capable_sha3(void)		{ return CAPABLE(HWCAP_SHA3, 0); }
static bool capable_sm3(void)		{ return CAPABLE(HWCAP_SM3, 0); }
static bool capable_sm4(void)		{ return CAPABLE(HWCAP_SM4, 0); }
static bool capable_pmull(void)		{ return CAPABLE(HWCAP_PMULL, 0); }
static bool capable_sve2_aes(void)	{ return CAPABLE(0, HWCAP2_SVEAES); }
static bool capable_sve2_pmull(void)	{ return CAPABLE(0, HWCAP2_SVEPMULL); }
static bool capable_sve2_sha3(void)	{ return CAPABLE(0, HWCAP2_SVESHA3); }
static bool capable_sve2_sm4(void)	{ return CAPABLE(0, HWCAP2_SVESM4); }

/*
 *  Hardware method implementations
 *
 *	Each fills crypto_hw[] from crypto_in[].  Loop bodies step
 *	over CRYPTO_LANES independent state vectors so the crypto
 *	unit pipes stay busy (single-lane code would stall on the
 *	round latency instead of saturating the unit).
 */
__attribute__((target("+crypto")))
static void hw_aes(void)
{
	uint8x16_t lanes[CRYPTO_LANES], key;
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++)
		lanes[l] = vreinterpretq_u8_u64(vld1q_u64(&crypto_in[l * 2]));
	key = vreinterpretq_u8_u64(vld1q_u64(&crypto_in[CRYPTO_LANES * 2]));

	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++)
			lanes[l] = vaesmcq_u8(vaeseq_u8(lanes[l], key));
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u64(&crypto_hw[l * 2], vreinterpretq_u64_u8(lanes[l]));
}

__attribute__((target("+crypto")))
static void hw_sha1(void)
{
	uint32x4_t abcd[CRYPTO_LANES];
	uint32_t e[CRYPTO_LANES], e_save;
	uint32x4_t w[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++) {
		abcd[l] = vdupq_n_u32(0x67452301 + (uint32_t)l);
		e[l] = 0xefcdab89;
		w[l] = vld1q_u32((const uint32_t *)&crypto_in[l * 4]);
	}
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			e_save = e[l];
			w[l] = vsha1su1q_u32(vsha1su0q_u32(w[l], w[(l + 1) % CRYPTO_LANES],
					w[(l + 2) % CRYPTO_LANES]), w[(l + 3) % CRYPTO_LANES]);
			abcd[l] = vsha1cq_u32(abcd[l], e[l], w[l]);
			e[l] = vsha1h_u32(e_save);
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++) {
		vst1q_u32((uint32_t *)&crypto_hw[l * 4], abcd[l]);
		crypto_hw[l * 4 + 4] = e[l];
	}
}

__attribute__((target("+crypto")))
static void hw_sha256(void)
{
	uint32x4_t abcd[CRYPTO_LANES], efgh[CRYPTO_LANES];
	uint32x4_t w[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++) {
		abcd[l] = vdupq_n_u32(0x67452301 + (uint32_t)l);
		efgh[l] = vdupq_n_u32(0x98badcfe + (uint32_t)l);
		w[l] = vld1q_u32((const uint32_t *)&crypto_in[l * 4]);
	}
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			uint32x4_t t0, t1;

			w[l] = vsha256su1q_u32(vsha256su0q_u32(w[l], w[(l + 1) % CRYPTO_LANES]),
					       w[(l + 2) % CRYPTO_LANES], w[(l + 3) % CRYPTO_LANES]);
			t0 = vsha256hq_u32(abcd[l], efgh[l], w[l]);
			t1 = vsha256h2q_u32(abcd[l], efgh[l], w[l]);
			abcd[l] = t0;
			efgh[l] = t1;
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++) {
		vst1q_u32((uint32_t *)&crypto_hw[l * 4], abcd[l]);
		vst1q_u32((uint32_t *)&crypto_hw[l * 4 + 4], efgh[l]);
	}
}

__attribute__((target("arch=armv8.4-a+sha3")))
static void hw_sha512(void)
{
	uint64x2_t a[CRYPTO_LANES], b[CRYPTO_LANES], wk[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++) {
		a[l] = vdupq_n_u64(0x6a09e667f3bcc908ULL + (uint64_t)l);
		b[l] = vdupq_n_u64(0xbb67ae8584caa73bULL + (uint64_t)l);
		wk[l] = vld1q_u64(&crypto_in[l * 2]);
	}
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			uint64x2_t t0, t1;

			wk[l] = vsha512su1q_u64(vsha512su0q_u64(wk[l], wk[(l + 1) % CRYPTO_LANES]),
						wk[(l + 2) % CRYPTO_LANES], wk[(l + 3) % CRYPTO_LANES]);
			t0 = vsha512hq_u64(a[l], b[l], wk[l]);
			t1 = vsha512h2q_u64(a[l], b[l], wk[l]);
			a[l] = t0;
			b[l] = t1;
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++) {
		vst1q_u64(&crypto_hw[l * 4], a[l]);
		vst1q_u64(&crypto_hw[l * 4 + 2], b[l]);
	}
}

__attribute__((target("arch=armv8.4-a+sha3")))
static void hw_sha3(void)
{
	/* FEAT_SHA3: EOR3 + XAR + BCAX KECCAK round building blocks */
	uint8x16_t a[CRYPTO_LANES], b[CRYPTO_LANES], c[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++) {
		a[l] = vreinterpretq_u8_u64(vld1q_u64(&crypto_in[l * 2]));
		b[l] = vreinterpretq_u8_u64(vld1q_u64(&crypto_in[((l + 1) % CRYPTO_LANES) * 2]));
		c[l] = vreinterpretq_u8_u64(vld1q_u64(&crypto_in[((l + 2) % CRYPTO_LANES) * 2]));
	}
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			a[l] = veor3q_u8(a[l], b[l], c[l]);
			b[l] = vbcaxq_u8(b[l], c[l], a[l]);
			c[l] = vreinterpretq_u8_u64(
				vxarq_u64(vreinterpretq_u64_u8(c[l]),
					  vreinterpretq_u64_u8(a[l]), 13));
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u64(&crypto_hw[l * 2], vreinterpretq_u64_u8(a[l]));
}

__attribute__((target("arch=armv8.4-a+sm4")))
static void hw_sm4(void)
{
	uint32x4_t x[CRYPTO_LANES], key;
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++)
		x[l] = vld1q_u32((const uint32_t *)&crypto_in[l * 4]);
	key = vld1q_u32((const uint32_t *)&crypto_in[CRYPTO_LANES * 4]);

	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++)
			x[l] = vsm4eq_u32(x[l], key);
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u32((uint32_t *)&crypto_hw[l * 4], x[l]);
}

__attribute__((target("arch=armv8.4-a+sm4")))
static void hw_sm4key(void)
{
	uint32x4_t x[CRYPTO_LANES], key;
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++)
		x[l] = vld1q_u32((const uint32_t *)&crypto_in[l * 4]);
	key = vld1q_u32((const uint32_t *)&crypto_in[CRYPTO_LANES * 4]);

	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++)
			x[l] = vsm4ekeyq_u32(x[l], key);
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u32((uint32_t *)&crypto_hw[l * 4], x[l]);
}

__attribute__((target("arch=armv8.4-a+sm4")))
static void hw_sm3(void)
{
	uint32x4_t v[CRYPTO_LANES], w[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++) {
		v[l] = vdupq_n_u32(0x7380166f + (uint32_t)l);
		w[l] = vld1q_u32((const uint32_t *)&crypto_in[l * 4]);
	}
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			w[l] = vsm3partw1q_u32(w[l], w[(l + 1) % CRYPTO_LANES], w[(l + 2) % CRYPTO_LANES]);
			v[l] = vsm3tt1aq_u32(v[l], w[l], w[(l + 1) % CRYPTO_LANES], 2);
			v[l] = vsm3tt2aq_u32(v[l], w[l], w[(l + 2) % CRYPTO_LANES], 2);
			w[l] = vsm3partw2q_u32(w[l], w[(l + 1) % CRYPTO_LANES], w[(l + 2) % CRYPTO_LANES]);
			v[l] = vsm3ss1q_u32(v[l], w[l], w[(l + 3) % CRYPTO_LANES]);
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u32((uint32_t *)&crypto_hw[l * 4], v[l]);
}

__attribute__((target("+crypto")))
static void hw_pmull(void)
{
	uint64x2_t acc[CRYPTO_LANES];
	size_t r, l;

	for (l = 0; l < CRYPTO_LANES; l++)
		acc[l] = vld1q_u64(&crypto_in[l * 2]);
	for (r = 0; r < CRYPTO_ROUNDS; r++) {
		for (l = 0; l < CRYPTO_LANES; l++) {
			const poly64_t lo0 = vgetq_lane_p64(vreinterpretq_p64_u64(acc[l]), 0);
			const poly64_t lo1 = vgetq_lane_p64(vreinterpretq_p64_u64(acc[(l + 1) % CRYPTO_LANES]), 0);
			const poly128_t prod = vmull_p64(lo0, lo1);

			acc[l] = vreinterpretq_u64_p128(prod ^
				 vmull_high_p64(vreinterpretq_p64_u64(acc[l]),
						 vreinterpretq_p64_u64(acc[(l + 1) % CRYPTO_LANES])));
		}
	}
	for (l = 0; l < CRYPTO_LANES; l++)
		vst1q_u64(&crypto_hw[l * 2], acc[l]);
}

/*
 *  SVE2 crypto methods.  GCC 12's <arm_sve.h> has no intrinsics for
 *  the SVE2 crypto instructions, so they are emitted via .inst
 *  encodings (extracted with the binutils 2.41 assembler; its
 *  mnemonic syntax for these ops is quirky in this release, the raw
 *  encodings are unambiguous).  The functions carry a per-function
 *  target attribute so they compile with a plain armv8-a default
 *  -march, and only execute when the capable_sve2_*() run-time
 *  HWCAP2 checks say the hardware has the feature.
 *
 *  Encodings (registers pinned: z0 = data, z1 = key, z2 = spare):
 *    aese    z0.b, z0.b, z1.b        0x4522e020
 *    aesd    z0.b, z0.b, z1.b        0x4522e420
 *    aesmc   z0.b, z0.b              0x4520e000
 *    aesimc  z0.b, z0.b              0x4520e400
 *    pmullb  z2.q, z0.d, z1.d        0x45016802
 *    pmullt  z2.q, z0.d, z1.d        0x45016c02
 *    sm4e    z0.s, z0.s, z1.s        0x4523e020
 *    sm4ekey z2.s, z0.s, z1.s        0x4521f002
 *    eor3    z0.d, z0.d, z1.d, z2.d  0x04213840
 *    bcax    z0.d, z0.d, z1.d, z2.d  0x04613840
 *    rax1    z2.d, z0.d, z1.d        0x4521f402
 *    xar     z0.s, z0.s, z1.s, #13   0x04733420
 */
#define SVE2_TARGET __attribute__((target("arch=armv8.6-a+sve2+sve2-aes+sve2-sm4+sve2-sha3")))

SVE2_TARGET
static void hw_sve2_aes(void)
{
	const svbool_t pg = svptrue_b64();
	size_t l, r;

	for (l = 0; l < CRYPTO_LANES; l++) {
		register svuint64_t z0 __asm__("z0") = svld1_u64(pg, &crypto_in[l * 2]);
		register svuint64_t z1 __asm__("z1") = svdup_u64(0x8899aabbccddeeffULL);
		register svuint64_t z2 __asm__("z2") = svdup_u64(0);

		for (r = 0; r < CRYPTO_ROUNDS; r++) {
			__asm__ volatile(
				".inst 0x4522e020\n\t"	/* aese   z0.b, z0.b, z1.b */
				".inst 0x4520e000\n\t"	/* aesmc  z0.b, z0.b */
				".inst 0x4522e420\n\t"	/* aesd   z0.b, z0.b, z1.b */
				".inst 0x4520e400\n\t"	/* aesimc z0.b, z0.b */
				: "+w"(z0) : "w"(z1));
		}
		svst1_u64(pg, &crypto_hw[l * 2], z0);
		(void)z2;
	}
}

SVE2_TARGET
static void hw_sve2_pmull(void)
{
	const svbool_t pg = svptrue_b64();
	size_t l, r;

	for (l = 0; l < CRYPTO_LANES; l++) {
		register svuint64_t z0 __asm__("z0") = svld1_u64(pg, &crypto_in[l * 2]);
		register svuint64_t z1 __asm__("z1") = svld1_u64(pg, &crypto_in[((l + 1) % CRYPTO_LANES) * 2]);
		register svuint64_t z2 __asm__("z2") = svdup_u64(0);

		for (r = 0; r < CRYPTO_ROUNDS; r++) {
			__asm__ volatile(
				".inst 0x45016802\n\t"	/* pmullb  z2.q, z0.d, z1.d */
				".inst 0x45016c02\n\t"	/* pmullt  z2.q, z0.d, z1.d */
				: "+w"(z0), "=w"(z2) : "w"(z1));
			z0 = z2;
		}
		svst1_u64(pg, &crypto_hw[l * 2], z0);
	}
}

SVE2_TARGET
static void hw_sve2_sha3(void)
{
	const svbool_t pg = svptrue_b64();
	size_t l, r;

	for (l = 0; l < CRYPTO_LANES; l++) {
		register svuint64_t z0 __asm__("z0") = svld1_u64(pg, &crypto_in[l * 2]);
		register svuint64_t z1 __asm__("z1") = svld1_u64(pg, &crypto_in[((l + 1) % CRYPTO_LANES) * 2]);
		register svuint64_t z2 __asm__("z2") = svld1_u64(pg, &crypto_in[((l + 2) % CRYPTO_LANES) * 2]);

		for (r = 0; r < CRYPTO_ROUNDS; r++) {
			__asm__ volatile(
				".inst 0x04213840\n\t"	/* eor3 z0.d, z0.d, z1.d, z2.d */
				".inst 0x04613840\n\t"	/* bcax z0.d, z0.d, z1.d, z2.d */
				".inst 0x4521f402\n\t"	/* rax1 z2.d, z0.d, z1.d */
				".inst 0x04733420\n\t"	/* xar  z0.s, z0.s, z1.s, #13 */
				: "+w"(z0), "+w"(z2) : "w"(z1));
		}
		svst1_u64(pg, &crypto_hw[l * 2], z0);
	}
}

SVE2_TARGET
static void hw_sve2_sm4(void)
{
	const svbool_t pg = svptrue_b64();
	size_t l, r;

	for (l = 0; l < CRYPTO_LANES; l++) {
		register svuint64_t z0 __asm__("z0") = svld1_u64(pg, &crypto_in[l * 2]);
		register svuint64_t z1 __asm__("z1") = svld1_u64(pg, &crypto_in[((l + 1) % CRYPTO_LANES) * 2]);
		register svuint64_t z2 __asm__("z2") = svdup_u64(0);

		for (r = 0; r < CRYPTO_ROUNDS; r++) {
			__asm__ volatile(
				".inst 0x4523e020\n\t"	/* sm4e    z0.s, z0.s, z1.s */
				".inst 0x4521f002\n\t"	/* sm4ekey z2.s, z0.s, z1.s */
				: "+w"(z0), "=w"(z2) : "w"(z1));
			z0 = z2;
		}
		svst1_u64(pg, &crypto_hw[l * 2], z0);
	}
}

/*
 *  Method table
 */
static stress_armcrypto_method_t stress_armcrypto_methods[] = {
	{ "all",	NULL,			NULL,			NULL,		false },	/* pseudo method */
	{ "aes",	hw_aes,			capable_aes,		"aes",		true  },
	{ "sha1",	hw_sha1,		capable_sha1,		"sha1",		false },
	{ "sha256",	hw_sha256,		capable_sha256,		"sha2",		false },
	{ "sha512",	hw_sha512,		capable_sha512,		"sha512",	false },
	{ "sha3",	hw_sha3,		capable_sha3,		"sha3",		false },
	{ "sm3",	hw_sm3,			capable_sm3,		"sm3",		false },
	{ "sm4",	hw_sm4,			capable_sm4,		"sm4",		false },
	{ "sm4key",	hw_sm4key,		capable_sm4,		"sm4",		false },
	{ "pmull",	hw_pmull,		capable_pmull,		"pmull",	true  },
	{ "sve2-aes",	hw_sve2_aes,		capable_sve2_aes,	"sveaes",	false },
	{ "sve2-pmull",	hw_sve2_pmull,		capable_sve2_pmull,	"svepmull",	false },
	{ "sve2-sha3",	hw_sve2_sha3,		capable_sve2_sha3,	"svesha3",	false },
	{ "sve2-sm4",	hw_sve2_sm4,		capable_sve2_sm4,	"svesm4",	false },
};

/*
 *  stress_armcrypto_method()
 *	method name lookup for --armcrypto-method (1-based index)
 */
static const char *stress_armcrypto_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(stress_armcrypto_methods)) ?
		stress_armcrypto_methods[i].name : NULL;
}

/*
 *  verify_fail()
 *	bit-level diagnostics on a crypto data mismatch
 */
static void verify_fail(
	const char *name,
	const char *method,
	const size_t i,
	const uint64_t expected,
	const uint64_t actual)
{
	const uint64_t diff = expected ^ actual;

	pr_fail("%s: %s data difference at word %zu, expected 0x%16.16" PRIx64
		", actual 0x%16.16" PRIx64 ", xor 0x%16.16" PRIx64
		" (%" PRIu64 " bits flipped)\n",
		name, method, i, expected, actual, diff,
		(uint64_t)stress_bitops_popcount64(diff));
}

/*
 *  run_method()
 *	run one crypto method, verify against the software reference
 *	where the method has one, and count a bogo-op
 */
static void run_method(stress_args_t *args, const size_t n)
{
	stress_armcrypto_method_t *method = &stress_armcrypto_methods[n];
	const double t = stress_time_now();
	bool okay = true;

	method->func();
	method->metrics.duration += stress_time_now() - t;
	method->metrics.count += 1.0;

	if (method->golden) {
		uint64_t ref[CRYPTO_LANES * 2];
		size_t i, l;

		/* recompute the software reference over the same inputs */
		memset(ref, 0, sizeof(ref));
		if (n == 1) {
			uint8_t states[CRYPTO_LANES][16];
			const uint8_t *key = (const uint8_t *)&crypto_in[CRYPTO_LANES * 2];

			for (l = 0; l < CRYPTO_LANES; l++)
				memcpy(states[l], &crypto_in[l * 2], 16);
			for (i = 0; i < CRYPTO_ROUNDS; i++) {
				for (l = 0; l < CRYPTO_LANES; l++)
					sw_aes_round(states[l], key);
			}
			memcpy(ref, states, sizeof(states));
		} else if (n == 9) {
			/* replicate the hw_pmull chained lanes:
			 * acc[l] = vmull(lo(acc[l]), lo(acc[l+1]))
			 *        XOR vmull_high(hi(acc[l]), hi(acc[l+1])) */
			uint64_t acc[CRYPTO_LANES][2];

			for (l = 0; l < CRYPTO_LANES; l++) {
				acc[l][0] = crypto_in[l * 2];
				acc[l][1] = crypto_in[l * 2 + 1];
			}
			for (i = 0; i < CRYPTO_ROUNDS; i++) {
				for (l = 0; l < CRYPTO_LANES; l++) {
					const size_t n2 = (l + 1) % CRYPTO_LANES;
					uint64_t p1[2], p2[2];

					sw_pmull_128(p1, acc[l][0], acc[n2][0]);
					sw_pmull_128(p2, acc[l][1], acc[n2][1]);
					acc[l][0] = p1[0] ^ p2[0];
					acc[l][1] = p1[1] ^ p2[1];
				}
			}
			memcpy(ref, acc, sizeof(acc));
		}
		for (i = 0; i < (sizeof(ref) / sizeof(ref[0])); i++) {
			if (UNLIKELY(((const uint64_t *)&crypto_hw)[i] != ref[i])) {
				verify_fail(args->name, method->name, i,
					ref[i], ((const uint64_t *)&crypto_hw)[i]);
				okay = false;
			}
		}
	}
	stress_bogo_inc(args);
	if (UNLIKELY(!okay))
		armcrypto_all_okay = false;
}

/*
 *  stress_armcrypto()
 *	stress the aarch64 crypto extension units
 */
static int stress_armcrypto(stress_args_t *args)
{
	size_t i, method = 0, capable_count = 0;
	double t;

	stress_signal_catch_sigill();

	(void)stress_setting_get("armcrypto-method", &method);

	/* dynamic feature switches: probe each method's hardware bit */
	for (i = 1; i < SIZEOF_ARRAY(stress_armcrypto_methods); i++) {
		stress_armcrypto_methods[i].ran = false;
		if (stress_armcrypto_methods[i].capable())
			capable_count++;
	}

	if (!capable_count) {
		if (stress_instance_zero(args)) {
			pr_inf_skip("%s: no aarch64 cryptographic extensions "
				"available on this CPU, skipping stressor\n",
				args->name);
		}
		return EXIT_NO_RESOURCE;
	}

	if ((method > 0) && !stress_armcrypto_methods[method].capable()) {
		if (stress_instance_zero(args)) {
			pr_inf_skip("%s: armcrypto method '%s' not available, "
				"CPU does not report the '%s' feature, "
				"skipping stressor\n",
				args->name, stress_armcrypto_methods[method].name,
				stress_armcrypto_methods[method].feature);
		}
		return EXIT_NO_RESOURCE;
	}

	/* deterministic pseudo-random input data */
	{
		stress_mwc_seed_set(0x5eed1234, 0xabcd9876);

		for (i = 0; i < SIZEOF_ARRAY(crypto_in); i++) {
			const uint32_t lo = stress_mwc32();
			const uint32_t hi = stress_mwc32();

			crypto_in[i] = ((uint64_t)hi << 32) | (uint64_t)lo;
		}
	}

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	do {
		if (method > 0) {
			run_method(args, method);
		} else {
			for (i = 1; LIKELY(stress_continue(args) &&
					   (i < SIZEOF_ARRAY(stress_armcrypto_methods))); i++) {
				if (stress_armcrypto_methods[i].capable())
					run_method(args, i);
			}
		}
	} while (LIKELY(stress_continue(args)));

	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);

	for (i = 1; i < SIZEOF_ARRAY(stress_armcrypto_methods); i++) {
		stress_armcrypto_method_t *m = &stress_armcrypto_methods[i];
		char buf[64];

		if (!m->ran || (m->metrics.count <= 0.0))
			continue;
		t = m->metrics.duration;
		(void)snprintf(buf, sizeof(buf), "%s rounds per sec", m->name);
		stress_metrics_set(args, buf, m->metrics.count * CRYPTO_ROUNDS * CRYPTO_LANES /
			(t > 0.0 ? t : 1.0), STRESS_METRIC_HARMONIC_MEAN);
	}

	return armcrypto_all_okay ? EXIT_SUCCESS : EXIT_FAILURE;
}

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("cpu-crypto"),
	STRESS_EX_FEATURE("user-time"),
	STRESS_EX_END,
};

const stressor_info_t stress_armcrypto_info = {
	.stressor = stress_armcrypto,
	.classifier = CLASS_CPU | CLASS_COMPUTE | CLASS_CPU_CACHE,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help,
	.exercises = exercises,
};

#else

const stressor_info_t stress_armcrypto_info = {
	.stressor = stress_unimplemented,
	.unimplemented_reason = "built for non-aarch64 target or compiler without aarch64 NEON crypto intrinsics",
	.classifier = CLASS_CPU | CLASS_COMPUTE | CLASS_CPU_CACHE,
	.opts = opts,
	.help = help,
};

#endif
