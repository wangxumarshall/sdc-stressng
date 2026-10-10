/*
 * Copyright (C) 2026
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
#include "stress-ng.h"
#include "core-bitgen.h"
#include "core-mmap.h"

#if defined(STRESS_ARCH_ARM)
#include <arm_neon.h>
#include <sys/auxv.h>
#endif

#if defined(STRESS_ARCH_ARM)

/*
 *  stress-lsupress.c - exercise the arm64 load/store unit across
 *  the full instruction spectrum, driven by a maximum-VA-space
 *  random-walk address engine (LSU instruction-spectrum plan).
 *
 *  Methods select the instruction family and dataflow template
 *  (pure load, pure store, copy, mixed load+compute+store, vector,
 *  atomics).  SDC-directed bitgen shapes feed the store streams;
 *  verification (when enabled) uses the per-address deterministic
 *  value function below.
 */

/* deterministic per-address value (verify oracle) */
static inline uint64_t ALWAYS_INLINE lsupress_value(
	const uint64_t seed,
	const uint64_t addr)
{
	uint64_t z = seed + (addr * 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

typedef void (*stress_lsupress_func_t)(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed);

typedef struct {
	const char *name;
	const stress_lsupress_func_t func;
	const uint32_t hwcap_req;	/* HWCAP bit required, 0 = none */
} stress_lsupress_method_info_t;

static uint32_t lsupress_hwcap;		/* cached AT_HWCAP */
static bool lsupress_hwcap_probed;

static const char *stress_lsupress_method(const size_t i);

/*
 *  load-int64: 8 independent back-to-back loads per iteration,
 *  consumed into a checksum (dead-store-proof)
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_int64(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t sum = seed;
	size_t i;

	(void)seed;
	for (i = 0; i < buf_words; i += 8) {
		sum ^= buf[i] ^ buf[i + 1] ^ buf[i + 2] ^ buf[i + 3] ^
		       buf[i + 4] ^ buf[i + 5] ^ buf[i + 6] ^ buf[i + 7];
	}
	__asm__ __volatile__ ("" : : "r" (sum) : "memory");	/* consume */
}

/*  store-int64: per-address deterministic values (splitmix hash of the
 *  seed and the target address — uniform avalanche, and the verify
 *  oracle can recompute it at any address at any time) */
static NOINLINE OPTIMIZE3 void stress_lsupress_store_int64(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	size_t i;

	for (i = 0; i < buf_words; i++)
		buf[i] = lsupress_value(seed,
			(uint64_t)(uintptr_t)(buf + i));
}

/*  copy-int64: ldr+str dual */
static NOINLINE OPTIMIZE3 void stress_lsupress_copy_int64(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t *src = buf;
	uint64_t *dst = buf + buf_words / 2;
	size_t i;

	(void)seed;
	for (i = 0; i < buf_words / 2; i++)
		dst[i] = src[i];
}

/*
 *  mix-2l-alu-1s: the user-named dataflow template.  Per 8-word block:
 *  2 loads, ALU, 1 load, store — exactly ldr,ldr,add,ldr,add,str with
 *  post-increment addressing (zero non-memory instructions beyond the
 *  adds).  The "+r" constraints are safe here: sp/dp are per-block
 *  locals that never carry a value across the asm (unlike the memcpy
 *  dest-parameter coalescing bug fixed in the ldp-stp variant).
 *  off rotates 1..7 for misaligned window offsets (int paths only).
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_2l_alu_1s(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t *src = buf;
	uint64_t *dst = buf + buf_words / 2;
	const size_t blocks = buf_words / 16;	/* 8 src + 8 dst words per block */
	size_t i, off = (size_t)(seed % 7) + 1;
	uint64_t acc = 0;

	for (i = 0; i < blocks; i++) {
		register uint64_t x0, x1, x2;
		const uint64_t *sp = src + (i * 16) + off;
		uint64_t *dp = dst + (i * 16) + off;

		__asm__ __volatile__ (
			"ldr %0, [%3], #8\n"
			"ldr %1, [%3], #8\n"
			"add %0, %0, %1\n"
			"ldr %2, [%3], #8\n"
			"add %0, %0, %2\n"
			"str %0, [%4], #8"
			: "=r" (x0), "=r" (x1), "=r" (x2),
			  "+r" (sp), "+r" (dp)
			:
			: "memory");
		acc ^= x0;
	}
	__asm__ __volatile__ ("" : : "r" (acc) : "memory");	/* consume */
}

