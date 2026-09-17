/*
 * Copyright (C) 2021-2026 Colin Ian King
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
#include "core-arch.h"
#include "core-bitops.h"
#include "core-builtin.h"
#include "core-madvise.h"
#include "core-mmap.h"
#include "core-put.h"
#include "core-pragma.h"
#include "core-signal.h"
#include "core-target-clones.h"

#include <math.h>

#define D_ALIGNED	ALIGNED(64)	/* alignment for doubles */
#define F_ALIGNED       ALIGNED(64)	/* alignment for floats */

#define FMA_ELEMENTS	(512)
#define FMA_UNROLL	(8)
#define FMA_FUNCS	(6)

/* #define USE_FMA_FAST	 */

typedef struct {
	double  *double_a;
	double	double_init[FMA_ELEMENTS] D_ALIGNED;
	double	double_a1[FMA_ELEMENTS] D_ALIGNED;
	double	double_a2[FMA_ELEMENTS] D_ALIGNED;

	float	*float_a;
	float	float_init[FMA_ELEMENTS] F_ALIGNED;
	float	float_a1[FMA_ELEMENTS] F_ALIGNED;
	float	float_a2[FMA_ELEMENTS] F_ALIGNED;

	double	double_b;
	double	double_c;

	float	float_b;
	float	float_c;
} stress_fma_t;

typedef void (*stress_fma_func_t)(stress_fma_t *fma);

static const stress_help_t help[] = {
	{ NULL,	"fma N",	"start N workers performing floating point multiply-add ops" },
	{ NULL, "fma-libc",	"use fma libc fused multiply-add helpers" },
	{ NULL,	"fma-ops N",	"stop after N floating point multiply-add bogo operations" },
	{ NULL,	NULL,		 NULL }
};

static inline float stress_fma_rnd_float(void)
{
	register const float fhalfpwr32 = (float)1.0 / (float)(0x80000000);

	return (float)stress_mwc32() * fhalfpwr32;
}

static void TARGET_CLONES stress_fma_add132_double(stress_fma_t *fma)
{
	register size_t i;
	register double *a = fma->double_a;
	register const double b = fma->double_b;
	register const double c = fma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (a[i] * c) + b;
}

static void TARGET_CLONES stress_fma_sub132_double(stress_fma_t *fma)
{
	register size_t i;
	register double *a = fma->double_a;
	register const double b = fma->double_b;
	register const double c = fma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (a[i] * c) - b;
}

static void TARGET_CLONES stress_fma_add132_float(stress_fma_t *fma)
{
	register size_t i;
	register float *a = fma->float_a;
	register const float b = fma->float_b;
	register const float c = fma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (a[i] * c) + b;
}

static void TARGET_CLONES stress_fma_sub132_float(stress_fma_t *fma)
{
	register size_t i;
	register float *a = fma->float_a;
	register const float b = fma->float_b;
	register const float c = fma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (a[i] * c) - b;
}

static void TARGET_CLONES stress_fma_add213_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (b * a[i]) + c;
}

static void TARGET_CLONES stress_fma_sub213_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (b * a[i]) - c;
}

static void TARGET_CLONES stress_fma_add213_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (b * a[i]) + c;
}

static void TARGET_CLONES stress_fma_sub213_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++)
		a[i] = (b * a[i]) - c;
}

static void TARGET_CLONES stress_fma_add231_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
		a[i] = (b * c) + a[i];
		b += 0.125;
	}
}

static void TARGET_CLONES stress_fma_sub231_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
		a[i] = (b * c) - a[i];
		b += 0.125;
	}
}

static void TARGET_CLONES stress_fma_add231_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
		a[i] = (b * c) + a[i];
		b += 0.125f;
	}
}

static void TARGET_CLONES stress_fma_sub231_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
		a[i] = (b * c) - a[i];
		b += 0.125f;
	}
}

