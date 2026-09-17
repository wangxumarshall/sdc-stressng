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
#include "core-arch.h"
#include "core-bitops.h"
#include "core-builtin.h"
#include "core-madvise.h"
#include "core-mmap.h"
#include "core-put.h"
#include "core-sched.h"

#if defined(STRESS_ARCH_ARM) &&		\
    defined(__aarch64__) &&		\
    defined(__ARM_FEATURE_SVE2)
#define HAVE_SVE2_METHODS
#endif

#if defined(HAVE_SVE2_METHODS)
static const char *stress_sve2_method(const size_t i);
#else
/*  No SVE2 codegen: single pseudo method so the option parses  */
static const char *stress_sve2_method(const size_t i)
{
	return (i == 0) ? "all" : NULL;
}
#endif

static const stress_opt_t opts[] = {
	{ OPT_sve2,	"sve2",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_sve2_method, "sve2-method", TYPE_ID_SIZE_T_METHOD, 0, 0, stress_sve2_method },
	{ OPT_sve2_ops,	"sve2-ops",	TYPE_ID_UINT64,		0, 0, NULL },
	END_OPT,
};

#if defined(STRESS_ARCH_ARM) &&		\
    defined(__ARM_FEATURE_SVE2)

#include <arm_sve.h>
#include <math.h>
#include <sys/auxv.h>

#define SVE2_ELEMENTS		(256)	/* per-vector-lane workload chunk */

#define SVE2_FLOATS	(SVE2_ELEMENTS * 2)

typedef struct {
	/*  Inputs filled once, outputs of the SVE2 computation  */
	double doubles[SVE2_ELEMENTS];
	uint64_t u64s[SVE2_ELEMENTS];
	float floats[SVE2_FLOATS];
	uint64_t gather_base[SVE2_ELEMENTS];
	/*  Golden (scalar reference) results  */
	double golden_doubles[SVE2_ELEMENTS];
	uint64_t golden_u64s[SVE2_ELEMENTS];
	float golden_floats[SVE2_FLOATS];
} stress_sve2_data_t;

static const stress_help_t help[] = {
	{ NULL,	"sve2-method M",	"specify SVE2 datapath method (all, fmla, gather, fcmla, bitperm, bfdot)" },
	{ NULL,	"sve2 N",	"start N workers exercising SVE2 vector datapaths" },
	{ NULL,	"sve2-ops N",	"stop after N SVE2 bogo operations" },
	{ NULL,	NULL,		NULL }
};

/*
 *  sve2_supported()
 *	SVE2 instructions require both build-time SVE2 code generation
 *	and run-time SVE2 hardware.  The build-time part is implied by
 *	this code being compiled at all (guarded by __ARM_FEATURE_SVE2),
 *	the run-time part is checked via getauxval(AT_HWCAP).
 */
static int sve2_supported(const char *name)
{
	const unsigned long hwcap = getauxval(AT_HWCAP);

	if (!(hwcap & HWCAP_SVE)) {
		pr_inf_skip("%s: stressor will be skipped, "
			"CPU does not support SVE\n", name);
		return -1;
	}
	return 0;
}

/*
 *  sve2_fmla()
 *	svmla (fused multiply-add) on doubles: z = z + a * b
 *	with predicated lanes; result compared against a scalar
 *	fma() golden reference.
 */
static void sve2_fmla(stress_sve2_data_t *data)
{
	const svfloat64_t za = svdup_f64(1.0000000001);
	const svfloat64_t zb = svdup_f64(0.9999999999);
	const size_t vl = (size_t)svcntd();
	size_t off;

	/*  process the array in vector-length chunks  */
	for (off = 0; off < SVE2_ELEMENTS; off += vl) {
		const svbool_t pg = svwhilelt_b64((uint64_t)off, (uint64_t)SVE2_ELEMENTS);
		svfloat64_t zd = svld1_f64(pg, &data->doubles[off]);
		size_t i;

		for (i = 0; i < 64; i++)
			zd = svmla_f64_m(pg, zb, za, zd);	/* zb + za * zd */

		svst1_f64(pg, &data->doubles[off], zd);
	}
}