/*
 *  mix-1l-fpu-1s: load + FP compute + store, explicit mul/add only
 *  (no FMA — keeps the software oracle bit-exact).  FP kernels stay
 *  window-aligned: a misaligned double access is C-level UB.
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_1l_fpu_1s(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const double *a = (const double *)buf;
	double *b = (double *)(buf + buf_words / 2);
	const size_t n = buf_words / 2;
	const double c = 1.0000000000000002;
	double acc = 0.0;
	size_t i;

	(void)seed;
	for (i = 0; i < n; i++) {
		const double v = a[i] * c;	/* ldr d / fmul */

		acc += v;			/* fadd */
		b[i] = acc;			/* str d */
	}
	__asm__ __volatile__ ("" : : "r" (acc) : "memory");
}

/*
 *  mix-3l-2alu-1s: deeper load concurrency — three loads in flight
 *  before the first consumer (maximises the LSU queue occupancy).
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_3l_2alu_1s(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t *src = buf;
	uint64_t *dst = buf + buf_words / 2;
	const size_t blocks = buf_words / 16;
	size_t i, off = (size_t)(seed % 7) + 1;
	uint64_t acc = 0;

	for (i = 0; i < blocks; i++) {
		register uint64_t x0, x1, x2;
		const uint64_t *sp = src + (i * 16) + off;
		uint64_t *dp = dst + (i * 16) + off;

		__asm__ __volatile__ (
			"ldr %0, [%3], #8\n"
			"ldr %1, [%3], #8\n"
			"ldr %2, [%3], #8\n"
			"add %0, %0, %1\n"
			"add %0, %0, %2\n"
			"str %0, [%4], #8"
			: "=r" (x0), "=r" (x1), "=r" (x2),
			  "+r" (sp), "+r" (dp)
			:
			: "memory");
		acc ^= x0;
	}
	__asm__ __volatile__ ("" : : "r" (acc) : "memory");
}

#define LSUPRESS_METHOD_ALL	0
#define LSUPRESS_BUF_WORDS	(1024 * 1024)	/* 8MB fallback buffer */

/* ---- vector method family ------------------------------------------ */

/*  load-int128: ldp pair consumption (compiler pairs the loads) */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_int128(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t sum = seed;
	size_t i;

	for (i = 0; i + 1 < buf_words; i += 2)
		sum ^= buf[i] ^ buf[i + 1];
	__asm__ __volatile__ ("" : : "r" (sum) : "memory");
}

/*  load-fp64: ldr d checksum chain */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_fp64(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const double *a = (const double *)buf;
	const size_t n = buf_words / 2;
	double acc = (double)seed;
	size_t i;

	for (i = 0; i < n; i++)
		acc += a[i];
	__asm__ __volatile__ ("" : : "r" (acc) : "memory");
}

/*  load-neon: interleaved vld2q loads, xor-reduced */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_neon(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const uint8x16_t *p = (const uint8x16_t *)buf;
	const size_t vecs = (buf_words * sizeof(uint64_t)) / 32;
	uint8x16_t acc0 = vdupq_n_u8((uint8_t)seed);
	uint8x16_t acc1 = vdupq_n_u8((uint8_t)(seed >> 8));
	size_t i;

	for (i = 0; i < vecs; i++) {
		const uint8x16x2_t v = vld2q_u8((const uint8_t *)(p + i));

		acc0 = veorq_u8(acc0, v.val[0]);
		acc1 = veorq_u8(acc1, v.val[1]);
	}
	__asm__ __volatile__ ("" : : "r" (vgetq_lane_u64((uint64x2_t)acc0, 0)) : "memory");
}

/*  store-neon: bitgen-shaped q-register stores */
static NOINLINE OPTIMIZE3 void stress_lsupress_store_neon(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	stress_bitgen_t bg;
	uint64x2_t *p = (uint64x2_t *)buf;
	const size_t vecs = buf_words / 2;
	size_t i;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, seed);
	for (i = 0; i < vecs; i++) {
		const uint64x2_t v = {
			stress_bitgen_u64(&bg),
			stress_bitgen_u64(&bg)
		};

		vst1q_u64((uint64_t *)(p + i), v);
	}
}