static const stress_fma_func_t stress_fma_funcs[] = {
	stress_fma_add132_double,
	stress_fma_add132_float,
	stress_fma_add213_double,
	stress_fma_add213_float,
	stress_fma_add231_double,
	stress_fma_add231_float,

	stress_fma_sub132_double,
	stress_fma_sub132_float,
	stress_fma_sub213_double,
	stress_fma_sub213_float,
	stress_fma_sub231_double,
	stress_fma_sub231_float,
};

/*
 *  SVE2 kernel variants, compiled with a per-function target
 *  attribute (so the file still builds on a plain armv8-a
 *  default -march) and dispatched at run time by the HWCAP_SVE
 *  feature check - one binary runs the SVE2 256-bit datapath
 *  on SVE hardware and the auto-vectorised NEON path elsewhere.
 *  GCC 12 has no aarch64 target_clones (function multi-
 *  versioning) support, so the dispatch is a runtime table
 *  selection, the standard FMV-equivalent pattern.
 */
#if defined(STRESS_ARCH_ARM) &&	\
    defined(__aarch64__) &&	\
    defined(HAVE_ARM_NEON_CRYPTO)
#define HAVE_FMA_SVE2

#include <arm_sve.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>

#define FMA_SVE2_TARGET __attribute__((target("arch=armv9-a+sve2")))

#define FMA_SVE2_KERNEL_D(name, acc)					\
FMA_SVE2_TARGET								\
static void stress_fma_ ## name ## _double_sve2(stress_fma_t *fma)	\
{									\
	register size_t i;						\
	register const size_t vl = (size_t)svcntd();			\
	const svbool_t pg = svptrue_b64();				\
	register const double b = fma->double_b;			\
	register const double c = fma->double_c;			\
	const svfloat64_t vb = svdup_f64(b);				\
	const svfloat64_t vc = svdup_f64(c);				\
									\
	for (i = 0; i < FMA_ELEMENTS; i += vl) {			\
		svfloat64_t va = svld1_f64(pg, &fma->double_a[i]);	\
									\
		svst1_f64(pg, &fma->double_a[i], acc);			\
	}								\
}

#define FMA_SVE2_KERNEL_F(name, acc)					\
FMA_SVE2_TARGET								\
static void stress_fma_ ## name ## _float_sve2(stress_fma_t *fma)	\
{									\
	register size_t i;						\
	register const size_t vl = (size_t)svcntw();			\
	const svbool_t pg = svptrue_b32();				\
	register const float b = fma->float_b;				\
	register const float c = fma->float_c;				\
	const svfloat32_t vb = svdup_f32(b);				\
	const svfloat32_t vc = svdup_f32(c);				\
									\
	for (i = 0; i < FMA_ELEMENTS; i += vl) {			\
		svfloat32_t va = svld1_f32(pg, &fma->float_a[i]);	\
									\
		svst1_f32(pg, &fma->float_a[i], acc);			\
	}								\
}

/*  svmla(pg, x, y, z) = x + y * z;  svmsb = y*z - x;  svnmsb = -(y*z) + x  */

/*  double: a[i] = (a[i] * c) + b  */
FMA_SVE2_KERNEL_D(add132, svmla_f64_m(pg, vb, va, vc))
/*  double: a[i] = (a[i] * c) - b  */
FMA_SVE2_KERNEL_D(sub132, svmsb_f64_m(pg, va, vc, vb))
/*  double: a[i] = (b * a[i]) + c  */
FMA_SVE2_KERNEL_D(add213, svmla_f64_m(pg, vc, vb, va))
/*  double: a[i] = (b * a[i]) - c  */
FMA_SVE2_KERNEL_D(sub213, svmsb_f64_m(pg, vb, va, vc))
/*  double: a[i] = c + (a[i] * b)  */
FMA_SVE2_KERNEL_D(add231, svmla_f64_m(pg, vc, va, vb))
/*  double: a[i] = c - (a[i] * b)  */
FMA_SVE2_KERNEL_D(sub231, svnmsb_f64_m(pg, va, vb, vc))

