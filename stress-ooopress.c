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
#include "core-memory.h"

/*
 *  stress-ooopress.c - out-of-order scheduler pressure: drive the
 *  rename unit, issue scheduler and reorder buffer of an OoO core
 *  into their boundary states (excitation-guide gap row "OoO
 *  scheduler pressure with shaped dependencies").
 *
 *  Every method runs ~256 shaped integer operations per bogo-op so
 *  the bogo-op rates are directly comparable across methods - the
 *  dep-chain vs indep-max ratio is the achieved IPC of the core.
 *
 *  SDC-directed bitgen shapes flow into the pressure paths: a
 *  256-word operand pool drawn once per worker from the mode-mixed
 *  generator (bandwalk / edge dictionary / jittered uniform) is
 *  re-coloured every bogo-op by a fresh bitgen value, so the shape
 *  mix keeps evolving without a serial generator in the hot loop.
 *
 *  Kernels make zero framework calls (bogo accounting stays in the
 *  main loop) and protect every computed value from dead-store
 *  elimination with the fork's consume-barrier pattern.
 *
 *  No verification oracle: this is a pure excitation stressor,
 *  detection is delegated to SDCShield running alongside.
 */

#define OOOPRESS_METHOD_ALL	0
#define OOOPRESS_POOL_WORDS	256	/* shaped operand pool */
#define OOOPRESS_CHAIN_DEPTH	64	/* serial chain steps per segment */
#define OOOPRESS_INDEP_WAVES	16	/* independent waves per bogo-op */
#define OOOPRESS_LINK_BITS	17	/* 128K entries = 1MB chase array */
#define OOOPRESS_LINK_WORDS	(1U << OOOPRESS_LINK_BITS)
#define OOOPRESS_CHASE_STEPS	256	/* dependent loads per bogo-op */

typedef struct {
	uint64_t	seed;			/* rolling chain state */
	uint64_t	chase;			/* rolling load-use pointer */
	stress_bitgen_t bg;			/* per-worker shape stream */
	uint64_t	pool[OOOPRESS_POOL_WORDS];	/* shaped operands */
	uint64_t	*links;			/* load-use permutation (may be NULL) */
} stress_ooopress_ctx_t;

typedef void (*stress_ooopress_func_t)(stress_ooopress_ctx_t *ctx);

/* dead-store-proof consumption of a computed value */
#if defined(__GNUC__)
#define OOOPRESS_CONSUME(v)	__asm__ __volatile__ ("" : : "r" (v) : "memory")
#else
static volatile uint64_t ooopress_sink;
#define OOOPRESS_CONSUME(v)	(ooopress_sink = (uint64_t)(v))
#endif

/*
 *  Four heterogeneous dependent steps: xor, add, rotate-left-1, sub.
 *  Adjacent distinct operations cannot be algebraically merged, so
 *  the chain stays 4 real serial instructions per expansion - the
 *  compiler cannot shorten it, only schedule it as written.
 */
#define OOOPRESS_CHAIN_4(x, k) do {	\
	(x) ^= (k);			\
	(x) += (k);			\
	(x) = ((x) << 1) | ((x) >> 63);	\
	(x) -= (k);			\
} while (0)

#define OOOPRESS_CHAIN_16(x, k)	do {	\
	OOOPRESS_CHAIN_4(x, k);		\
	OOOPRESS_CHAIN_4(x, k);		\
	OOOPRESS_CHAIN_4(x, k);		\
	OOOPRESS_CHAIN_4(x, k);		\
} while (0)

#define OOOPRESS_CHAIN_64(x, k)	do {	\
	OOOPRESS_CHAIN_16(x, k);	\
	OOOPRESS_CHAIN_16(x, k);	\
	OOOPRESS_CHAIN_16(x, k);	\
	OOOPRESS_CHAIN_16(x, k);	\
} while (0)

/*
 *  One independent wave: 8 mutually independent (xor + add) ops fed
 *  from distinct pool slots, tree-reduced (depth 3).  Everything is
 *  issueable in parallel - this is the max-ILP building block.
 */
static inline uint64_t ALWAYS_INLINE ooopress_indep_wave(
	const uint64_t *pool,
	const uint64_t m,
	const unsigned int b)
{
	const uint64_t v0 = (pool[((b << 3) + 0) & 255] ^ m) + pool[((b << 3) + 1) & 255];
	const uint64_t v1 = (pool[((b << 3) + 2) & 255] ^ m) + pool[((b << 3) + 3) & 255];
	const uint64_t v2 = (pool[((b << 3) + 4) & 255] ^ m) + pool[((b << 3) + 5) & 255];
	const uint64_t v3 = (pool[((b << 3) + 6) & 255] ^ m) + pool[((b << 3) + 7) & 255];
	const uint64_t v4 = (pool[((b << 3) + 8) & 255] ^ m) + pool[((b << 3) + 9) & 255];
	const uint64_t v5 = (pool[((b << 3) + 10) & 255] ^ m) + pool[((b << 3) + 11) & 255];
	const uint64_t v6 = (pool[((b << 3) + 12) & 255] ^ m) + pool[((b << 3) + 13) & 255];
	const uint64_t v7 = (pool[((b << 3) + 14) & 255] ^ m) + pool[((b << 3) + 15) & 255];

	return ((v0 ^ v1) ^ (v2 ^ v3)) ^ ((v4 ^ v5) ^ (v6 ^ v7));
}