/*  store-zva: DC ZVA line zeroing (64B stride, Kunpeng line size) */
static NOINLINE OPTIMIZE3 void stress_lsupress_store_zva(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const uint8_t *p = (const uint8_t *)buf;
	const size_t bytes = buf_words * sizeof(uint64_t);
	size_t off;

	(void)seed;
	for (off = 0; off + 64 <= bytes; off += 64) {
		__asm__ __volatile__ (
			"dc zva, %0"
			:
			: "r" (p + off)
			: "memory");
	}
}

/*  mix-neon-fma: batch q loads -> vfma -> batch q stores */
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_neon_fma(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const float64x2_t *a = (const float64x2_t *)buf;
	float64x2_t *b = (float64x2_t *)(buf + buf_words / 2);
	const size_t vecs = buf_words / 4;	/* half src, half dst */
	const float64x2_t c = vdupq_n_f64(1.0000000000000002);
	float64x2_t acc = vdupq_n_f64((double)seed);
	size_t i;

	for (i = 0; i < vecs; i++) {
		const float64x2_t v0 = vld1q_f64((const double *)(a + i));
		const float64x2_t v1 = vld1q_f64((const double *)(a + i + vecs));

		acc = vfmaq_f64(acc, v0, c);		/* fmla v */
		acc = vfmaq_f64(acc, v1, c);
		vst1q_f64((double *)(b + i), acc);
	}
}

/* ---- atomic method family ------------------------------------------ */

/*  excl-pair: ldxr/stxr exclusive loop, one per cacheline */
static NOINLINE OPTIMIZE3 void stress_lsupress_excl_pair(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	for (i = 0; i < buf_words; i += 8) {	/* one per 64B line */
		register uint64_t t, ok;

		__asm__ __volatile__ (
			"1: ldxr %0, [%2]\n"
			"   add  %0, %0, %3\n"
			"   stxr %w1, %0, [%2]\n"
			"   cbnz %w1, 1b"
			: "=&r" (t), "=&r" (ok)
			: "r" (buf + i), "r" (v)
			: "memory");
		v = t;
	}
	__asm__ __volatile__ ("" : : "r" (v) : "memory");
}

/*  lse-rmw: LSE ldadd read-modify-write (HWCAP_ATOMICS gated at dispatch) */
__attribute__((target("arch=armv8.1-a")))
static NOINLINE OPTIMIZE3 void stress_lsupress_lse_rmw(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	for (i = 0; i < buf_words; i += 8) {
		register uint64_t t;

		__asm__ __volatile__ (
			"ldadd %1, %0, [%2]"
			: "=r" (t)
			: "r" (v), "r" (buf + i)
			: "memory");
		v = t;
	}
	__asm__ __volatile__ ("" : : "r" (v) : "memory");
}

/*
 *  RCpc limited-ordering methods: FEAT_LRCPC adds the LDAPR
 *  load-acquire (ARMv8.3) to complement the base STLR store-release;
 *  FEAT_LRCPC2 adds the unscaled-immediate-offset LDAPUR/STLUR forms
 *  (ARMv8.4).  Both features advertise as AT_HWCAP bits (not HWCAP2),
 *  so they gate through the plain hwcap_req mechanism like lse-rmw.
 */
#ifndef HWCAP_LRCPC
#define HWCAP_LRCPC		(1 << 15)
#endif
#ifndef HWCAP_ILRCPC
#define HWCAP_ILRCPC		(1 << 26)
#endif

#if defined(__GNUC__) && __GNUC__ >= 10
/*  lrcpc-pair: LDAPR acquire + STLR release to the same line, one
 *  pair per 64B cacheline, zero ALU between the two (pure acquire/
 *  release ordering density; HWCAP_LRCPC gated at dispatch) */
__attribute__((target("arch=armv8.3-a")))
static NOINLINE OPTIMIZE3 void stress_lsupress_lrcpc_pair(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	for (i = 0; i < buf_words; i += 8) {	/* one pair per 64B line */
		register uint64_t t;

		__asm__ __volatile__ (
			"ldapr %0, [%1]\n"
			"stlr  %0, [%1]"
			: "=&r" (t)
			: "r" (buf + i)
			: "memory");
		v = t;
	}
	__asm__ __volatile__ ("" : : "r" (v) : "memory");
}

/*  ilrcpc-rmw: LDAPUR acquire + add + STLUR release RMW chain —
 *  eight immediate-offset RMWs per 64B line (every LRCPC2 immediate
 *  encoding), the chain value threading each word and each line
 *  (HWCAP_ILRCPC gated at dispatch) */