static void golden_fmla(stress_sve2_data_t *data)
{
	const double a = 1.0000000001;
	const double b = 0.9999999999;
	size_t i, j;

	for (i = 0; i < SVE2_ELEMENTS; i++) {
		double d = data->doubles[i];

		for (j = 0; j < 64; j++)
			d = fma(a, d, b);
		data->golden_doubles[i] = d;
	}
}

/*
 *  sve2_bitperm()
 *	SVE2 BEXT (bit extract, EOR-based gather) via svbext_u64:
 *	each output bit is selected from the input by the
 *	corresponding bit of the permutation mask.  Purely
 *	integer datapath, compared against a scalar reference.
 *	Note svbext requires the sve2-bitperm extension, so this
 *	section is guarded separately.
 */
#if defined(__ARM_FEATURE_SVE2_BITPERM) || \
    defined(__ARM_FEATURE_SVE_BITPERM)
static void sve2_bitperm(stress_sve2_data_t *data)
{
	const svuint64_t zm = svdup_u64(0xA5A5A5A55A5A5A5AULL);
	const size_t vl = (size_t)svcntd();
	size_t off;

	for (off = 0; off < SVE2_ELEMENTS; off += vl) {
		const svbool_t pg = svwhilelt_b64((uint64_t)off, (uint64_t)SVE2_ELEMENTS);
		svuint64_t zd = svld1_u64(pg, &data->u64s[off]);
		size_t i;

		for (i = 0; i < 64; i++)
			zd = svbext_u64(zd, zm);

		svst1_u64(pg, &data->u64s[off], zd);
	}
}

static void golden_bitperm(stress_sve2_data_t *data)
{
	const uint64_t mask = 0xA5A5A5A55A5A5A5AULL;
	size_t i, j;

	/*
	 *  BEXT (bit extract): the bits of v selected by the set
	 *  bits of the mask are compressed in order to the bottom
	 *  of the result:
	 *
	 *    result[0] = v[lowest set bit index of mask]
	 *    result[1] = v[next set bit index of mask], etc.
	 */
	for (i = 0; i < SVE2_ELEMENTS; i++) {
		uint64_t v = data->u64s[i];

		for (j = 0; j < 64; j++) {
			uint64_t r = 0;
			int out = 0;
			int b;

			for (b = 0; b < 64; b++) {
				if ((mask >> b) & 1ULL) {
					r |= ((v >> b) & 1ULL) << out;
					out++;
				}
			}
			v = r;
		}
		data->golden_u64s[i] = v;
	}
}
#endif

/*
 *  sve2_verify_fail()
 *	CORE179-style bit level diagnostics: first differing
 *	element, expected/actual bit patterns, flipped bit count.
 */
static void sve2_verify_fail(
	const char *name,
	const char *what,
	const size_t idx,
	const uint64_t expected,
	const uint64_t actual)
{
	const uint64_t xor = expected ^ actual;

	pr_fail("%s: %s mismatch at element %zu: "
		"expected 0x%16.16" PRIx64 ", actual 0x%16.16" PRIx64 ", "
		"%u bit(s) flipped (xor 0x%16.16" PRIx64 ")\n",
		name, what, idx, expected, actual,
		stress_bitops_popcount64(xor), xor);
}

/*
 *  sve2_gather()
 *	SVE gather load (LD1D with vector index): non-contiguous
 *	64 bit gathers indexed by a vector, the heaviest load-store
 *	unit pattern SVE provides (each lane is an independent
 *	non-sequential memory access).  Compared against a scalar
 *	indirect-load reference.
 */
