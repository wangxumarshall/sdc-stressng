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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-affinity.h"
#include "core-asm-arm.h"
#include "core-bitgen.h"
#include "core-builtin.h"
#include "core-cpu-cache.h"
#include "core-filesystem.h"
#include "core-helper.h"
#include "core-killpid.h"
#include "core-mmap.h"
#include "core-numa.h"

#include <ctype.h>
#include <sched.h>

/*
 *  stress-llccross.c - interconnect / L3 cross-instance directed
 *  excitation (excitation-guide gap roadmap, item 1).
 *
 *  The coherence fabric between LLC (L3) instances and across sockets
 *  is a documented low-coverage area: no other stressor shapes the
 *  traffic that crosses it.  Each worker forks one partner process;
 *  the pair is bound to two different memory domains so every beat of
 *  the workload crosses the interconnect:
 *
 *    pingpong      turn-based shared cacheline ping-pong - each line
 *                  bounces between the two private L1/L2 caches of
 *                  the pair, forcing a line transfer per beat
 *    remote-write  bidirectional full-line store streams into a
 *                  buffer first-touched (and thus resident) on the
 *                  partner's domain
 *    remote-stream bidirectional mixed scan (sequential lines plus
 *                  bitgen-shaped jump offsets) of the partner's
 *                  buffer with sparse dirtying writes
 *
 *  Domain model: NUMA nodes (cross-socket / cross-die pairing) are
 *  preferred; when the CPU affinity allows only one node, the L3
 *  instances inside it (sysfs index3 shared_cpu_list) become the
 *  domains; a single-domain machine degrades honestly to
 *  intra-domain traffic with a log message.
 *
 *  Data shapes: bitgen operands (bandwalk/edge-mixed) flow in the
 *  pressure path.  Under --verify the payloads switch to a
 *  deterministic splitmix hash of (seed, address) - the lsupress
 *  oracle model - so every transferred word can be recomputed and
 *  compared; mismatches report address, expected/actual patterns and
 *  flipped-bit count.  The pingpong tag word is a deterministic hash
 *  of (seed, round, line, side) and is checked on every beat even
 *  without --verify (an always-on sentinel, one compare per beat).
 */

#define LLCCROSS_MAX_CPUS		(1024)	/* matches cpu_set_t bits */
#define LLCCROSS_MAX_DOMAINS		(64)
#define LLCCROSS_MAX_LINE_WORDS		(32)	/* 256 byte max cacheline */
#define LLCCROSS_MIN_BYTES		(64 * STRESS_KB)
#define LLCCROSS_DEFAULT_BYTES		(8 * STRESS_MB)	/* per side */
#define LLCCROSS_MIN_LINES		(1)
#define LLCCROSS_MAX_LINES		(8192)
#define LLCCROSS_DEFAULT_LINES		(64)
#define LLCCROSS_METHOD_ALL		(0)
#define LLCCROSS_METHOD_PINGPONG	(1)
#define LLCCROSS_METHOD_REMOTE_WRITE	(2)
#define LLCCROSS_METHOD_REMOTE_STREAM	(3)
#define LLCCROSS_SIDE_A			(0)	/* parent, home domain */
#define LLCCROSS_SIDE_B			(1)	/* child, remote domain */
#define LLCCROSS_TAG_SLOT		(63)	/* value() slot of the tag word */
#define LLCCROSS_ADDR_MIX		(0xA110CA8EULL)	/* y-mix for per-address values */

#if defined(HAVE_SCHED_SETAFFINITY)

/* shared control words at the head of the mapping */
typedef struct {
	volatile uint32_t ready[2];	/* per-side first-touch done flags */
	volatile uint32_t fatal;	/* set on any failure / early exit */
} llccross_ctrl_t;

/* one memory domain = CPUs of one NUMA node or one L3 instance */
typedef struct {
	uint32_t cpus[LLCCROSS_MAX_CPUS];
	size_t count;
	uint32_t first;			/* lowest CPU - domain key */
	char desc[48];
} llccross_domain_t;

typedef struct {
	llccross_domain_t domains[LLCCROSS_MAX_DOMAINS];
	size_t n;
	bool cross_node;		/* domains are NUMA nodes */
	bool cross_l3;			/* domains are L3 instances */
} llccross_topo_t;

typedef struct {
	stress_args_t *args;
	llccross_topo_t *topo;
	size_t method;
	size_t home_idx;
	size_t remote_idx;
	size_t bytes;			/* per-side buffer size (remote modes) */
	size_t lines;			/* pingpong line count */
	size_t line_size;		/* cacheline size used everywhere */
	uint64_t seed;
	bool verify;
} llccross_ctx_t;