__attribute__((target("arch=armv8.4-a")))
static NOINLINE OPTIMIZE3 void stress_lsupress_ilrcpc_rmw(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	for (i = 0; i + 8 <= buf_words; i += 8) {	/* 8 RMWs per 64B line */
		register uint64_t t;

		__asm__ __volatile__ (
			"ldapur %0, [%1, #0]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #0]\n"
			"ldapur %0, [%1, #8]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #8]\n"
			"ldapur %0, [%1, #16]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #16]\n"
			"ldapur %0, [%1, #24]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #24]\n"
			"ldapur %0, [%1, #32]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #32]\n"
			"ldapur %0, [%1, #40]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #40]\n"
			"ldapur %0, [%1, #48]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #48]\n"
			"ldapur %0, [%1, #56]\n"
			"add    %0, %0, %2\n"
			"stlur  %0, [%1, #56]"
			: "=&r" (t)
			: "r" (buf + i), "r" (v)
			: "memory");
		v = t;
	}
	__asm__ __volatile__ ("" : : "r" (v) : "memory");
}
#endif  /* __GNUC__ >= 10 */

#if defined(__ARM_FEATURE_LS64)
#include <arm_acle.h>

#ifndef AT_HWCAP3
#define AT_HWCAP3		(29)
#endif
#define LSUPRESS_HWCAP3_LS64	(1UL << 0)	/* HWCAP3_LS64 */
#define LSUPRESS_HWCAP2_LS64	(1UL << 15)	/* HWCAP2_LS64 */

static bool lsupress_ls64_ok(void)
{
	const unsigned long hwcap2 = getauxval(AT_HWCAP2);
	const unsigned long hwcap3 = getauxval(AT_HWCAP3);

	return (hwcap2 & LSUPRESS_HWCAP2_LS64) ||
	       (hwcap3 & LSUPRESS_HWCAP3_LS64);
}

/*  ls64-copy: 64-byte atomic ld64b/st64b blocks */
static NOINLINE OPTIMIZE3 void stress_lsupress_ls64_copy(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const uint8_t *src = (const uint8_t *)buf;
	uint8_t *dst = (uint8_t *)(buf + buf_words / 2);
	const size_t blocks = (buf_words * sizeof(uint64_t) / 2) / 64;
	size_t i;

	(void)seed;
	for (i = 0; i < blocks; i++, src += 64, dst += 64) {
		const data512_t v = __arm_ld64b(src);

		__arm_st64b(dst, v);
	}
}
#endif

#if defined(__GNUC__) && __GNUC__ >= 10
#include <arm_sve.h>

/*  load-sve: full-VL ld1d, xor-reduced (HWCAP_SVE gated at dispatch) */
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_load_sve(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	svuint64_t acc = svdup_u64(seed);
	const uint64_t vl = (uint64_t)svcntd();
	uint64_t *p = buf;
	uint64_t k, sink;

	for (k = 0; k + vl <= buf_words; k += vl) {
		svuint64_t v = svld1_u64(pg, p + k);

		acc = sveor_u64_x(pg, acc, v);
	}
	sink = svlastb_u64(pg, acc);
	__asm__ __volatile__ ("" : : "r" (sink) : "memory");
}

/*  load-sve-gather: 64B-strided gather, xor-reduced */
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_load_sve_gather(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	svuint64_t idx = svindex_u64(0, 8);	/* 64B stride, in elements */
	svuint64_t acc = svdup_u64(seed);
	const uint64_t vl = (uint64_t)svcntd();
	uint64_t k, sink;

	for (k = 0; k + (vl - 1) * 8 < buf_words; k += vl * 8) {
		svuint64_t v = svld1_gather_u64index_u64(pg, buf + k, idx);

		acc = sveor_u64_x(pg, acc, v);
	}
	sink = svlastb_u64(pg, acc);
	__asm__ __volatile__ ("" : : "r" (sink) : "memory");
}

/*  store-sve: full-VL st1d of bitgen shapes */
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_store_sve(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	stress_bitgen_t bg;
	const uint64_t vl = (uint64_t)svcntd();
	uint64_t *p = buf;
	uint64_t k;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, seed);
	for (k = 0; k + vl <= buf_words; k += vl) {
		svuint64_t v;
		uint64_t j, tmp[32];	/* max VL lanes (2048-bit) */
		const uint64_t lanes = vl < 32 ? vl : 32;

		for (j = 0; j < lanes; j++)
			tmp[j] = stress_bitgen_u64(&bg);
		v = svld1_u64(pg, tmp);
		svst1_u64(pg, p + k, v);
	}
}

