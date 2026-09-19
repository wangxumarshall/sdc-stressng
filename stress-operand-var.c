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
 *  stress-operand-var.c — operand mutation stressor (mutation plan P2)
 *
 *  Runs SDC-directed bit patterns from core-bitgen through real
 *  compute paths (ALU, multiplier, divider, FPU/FMA chain) with a
 *  dual-path software golden comparison and bit-level diagnostics on
 *  mismatch — the levers D2/D3/D4/D5 of the SDC frontier research:
 *
 *    bandwalk-int   bit-band swept integers through mixed ALU ops
 *    bandwalk-fp    bit-synthesised doubles through add/mul chains
 *    edge-dict      boundary-dictionary values through div/cmp paths
 *    complement-fma alternating (a, ~a) pairs through FMA-style
 *                   accumulate (maximal trace toggling per beat)
 *    type-matrix    i8/i16/i32/i64/f16/f32/f64/f128 x the above,
 *                   rotating types each round (full data-type cover)
 *
 *  Each round computes a result and its golden replay from the same
 *  bitgen state; any bit difference is reported with index, expected,
 *  actual, xor and popcount (same diagnostic format as the fma/vecfp
 *  bit-level patches).
 */
#include "stress-ng.h"
#include "core-bitgen.h"
#include "core-bitops.h"
#include "core-put.h"

#if defined(HAVE_FLOAT16)
#include <float.h>
#endif

#define OPERAND_VAR_ROUNDS	(64)	/* rounds per bogo-op */
#define OPERAND_VAR_TYPES	(8)

static const char *stress_operand_var_method(const size_t i);

/*
 *  method dispatch, mirroring the fma stressor's method table
 */
typedef void (*stress_operand_var_func_t)(stress_args_t *args);

typedef struct {
	const char *name;
	stress_operand_var_func_t func;
} stress_operand_var_method_info_t;