typedef int (*llccross_side_func_t)(
	llccross_ctx_t *ctx,
	const unsigned int side,
	uint8_t *map);

/*
 *  llccross_mb()
 *	hardware memory barrier (the peterson/dekker pattern).  Without
 *	it arm64 reorders the tag/payload loads above the spin that
 *	observed the turn handover (observed as a round-counter desync
 *	during bring-up), and the ready-flag handshake can pass the
 *	first-touch stores it publishes.
 */
static inline void ALWAYS_INLINE llccross_mb(void)
{
	shim_mfence();
#if defined(HAVE_ASM_ARM_DMB_SY)
	stress_asm_arm_dmb_sy();
#endif
	stress_asm_mb();
}

/*
 *  llccross_value()
 *	deterministic value oracle (splitmix-style hash).  For the
 *	streaming modes x is the address and y a fixed mix; for the
 *	pingpong beats x packs (round * 64 + word slot) and y packs
 *	(line * 2 + writer side).
 */
static inline uint64_t ALWAYS_INLINE llccross_value(
	const uint64_t seed,
	const uint64_t x,
	const uint64_t y)
{
	uint64_t z = seed + x * 0x9E3779B97F4A7C15ULL
			   + y * 0xC2B2AE3D27D4EB4FULL;

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/*
 *  llccross_fail_diag()
 *	bit-level mismatch diagnostics (lsupress format): address,
 *	expected vs actual word and the number of flipped bits.
 */
static void llccross_fail_diag(
	const char *name,
	const void *addr,
	const uint64_t expect,
	const uint64_t actual)
{
	const uint64_t diff = expect ^ actual;
	int bits = 0, b;

	for (b = 0; b < 64; b++) {
		if (diff & (1ULL << b))
			bits++;
	}
	pr_fail("%s: addr %p expected 0x%16.16" PRIx64
		" actual 0x%16.16" PRIx64
		" %d bit(s) flipped (xor 0x%16.16" PRIx64 ")\n",
		name, addr, expect, actual, bits, diff);
}

/*
 *  llccross_parse_cpulist()
 *	parse a sysfs cpu list ("0-3,8,10-11") into out[], keeping only
 *	CPUs present in filter (when filter is non-NULL).  Returns 0 on
 *	success, -1 when nothing parseable/allowed was found.
 */
static int llccross_parse_cpulist(
	const char *buf,
	const cpu_set_t *filter,
	uint32_t *out,
	size_t *count,
	const size_t max)
{
	const char *ptr = buf;
	size_t n = 0;

	while (*ptr && (*ptr != '\n')) {
		int cpu = 0, cpu_end;
		bool got = false;

		while (isdigit((int)*ptr)) {
			cpu = (cpu * 10) + (*ptr - '0');
			ptr++;
			got = true;
		}
		if (!got)
			break;
		cpu_end = cpu;
		if (*ptr == '-') {
			int v = 0;

			ptr++;
			got = false;
			while (isdigit((int)*ptr)) {
				v = (v * 10) + (*ptr - '0');
				ptr++;
				got = true;
			}
			if (got)
				cpu_end = v;
		}
		for (; cpu <= cpu_end; cpu++) {
			if (cpu < 0 || (size_t)cpu >= max)
				break;
			if (filter && !CPU_ISSET(cpu, filter))
				continue;
			out[n++] = (uint32_t)cpu;
			if (n >= max)
				break;
		}
		if (*ptr == ',') {
			ptr++;
			continue;
		}
		break;
	}
	*count = n;
	return (n > 0) ? 0 : -1;
}

/*
 *  llccross_domain_add()
 *	append a domain from a parsed (ascending) cpu list
 */
static void llccross_domain_add(
	llccross_topo_t *t,
	const uint32_t *cpus,
	const size_t count)
{
	llccross_domain_t *d = &t->domains[t->n];

	(void)shim_memcpy(d->cpus, cpus, count * sizeof(*cpus));
	d->count = count;
	d->first = cpus[0];
	t->n++;
}

/*
 *  llccross_topo_init()
 *	discover the pairing domains:
 *	  1. NUMA nodes (cross-socket / cross-die), filtered by the
 *	     current CPU affinity
 *	  2. if only one node is available: the L3 instances inside it
 *	     (sysfs index3 shared_cpu_list)
 *	  3. a single domain covering everything allowed (degraded
 *	     intra-domain mode)
 */
static void llccross_topo_init(llccross_topo_t *t)
{
	cpu_set_t allowed;
	bool have_allowed = false;
	char buffer[4096];
	char path[PATH_MAX];
	long int node;
	size_t i;

	t->n = 0;
	t->cross_node = false;
	t->cross_l3 = false;

	if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
		have_allowed = true;

	/* 1. NUMA node domains (CPU topology: enumerate the sysfs node
	 *    list; the memory-node count under-reports inside cpuset
	 *    containers and is checked separately for the placement
	 *    warning below) */
	for (node = 0; node < LLCCROSS_MAX_DOMAINS; node++) {
		uint32_t cpus[LLCCROSS_MAX_CPUS];
		size_t count = 0;

		(void)snprintf(path, sizeof(path),
			"/sys/devices/system/node/node%ld/cpulist", node);
		if (stress_fs_file_read(path, buffer, sizeof(buffer)) < 1)
			break;		/* no more nodes */
		if (llccross_parse_cpulist(buffer,
				have_allowed ? &allowed : NULL,
				cpus, &count, LLCCROSS_MAX_CPUS) < 0)
			continue;
		if (t->n < LLCCROSS_MAX_DOMAINS) {
			llccross_domain_add(t, cpus, count);
			(void)snprintf(t->domains[t->n - 1].desc,
				sizeof(t->domains[0].desc),
				"node %ld [%u-%u]", node, cpus[0],
				cpus[count - 1]);
		}
	}
	if (t->n >= 2) {
		t->cross_node = true;
		return;
	}

	/* 2. L3 instance domains over the allowed CPUs */
	{
		uint32_t keys[LLCCROSS_MAX_DOMAINS];
		size_t n_l3 = 0;

		t->n = 0;
		for (i = 0; i < LLCCROSS_MAX_CPUS; i++) {
			uint32_t cpus[LLCCROSS_MAX_CPUS];
			size_t count = 0, j;
			bool seen = false;

			if (have_allowed && !CPU_ISSET((int)i, &allowed))
				continue;
			(void)snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/cpu%zu/cache/index3/shared_cpu_list", i);
			if (stress_fs_file_read(path, buffer, sizeof(buffer)) < 1)
				continue;
			if (llccross_parse_cpulist(buffer,
					have_allowed ? &allowed : NULL,
					cpus, &count, LLCCROSS_MAX_CPUS) < 0)
				continue;
			/* dedupe by lowest CPU in the instance */
			for (j = 0; j < n_l3; j++) {
				if (keys[j] == cpus[0]) {
					seen = true;
					break;
				}
			}
			if (seen || n_l3 >= LLCCROSS_MAX_DOMAINS)
				continue;
			llccross_domain_add(t, cpus, count);
			keys[n_l3++] = cpus[0];
			(void)snprintf(t->domains[t->n - 1].desc,
				sizeof(t->domains[0].desc),
				"L3 [%u-%u]", cpus[0], cpus[count - 1]);
		}
		if (n_l3 >= 2) {
			t->cross_l3 = true;
			return;
		}
		if (n_l3 == 1)
			return;		/* single L3 domain, degraded */
		t->n = 0;
	}

	/* 3. single domain: everything allowed (degraded intra-domain) */
	{
		uint32_t cpus[LLCCROSS_MAX_CPUS];
		size_t count = 0;

		for (i = 0; i < LLCCROSS_MAX_CPUS; i++) {
			if (have_allowed && !CPU_ISSET((int)i, &allowed))
				continue;
			cpus[count++] = (uint32_t)i;
		}
		if (count == 0) {
			/* no affinity info at all: CPU 0 */
			cpus[0] = 0;
			count = 1;
		}
		llccross_domain_add(t, cpus, count);
		(void)snprintf(t->domains[0].desc,
			sizeof(t->domains[0].desc),
			"allowed [%u-%u]", cpus[0], cpus[count - 1]);
	}
}

/*
 *  llccross_domain_has_cpu()
 */
static bool llccross_domain_has_cpu(
	const llccross_domain_t *d,
	const uint32_t cpu)
{
	size_t i;

	for (i = 0; i < d->count; i++) {
		if (d->cpus[i] == cpu)
			return true;
	}
	return false;
}

/*
 *  llccross_bind()
 *	bind the current process to a domain; failure is non-fatal
 *	(the pair runs unbound, traffic stays best-effort)
 */
static bool llccross_bind(const llccross_domain_t *d)
{
	cpu_set_t mask;
	size_t i;

	if (d->count == 0)
		return false;
	CPU_ZERO(&mask);
	for (i = 0; i < d->count; i++)
		CPU_SET((int)d->cpus[i], &mask);
	return sched_setaffinity(0, sizeof(mask), &mask) == 0;
}

/*
 *  llccross_write_pass()
 *	one full-line store sweep over a buffer.  verify mode writes the
 *	deterministic per-address hash; otherwise a bitgen stream.
 */
static void OPTIMIZE3 llccross_write_pass(
	uint8_t *base,
	const size_t bytes,
	const size_t line_size,
	const bool forward,
	const bool verify,
	stress_bitgen_t *bg,
	const uint64_t seed)
{
	const size_t words = line_size / sizeof(uint64_t);
	const size_t n_lines = bytes / line_size;
	size_t li, k;

	for (li = 0; li < n_lines; li++) {
		const size_t idx = forward ? li : (n_lines - 1 - li);
		volatile uint64_t *w =
			(volatile uint64_t *)(base + (idx * line_size));

		for (k = 0; k < words; k++) {
			w[k] = verify ?
				llccross_value(seed,
					(uint64_t)(uintptr_t)(w + k),
					LLCCROSS_ADDR_MIX) :
				stress_bitgen_u64(bg);
		}
	}
}

/*
 *  llccross_check_pass()
 *	sampled read-back of a write pass: every 64th line, every word,
 *	recomputed against the deterministic per-address hash.
 */
static int llccross_check_pass(
	const llccross_ctx_t *ctx,
	uint8_t *base)
{
	const size_t words = ctx->line_size / sizeof(uint64_t);
	const size_t n_lines = ctx->bytes / ctx->line_size;
	size_t li, k;
	int fails = 0;

	for (li = 0; li < n_lines; li += 64) {
		volatile uint64_t *w =
			(volatile uint64_t *)(base + (li * ctx->line_size));

		for (k = 0; k < words; k++) {
			const uint64_t expect = llccross_value(ctx->seed,
				(uint64_t)(uintptr_t)(w + k),
				LLCCROSS_ADDR_MIX);

			if (UNLIKELY(w[k] != expect)) {
				llccross_fail_diag(ctx->args->name,
					(const void *)(w + k), expect, w[k]);
				if (++fails >= 4)
					return EXIT_FAILURE;
			}
		}
	}
	return fails ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 *  llccross_running()
 *	loop condition: the parent honours the bogo-ops budget and the
 *	framework stop; the forked partner only sees its inherited copy
 *	of the continue flag (the parent kills it on exit).
 */
static inline bool ALWAYS_INLINE llccross_running(
	const llccross_ctx_t *ctx,
	const unsigned int side)
{
	if (side == LLCCROSS_SIDE_A)
		return stress_continue(ctx->args);
	return stress_continue_flag();
}

/*
 *  llccross_pingpong_init()
 *	pre-fork line initialisation by the parent: every line is handed
 *	to side A with side B's "round 0" tag/payload pre-written, so
 *	round 1 of side A has a well-defined expectation to check.
 */
static void llccross_pingpong_init(
	llccross_ctx_t *ctx,
	uint8_t *map)
{
	volatile uint64_t *lines = (volatile uint64_t *)(map + 64);
	const size_t words = ctx->line_size / sizeof(uint64_t);
	size_t i, k;

	for (i = 0; i < ctx->lines; i++) {
		volatile uint64_t *w = lines + (i * words);

		w[0] = LLCCROSS_SIDE_A;			/* A to move first */
		w[1] = llccross_value(ctx->seed, LLCCROSS_TAG_SLOT,
			(i << 1) + LLCCROSS_SIDE_B);
		for (k = 0; k < words - 2; k++)
			w[2 + k] = llccross_value(ctx->seed, k,
				(i << 1) + LLCCROSS_SIDE_B);
	}
}

/*
 *  llccross_pingpong_side()
 *	shared cacheline ping-pong.  Each round both sides visit every
 *	line in turn: spin for ownership, check the partner's beat
 *	(always the tag word, plus payload words under --verify),
 *	write the next beat, hand the line over.
 */
static int OPTIMIZE3 llccross_pingpong_side(
	llccross_ctx_t *ctx,
	const unsigned int side,
	uint8_t *map)
{
	stress_args_t *args = ctx->args;
	llccross_ctrl_t *ctrl = (llccross_ctrl_t *)map;
	volatile uint64_t *lines = (volatile uint64_t *)(map + 64);
	const size_t words = ctx->line_size / sizeof(uint64_t);
	stress_bitgen_t bg;
	uint64_t round = 1;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, ctx->seed ^
		(side ? 0xB5B5B5B5B5B5B5B5ULL : 0x4D4D4D4D4D4D4D4DULL));

	while (!ctrl->fatal && llccross_running(ctx, side)) {
		size_t i, k;

		for (i = 0; i < ctx->lines; i++) {
			volatile uint64_t *w = lines + (i * words);
			/* round the partner's checked beat was written in */
			const uint64_t expect_round = (round - 1) + side;
			const uint64_t y = (i << 1) + (1 - side);
			uint64_t expect, got;

			while (w[0] != side) {
				if (UNLIKELY(ctrl->fatal))
					return EXIT_FAILURE;
				if (UNLIKELY(!llccross_running(ctx, side)))
					return EXIT_SUCCESS;
			}
			llccross_mb();	/* acquire: order reads after the turn observation */

			/* always-on sentinel: the partner's tag word */
			expect = llccross_value(ctx->seed,
				(expect_round << 6) + LLCCROSS_TAG_SLOT, y);
			got = w[1];
			if (UNLIKELY(got != expect)) {
				llccross_fail_diag(args->name,
					(const void *)&w[1], expect, got);
				ctrl->fatal = 1;
				return EXIT_FAILURE;
			}
			if (ctx->verify) {
				for (k = 0; k < words - 2; k++) {
					expect = llccross_value(ctx->seed,
						(expect_round << 6) + k, y);
					got = w[2 + k];
					if (UNLIKELY(got != expect)) {
						llccross_fail_diag(args->name,
							(const void *)&w[2 + k],
							expect, got);
						ctrl->fatal = 1;
						return EXIT_FAILURE;
					}
				}
			}

			/* write our beat and hand the line over */
			for (k = 0; k < words - 2; k++) {
				w[2 + k] = ctx->verify ?
					llccross_value(ctx->seed,
						(round << 6) + k,
						(i << 1) + side) :
					stress_bitgen_u64(&bg);
			}
			w[1] = llccross_value(ctx->seed,
				(round << 6) + LLCCROSS_TAG_SLOT,
				(i << 1) + side);
			llccross_mb();	/* release: payload+tag before the handover */
			w[0] = 1 - side;
		}
		round++;
		if (side == LLCCROSS_SIDE_A)
			stress_bogo_inc(args);
	}
	return EXIT_SUCCESS;
}

/*
 *  llccross_remote_write_side()
 *	bidirectional remote store streams.  Each side first-touches its
 *	own half of the shared mapping (so those pages are local to its
 *	domain), then repeatedly streams full-line stores into the
 *	partner's half - every write crosses the interconnect.  Passes
 *	alternate direction; verify mode adds a sampled read-back.
 */
static int OPTIMIZE3 llccross_remote_write_side(
	llccross_ctx_t *ctx,
	const unsigned int side,
	uint8_t *map)
{
	stress_args_t *args = ctx->args;
	llccross_ctrl_t *ctrl = (llccross_ctrl_t *)map;
	const size_t page_size = args->page_size;
	uint8_t *mine = map + page_size + (side * ctx->bytes);
	uint8_t *remote = map + page_size + ((1 - side) * ctx->bytes);
	stress_bitgen_t bg;
	uint64_t passes = 0;
	int rc = EXIT_SUCCESS;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, ctx->seed ^
		(side ? 0xB5B5B5B5B5B5B5B5ULL : 0x4D4D4D4D4D4D4D4DULL));

	/* first-touch own half: pages become local to this side's node */
	llccross_write_pass(mine, ctx->bytes, ctx->line_size, true,
		false, &bg, ctx->seed);
	llccross_mb();	/* publish the touch stores before the ready flag */
	ctrl->ready[side] = 1;
	while (!ctrl->ready[1 - side] && !ctrl->fatal &&
	       llccross_running(ctx, side))
		;
	llccross_mb();	/* order remote writes after the partner's ready */

	while (!ctrl->fatal && llccross_running(ctx, side)) {
		const bool forward = (passes & 1) == 0;

		llccross_write_pass(remote, ctx->bytes, ctx->line_size,
			forward, ctx->verify, &bg, ctx->seed);
		if (ctx->verify) {
			rc = llccross_check_pass(ctx, remote);
			if (UNLIKELY(rc != EXIT_SUCCESS)) {
				ctrl->fatal = 1;
				return rc;
			}
		}
		passes++;
		if (side == LLCCROSS_SIDE_A)
			stress_bogo_inc(args);
	}
	return rc;
}