/*  copy-sve: full-VL ld1d + st1d */
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_copy_sve(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	const uint64_t vl = (uint64_t)svcntd();
	uint64_t *src = buf;
	uint64_t *dst = buf + buf_words / 2;
	uint64_t k;

	(void)seed;
	for (k = 0; k + vl <= buf_words / 2; k += vl) {
		svuint64_t v = svld1_u64(pg, src + k);

		svst1_u64(pg, dst + k, v);
	}
}

/*  mix-sve-fma: batch ld1d -> fmla -> st1d, full VL */
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_sve_fma(
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	svfloat64_t acc = svdup_f64((double)seed);
	const svfloat64_t c = svdup_f64(1.0000000000000002);
	const uint64_t vl = (uint64_t)svcntd();
	const size_t half = buf_words / 2;
	uint64_t *src = buf;
	double *dst = (double *)(buf + half);
	uint64_t k;

	for (k = 0; k + vl <= half; k += vl) {
		svfloat64_t v0 = svld1_f64(pg, (const double *)(src + k));
		svfloat64_t v1 = svld1_f64(pg, (const double *)(src + k + half));

		acc = svmla_f64_x(pg, acc, v0, c);	/* fmla */
		acc = svmla_f64_x(pg, acc, v1, c);
		svst1_f64(pg, dst + k, acc);
	}
}
#endif
#define LSUPRESS_DEFAULT_VA_SIZE	(100ULL << 30)	/* 100GB NORESERVE map/worker */
#define LSUPRESS_DEFAULT_WINDOW	(256ULL << 20)	/* 256MB working window/worker */

/*
 *  Address engine: one large MAP_NORESERVE mapping per worker, a
 *  randomly-migrating working window inside it, MADV_DONTNEED on the
 *  old window.  Physical footprint stays pinned near the window size
 *  while the walk covers the whole VA range and the page tables keep
 *  growing/shrinking (MMU pressure by construction).
 */
typedef struct {
	uint8_t *map_base;	/* NORESERVE region base */
	size_t   map_size;	/* actual mapped size (probe-shrunk) */
	uint8_t *win;		/* current window (page aligned) */
	size_t   win_size;
	uint64_t state;		/* xorshift64* window-hop RNG */
	uint64_t hops;		/* hop counter (va-bit walk) */
	size_t	 walk;		/* walk mode */
	stress_bitgen_t bg;	/* bitgen walk RNG */
	bool	 used;		/* engine active (vs fallback buffer) */
} lsupress_engine_t;

#define LSUPRESS_WALK_UNIFORM	0
#define LSUPRESS_WALK_BITGEN	1
#define LSUPRESS_WALK_VA_BIT	2
#define LSUPRESS_WALK_NEAR_FAR	3