/*  float variants  */
FMA_SVE2_KERNEL_F(add132, svmla_f32_m(pg, vb, va, vc))
FMA_SVE2_KERNEL_F(sub132, svmsb_f32_m(pg, va, vc, vb))
FMA_SVE2_KERNEL_F(add213, svmla_f32_m(pg, vc, vb, va))
FMA_SVE2_KERNEL_F(sub213, svmsb_f32_m(pg, vb, va, vc))
FMA_SVE2_KERNEL_F(add231, svmla_f32_m(pg, vc, va, vb))
FMA_SVE2_KERNEL_F(sub231, svnmsb_f32_m(pg, va, vb, vc))

/*
 *  fma_sve2_supported()
 *	run-time dynamic switch for the SVE2 dispatch
 */
static bool fma_sve2_supported(void)
{
	return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0;
}

static const stress_fma_func_t stress_fma_sve2_funcs[] = {
	stress_fma_add132_double_sve2,
	stress_fma_add132_float_sve2,
	stress_fma_add213_double_sve2,
	stress_fma_add213_float_sve2,
	stress_fma_add231_double_sve2,
	stress_fma_add231_float_sve2,

	stress_fma_sub132_double_sve2,
	stress_fma_sub132_float_sve2,
	stress_fma_sub213_double_sve2,
	stress_fma_sub213_float_sve2,
	stress_fma_sub231_double_sve2,
	stress_fma_sub231_float_sve2,
};
#endif

/* libc variants */
#if (defined(HAVE_FMA)  || defined(FP_FAST_FMA)) && 	\
    (defined(HAVE_FMAF) || defined(FP_FAST_FMAF))
static void TARGET_CLONES stress_fma_add132_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(a[i], c, b);
#else
		a[i] = shim_fma(a[i], c, b);
#endif
	}
}

static void TARGET_CLONES stress_fma_sub132_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = -(pfma->double_b);
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(a[i], c, b);
#else
		a[i] = shim_fma(a[i], c, b);
#endif
	}
}

static void TARGET_CLONES stress_fma_add132_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(a[i], c, b);
#else
		a[i] = shim_fmaf(a[i], c, b);
#endif
	}
}

static void TARGET_CLONES stress_fma_sub132_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = -(pfma->float_b);
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(a[i], c, b);
#else
		a[i] = shim_fmaf(a[i], c, b);
#endif
	}
}

static void TARGET_CLONES stress_fma_add213_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) && 	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(b, a[i], c);
#else
		a[i] = shim_fma(b, a[i], c);
#endif
	}
}

static void TARGET_CLONES stress_fma_sub213_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register const double b = pfma->double_b;
	register const double c = -(pfma->double_c);

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) && 	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(b, a[i], c);
#else
		a[i] = shim_fma(b, a[i], c);
#endif
	}
}

static void TARGET_CLONES stress_fma_add213_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(b, a[i], c);
#else
		a[i] = shim_fmaf(b, a[i], c);
#endif
	}
}

static void TARGET_CLONES stress_fma_sub213_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register const float b = pfma->float_b;
	register const float c = -(pfma->float_c);

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(b, a[i], c);
#else
		a[i] = shim_fmaf(b, a[i], c);
#endif
	}
}

static void TARGET_CLONES stress_fma_add231_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(b, c, a[i]);
#else
		a[i] = shim_fma(b, c, a[i]);
#endif
		b += 0.125;
	}
}

static void TARGET_CLONES stress_fma_sub231_libc_double(stress_fma_t *pfma)
{
	register size_t i;
	register double *a = pfma->double_a;
	register double b = pfma->double_b;
	register const double c = pfma->double_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMA) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMA(b, c, -a[i]);
#else
		a[i] = shim_fma(b, c, -a[i]);
#endif
		b += 0.125;
	}
}

static void TARGET_CLONES stress_fma_add231_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(b, c, a[i]);
#else
		a[i] = shim_fmaf(b, c, a[i]);
#endif
		b += 0.125f;
	}
}