/*
 *  llccross_remote_stream_side()
 *	bidirectional mixed scan of the partner's buffer.  Pressure
 *	path: sequential line reads (xor-consumed) with a bitgen-shaped
 *	jump every 16th step and a sparse full-line write every 64th step
 *	to keep lines dirty-migrating.  Verify path: full deterministic
 *	write pass followed by a sampled read-back.
 */
static int OPTIMIZE3 llccross_remote_stream_side(
	llccross_ctx_t *ctx,
	const unsigned int side,
	uint8_t *map)
{
	stress_args_t *args = ctx->args;
	llccross_ctrl_t *ctrl = (llccross_ctrl_t *)map;
	const size_t page_size = args->page_size;
	const size_t words = ctx->line_size / sizeof(uint64_t);
	uint8_t *mine = map + page_size + (side * ctx->bytes);
	uint8_t *remote = map + page_size + ((1 - side) * ctx->bytes);
	const size_t span = ctx->bytes / ctx->line_size;	/* in lines */
	stress_bitgen_t bg;
	int rc = EXIT_SUCCESS;

	stress_bitgen_init(&bg);
	stress_bitgen_seed(&bg, ctx->seed ^
		(side ? 0xB5B5B5B5B5B5B5B5ULL : 0x4D4D4D4D4D4D4D4DULL));

	/* first-touch own half: pages become local to this side's node */
	llccross_write_pass(mine, ctx->bytes, ctx->line_size, true,
		false, &bg, ctx->seed);
	llccross_mb();	/* publish the touch stores before the ready flag */
	ctrl->ready[side] = 1;
	while (!ctrl->ready[1 - side] && !ctrl->fatal &&
	       llccross_running(ctx, side))
		;
	llccross_mb();	/* order remote writes after the partner's ready */

	if (ctx->verify) {
		while (!ctrl->fatal && llccross_running(ctx, side)) {
			llccross_write_pass(remote, ctx->bytes,
				ctx->line_size, true, true, &bg, ctx->seed);
			rc = llccross_check_pass(ctx, remote);
			if (UNLIKELY(rc != EXIT_SUCCESS)) {
				ctrl->fatal = 1;
				return rc;
			}
			if (side == LLCCROSS_SIDE_A)
				stress_bogo_inc(args);
		}
		return rc;
	}

	/* shaped scan: sequential lines + bitgen jumps + sparse writes */
	{
		uint64_t acc = 0;
		uint64_t step;

		while (!ctrl->fatal && llccross_running(ctx, side)) {
			size_t off = 0;

			for (step = 0; step < span; step++) {
				volatile uint64_t *w =
					(volatile uint64_t *)
					(remote + (off * ctx->line_size));
				size_t k;

				for (k = 0; k < words; k++)
					acc ^= w[k];
				if ((step & 15) == 15) {
					/* bitgen-shaped jump */
					off = (size_t)(stress_bitgen_u64(&bg) % span);
				} else {
					off = (off + 1 < span) ? off + 1 : 0;
				}
				if ((step & 63) == 0) {
					/* sparse dirtying write */
					for (k = 0; k < words; k++)
						w[k] = stress_bitgen_u64(&bg);
				}
			}
			__asm__ __volatile__ ("" : : "r" (acc) : "memory");
			if (side == LLCCROSS_SIDE_A)
				stress_bogo_inc(args);
		}
	}
	return rc;
}

