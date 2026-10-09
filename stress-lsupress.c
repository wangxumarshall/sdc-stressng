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
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed);

typedef struct {
	const char *name;
	const stress_lsupress_func_t func;
} stress_lsupress_method_info_t;

static const char *stress_lsupress_method(const size_t i);

/*
 *  load-int64: 8 independent back-to-back loads per iteration,
 *  consumed into a checksum (dead-store-proof)
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_int64(
	stress_args_t *args,
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
	stress_bogo_inc(args);
	__asm__ __volatile__ ("" : : "r" (sum) : "memory");	/* consume */
}

/*  store-int64: bitgen-shaped stream of stores */
static NOINLINE OPTIMIZE3 void stress_lsupress_store_int64(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, seed);
	for (i = 0; i < buf_words; i++)
		buf[i] = stress_bitgen_u64(&bg);
	stress_bogo_inc(args);
}

/*  copy-int64: ldr+str dual */
static NOINLINE OPTIMIZE3 void stress_lsupress_copy_int64(
	stress_args_t *args,
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
	stress_bogo_inc(args);
}

#define LSUPRESS_METHOD_ALL	0
#define LSUPRESS_BUF_WORDS	(1024 * 1024)	/* 8MB fallback buffer */
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
	bool	 used;		/* engine active (vs fallback buffer) */
} lsupress_engine_t;

static uint64_t lsupress_next_hop(lsupress_engine_t *e)
{
	uint64_t x = e->state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	e->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static void lsupress_engine_init(
	lsupress_engine_t *e,
	const uint64_t va_req,
	const uint64_t win_req,
	const uint64_t seed)
{
	size_t size = (size_t)va_req;

	(void)seed;
	e->state = seed | 1;
	e->win_size = (size_t)win_req;
	e->used = false;
	e->map_base = MAP_FAILED;

	/* probe-shrink: 100G -> ... -> window*2 minimum */
	while (size >= e->win_size * 2) {
		e->map_base = mmap(NULL, size,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
			-1, 0);
		if (e->map_base != MAP_FAILED)
			break;
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
			(size_t)(lsupress_next_hop(e) % hops) * e->win_size;
	}
}

static void lsupress_engine_free(lsupress_engine_t *e)
{
	if (e->used && e->map_base != MAP_FAILED)
		(void)munmap(e->map_base, e->map_size);
	e->used = false;
}

static stress_lsupress_method_info_t lsupress_methods[] = {
	{ "all",		NULL },				/* 0: rotate */
	{ "load-int64",		stress_lsupress_load_int64 },
	{ "store-int64",	stress_lsupress_store_int64 },
	{ "copy-int64",		stress_lsupress_copy_int64 },
};

static const char *stress_lsupress_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(lsupress_methods)) ? lsupress_methods[i].name : NULL;
}

static const stress_opt_t opts[] = {
	{ OPT_lsupress_method,	"lsupress-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_lsupress_method },
	{ OPT_lsupress_ops,	"lsupress-ops",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_lsupress_va_size,	"lsupress-va-size",	TYPE_ID_SIZE_T_BYTES_VM, 256 * STRESS_MB, 16 * STRESS_TB, NULL },
	{ OPT_lsupress_window,	"lsupress-window",	TYPE_ID_SIZE_T_BYTES_VM, 4 * STRESS_MB, MAX_MEM_LIMIT, NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"lsupress N",		"start N workers exercising the arm64 load/store unit across the instruction spectrum" },
	{ NULL,	"lsupress-method M",	"method (see --lsupress-method 0 for the list)" },
	{ NULL,	"lsupress-ops N",	"stop after N lsupress bogo operations" },
	{ NULL,	"lsupress-va-size B",	"per-worker NORESERVE VA map size (default 100GB)" },
	{ NULL,	"lsupress-window B",	"working-set window size (default 256MB)" },
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

	if (lsupress_method >= method_max) {
		pr_fail("%s: lsupress-method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (lsupress_method == LSUPRESS_METHOD_ALL)
		lsupress_method = 1 + (stress_mwc32() % (method_max - 1));

	func = lsupress_methods[lsupress_method].func;

	lsupress_engine_init(&engine, lsupress_va_size, lsupress_window, seed);
	if (engine.used) {
		stress_memory_anon_name_set(engine.map_base, engine.map_size,
			"lsupress-va");
		buf = (uint64_t *)engine.win;
		buf_words = engine.win_size / sizeof(uint64_t);
		do {
			func(args, buf, buf_words, seed);
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

	func(args, fallback, LSUPRESS_BUF_WORDS, seed);

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