static void TARGET_CLONES stress_fma_sub231_libc_float(stress_fma_t *pfma)
{
	register size_t i;
	register float *a = pfma->float_a;
	register float b = pfma->float_b;
	register const float c = pfma->float_c;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
#if defined(FP_FAST_FMAF) &&	\
    defined(USE_FMA_FAST)
		a[i] = FP_FAST_FMAF(b, c, -a[i]);
#else
		a[i] = shim_fmaf(b, c, -a[i]);
#endif
		b += 0.125f;
	}
}

static const stress_fma_func_t stress_fma_libc_funcs[] = {
	stress_fma_add132_libc_double,
	stress_fma_add132_libc_float,
	stress_fma_add213_libc_double,
	stress_fma_add213_libc_float,
	stress_fma_add231_libc_double,
	stress_fma_add231_libc_float,

	stress_fma_sub132_libc_double,
	stress_fma_sub132_libc_float,
	stress_fma_sub213_libc_double,
	stress_fma_sub213_libc_float,
	stress_fma_sub231_libc_double,
	stress_fma_sub231_libc_float,
};
#endif

static inline void OPTIMIZE3 TARGET_CLONES stress_fma_init(stress_fma_t *pfma)
{
	register size_t i;

PRAGMA_UNROLL_N(FMA_UNROLL)
	for (i = 0; i < FMA_ELEMENTS; i++) {
		register const float rnd = stress_fma_rnd_float();

		pfma->double_init[i] = (double)rnd;
		pfma->float_init[i] = rnd;
	}
}

static inline void OPTIMIZE3 TARGET_CLONES stress_fma_reset_a(stress_fma_t *pfma)
{
	(void)shim_memcpy(pfma->double_a1, pfma->double_init, sizeof(pfma->double_init));
	(void)shim_memcpy(pfma->double_a2, pfma->double_init, sizeof(pfma->double_init));

	(void)shim_memcpy(pfma->float_a1, pfma->float_init, sizeof(pfma->float_init));
	(void)shim_memcpy(pfma->float_a2, pfma->float_init, sizeof(pfma->float_init));
}

/*
 *  stress_fma_verify_fail()
 *	report the first differing element between the two identical
 *	computations with bit level diagnostics: element index, the
 *	expected (first run) and actual (second run) bit patterns and
 *	the number of flipped bits (hamming distance of the xor).
 *	This mirrors the CORE179 style diagnosis used to identify
 *	which datapath produced the silent data corruption.
 */
static void stress_fma_verify_fail(
	const char *name,
	const char *type,
	const size_t nelems,
	const uint64_t *expected,
	const uint64_t *actual)
{
	size_t i;

	for (i = 0; i < nelems; i++) {
		if (expected[i] != actual[i]) {
			const uint64_t xor = expected[i] ^ actual[i];

			pr_fail("%s: data difference between identical %s fma computations\n",
				name, type);
			pr_fail("%s:   first difference at element %zu: "
				"expected 0x%16.16" PRIx64 ", actual 0x%16.16" PRIx64 ", "
				"%u bit(s) flipped (xor 0x%16.16" PRIx64 ")\n",
				name, i, expected[i], actual[i],
				stress_bitops_popcount64(xor), xor);
			return;
		}
	}
}