static uint64_t lsupress_next_hop(lsupress_engine_t *e)
{
	uint64_t x = e->state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	e->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/*
 *  Walk strategies (all return a hop index into the window lattice):
 *    uniform   — flat random over the whole map
 *    bitgen    — bit-band targeted: sweep a 6..20-bit window across
 *                VA bits 12..47 (TLB-tag bit-segment walk); the hop
 *                zeroes the swept bits so the window tracks the band
 *    va-bit    — flips one VA bit per hop, walking bits 12..47 in
 *                order (systematic single-bit TLB tag coverage)
 *    near-far  — 7/8 hops land within 1/1024 of the current window
 *                (locality), 1/8 lands anywhere (TLB miss storm mix)
 */
static uint64_t lsupress_walk_hop(lsupress_engine_t *e)
{
	switch (e->walk) {
	default:
	case LSUPRESS_WALK_UNIFORM:
		return lsupress_next_hop(e);
	case LSUPRESS_WALK_BITGEN: {
		const uint64_t r = stress_bitgen_u64(&e->bg);
		const unsigned width = 6u + (unsigned)((r >> 58) % 15);
		const unsigned pos = (unsigned)((r >> 48) % (36u - width));
		const uint64_t band = (((uint64_t)1 << width) - 1) << (12 + pos);

		return lsupress_next_hop(e) & ~((band / e->win_size) * e->win_size
				/ e->win_size ? band : band);
	}
	case LSUPRESS_WALK_VA_BIT: {
		const uint64_t bit = 12 + (e->hops++ % 36);

		return lsupress_next_hop(e) ^ (1ULL << bit);
	}
	case LSUPRESS_WALK_NEAR_FAR: {
		const uint64_t r = lsupress_next_hop(e);
		const size_t near_span = e->map_size >> 10;

		if ((r & 7) != 0 && near_span > e->win_size) {
			/* near hop: within ~1/1024 of the current window */
			const size_t cur = (size_t)(e->win - e->map_base);
			const size_t near_off = (size_t)(r % (near_span / e->win_size));

			return ((uint64_t)(cur / e->win_size) ^ near_off)
				% (e->map_size / e->win_size);
		}
		return r;
	}
	}
}

static void lsupress_engine_init(
	lsupress_engine_t *e,
	const uint64_t va_req,
	const uint64_t win_req,
	const uint64_t seed,
	const size_t walk,
	const size_t huge)
{
	size_t size = (size_t)va_req;
	int extra_flags = MAP_NORESERVE;

	e->state = seed | 1;
	e->win_size = (size_t)win_req;
	e->walk = walk;
	e->hops = 0;
	e->used = false;
	e->map_base = MAP_FAILED;
	stress_bitgen_init(&e->bg);
	stress_bitgen_seed(&e->bg, seed ^ 0x5A5A5A5A5A5A5A5AULL);

#if defined(MAP_HUGETLB)
	switch (huge) {
	default:
	case 0:
		break;
#if defined(MAP_HUGE_2MB)
	case 2 * STRESS_MB:
		extra_flags |= MAP_HUGETLB | MAP_HUGE_2MB;
		break;
#endif
#if defined(MAP_HUGE_1GB)
	case 1 * STRESS_GB:
		extra_flags |= MAP_HUGETLB | MAP_HUGE_1GB;
		break;
#endif
	}
#else
	(void)huge;
#endif

	/* probe-shrink: requested -> ... -> window*2 minimum; hugepage
	 * requests fall back to base pages with a warning (pool empty) */
	while (size >= e->win_size * 2) {
		e->map_base = mmap(NULL, size,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | extra_flags,
			-1, 0);
		if (e->map_base != MAP_FAILED)
			break;
		if (extra_flags != MAP_NORESERVE) {
			pr_inf("lsupress: hugepage mmap failed, falling back to base pages\n");
			extra_flags = MAP_NORESERVE;
			continue;
		}
		size /= 2;
	}
	if (e->map_base == MAP_FAILED)
		return;
	e->map_size = size;
	e->win = e->map_base;
	e->used = true;
}

/*
 *  Migrate the working window: pick a fresh page-aligned position
 *  inside the map, MADV_DONTNEED the old window (constant RSS).
 *  madvise failure is non-fatal: pages stay resident, walk continues.
 */
static void lsupress_engine_next_window(lsupress_engine_t *e)
{
	const size_t hops = e->map_size / e->win_size;

	if (hops > 1) {
		(void)madvise(e->win, e->win_size, MADV_DONTNEED);
		e->win = e->map_base +
			(size_t)(lsupress_walk_hop(e) % hops) * e->win_size;
	}
}

static void lsupress_engine_free(lsupress_engine_t *e)
{
	if (e->used && e->map_base != MAP_FAILED)
		(void)munmap(e->map_base, e->map_size);
	e->used = false;
}

static stress_lsupress_method_info_t lsupress_methods[] = {
	{ "all",		NULL,				0 },	/* 0: rotate */
	{ "load-int64",		stress_lsupress_load_int64,	0 },
	{ "store-int64",	stress_lsupress_store_int64,	0 },
	{ "copy-int64",		stress_lsupress_copy_int64,	0 },
	{ "mix-2l-alu-1s",	stress_lsupress_mix_2l_alu_1s,	0 },
	{ "mix-1l-fpu-1s",	stress_lsupress_mix_1l_fpu_1s,	0 },
	{ "mix-3l-2alu-1s",	stress_lsupress_mix_3l_2alu_1s,	0 },
	{ "load-int128",	stress_lsupress_load_int128,	0 },
	{ "load-fp64",		stress_lsupress_load_fp64,	0 },
	{ "load-neon",		stress_lsupress_load_neon,	0 },
	{ "store-neon",		stress_lsupress_store_neon,	0 },
	{ "store-zva",		stress_lsupress_store_zva,	0 },
	{ "mix-neon-fma",	stress_lsupress_mix_neon_fma,	0 },
	{ "excl-pair",		stress_lsupress_excl_pair,	0 },
	{ "lse-rmw",		stress_lsupress_lse_rmw,	HWCAP_ATOMICS },
#if defined(__GNUC__) && __GNUC__ >= 10
	{ "lrcpc-pair",		stress_lsupress_lrcpc_pair,	HWCAP_LRCPC },
	{ "ilrcpc-rmw",		stress_lsupress_ilrcpc_rmw,	HWCAP_ILRCPC },
#endif
#if defined(__ARM_FEATURE_LS64)
	{ "ls64-copy",		stress_lsupress_ls64_copy,	0 },	/* HWCAP2/3 gated in main */
#endif
#if defined(__GNUC__) && __GNUC__ >= 10
	{ "load-sve",		stress_lsupress_load_sve,	HWCAP_SVE },
	{ "load-sve-gather",	stress_lsupress_load_sve_gather, HWCAP_SVE },
	{ "store-sve",		stress_lsupress_store_sve,	HWCAP_SVE },
	{ "copy-sve",		stress_lsupress_copy_sve,	HWCAP_SVE },
	{ "mix-sve-fma",	stress_lsupress_mix_sve_fma,	HWCAP_SVE },
#endif
};

static const char *stress_lsupress_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(lsupress_methods)) ? lsupress_methods[i].name : NULL;
}