/*
 *  dep-chain: 4 x 64-step serial ALU chains (256 dependent ops).
 *  The head of the chain cannot retire until the whole tail has
 *  executed, so the in-flight window stays compressed and the
 *  issue ports starve behind the latency.
 */
static NOINLINE OPTIMIZE3 void ooopress_dep_chain(stress_ooopress_ctx_t *ctx)
{
	const uint64_t k = stress_bitgen_u64(&ctx->bg);
	const uint64_t k1 = (k << 17) | (k >> 47);		/* rol 17 */
	const uint64_t k2 = ~k;
	const uint64_t k3 = k + 0x9E3779B97F4A7C15ULL;
	uint64_t x = ctx->seed;

	OOOPRESS_CHAIN_64(x, k);
	OOOPRESS_CHAIN_64(x, k1);
	OOOPRESS_CHAIN_64(x, k2);
	OOOPRESS_CHAIN_64(x, k3);
	OOOPRESS_CONSUME(x);
	ctx->seed = x;		/* the chain never restarts */
}

/*
 *  indep-max: 16 independent waves (~256 ops, no cross-wave data
 *  dependencies beyond the 1-op checksum).  The scheduler can issue
 *  every wave op as soon as an execution port is free, so the ROB
 *  fills to capacity and retirement runs at allocation rate.
 */
static NOINLINE OPTIMIZE3 void ooopress_indep_max(stress_ooopress_ctx_t *ctx)
{
	const uint64_t m = stress_bitgen_u64(&ctx->bg);
	uint64_t sum = 0;
	unsigned int b;

	for (b = 0; b < OOOPRESS_INDEP_WAVES; b++)
		sum ^= ooopress_indep_wave(ctx->pool, m, b);
	OOOPRESS_CONSUME(sum);
	ctx->seed ^= sum;
}

/*
 *  alt: 64 dependent chain steps then 4 independent waves (64 ops),
 *  twice - the ROB drains behind each chain head and refills to
 *  capacity on each independent block, a block-granularity
 *  scheduler pressure waveform.
 */
static NOINLINE OPTIMIZE3 void ooopress_alt(stress_ooopress_ctx_t *ctx)
{
	const uint64_t k = stress_bitgen_u64(&ctx->bg);
	const uint64_t m = stress_bitgen_u64(&ctx->bg);
	uint64_t x = ctx->seed;
	uint64_t sum = 0;

	OOOPRESS_CHAIN_64(x, k);
	sum ^= ooopress_indep_wave(ctx->pool, m, 0);
	sum ^= ooopress_indep_wave(ctx->pool, m, 1);
	sum ^= ooopress_indep_wave(ctx->pool, m, 2);
	sum ^= ooopress_indep_wave(ctx->pool, m, 3);

	OOOPRESS_CHAIN_64(x, ~k);
	sum ^= ooopress_indep_wave(ctx->pool, m, 4);
	sum ^= ooopress_indep_wave(ctx->pool, m, 5);
	sum ^= ooopress_indep_wave(ctx->pool, m, 6);
	sum ^= ooopress_indep_wave(ctx->pool, m, 7);

	OOOPRESS_CONSUME(x ^ sum);
	ctx->seed = x;
}

/*
 *  rename-reuse: bursts of 32 consecutive writes to ONE architectural
 *  destination (the rename map slot is remapped on every op - physical
 *  registers churn at maximum alloc/free rate) interleaved with
 *  16-wide bursts writing 16 DISTINCT destinations at once (rename
 *  map occupancy with 16 simultaneously live mappings).
 */