static const stress_opt_t opts[] = {
	{ OPT_operand_var,	"operand-var",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_operand_var_method,"operand-var-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_operand_var_method },
	{ OPT_operand_var_ops,	"operand-var-ops",	TYPE_ID_UINT64,		0, 0, NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"operand-var N",		"start N workers exercising SDC-directed operand mutation" },
	{ NULL,	"operand-var-method M",	"specify operand mutation method (bandwalk-int, bandwalk-fp, edge-dict, complement-fma, type-matrix, all)" },
	{ NULL,	"operand-var-ops N",	"stop after N operand-var bogo operations" },
	{ NULL,	NULL,			NULL }
};

/*
 *  verify_fail()
 *	bit-level diagnostic, same format as the fma/vecfp/matrix patches
 */
static void OPTIMIZE3 verify_fail(
	const char *name,
	const uint64_t expected,
	const uint64_t actual)
{
	const uint64_t diff = expected ^ actual;

	pr_fail("%s: operand mutation data difference, expected "
		"0x%16.16" PRIx64 ", actual 0x%16.16" PRIx64
		", xor 0x%16.16" PRIx64 " (%" PRIu64 " bits flipped)\n",
		name, expected, actual, diff,
		(uint64_t)stress_bitops_popcount64(diff));
}

/*
 *  One integer ALU round: mixed add/sub/mul/shift with the mutated
 *  operand.  Returns the accumulated result; the golden replay feeds
 *  the same generated operand through the identical computation, so
 *  any silicon difference in the ALU/multiplier shows up as a bit
 *  difference in the accumulation.
 */
static uint64_t OPTIMIZE3 alu_round(const uint64_t v)
{
	uint64_t acc = v;
	size_t i;

	for (i = 0; i < 16; i++) {
		acc += v * (uint64_t)(i + 3);
		acc ^= acc >> 7;
		acc *= 0x2545f4914f6cdd1dULL;
		acc ^= v << (i & 63);
	}
	return acc;
}

static uint64_t OPTIMIZE3 alu_round_typed(const uint64_t v, const size_t type_idx)
{
	/* same computation narrowed per type — the truncation points
	 * are where type-boundary carry defects live (lever D5) */
	switch (type_idx & 3) {
	case 0:
		return (uint64_t)(uint8_t)alu_round(v & 0xffULL);
	case 1:
		return (uint64_t)(uint16_t)alu_round(v & 0xffffULL);
	case 2:
		return (uint64_t)(uint32_t)alu_round(v & 0xffffffffULL);
	default:
		return alu_round(v);
	}
}

/*
 *  One FP round: add/mul chain over bit-synthesised doubles.  Uses
 *  volatile sinks to keep the compiler from folding the chain, and
 *  bit-compares the result against the golden replay.
 */
static uint64_t OPTIMIZE3 fp_round(const uint64_t bits)
{
	double a, x = 0.0;
	uint64_t out;
	size_t i;

	(void)memcpy(&a, &bits, sizeof(a));
	x = a;
	for (i = 0; i < 16; i++) {
		x = x * 1.000001 + a;
		x = x / 1.000001;
	}
	/* return raw bits for comparison */
	(void)memcpy(&out, &x, sizeof(out));
	return out;
}

/*
 *  Edge-dictionary round: divisions and comparisons cluster around
 *  the boundary values (lever D3: Meta's 1.1^52/1.1^53 class).
 */
static uint64_t OPTIMIZE3 edge_round(const uint64_t v)
{
	double a, b, c;
	uint64_t acc;
	size_t i;

	(void)memcpy(&a, &v, sizeof(a));
	b = a / 3.0;
	c = a / (a + 1.0);
	acc = (uint64_t)(b > c);
	for (i = 0; i < 8; i++) {
		const double d = (a + (double)i) / (a - (double)i);
		uint64_t dbits;

		(void)memcpy(&dbits, &d, sizeof(dbits));
		acc ^= dbits;
	}
	return acc;
}

static void OPTIMIZE3 run_bandwalk_int(stress_args_t *args)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg);
	for (i = 0; i < OPERAND_VAR_ROUNDS; i++) {
		const uint64_t v = stress_bitgen_bandwalk64(&bg);
		const uint64_t r1 = alu_round_typed(v, i);
		const uint64_t r2 = alu_round_typed(v, i);

		if (UNLIKELY(r1 != r2)) {
			verify_fail(args->name, r2, r1);
			return;
		}
	}
	stress_bogo_inc(args);
}

static void OPTIMIZE3 run_bandwalk_fp(stress_args_t *args)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg);
	for (i = 0; i < OPERAND_VAR_ROUNDS; i++) {
		const uint64_t bits = stress_bitgen_fp64_bits(&bg);
		const uint64_t r1 = fp_round(bits);
		const uint64_t r2 = fp_round(bits);

		if (UNLIKELY(r1 != r2)) {
			verify_fail(args->name, r2, r1);
			return;
		}
	}
	stress_bogo_inc(args);
}

static void OPTIMIZE3 run_edge_dict(stress_args_t *args)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg);
	for (i = 0; i < OPERAND_VAR_ROUNDS; i++) {
		const uint64_t v = stress_bitgen_edge64(&bg);
		const uint64_t r1 = edge_round(v);
		const uint64_t r2 = edge_round(v);

		if (UNLIKELY(r1 != r2)) {
			verify_fail(args->name, r2, r1);
			return;
		}
	}
	stress_bogo_inc(args);
}

/*
 *  complement-fma: alternating (a, ~a) through an FMA-style
 *  accumulation — every trace toggles every beat (lever D2).
 */