static int stress_fma(stress_args_t *args)
{
	stress_fma_t *pfma;
	register size_t idx_b = 0;
	register size_t idx_c = 0;
	const bool verify = !!(g_opt_flags & OPT_FLAGS_VERIFY);
	const stress_fma_func_t *fma_func_array;
	bool fma_libc = false;
	int rc = EXIT_SUCCESS;
	size_t offset = 0;

	(void)stress_setting_get("fma-libc", &fma_libc);
#if (defined(HAVE_FMA)  || defined(FP_FAST_FMA)) && 	\
    (defined(HAVE_FMAF) || defined(FP_FAST_FMAF))
	fma_func_array = fma_libc ? stress_fma_libc_funcs : stress_fma_funcs;
#else
	if (fma_libc) {
		pr_inf("%s: libc fma functions not available, defaulting "
			"to non-libc fma operations\n", args->name);
	}
	fma_func_array = stress_fma_funcs;
#endif
#if defined(HAVE_FMA_SVE2)
	/*
	 *  SVE2 dispatch (FMV equivalent): pick the SVE2 kernel
	 *  table when the hardware reports SVE and the libc
	 *  variants were not explicitly requested.  The SVE2
	 *  kernels implement the exact same arithmetic as the
	 *  scalar kernels, so the a1/a2 cross-verification
	 *  works unchanged across both paths.
	 */
	if (!fma_libc && fma_sve2_supported())
		fma_func_array = stress_fma_sve2_funcs;
#endif

	stress_signal_catch_sigill();

	pfma = (stress_fma_t *)stress_mmap_populate(NULL, sizeof(*pfma),
				PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (pfma == MAP_FAILED) {
		pr_inf_skip("%s: failed to mmap %zu bytes for FMA data%s, skipping stressor\n",
			args->name, sizeof(*pfma), stress_memory_free_get());
		return EXIT_NO_RESOURCE;
	}
	stress_memory_anon_name_set(pfma, sizeof(*pfma), "fma-data");
	stress_madvise_mergeable(pfma, sizeof(*pfma));

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	stress_fma_init(pfma);

	do {
		register size_t i;

		stress_fma_reset_a(pfma);

		idx_b++;
		if (idx_b >= FMA_ELEMENTS)
			idx_b = 0;
		idx_c += 3;
		if (idx_c >= FMA_ELEMENTS)
			idx_c = 0;

		pfma->double_a = pfma->double_a1;
		pfma->double_b = pfma->double_a[idx_b];
		pfma->double_c = pfma->double_a[idx_c];
		pfma->float_a = pfma->float_a1;
		pfma->float_b = pfma->float_a[idx_b];
		pfma->float_c = pfma->float_a[idx_c];

		for (i = 0; i < FMA_FUNCS; i++) {
			fma_func_array[i + offset](pfma);
		}
		stress_bogo_inc(args);

		if (verify) {
			pfma->double_a = pfma->double_a2;
			pfma->double_b = pfma->double_a[idx_b];
			pfma->double_c = pfma->double_a[idx_c];
			pfma->float_a = pfma->float_a2;
			pfma->float_b = pfma->float_a[idx_b];
			pfma->float_c = pfma->float_a[idx_c];

			for (i = 0; i < FMA_FUNCS; i++) {
				fma_func_array[i + offset](pfma);
			}
			stress_bogo_inc(args);

			if (shim_memcmp(pfma->double_a1, pfma->double_a2, sizeof(pfma->double_a1))) {
				stress_fma_verify_fail(args->name, "double",
					SIZEOF_ARRAY(pfma->double_a1),
					(const uint64_t *)pfma->double_a1,
					(const uint64_t *)pfma->double_a2);
				rc = EXIT_FAILURE;
			}
			if (shim_memcmp(pfma->float_a1, pfma->float_a2, sizeof(pfma->float_a1))) {
				stress_fma_verify_fail(args->name, "float",
					SIZEOF_ARRAY(pfma->float_a1),
					(const uint64_t *)pfma->float_a1,
					(const uint64_t *)pfma->float_a2);
				rc = EXIT_FAILURE;
			}
		}
		offset = 6 - offset;
	} while (stress_continue(args));

	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);

	(void)munmap((void *)pfma, sizeof(*pfma));

	return rc;
}

static const stress_opt_t opts[] = {
	{ OPT_fma_libc, "fma-libc", TYPE_ID_BOOL, 0, 1, NULL },
	END_OPT,
};

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("bogo-ops-stable"),
	STRESS_EX_FEATURE("fp"),
	STRESS_EX_FEATURE("fp-ops"),
	STRESS_EX_FEATURE("hot-package"),
	STRESS_EX_FEATURE("memory-loads"),
	STRESS_EX_FEATURE("memory-stores"),
	STRESS_EX_FEATURE("user-time"),

	STRESS_EX_LIBRARY("m"),

	STRESS_EX_END,
};

const stressor_info_t stress_fma_info = {
	.stressor = stress_fma,
	.classifier = CLASS_CPU | CLASS_FP | CLASS_COMPUTE,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help,
	.exercises = exercises,
};