static NOINLINE OPTIMIZE3 void ooopress_rename_reuse(stress_ooopress_ctx_t *ctx)
{
	const uint64_t k = stress_bitgen_u64(&ctx->bg);
	const uint64_t m = stress_bitgen_u64(&ctx->bg);
	const uint64_t *pool = ctx->pool;
	uint64_t y = ctx->seed;
	uint64_t sum = 0;

	/* burst A: one destination, 32 serial writes (WAW + RAW) */
	OOOPRESS_CHAIN_16(y, k);
	OOOPRESS_CHAIN_16(y, ~k);
	sum ^= y;

	/* burst B: 16 distinct destinations, each written once */
	{
		const uint64_t t0  = (pool[0]  ^ m) + pool[1];
		const uint64_t t1  = (pool[2]  ^ m) + pool[3];
		const uint64_t t2  = (pool[4]  ^ m) + pool[5];
		const uint64_t t3  = (pool[6]  ^ m) + pool[7];
		const uint64_t t4  = (pool[8]  ^ m) + pool[9];
		const uint64_t t5  = (pool[10] ^ m) + pool[11];
		const uint64_t t6  = (pool[12] ^ m) + pool[13];
		const uint64_t t7  = (pool[14] ^ m) + pool[15];
		const uint64_t t8  = (pool[16] ^ m) + pool[17];
		const uint64_t t9  = (pool[18] ^ m) + pool[19];
		const uint64_t t10 = (pool[20] ^ m) + pool[21];
		const uint64_t t11 = (pool[22] ^ m) + pool[23];
		const uint64_t t12 = (pool[24] ^ m) + pool[25];
		const uint64_t t13 = (pool[26] ^ m) + pool[27];
		const uint64_t t14 = (pool[28] ^ m) + pool[29];
		const uint64_t t15 = (pool[30] ^ m) + pool[31];

		sum ^= ((t0 ^ t1) ^ (t2 ^ t3)) ^ ((t4 ^ t5) ^ (t6 ^ t7));
		sum ^= ((t8 ^ t9) ^ (t10 ^ t11)) ^ ((t12 ^ t13) ^ (t14 ^ t15));
	}

	/* second pair with fresh shapes */
	OOOPRESS_CHAIN_16(y, k + m);
	OOOPRESS_CHAIN_16(y, ~(k + m));
	sum ^= y;

	{
		const uint64_t t0  = (pool[32] ^ m) + pool[33];
		const uint64_t t1  = (pool[34] ^ m) + pool[35];
		const uint64_t t2  = (pool[36] ^ m) + pool[37];
		const uint64_t t3  = (pool[38] ^ m) + pool[39];
		const uint64_t t4  = (pool[40] ^ m) + pool[41];
		const uint64_t t5  = (pool[42] ^ m) + pool[43];
		const uint64_t t6  = (pool[44] ^ m) + pool[45];
		const uint64_t t7  = (pool[46] ^ m) + pool[47];
		const uint64_t t8  = (pool[48] ^ m) + pool[49];
		const uint64_t t9  = (pool[50] ^ m) + pool[51];
		const uint64_t t10 = (pool[52] ^ m) + pool[53];
		const uint64_t t11 = (pool[54] ^ m) + pool[55];
		const uint64_t t12 = (pool[56] ^ m) + pool[57];
		const uint64_t t13 = (pool[58] ^ m) + pool[59];
		const uint64_t t14 = (pool[60] ^ m) + pool[61];
		const uint64_t t15 = (pool[62] ^ m) + pool[63];

		sum ^= ((t0 ^ t1) ^ (t2 ^ t3)) ^ ((t4 ^ t5) ^ (t6 ^ t7));
		sum ^= ((t8 ^ t9) ^ (t10 ^ t11)) ^ ((t12 ^ t13) ^ (t14 ^ t15));
	}

	OOOPRESS_CONSUME(sum);
	ctx->seed = y;
}

/*
 *  branch-mix: 64 data branches; each condition tests the MSB of the
 *  chain value x, so the branch resolves late in the dependency
 *  chain and every branch behind it is speculative.  Direction is
 *  shaped by the bitgen pool: bandwalk values come in runs of
 *  similar bit patterns (predictor-learnable stretches), edge
 *  dictionary values flip abruptly (mispredict bursts).  The taken
 *  arm divides (the longest-latency integer unit and a classic
 *  corner-case datapath); besides shaping the path-latency
 *  asymmetry it makes the diamond too expensive for the compiler's
 *  if-conversion pass, so a real conditional branch is emitted
 *  where a cheap ALU diamond would have been folded into csel.
 */
static NOINLINE OPTIMIZE3 void ooopress_branch_mix(stress_ooopress_ctx_t *ctx)
{
	const uint64_t m = stress_bitgen_u64(&ctx->bg);
	const uint64_t *pool = ctx->pool;
	uint64_t x = ctx->seed;
	int i;

	for (i = 0; i < OOOPRESS_CHAIN_DEPTH; i++) {
		const uint64_t d = pool[(i << 2) & 255] ^ m;

		if (x & (1ULL << 63))
			x = (x ^ d) / ((d | 1ULL) & 0xffffULL);
		else
			x = (x + d) ^ 0x61C8864680B583EBULL;
	}
	OOOPRESS_CONSUME(x);
	ctx->seed = x;
}