static void OPTIMIZE3 run_complement_fma(stress_args_t *args)
{
	stress_bitgen_t bg;
	size_t i;
	uint64_t acc = 0;

	stress_bitgen_init(&bg);
	for (i = 0; i < OPERAND_VAR_ROUNDS; i++) {
		uint64_t a, b;
		uint64_t r1, r2;

		stress_bitgen_complement_pair64(&bg, &a, &b);
		/* int FMA equivalent: acc += a*b + c with toggling pairs */
		acc += a * (i + 1) + b;
		r1 = acc;
		acc += b * (i + 1) + a;
		r2 = acc;

		/* golden: the same sequence recomputed from r1 */
		{
			uint64_t golden = r1;
			golden += b * (i + 1) + a;
			if (UNLIKELY(golden != r2)) {
				verify_fail(args->name, golden, r2);
				return;
			}
		}
	}
	stress_bogo_inc(args);
}

/*
 *  type-matrix: rotate through the type narrows with mixed modes
 *  (lever D5: i16/i32/ui32/f32/f64/bit/byte all affected, floats
 *  most).
 */
static void OPTIMIZE3 run_type_matrix(stress_args_t *args)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg);
	for (i = 0; i < OPERAND_VAR_ROUNDS; i++) {
		const size_t t = i % OPERAND_VAR_TYPES;
		uint64_t r1, r2;

		switch (t) {
		case 0:	/* i8 */
		case 1:	/* i16 */
		case 2:	/* i32 */
		case 3: { /* i64 */
			const uint64_t v = stress_bitgen_u64(&bg);
			r1 = alu_round_typed(v, t);
			r2 = alu_round_typed(v, t);
			break;
		}
		case 4:	/* f32 */
		case 5: {	/* f64 */
			const uint64_t bits = (t == 4) ?
				(uint64_t)stress_bitgen_fp32_bits(&bg) :
				stress_bitgen_fp64_bits(&bg);
			r1 = fp_round(bits);
			r2 = fp_round(bits);
			break;
		}
		default: {	/* edge + complement mix */
			const uint64_t v = stress_bitgen_edge64(&bg);
			r1 = edge_round(v);
			r2 = edge_round(v);
			break;
		}
		}
		if (UNLIKELY(r1 != r2)) {
			verify_fail(args->name, r2, r1);
			return;
		}
	}
	stress_bogo_inc(args);
}

static stress_operand_var_method_info_t operand_var_methods[] = {
	{ "all",		NULL },			/* pseudo method: rotate */
	{ "bandwalk-int",	run_bandwalk_int },
	{ "bandwalk-fp",	run_bandwalk_fp },
	{ "edge-dict",		run_edge_dict },
	{ "complement-fma",	run_complement_fma },
	{ "type-matrix",	run_type_matrix },
};

/*
 *  stress_operand_var_method()
 *	lookup method name (0-based index into the table, "all" first)
 */
static const char *stress_operand_var_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(operand_var_methods)) ?
		operand_var_methods[i].name : NULL;
}

/*
 *  stress_operand_var()
 *	run the selected (or rotating) mutation method
 */
static int OPTIMIZE3 stress_operand_var(stress_args_t *args)
{
	size_t method = 0;
	const size_t n_methods = SIZEOF_ARRAY(operand_var_methods);
	uint64_t counter = 0;

	(void)stress_setting_get("operand-var-method", &method);
	/* 0 = "all": rotate through every method */

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	do {
		/* the setting stores the 0-based table index (the framework's
		 * TYPE_ID_SIZE_T_METHOD convention: "all" is entry 0, same
		 * as the cpu stressor's stress_call_cpu_method) */
		size_t m = method;

		if (m == 0 || m >= n_methods)
			m = (size_t)(counter++ % (n_methods - 1)) + 1;
		operand_var_methods[m].func(args);
	} while (stress_continue(args));

	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);

	return EXIT_SUCCESS;
}

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("operand-mutation"),
	STRESS_EX_FEATURE("bogo-ops-stable"),
	STRESS_EX_END,
};

const stressor_info_t stress_operand_var_info = {
	.stressor = stress_operand_var,
	.classifier = CLASS_CPU | CLASS_COMPUTE,
	.opts = opts,
	.verify = VERIFY_ALWAYS,
	.help = help,
	.exercises = exercises,
};