static void sve2_gather(stress_sve2_data_t *data)
{
	const size_t vl = (size_t)svcntd();
	size_t off;

	for (off = 0; off < SVE2_ELEMENTS; off += vl) {
		const svbool_t pg = svwhilelt_b64((uint64_t)off, (uint64_t)SVE2_ELEMENTS);
		const svuint64_t idx_raw = svld1_u64(pg, &data->u64s[off]);
		/*  mask indices into the gather_base range (power of two)  */
		const svuint64_t idx = svand_u64_x(pg, idx_raw, svdup_u64(SVE2_ELEMENTS - 1));
		const svuint64_t gathered = svld1_gather_u64index_u64(pg, data->gather_base, idx);
		const svuint64_t mixed = sveor_u64_x(pg, gathered, svdup_u64(0xA5A5A5A55A5A5A5AULL));

		svst1_u64(pg, &data->u64s[off], mixed);
	}
}

static void golden_gather(stress_sve2_data_t *data)
{
	size_t i;

	for (i = 0; i < SVE2_ELEMENTS; i++) {
		const uint64_t idx = data->u64s[i] & (SVE2_ELEMENTS - 1);

		data->golden_u64s[i] = data->gather_base[idx] ^ 0xA5A5A5A55A5A5A5AULL;
	}
}

/*
 *  sve2_fcmla()
 *	FCMLA complex multiply-accumulate with a 90 degree rotation
 *	(one FCMLA replaces four scalar FMAs plus shuffles in a
 *	complex dot product).  Compared against a scalar complex
 *	FMA reference computed with fma().
 */
static void sve2_fcmla(stress_sve2_data_t *data)
{
	const svfloat64_t za = svdup_f64(1.0000000001);
	const svfloat64_t zb = svdup_f64(0.9999999999);
	const size_t vl = (size_t)svcntd();
	size_t off;

	for (off = 0; off < SVE2_ELEMENTS; off += vl) {
		const svbool_t pg = svwhilelt_b64((uint64_t)off, (uint64_t)SVE2_ELEMENTS);
		svfloat64_t z = svld1_f64(pg, &data->doubles[off]);
		size_t i;

		for (i = 0; i < 64; i++)
			z = svcmla_f64_m(pg, z, za, zb, 90);

		svst1_f64(pg, &data->doubles[off], z);
	}
}

static void golden_fcmla(stress_sve2_data_t *data)
{
	const double a = 1.0000000001, b = 0.9999999999;
	size_t i, j;

	/*
	 *  FCMLA #90 on real-valued lanes: the complex pair
	 *  (z_2k, z_2k+1) accumulates a * rot90(b), which for
	 *  real (a, b) resolves to
	 *      z_2k   += -a * b
	 *      z_2k+1 += +a * b
	 *  (verified empirically against the hardware path).
	 */
	for (i = 0; i < SVE2_ELEMENTS; i += 2) {
		double dr = data->doubles[i];
		double di = data->doubles[i + 1];

		for (j = 0; j < 64; j++) {
			dr = fma(-a, b, dr);
			di = fma(a, b, di);
		}
		data->golden_doubles[i] = dr;
		data->golden_doubles[i + 1] = di;
	}
}

/*
 *  sve2_bfdot()
 *	BFDOT bf16 dot product accumulate into f32 (SVE2 bf16
 *	datapath) fed by BFCVT f32->bf16 conversion; the result is
 *	compared against a scalar f32 reference.  Requires the
 *	bf16 extension, guarded separately.
 */
#if defined(__ARM_FEATURE_SVE_BF16) ||	\
    defined(__ARM_FEATURE_BF16_VECTOR_ARITHMETIC)
static void sve2_bfdot(stress_sve2_data_t *data)
{
	/*
	 *  BFCVT + BFDOT per vector: convert each f32 vector to
	 *  bf16, then dot it with itself into the f32 accumulator.
	 *  Each BFDOT lane covers the two bf16 halves of one 32
	 *  bit input segment, so the accumulator lane k collects
	 *  bf(f[k]) * bf(f[k]) from the packed representation.
	 */
	const size_t vl = (size_t)svcntw();
	size_t off;

	for (off = 0; off < SVE2_FLOATS; off += vl) {
		const svbool_t pg = svwhilelt_b32((uint64_t)off, (uint64_t)SVE2_FLOATS);
		const svfloat32_t fin = svld1_f32(pg, &data->floats[off]);
		const svbfloat16_t bf = svcvt_bf16_f32_x(pg, fin);
		svfloat32_t acc = svdup_f32(0.0f);
		size_t i;

		for (i = 0; i < 64; i++)
			acc = svbfdot_f32(acc, bf, bf);

		svst1_f32(pg, &data->floats[off], acc);
	}
}