/*
 *  load-use: 256 dependent loads, each address is the previous
 *  load's data - the head-of-line blocking pattern that fills the
 *  ROB to capacity behind one unresolved memory access.  links[]
 *  is a single-cycle permutation (affine map with a == 1 mod 4 and
 *  odd b, parameters drawn from bitgen), so the walk covers every
 *  entry without revisiting and never settles into a short cycle.
 */
static NOINLINE OPTIMIZE3 void ooopress_load_use(stress_ooopress_ctx_t *ctx)
{
	uint64_t p = ctx->chase;
	int i;

	for (i = 0; i < OOOPRESS_CHASE_STEPS; i++)
		p = ctx->links[p & (OOOPRESS_LINK_WORDS - 1)];
	OOOPRESS_CONSUME(p);
	ctx->chase = p;
}

typedef struct {
	const char *name;
	const stress_ooopress_func_t func;
} stress_ooopress_method_info_t;

static const stress_ooopress_method_info_t ooopress_methods[] = {
	{ "all",		NULL, },			/* 0: random pick per worker */
	{ "dep-chain",		ooopress_dep_chain },
	{ "indep-max",		ooopress_indep_max },
	{ "alt",		ooopress_alt },
	{ "rename-reuse",	ooopress_rename_reuse },
	{ "branch-mix",		ooopress_branch_mix },
	{ "load-use",		ooopress_load_use },
};

static const char *stress_ooopress_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(ooopress_methods)) ? ooopress_methods[i].name : NULL;
}

static const stress_opt_t opts[] = {
	{ OPT_ooopress_method,	"ooopress-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_ooopress_method },
	{ OPT_ooopress_ops,	"ooopress-ops",		TYPE_ID_UINT64,		0, 0, NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"ooopress N",		"start N workers pressuring the out-of-order scheduler (rename, issue window, reorder buffer)" },
	{ NULL,	"ooopress-method M",	"method (see --ooopress-method 0 for the list)" },
	{ NULL,	"ooopress-ops N",	"stop after N ooopress bogo operations" },
	{ NULL,	NULL,			NULL }
};

/*
 *  stress_ooopress()
 *	drive the OoO engine into boundary states
 */
static int stress_ooopress(stress_args_t *args)
{
	size_t ooopress_method = 0;
	stress_ooopress_ctx_t ctx;
	stress_ooopress_func_t func;
	const size_t method_max = SIZEOF_ARRAY(ooopress_methods);
	const uint64_t seed = stress_mwc64();
	size_t i;

	(void)stress_setting_get("ooopress-method", &ooopress_method);

	if (ooopress_method >= method_max) {
		pr_fail("%s: ooopress-method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (ooopress_method == OOOPRESS_METHOD_ALL)
		ooopress_method = 1 + (stress_mwc32() % (method_max - 1));

	func = ooopress_methods[ooopress_method].func;

	ctx.seed = seed;
	ctx.chase = seed;
	ctx.links = NULL;
	stress_bitgen_init(&ctx.bg);
	stress_bitgen_seed(&ctx.bg, seed);
	for (i = 0; i < OOOPRESS_POOL_WORDS; i++)
		ctx.pool[i] = stress_bitgen_u64(&ctx.bg);

	if (func == ooopress_load_use) {
		/* single-cycle affine permutation, parameters bitgen-shaped:
		 * a == 1 (mod 4) and odd b guarantees one 2^17-long cycle */
		const uint64_t va = stress_bitgen_u64(&ctx.bg);
		const uint64_t a = (va & ~3ULL) | 5ULL;
		const uint64_t b = stress_bitgen_u64(&ctx.bg) | 1ULL;

		ctx.links = (uint64_t *)stress_mmap_populate(NULL,
			(size_t)OOOPRESS_LINK_WORDS * sizeof(uint64_t),
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (ctx.links == MAP_FAILED) {
			pr_inf("%s: mmap of the load-use chase array failed, errno=%d (%s)%s\n",
				args->name, errno, strerror(errno),
				stress_memory_free_get());
			return EXIT_NO_RESOURCE;
		}
		stress_memory_anon_name_set(ctx.links,
			(size_t)OOOPRESS_LINK_WORDS * sizeof(uint64_t),
			"ooopress-links");
		for (i = 0; i < OOOPRESS_LINK_WORDS; i++)
			ctx.links[i] = (a * (uint64_t)i + b) &
					(uint64_t)(OOOPRESS_LINK_WORDS - 1);
	}

	do {
		func(&ctx);
		stress_bogo_inc(args);
	} while (stress_continue(args));

	if (ctx.links)
		(void)munmap(ctx.links, (size_t)OOOPRESS_LINK_WORDS * sizeof(uint64_t));

	return EXIT_SUCCESS;
}

const stressor_info_t stress_ooopress_info = {
	.stressor = stress_ooopress,
	.classifier = CLASS_CPU | CLASS_MEMORY,
	.opts = opts,
	.verify = VERIFY_NONE,
	.help = help
};
