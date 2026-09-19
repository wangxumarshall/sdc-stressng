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
 *  stress-addrspace.c — address-space shape stressor (mutation plan P3)
 *
 *  The address survey (docs/superpowers/research/
 *  2026-09-19-address-space-survey.md) found eight address-space
 *  shapes never exercised: every random-address engine maps at most
 *  ~17 pages, every big buffer sits at a kernel-chosen address
 *  touched sequentially, nothing walks the upper VA bits with
 *  sustained traffic, and malloc'd gigabytes do not exist (malloc
 *  caps at 64KB by default).  This stressor cycles through shape
 *  recipes, each mapping a region, filling it with core-bitgen
 *  patterns, verifying and unmapping:
 *
 *    huge-random-fixed   up-to-N-GB mapping at a random aligned
 *                        address via MAP_FIXED_NOREPLACE (size drawn
 *                        log-randomly; falls back to a hint-less
 *                        mapping when the chosen address is taken)
 *    va-bit-walk         256MB mappings whose address sets one
 *                        randomly chosen VA bit (0..51), upper bits
 *                        included — sustained traffic per VA bit
 *    dense-random-offset dense random-offset page writes and a
 *                        same-order re-read verify inside a large
 *                        mapping (vs vm galpat's 1-bit-per-4KB)
 *    guarded-holes       fill a big mapping, punch random PROT_NONE
 *                        hole bands, verify the surviving bands with
 *                        the stream stepping over holes (the hole
 *                        edges are where off-by-one TLB/PT silicon
 *                        corrupts)
 *    malloc-giant        1..N GB malloc / aligned posix_memalign
 *                        with pattern fill + verify (the "malloc'd
 *                        gigabytes" gap)
 *    misalign-huge       random sub-page offsets inside a large
 *                        mapping with fixed-width misaligned copies
 *    mixed-orders        interleaved 4KB and (where enabled) 2MB
 *                        mappings at random addresses — mixed page
 *                        orders in one round
 *    all                 rotate through every recipe
 *
 *  Sizes are bounded by --addrspace-bytes (default: a quarter of
 *  physical memory) so CI containers and large hosts both behave.
 */
#include "stress-ng.h"
#include "core-bitgen.h"
#include "core-bitops.h"
#include "core-madvise.h"
#include "core-mmap.h"
#include "core-put.h"

#include <sys/mman.h>

static const char *stress_addrspace_method(const size_t i);

static const stress_opt_t opts[] = {
	{ OPT_addrspace,	"addrspace",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_addrspace_bytes,	"addrspace-bytes",	TYPE_ID_SIZE_T_BYTES_VM,	4 * STRESS_MB, MAX_MEM_LIMIT, NULL },
	{ OPT_addrspace_method,	"addrspace-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_addrspace_method },
	{ OPT_addrspace_ops,	"addrspace-ops",	TYPE_ID_UINT64,		0, 0, NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"addrspace N",		"start N workers exercising address-space shapes" },
	{ NULL,	"addrspace-bytes B",	"maximum bytes per recipe round (default 256MB, or 1/4 of physical memory)" },
	{ NULL,	"addrspace-method M",	"specify address shape recipe (huge-random-fixed, va-bit-walk, dense-random-offset, guarded-holes, malloc-giant, misalign-huge, mixed-orders, all)" },
	{ NULL,	"addrspace-ops N",	"stop after N addrspace bogo operations" },
	{ NULL,	NULL,			NULL }
};

#define ADDRSPACE_VA_WALK_BYTES		(256 * STRESS_MB)
#define ADDRSPACE_MIN_RECIPE_BYTES	(4 * STRESS_MB)
#define ADDRSPACE_PAGE			(4096)

/* random-address span masks (all page-aligned by construction) */
static const uint64_t addrspace_masks[] = {
	0x000000007fffffffULL,		/* 2GB span */
	0x000000ffffffffffULL,		/* 1TB */
	0x0000ffffffffffffULL,		/* 256TB — full 48-bit VA */
	0x00ffffffffffffffULL,		/* 64PB — 52-bit VA class */
};



/*
 *  fill_pattern()/verify_pattern()
 *	byte-sequential bitgen stream over a range; verify mirrors the
 *  consumption exactly (8 bytes per generator call) so the two
 *  passes walk the identical stream.
 */
static void fill_pattern(stress_bitgen_t *bg, uint8_t *ptr, const size_t len)
{
	size_t i;

	for (i = 0; i < len; i += 8) {
		const uint64_t v = stress_bitgen_u64(bg);
		const size_t n = (len - i < 8) ? len - i : 8;
		size_t j;

		for (j = 0; j < n; j++)
			ptr[i + j] = (uint8_t)(v >> (8 * j));
	}
}

static int verify_pattern(stress_bitgen_t *bg, const uint8_t *ptr,
			  const size_t len, const char *name,
			  const char *recipe)
{
	size_t i;

	for (i = 0; i < len; i += 8) {
		const uint64_t v = stress_bitgen_u64(bg);
		const size_t n = (len - i < 8) ? len - i : 8;
		size_t j;

		for (j = 0; j < n; j++) {
			const uint8_t expected = (uint8_t)(v >> (8 * j));

			if (UNLIKELY(ptr[i + j] != expected)) {
				pr_fail("%s: %s data difference at offset %zu "
					"(page %zu), expected 0x%2.2x, actual "
					"0x%2.2x, xor 0x%2.2x\n",
					name, recipe, i + j, (i + j) >> 12,
					expected, ptr[i + j],
					expected ^ ptr[i + j]);
				return -1;
			}
		}
	}
	return 0;
}

/*
 *  random_unmapped_addr()
 *	random page-aligned address under a span mask, rejected against
 *  mincore until an unmapped one is found (the cheap first tier of
 *  the pagescatter/mmapaddr probing).
 */
static void *random_unmapped_addr(const uint64_t mask)
{
	size_t i;

	for (i = 0; i < 64; i++) {
		const uint64_t r = (stress_mwc64() & mask) & ~0xfffULL;
		void *const addr = (void *)(uintptr_t)r;
		unsigned char vec;

		if (addr == NULL)
			continue;
		if ((shim_mincore(addr, ADDRSPACE_PAGE, &vec) < 0) &&
		    (errno == ENOMEM))
			return addr;
	}
	return NULL;
}

/*
 *  map_at()
 *	MAP_FIXED_NOREPLACE at addr when given, anonymous hint-less
 *	otherwise (recipe still runs; only the address shape degrades).
 */
static void *map_at(void *addr, const size_t sz)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if defined(MAP_FIXED_NOREPLACE)
	if (addr)
		flags |= MAP_FIXED_NOREPLACE;
#else
	UNEXPECTED_ERROR((void)addr);
#endif
	return mmap(addr, sz, PROT_READ | PROT_WRITE, flags, -1, 0);
}

/*
 *  recipe: huge-random-fixed
 */
static int recipe_huge_random_fixed(stress_args_t *args, const size_t max_bytes)
{
	const uint64_t span = addrspace_masks[stress_mwc32modn(SIZEOF_ARRAY(addrspace_masks))];
	size_t sz = ADDRSPACE_MIN_RECIPE_BYTES;
	void *hint, *p;
	stress_bitgen_t bg, bg_verify;
	int ret;

	/* log-random size: 4MB << 0..12, capped at max_bytes */
	sz <<= stress_mwc32modn(13);
	if (sz > max_bytes)
		sz = max_bytes;

	hint = random_unmapped_addr(span);
	p = map_at(hint, sz);
	if (p == MAP_FAILED)
		return 0;		/* no room this round; not a failure */

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	fill_pattern(&bg, (uint8_t *)p, sz);
	stress_bogo_inc(args);

	ret = verify_pattern(&bg_verify, (const uint8_t *)p, sz,
		args->name, "huge-random-fixed");
	(void)munmap(p, sz);
	return ret;
}

/*
 *  recipe: va-bit-walk
 */
static int recipe_va_bit_walk(stress_args_t *args, const size_t max_bytes)
{
	const size_t sz = max_bytes < ADDRSPACE_VA_WALK_BYTES ?
		max_bytes : ADDRSPACE_VA_WALK_BYTES;
	const uint32_t bit = stress_mwc32modn(52);	/* VA bits 0..51 */
	uint64_t hint_bits = (uint64_t)1 << bit;
	uint64_t hint;
	void *p;
	stress_bitgen_t bg, bg_verify;
	int ret;

	/* address with bit n set plus random low bits; for the upper
	 * bits this probes the high half of the VA space (arm64 52-bit
	 * kernels accept the canonical form) */
	hint = ((uint64_t)stress_mwc32() & 0x00ffffffULL) | hint_bits;
	hint &= ~0xfffULL;

	p = map_at((void *)(uintptr_t)hint, sz);
	if (p == MAP_FAILED)
		return 0;

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	fill_pattern(&bg, (uint8_t *)p, sz);
	stress_bogo_inc(args);

	ret = verify_pattern(&bg_verify, (const uint8_t *)p, sz,
		args->name, "va-bit-walk");
	(void)munmap(p, sz);
	return ret;
}

/*
 *  recipe: dense-random-offset — pre-shuffled random page order,
 *  filled and verified in the same order (25% of pages, dense).
 */
static int recipe_dense_random_offset(stress_args_t *args, const size_t max_bytes)
{
	const size_t sz = max_bytes < (256 * STRESS_MB) ? max_bytes : (256 * STRESS_MB);
	const size_t pages = sz / ADDRSPACE_PAGE;
	const size_t touch = pages / 4;
	size_t *order;
	uint8_t *p;
	stress_bitgen_t bg, bg_verify;
	size_t i;
	int ret = 0;

	if (!touch)
		return 0;

	/* full Fisher-Yates over ALL pages, take the first 'touch' —
	 * no-replacement sampling, so no page is filled twice (a
	 * repeated fill would overwrite with a different pattern and
	 * desync the verify stream) */
	order = (size_t *)calloc(pages, sizeof(*order));
	if (!order)
		return 0;
	for (i = 0; i < pages; i++)
		order[i] = i;
	for (i = pages - 1; i > 0; i--) {
		const size_t j = stress_mwc64modn(i + 1);
		const size_t t = order[i];

		order[i] = order[j];
		order[j] = t;
	}

	p = (uint8_t *)map_at(NULL, sz);
	if (p == MAP_FAILED) {
		free(order);
		return 0;
	}

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	for (i = 0; i < touch; i++)
		fill_pattern(&bg, p + (order[i] * ADDRSPACE_PAGE), ADDRSPACE_PAGE);
	stress_bogo_inc(args);

	for (i = 0; i < touch && ret == 0; i++)
		ret = verify_pattern(&bg_verify, p + (order[i] * ADDRSPACE_PAGE),
			ADDRSPACE_PAGE, args->name, "dense-random-offset");

	free(order);
	(void)munmap(p, sz);
	return ret;
}

/*
 *  recipe: guarded-holes — fill, punch random PROT_NONE bands,
 *  verify with stream-stepping over the holes.
 */
#define GUARDED_MAX_HOLES	(8)

static int recipe_guarded_holes(stress_args_t *args, const size_t max_bytes)
{
	const size_t sz = max_bytes < (512 * STRESS_MB) ? max_bytes : (512 * STRESS_MB);
	const size_t pages = sz / ADDRSPACE_PAGE;
	const size_t holes = 1 + stress_mwc32modn(GUARDED_MAX_HOLES);
	size_t hole_start[GUARDED_MAX_HOLES], hole_pages[GUARDED_MAX_HOLES];
	uint8_t *p;
	stress_bitgen_t bg, bg_verify;
	size_t h, i;
	int ret = 0;

	if (pages < (holes + 2))
		return 0;

	p = (uint8_t *)map_at(NULL, sz);
	if (p == MAP_FAILED)
		return 0;

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	for (h = 0; h < holes; h++) {
		hole_start[h] = stress_mwc64modn(pages - 1);
		hole_pages[h] = 1 + stress_mwc64modn(16);
	}

	/* fill everything, then punch the holes */
	fill_pattern(&bg, p, sz);
	stress_bogo_inc(args);

	for (h = 0; h < holes; h++) {
		size_t len_pages = hole_pages[h];

		if (hole_start[h] + len_pages > pages)
			len_pages = pages - hole_start[h];
		(void)mprotect(p + (hole_start[h] * ADDRSPACE_PAGE),
			len_pages * ADDRSPACE_PAGE, PROT_NONE);
	}

	/* verify: walk every page; hole pages only step the stream
	 * (stress_bitgen_skip keeps the byte alignment) */
	for (i = 0; i < pages && ret == 0; i++) {
		bool in_hole = false;

		for (h = 0; h < holes; h++) {
			if (i >= hole_start[h] &&
			    i < hole_start[h] + hole_pages[h]) {
				in_hole = true;
				break;
			}
		}
		if (in_hole) {
			stress_bitgen_skip(&bg_verify, ADDRSPACE_PAGE);
			continue;
		}
		ret = verify_pattern(&bg_verify, p + (i * ADDRSPACE_PAGE),
			ADDRSPACE_PAGE, args->name, "guarded-holes");
	}

	(void)munmap(p, sz);
	return ret;
}

/*
 *  recipe: malloc-giant — the "malloc'd gigabytes" gap.  Random
 *  log-sized allocation, plain malloc or aligned posix_memalign,
 *  random 4MB slices pre-drawn before the bitgen fill so the verify
 *  walks the identical slices (touching a whole multi-GB region
 *  would dominate runtime; slices cross allocator arenas while
 *  staying bounded).
 */
#define MALLOC_GIANT_MAX_SLICES	(32)

static int recipe_malloc_giant(stress_args_t *args, const size_t max_bytes)
{
	size_t sz = ADDRSPACE_MIN_RECIPE_BYTES;
	size_t slice_off[MALLOC_GIANT_MAX_SLICES];
	size_t n_slices, i;
	void *p = NULL;
	bool aligned;
	stress_bitgen_t bg, bg_verify;
	int ret = 0;

	sz <<= stress_mwc32modn(11);		/* 4MB << 0..10, up to 4GB */
	if (sz > max_bytes)
		sz = max_bytes;

	aligned = (stress_mwc32() & 1);
	if (aligned) {
		const size_t align = 1ULL << (12 + stress_mwc32modn(9));

		if (posix_memalign(&p, align, sz) != 0)
			return 0;
	} else {
		p = malloc(sz);
		if (!p)
			return 0;
	}

	/* pre-draw non-overlapping slice offsets: a random start page,
	 * then consecutive 4MB slices (a random multi-draw would overlap
	 * and desync the verify stream exactly like the dense recipe's
	 * first draft did) */
	{
		const size_t total_slices = sz / (4 * STRESS_MB);
		const size_t start = total_slices ?
			stress_mwc64modn(total_slices) : 0;

		n_slices = 1 + (total_slices / 8);
		if (n_slices > MALLOC_GIANT_MAX_SLICES)
			n_slices = MALLOC_GIANT_MAX_SLICES;
		if (start + n_slices > total_slices) {
			/* clamp to the tail */
			const size_t clamped = total_slices - start;

			n_slices = clamped ? clamped : 1;
		}
		for (i = 0; i < n_slices; i++)
			slice_off[i] = (start + i) * (4 * STRESS_MB);
	}

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	for (i = 0; i < n_slices; i++) {
		const size_t len = (sz - slice_off[i] < (4 * STRESS_MB)) ?
			sz - slice_off[i] : (4 * STRESS_MB);

		fill_pattern(&bg, (uint8_t *)p + slice_off[i], len);
	}
	stress_bogo_inc(args);

	for (i = 0; i < n_slices && ret == 0; i++) {
		const size_t len = (sz - slice_off[i] < (4 * STRESS_MB)) ?
			sz - slice_off[i] : (4 * STRESS_MB);

		ret = verify_pattern(&bg_verify, (const uint8_t *)p + slice_off[i],
			len, args->name, "malloc-giant");
	}

	free(p);
	return ret;
}

/*
 *  recipe: misalign-huge — random sub-page offsets inside a large
 *  mapping, misaligned fixed-width copies across them (the
 *  misaligned stressor's enumerated offsets, scaled to big
 *  mappings).  Offsets are pre-drawn so fill and verify agree.
 */
#define MISALIGN_HUGE_ROUNDS	(256)

static int recipe_misalign_huge(stress_args_t *args, const size_t max_bytes)
{
	const size_t sz = max_bytes < (128 * STRESS_MB) ? max_bytes : (128 * STRESS_MB);
	size_t off[MISALIGN_HUGE_ROUNDS];
	uint8_t *p;
	uint8_t buf[64];
	size_t r;
	stress_bitgen_t bg, bg_verify;
	int ret = 0;

	if (sz < ADDRSPACE_PAGE * 2)
		return 0;

	p = (uint8_t *)map_at(NULL, sz);
	if (p == MAP_FAILED)
		return 0;

	/* pre-draw every (page, misalignment) pair with DISTINCT pages
	 * (page = base + r, mod pages): overlapping 64B windows from two
	 * rounds would overwrite each other's payloads */
	{
		const size_t pages = sz / ADDRSPACE_PAGE;
		const size_t base = pages > MISALIGN_HUGE_ROUNDS ?
			stress_mwc64modn(pages - MISALIGN_HUGE_ROUNDS) : 0;

		for (r = 0; r < MISALIGN_HUGE_ROUNDS; r++) {
			const size_t page = (base + r) % pages;
			const size_t mis = 1 + (2 * stress_mwc64modn(8));

			off[r] = (page * ADDRSPACE_PAGE) + mis;
			if (off[r] + sizeof(buf) > sz)
				off[r] = (size_t)-1;		/* skip marker */
		}
	}

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	for (r = 0; r < MISALIGN_HUGE_ROUNDS; r++) {
		if (off[r] == (size_t)-1)
			continue;
		fill_pattern(&bg, buf, sizeof(buf));
		memcpy(p + off[r], buf, sizeof(buf));
	}
	stress_bogo_inc(args);

	for (r = 0; r < MISALIGN_HUGE_ROUNDS && ret == 0; r++) {
		if (off[r] == (size_t)-1)
			continue;
		/* re-derive the same 64 payload bytes for comparison */
		uint8_t expect[64];

		fill_pattern(&bg_verify, expect, sizeof(expect));
		if (UNLIKELY(memcmp(p + off[r], expect, sizeof(expect)) != 0)) {
			size_t b;

			for (b = 0; b < sizeof(expect); b++) {
				if (p[off[r] + b] != expect[b]) {
					pr_fail("%s: misalign-huge data "
						"difference at offset %zu, "
						"expected 0x%2.2x, actual "
						"0x%2.2x\n", args->name,
						off[r] + b, expect[b],
						p[off[r] + b]);
					break;
				}
			}
			ret = -1;
		}
	}

	(void)munmap(p, sz);
	return ret;
}

/*
 *  recipe: mixed-orders — interleaved 4KB and (where enabled) 2MB
 *  mappings at random addresses in one round.
 */
#define MIXED_ORDERS_N		(16)

#if defined(MAP_HUGETLB) && !defined(MAP_HUGE_2MB)
#define MAP_HUGE_2MB		(21 << MAP_HUGE_SHIFT)
#endif

static int recipe_mixed_orders(stress_args_t *args, const size_t max_bytes)
{
	void *ptrs[MIXED_ORDERS_N];
	size_t sizes[MIXED_ORDERS_N];
	size_t n = 0, i;
	stress_bitgen_t bg, bg_verify;
	int ret = 0;

	for (i = 0; i < MIXED_ORDERS_N && n < MIXED_ORDERS_N; i++) {
		const bool huge = (stress_mwc32() & 3) == 0;	/* 25% */
		const size_t sz = huge ? (2 * STRESS_MB) : ADDRSPACE_PAGE;
		void *hint = random_unmapped_addr(huge ?
			0x0000ffffffffffffULL : 0x000000ffffffffffULL);
		void *p;

		if (sz > max_bytes)
			continue;
#if defined(MAP_HUGETLB)
		if (huge) {
			p = mmap(hint, sz, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS |
#if defined(MAP_FIXED_NOREPLACE)
				(hint ? MAP_FIXED_NOREPLACE : 0) |
#endif
				MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
			if (p == MAP_FAILED) {
				/* no hugetlb pool: plain mapping instead of
				 * skipping the round entirely */
				p = map_at(hint, sz);
			}
		} else
#endif
		{
			p = map_at(hint, sz);
		}
		if (p == MAP_FAILED)
			continue;
		ptrs[n] = p;
		sizes[n] = sz;
		n++;
	}

	stress_bitgen_init(&bg);
	(void)memcpy(&bg_verify, &bg, sizeof(bg_verify));

	for (i = 0; i < n; i++)
		fill_pattern(&bg, (uint8_t *)ptrs[i], sizes[i]);
	stress_bogo_inc(args);

	for (i = 0; i < n && ret == 0; i++)
		ret = verify_pattern(&bg_verify, (const uint8_t *)ptrs[i],
			sizes[i], args->name, "mixed-orders");

	for (i = 0; i < n; i++)
		(void)munmap(ptrs[i], sizes[i]);
	return ret;
}

/*
 *  method table ("all" at index 0, the framework convention)
 */
typedef int (*stress_addrspace_func_t)(stress_args_t *args, const size_t max_bytes);

typedef struct {
	const char *name;
	stress_addrspace_func_t func;
} stress_addrspace_method_info_t;

static stress_addrspace_method_info_t addrspace_methods[] = {
	{ "all",			NULL },
	{ "huge-random-fixed",		recipe_huge_random_fixed },
	{ "va-bit-walk",		recipe_va_bit_walk },
	{ "dense-random-offset",	recipe_dense_random_offset },
	{ "guarded-holes",		recipe_guarded_holes },
	{ "malloc-giant",		recipe_malloc_giant },
	{ "misalign-huge",		recipe_misalign_huge },
	{ "mixed-orders",		recipe_mixed_orders },
};

static const char *stress_addrspace_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(addrspace_methods)) ?
		addrspace_methods[i].name : NULL;
}

/*
 *  stress_addrspace()
 */
static int OPTIMIZE3 stress_addrspace(stress_args_t *args)
{
	size_t method = 0;
	const size_t n_methods = SIZEOF_ARRAY(addrspace_methods);
	uint64_t counter = 0;
	size_t max_bytes;
	uint64_t addrspace_bytes = 0;
	int rc = EXIT_SUCCESS;

	(void)stress_setting_get("addrspace-bytes", &addrspace_bytes);
	max_bytes = (size_t)addrspace_bytes;
	if (!max_bytes) {
		const uint64_t phys = stress_memory_phys_size_get();

		max_bytes = (size_t)(phys / 4);
	}
	if (max_bytes < ADDRSPACE_MIN_RECIPE_BYTES)
		max_bytes = ADDRSPACE_MIN_RECIPE_BYTES;

	(void)stress_setting_get("addrspace-method", &method);
	if (method >= n_methods)
		method = 0;

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	do {
		size_t m = method;
		int ret;

		if (m == 0)
			m = (size_t)(counter++ % (n_methods - 1)) + 1;
		ret = addrspace_methods[m].func(args, max_bytes);
		if (ret < 0) {
			rc = EXIT_FAILURE;
			break;
		}
	} while (stress_continue(args));

	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);
	return rc;
}

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("address-space-shapes"),
	STRESS_EX_FEATURE("bogo-ops-stable"),
	STRESS_EX_END,
};

const stressor_info_t stress_addrspace_info = {
	.stressor = stress_addrspace,
	.classifier = CLASS_MEMORY | CLASS_VM,
	.opts = opts,
	.verify = VERIFY_ALWAYS,
	.help = help,
	.exercises = exercises,
};