typedef struct {
	const char *name;
	const llccross_side_func_t func;
	const bool needs_init;		/* pingpong pre-fork line init */
} llccross_method_info_t;

static const llccross_method_info_t llccross_methods[] = {
	{ "all",		NULL,					false },	/* 0: rotate */
	{ "pingpong",		llccross_pingpong_side,			true },
	{ "remote-write",	llccross_remote_write_side,		false },
	{ "remote-stream",	llccross_remote_stream_side,		false },
};

#endif

static const char *stress_llccross_method(const size_t i)
{
#if defined(HAVE_SCHED_SETAFFINITY)
	return (i < SIZEOF_ARRAY(llccross_methods)) ? llccross_methods[i].name : NULL;
#else
	(void)i;
	return NULL;
#endif
}

static const stress_opt_t opts[] = {
	{ OPT_llccross_method,	"llccross-method",	TYPE_ID_SIZE_T_METHOD,	0, 0, stress_llccross_method },
	{ OPT_llccross_ops,	"llccross-ops",		TYPE_ID_UINT64,		0, 0, NULL },
	{ OPT_llccross_bytes,	"llccross-bytes",	TYPE_ID_SIZE_T_BYTES_VM, LLCCROSS_MIN_BYTES, MAX_MEM_LIMIT, NULL },
	{ OPT_llccross_lines,	"llccross-lines",	TYPE_ID_SIZE_T,		LLCCROSS_MIN_LINES, LLCCROSS_MAX_LINES, NULL },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"llccross N",		"start N workers directing coherence traffic across L3 instances and NUMA nodes (each worker forks a partner bound in another domain)" },
	{ NULL,	"llccross-method M",	"method (see --llccross-method 0 for the list)" },
	{ NULL,	"llccross-ops N",	"stop after N llccross bogo operations" },
	{ NULL,	"llccross-bytes B",	"per-side buffer size for the remote methods (default 8MB)" },
	{ NULL,	"llccross-lines N",	"number of shared cachelines bounced by the pingpong method (default 64)" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_SCHED_SETAFFINITY)

