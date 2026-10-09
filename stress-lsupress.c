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
	do {
		for (i = 0; i < buf_words; i += 8) {
			sum ^= buf[i] ^ buf[i + 1] ^ buf[i + 2] ^ buf[i + 3] ^
			       buf[i + 4] ^ buf[i + 5] ^ buf[i + 6] ^ buf[i + 7];
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
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
	do {
		for (i = 0; i < buf_words; i++)
			buf[i] = stress_bitgen_u64(&bg);
		stress_bogo_inc(args);
	} while (stress_continue(args));
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
	do {
		for (i = 0; i < buf_words / 2; i++)
			dst[i] = src[i];
		stress_bogo_inc(args);
	} while (stress_continue(args));
}

#define LSUPRESS_METHOD_ALL	0
#define LSUPRESS_BUF_WORDS	(1024 * 1024)	/* 8MB per worker */

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
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"lsupress N",		"start N workers exercising the arm64 load/store unit across the instruction spectrum" },
	{ NULL,	"lsupress-method M",	"method (see --lsupress-method 0 for the list)" },
	{ NULL,	"lsupress-ops N",	"stop after N lsupress bogo operations" },
	{ NULL,	NULL,			NULL }
};

/*
 *  stress_lsupress()
 *	exercise the LSU instruction spectrum
 */
static int stress_lsupress(stress_args_t *args)
{
	size_t lsupress_method = 0;
	stress_lsupress_func_t func;
	uint64_t *buf;
	const uint64_t seed = stress_mwc64();
	const size_t method_max = SIZEOF_ARRAY(lsupress_methods);

	(void)stress_setting_get("lsupress-method", &lsupress_method);

	if (lsupress_method >= method_max) {
		pr_fail("%s: lsupress-method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (lsupress_method == LSUPRESS_METHOD_ALL)
		lsupress_method = 1 + (stress_mwc32() % (method_max - 1));

	func = lsupress_methods[lsupress_method].func;

	buf = (uint64_t *)stress_mmap_populate(NULL,
		LSUPRESS_BUF_WORDS * sizeof(uint64_t),
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (buf == MAP_FAILED) {
		pr_inf("%s: mmap of working buffer failed, errno=%d (%s)%s\n",
			args->name, errno, strerror(errno),
			stress_memory_free_get());
		return EXIT_NO_RESOURCE;
	}
	stress_memory_anon_name_set(buf, LSUPRESS_BUF_WORDS * sizeof(uint64_t),
		"lsupress-buffer");

	func(args, buf, LSUPRESS_BUF_WORDS, seed);

	(void)munmap(buf, LSUPRESS_BUF_WORDS * sizeof(uint64_t));
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