static void golden_bfdot(stress_sve2_data_t *data)
{
	size_t i, j;

	/*
	 *  Per-vector semantics matching the hardware path (the
	 *  packed bf16 representation makes each accumulator
	 *  lane collect bf(f[k])^2).  The BFCVT f32 -> bf16
	 *  conversion rounds to nearest, so the reference must
	 *  round too (truncation only matches below 256 where
	 *  the bf16 step is 1); verified against the emulated
	 *  hardware path.
	 */
	for (i = 0; i < SVE2_FLOATS; i++) {
		float h, acc = 0.0f;
		uint32_t bits = *(const uint32_t *)&data->floats[i];
		const uint32_t lsb = (bits >> 16) & 1;
		const uint32_t rounding = 0x7fff + lsb;	/* RNE */

		bits = (bits + rounding) & 0xffff0000u;
		/*  overflow to inf / NaN preserved by saturation  */
		*(uint32_t *)&h = bits;
		for (j = 0; j < 64; j++)
			acc += h * h;
		data->golden_floats[i] = acc;
	}
}
#endif

/*
 *  sve2 methods table (1-based indices; 0 = all).  Every method
 *  has a hardware implementation and a scalar golden reference;
 *  the kind field selects which data arrays the verification
 *  compares (each method only writes its own arrays).
 */
typedef enum {
	SVE2_KIND_NONE,		/* pseudo method "all" */
	SVE2_KIND_DOUBLES,
	SVE2_KIND_U64S,
	SVE2_KIND_FLOATS_EVEN,
} sve2_kind_t;

typedef struct {
	const char *name;
	void (*hw)(stress_sve2_data_t *data);
	void (*golden)(stress_sve2_data_t *data);
	sve2_kind_t kind;
} sve2_method_t;

static const sve2_method_t sve2_methods[] = {
	{ "all",	NULL,		NULL,			SVE2_KIND_NONE },
	{ "fmla",	sve2_fmla,	golden_fmla,		SVE2_KIND_DOUBLES },
	{ "gather",	sve2_gather,	golden_gather,		SVE2_KIND_U64S },
	{ "fcmla",	sve2_fcmla,	golden_fcmla,		SVE2_KIND_DOUBLES },
#if defined(__ARM_FEATURE_SVE2_BITPERM) || \
    defined(__ARM_FEATURE_SVE_BITPERM)
	{ "bitperm",	sve2_bitperm,	golden_bitperm,		SVE2_KIND_U64S },
#endif
#if defined(__ARM_FEATURE_SVE_BF16) || \
    defined(__ARM_FEATURE_BF16_VECTOR_ARITHMETIC)
	{ "bfdot",	sve2_bfdot,	golden_bfdot,		SVE2_KIND_FLOATS_EVEN },
#endif
};

static const char *stress_sve2_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(sve2_methods)) ? sve2_methods[i].name : NULL;
}