/*
 *  llccross_run_pair()
 *	fork the partner process and run both sides of the pair.  The
 *	child is bound to the remote domain, the parent stays on the
 *	home domain.  The parent kills and reaps the child on exit.
 */
static int llccross_run_pair(
	llccross_ctx_t *ctx,
	uint8_t *map,
	double *duration)
{
	stress_args_t *args = ctx->args;
	llccross_ctrl_t *ctrl = (llccross_ctrl_t *)map;
	const llccross_method_info_t *method =
		&llccross_methods[ctx->method];
	pid_t pid;
	int rc = EXIT_SUCCESS, status;
	double t_start;

	ctrl->ready[0] = 0;
	ctrl->ready[1] = 0;
	ctrl->fatal = 0;

	if (method->needs_init)
		llccross_pingpong_init(ctx, map);

	stress_proc_state_set(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_proc_state_set(args->name, STRESS_STATE_RUN);

	t_start = stress_time_now();
	pid = fork();
	if (pid < 0) {
		pr_inf_skip("%s: fork of partner process failed, errno=%d (%s)%s\n",
			args->name, errno, strerror(errno),
			stress_memory_free_get());
		stress_proc_state_set(args->name, STRESS_STATE_DEINIT);
		return EXIT_NO_RESOURCE;
	}
	if (pid == 0) {
		/* child: remote domain side */
		int crc;

		stress_proc_state_set(args->name, STRESS_STATE_RUN);
		stress_make_it_fail_set();
		if (!llccross_bind(&ctx->topo->domains[ctx->remote_idx]) &&
		    stress_instance_zero(args)) {
			pr_inf("%s: cannot bind partner to %s, running unbound\n",
				args->name, ctx->topo->domains[ctx->remote_idx].desc);
		}
		crc = method->func(ctx, LLCCROSS_SIDE_B, map);
		if (crc != EXIT_SUCCESS)
			ctrl->fatal = 1;
		_exit(crc);
	}

	/* parent: home domain side */
	rc = method->func(ctx, LLCCROSS_SIDE_A, map);
	if (rc != EXIT_SUCCESS)
		ctrl->fatal = 1;
	*duration = stress_time_now() - t_start;

	if (stress_kill_pid_wait(pid, &status) >= 0) {
		if (WIFEXITED(status)) {
			const int child_rc = WEXITSTATUS(status);

			if (child_rc != EXIT_SUCCESS)
				rc = child_rc;
		}
	}
	stress_proc_state_set(args->name, STRESS_STATE_DEINIT);
	return rc;
}

/*
 *  stress_llccross()
 *	direct coherence traffic across memory domains
 */
static int stress_llccross(stress_args_t *args)
{
	llccross_topo_t topo;
	llccross_ctx_t ctx;
	size_t llccross_method = 0;
	size_t llccross_bytes = LLCCROSS_DEFAULT_BYTES;
	size_t llccross_lines = LLCCROSS_DEFAULT_LINES;
	size_t line_size = 64, llc_size = 0;
	const size_t method_max = SIZEOF_ARRAY(llccross_methods);
	const uint64_t seed = stress_mwc64();
	unsigned int home_cpu;
	uint8_t *map;
	size_t map_size, i;
	double duration = 0.0, rate;
	int rc;

	(void)stress_setting_get("llccross-method", &llccross_method);
	(void)stress_setting_get("llccross-bytes", &llccross_bytes);
	(void)stress_setting_get("llccross-lines", &llccross_lines);

	if (llccross_method >= method_max) {
		pr_fail("%s: llccross-method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (llccross_method == LLCCROSS_METHOD_ALL)
		llccross_method = 1 + (stress_mwc32() % (method_max - 1));

	/* cacheline size: the pingpong turn/tag/payload words must share
	 * one coherence transfer unit, so use the smallest reported
	 * line (on the Kunpeng 920/950 the L3 line is 128 bytes while
	 * the L1/L2 coherence granule is 64) */
	{
		size_t l1_size = 0, l1_line = 0, llc_line = 0;

		stress_cpu_cache_level_size_get(1, &l1_size, &l1_line,
			CACHE_TYPE_DATA);
		stress_cpu_cache_llc_size_get(&llc_size, &llc_line);
		if (l1_line && llc_line)
			line_size = STRESS_MINIMUM(l1_line, llc_line);
		else if (l1_line)
			line_size = l1_line;
		else if (llc_line)
			line_size = llc_line;
		else
			line_size = 64;
	}
	if ((line_size < 64) ||
	    (line_size > LLCCROSS_MAX_LINE_WORDS * sizeof(uint64_t)) ||
	    (line_size & (line_size - 1)))
		line_size = 64;
	llccross_bytes = (llccross_bytes / line_size) * line_size;

	/* domain discovery and pair selection */
	llccross_topo_init(&topo);
	home_cpu = stress_cpu_get();
	for (i = 0; i < topo.n; i++) {
		if (llccross_domain_has_cpu(&topo.domains[i], home_cpu))
			break;
	}
	ctx.home_idx = (i < topo.n) ? i : 0;
	ctx.remote_idx = (topo.n >= 2) ?
		(ctx.home_idx + 1 + (args->instance % (topo.n - 1))) % topo.n :
		ctx.home_idx;

	if (stress_instance_zero(args)) {
		if (topo.n >= 2) {
			pr_inf("%s: pairing %s with %s (%s)\n",
				args->name,
				topo.domains[ctx.home_idx].desc,
				topo.domains[ctx.remote_idx].desc,
				topo.cross_node ? "cross-node" :
				(topo.cross_l3 ? "cross-L3 instance" :
				 "cross-domain"));
			if (topo.cross_node &&
			    (stress_numa_nodes() < (long int)topo.n)) {
				pr_inf("%s: memory allocation restricted to %ld node(s) by cpuset/mempolicy, "
					"first-touch page placement may collapse onto those nodes "
					"(CPU-side pairing still crosses the domains)\n",
					args->name, stress_numa_nodes());
			}
		} else {
			pr_inf("%s: only one memory domain available under the current CPU affinity, "
				"cross-domain traffic degraded to intra-domain\n",
				args->name);
		}
	}
	if (!llccross_bind(&topo.domains[ctx.home_idx]) &&
	    stress_instance_zero(args)) {
		pr_inf("%s: cannot bind to %s, running unbound\n",
			args->name, topo.domains[ctx.home_idx].desc);
	}

	ctx.args = args;
	ctx.topo = &topo;
	ctx.method = llccross_method;
	ctx.bytes = llccross_bytes;
	ctx.lines = llccross_lines;
	ctx.line_size = line_size;
	ctx.seed = seed;
	ctx.verify = (g_opt_flags & OPT_FLAGS_VERIFY) != 0;

	if (llccross_method == LLCCROSS_METHOD_PINGPONG)
		map_size = STRESS_MAXIMUM(args->page_size,
			64 + (llccross_lines * line_size));
	else
		map_size = args->page_size + (2 * llccross_bytes);

	map = (uint8_t *)mmap(NULL, map_size,
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (map == MAP_FAILED) {
		pr_inf_skip("%s: mmap of %zu bytes shared region failed%s, "
			"errno=%d (%s), skipping stressor\n",
			args->name, map_size, stress_memory_free_get(),
			errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	stress_memory_anon_name_set(map, map_size, "llccross-shared");
	if (stress_instance_zero(args))
		stress_memory_usage_get(args, map_size, map_size * args->instances);

	rc = llccross_run_pair(&ctx, map, &duration);

	if ((rc == EXIT_SUCCESS) && (duration > 0.0)) {
		const uint64_t bogo = stress_bogo_get(args);

		switch (llccross_method) {
		case LLCCROSS_METHOD_PINGPONG:
			rate = (double)bogo / duration;
			stress_metrics_set(args,
				"coherence ping-pong rounds per sec",
				rate, STRESS_METRIC_HARMONIC_MEAN);
			break;
		case LLCCROSS_METHOD_REMOTE_WRITE:
			rate = ((double)bogo * (double)llccross_bytes) /
				(STRESS_MB * duration);
			stress_metrics_set(args,
				"MB per sec remote write rate (per side)",
				rate, STRESS_METRIC_HARMONIC_MEAN);
			break;
		default:	/* remote-stream */
			rate = ((double)bogo * (double)llccross_bytes) /
				(STRESS_MB * duration);
			stress_metrics_set(args,
				"MB per sec remote scan rate (per side)",
				rate, STRESS_METRIC_HARMONIC_MEAN);
			break;
		}
	}

	(void)munmap(map, map_size);
	return rc;
}

static const stress_exercises_t exercises[] = {
	STRESS_EX_FEATURE("memory-bus"),
	STRESS_EX_FEATURE("d-cache-ll-read"),
	STRESS_EX_FEATURE("d-cache-ll-write"),
	STRESS_EX_FEATURE("d-cache-miss"),
	STRESS_EX_FEATURE("memory-stalls"),

	STRESS_EX_SYSCALL("sched_setaffinity"),
	STRESS_EX_END,
};

const stressor_info_t stress_llccross_info = {
	.stressor = stress_llccross,
	.classifier = CLASS_CPU_CACHE | CLASS_MEMORY,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help,
	.exercises = exercises,
};

#else

const stressor_info_t stress_llccross_info = {
	.stressor = stress_unimplemented,
	.classifier = CLASS_CPU_CACHE | CLASS_MEMORY,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help,
	.unimplemented_reason = "built without sched_setaffinity() support"
};

#endif