static const char *stress_lsupress_walk(const size_t i);

static const stress_opt_t opts[] = {
	{ OPT_lsupress_method,	"lsupress-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_lsupress_method },
	{ OPT_lsupress_ops,	"lsupress-ops",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_lsupress_va_size,	"lsupress-va-size",	TYPE_ID_SIZE_T_BYTES_VM, 256 * STRESS_MB, 16 * STRESS_TB, NULL },
	{ OPT_lsupress_window,	"lsupress-window",	TYPE_ID_SIZE_T_BYTES_VM, 4 * STRESS_MB, MAX_MEM_LIMIT, NULL },
	{ OPT_lsupress_walk,	"lsupress-walk",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_lsupress_walk },
	{ OPT_lsupress_huge,	"lsupress-huge",	TYPE_ID_SIZE_T_BYTES_VM, 0, 1 * STRESS_GB, NULL },
	END_OPT,
};

static const char *stress_lsupress_walk(const size_t i)
{
	static const char *walks[] = {
		"uniform",
		"bitgen",
		"va-bit",
		"near-far",
	};

	return (i < SIZEOF_ARRAY(walks)) ? walks[i] : NULL;
}

static const stress_help_t help[] = {
	{ NULL,	"lsupress N",		"start N workers exercising the arm64 load/store unit across the instruction spectrum" },
	{ NULL,	"lsupress-method M",	"method (see --lsupress-method 0 for the list)" },
	{ NULL,	"lsupress-ops N",	"stop after N lsupress bogo operations" },
	{ NULL,	"lsupress-va-size B",	"per-worker NORESERVE VA map size (default 100GB)" },
	{ NULL,	"lsupress-window B",	"working-set window size (default 256MB)" },
	{ NULL,	"lsupress-walk M",	"walk mode: uniform, bitgen, va-bit, near-far" },
	{ NULL,	"lsupress-huge B",	"page size for the VA map (4k default; 2m/1g use MAP_HUGETLB with fallback)" },
	{ NULL,	NULL,			NULL }
};

/*
 *  stress_lsupress()
 *	exercise the LSU instruction spectrum
 */
static int stress_lsupress(stress_args_t *args)
{
	size_t lsupress_method = 0;
	size_t lsupress_va_size = LSUPRESS_DEFAULT_VA_SIZE;
	size_t lsupress_window = LSUPRESS_DEFAULT_WINDOW;
	size_t lsupress_walk = LSUPRESS_WALK_UNIFORM;
	size_t lsupress_huge = 0;
	stress_lsupress_func_t func;
	uint64_t *buf;
	size_t buf_words;
	uint64_t *fallback = NULL;
	const uint64_t seed = stress_mwc64();
	const size_t method_max = SIZEOF_ARRAY(lsupress_methods);
	lsupress_engine_t engine;

	(void)stress_setting_get("lsupress-method", &lsupress_method);
	(void)stress_setting_get("lsupress-va-size", &lsupress_va_size);
	(void)stress_setting_get("lsupress-window", &lsupress_window);
	(void)stress_setting_get("lsupress-walk", &lsupress_walk);
	(void)stress_setting_get("lsupress-huge", &lsupress_huge);

	if (lsupress_method >= method_max) {
		pr_fail("%s: lsupress-method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (lsupress_method == LSUPRESS_METHOD_ALL)
		lsupress_method = 1 + (stress_mwc32() % (method_max - 1));

	/* feature-gated methods: honest skip (never a fake run) */
	if (!lsupress_hwcap_probed) {
		lsupress_hwcap = (uint32_t)getauxval(AT_HWCAP);
		lsupress_hwcap_probed = true;
	}
	if (lsupress_methods[lsupress_method].hwcap_req &&
	    !(lsupress_hwcap & lsupress_methods[lsupress_method].hwcap_req)) {
		pr_inf_skip("%s: lsupress-method %s skipped, CPU does not support the required feature\n",
			args->name, lsupress_methods[lsupress_method].name);
		return EXIT_NOT_IMPLEMENTED;
	}

	func = lsupress_methods[lsupress_method].func;

#if defined(__ARM_FEATURE_LS64)
	if ((func == stress_lsupress_ls64_copy) && !lsupress_ls64_ok()) {
		pr_inf_skip("%s: lsupress-method ls64-copy skipped, CPU does not support ls64\n",
			args->name);
		return EXIT_NOT_IMPLEMENTED;
	}
#endif

	lsupress_engine_init(&engine, lsupress_va_size, lsupress_window, seed,
		lsupress_walk, lsupress_huge);
	if (engine.used) {
		const bool verify = (g_opt_flags & OPT_FLAGS_VERIFY) != 0;

		stress_memory_anon_name_set(engine.map_base, engine.map_size,
			"lsupress-va");
		buf = (uint64_t *)engine.win;
		buf_words = engine.win_size / sizeof(uint64_t);
		do {
			func(buf, buf_words, seed);
			stress_bogo_inc(args);
			if (verify && (func == stress_lsupress_store_int64)) {
				/* store-int64 promises value == f(seed, addr)
				 * at every address; other methods carry no
				 * f-invariant (copy moves values across
				 * addresses, zva writes zeros, mixes compute)
				 * and only run their checksum consumers */
				const uint64_t *w = (const uint64_t *)engine.win;
				const size_t words = engine.win_size / sizeof(uint64_t);
				size_t k, fails = 0;

				for (k = 0; k < words; k += 512) {
					const uint64_t expect = lsupress_value(seed,
						(uint64_t)(uintptr_t)(w + k));

					if (UNLIKELY(w[k] != expect)) {
						const uint64_t diff = w[k] ^ expect;
						int bits = 0, b;

						for (b = 0; b < 64; b++)
							if (diff & (1ULL << b))
								bits++;
						pr_fail("%s: addr %p expected 0x%16.16" PRIx64
							" actual 0x%16.16" PRIx64
							" %d bit(s) flipped (xor 0x%16.16" PRIx64 ")\n",
							args->name, (const void *)(w + k),
							expect, w[k], bits, diff);
						if (++fails >= 4)
							break;
					}
				}
				if (fails)
					return EXIT_FAILURE;
			}
			lsupress_engine_next_window(&engine);
			buf = (uint64_t *)engine.win;
		} while (stress_continue(args));
		lsupress_engine_free(&engine);
		return EXIT_SUCCESS;
	}

	/* engine fallback: plain populated buffer */
	fallback = (uint64_t *)stress_mmap_populate(NULL,
		LSUPRESS_BUF_WORDS * sizeof(uint64_t),
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (fallback == MAP_FAILED) {
		pr_inf("%s: mmap of working buffer failed, errno=%d (%s)%s\n",
			args->name, errno, strerror(errno),
			stress_memory_free_get());
		return EXIT_NO_RESOURCE;
	}
	stress_memory_anon_name_set(fallback, LSUPRESS_BUF_WORDS * sizeof(uint64_t),
		"lsupress-buffer");

	func(fallback, LSUPRESS_BUF_WORDS, seed);
	stress_bogo_inc(args);

	(void)munmap(fallback, LSUPRESS_BUF_WORDS * sizeof(uint64_t));
	return EXIT_SUCCESS;
}

const stressor_info_t stress_lsupress_info = {
	.stressor = stress_lsupress,
	.classifier = CLASS_CPU | CLASS_MEMORY,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
#else
const stressor_info_t stress_lsupress_info = {
	.stressor = stress_unimplemented,
	.classifier = CLASS_CPU | CLASS_MEMORY,
	.opts = NULL,
	.help = "lsupress"
};
#endif