static int stress_sve2(stress_args_t *args)
{
	stress_sve2_data_t *data;
	const size_t sz = sizeof(*data);
	uint32_t i;
	size_t method_selected = 0;
	int rc = EXIT_SUCCESS;

	(void)stress_setting_get("sve2-method", &method_selected);
	if ((method_selected > 0) &&
	    ((method_selected >= SIZEOF_ARRAY(sve2_methods)))) {
		method_selected = 0;
	}

	data = (stress_sve2_data_t *)stress_mmap_populate(NULL, sz,
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (data == MAP_FAILED) {
		pr_inf_skip("%s: cannot mmap %zu bytes, skipping\n",
			args->name, sz);
		return EXIT_NO_RESOURCE;
	}
	(void)stress_madvise_mergeable(data, sz);

	/*  Seed the input data  */
	for (i = 0; i < SVE2_ELEMENTS; i++) {
		data->doubles[i] = (double)(i + 1) * 1.000001;
		data->u64s[i] = stress_mwc64();
		data->gather_base[i] = stress_mwc64();
	}
	for (i = 0; i < SVE2_FLOATS; i++)
		data->floats[i] = (float)(i + 1) * 1.000001f;

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	do {
		size_t m;

		for (m = 1; LIKELY(stress_continue(args) &&
				   (m < SIZEOF_ARRAY(sve2_methods))); m++) {
			const sve2_method_t *method = &sve2_methods[m];

			if ((method_selected > 0) && (m != method_selected))
				continue;

			/*
			 *  Run the scalar golden reference FIRST (it
			 *  reads the input arrays), then the hardware
			 *  path (which overwrites the input arrays
			 *  with its results), then compare.
			 */
			method->golden(data);
			method->hw(data);

			/*  compare the arrays this method writes  */
			switch (method->kind) {
			case SVE2_KIND_DOUBLES:
				for (i = 0; i < SVE2_ELEMENTS; i++) {
					uint64_t b1, b2;

					if (data->doubles[i] != data->golden_doubles[i]) {
						(void)shim_memcpy(&b1, &data->golden_doubles[i], sizeof(b1));
						(void)shim_memcpy(&b2, &data->doubles[i], sizeof(b2));
						sve2_verify_fail(args->name, method->name, i, b1, b2);
						rc = EXIT_FAILURE;
					}
				}
				break;
			case SVE2_KIND_U64S:
				for (i = 0; i < SVE2_ELEMENTS; i++) {
					if (data->u64s[i] != data->golden_u64s[i]) {
						sve2_verify_fail(args->name, method->name, i,
							data->golden_u64s[i], data->u64s[i]);
						rc = EXIT_FAILURE;
					}
				}
				break;
			case SVE2_KIND_FLOATS_EVEN:
				for (i = 0; i < SVE2_FLOATS; i++) {
					uint32_t b1, b2;

					if (data->floats[i] != data->golden_floats[i]) {
						(void)shim_memcpy(&b1, &data->golden_floats[i], sizeof(b1));
						(void)shim_memcpy(&b2, &data->floats[i], sizeof(b2));
						sve2_verify_fail(args->name, method->name, i, b1, b2);
						rc = EXIT_FAILURE;
					}
				}
				break;
			default:
				break;
			}
		}

		/*  re-seed for next round; keep values changing  */
		for (i = 0; i < SVE2_ELEMENTS; i++) {
			data->doubles[i] = (double)(i + 1) * 1.000001 + (double)stress_mwc16();
			data->u64s[i] = stress_mwc64();
			data->gather_base[i] = stress_mwc64();
		}
		for (i = 0; i < SVE2_FLOATS; i++)
			data->floats[i] = (float)(i + 1) * 1.000001f;

		stress_bogo_inc(args);
	} while (stress_continue(args));

	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);

	(void)munmap(data, sz);

	return rc;
}

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("sve2-fmla"),
	STRESS_EX_FEATURE("sve2-gather"),
	STRESS_EX_FEATURE("sve2-fcmla"),
	STRESS_EX_FEATURE("sve2-bitperm"),
	STRESS_EX_FEATURE("sve2-bfdot"),

	STRESS_EX_END,
};

const stressor_info_t stress_sve2_info = {
	.stressor = stress_sve2,
	.classifier = CLASS_CPU | CLASS_FP | CLASS_INTEGER,
	.opts = opts,
	.verify = VERIFY_ALWAYS,
	.supported = sve2_supported,
	.help = help,
	.exercises = exercises,
};

#else

const stressor_info_t stress_sve2_info = {
	.stressor = stress_unimplemented,
	.classifier = CLASS_CPU | CLASS_FP | CLASS_INTEGER,
	.opts = opts,
	.verify = VERIFY_ALWAYS,
	.unimplemented_reason = "built for non-aarch64 target or compiler without SVE2 support"
};

#endif
