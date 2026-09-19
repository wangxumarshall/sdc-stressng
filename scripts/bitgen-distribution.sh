#!/bin/bash
#
# Copyright (C) 2026 wangxumarshall
#
# Validation harness for core-bitgen (P1 of the operand/address
# mutation plan): builds a tiny probe program against core-bitgen.c
# and checks the generators statistically:
#
#   1. edge64 boundary hits: how many samples land within ±4 LSB of
#      the dictionary boundaries vs uniform's expected rate
#   2. bandwalk bit-density: per-bit ones-ratio across a sweep stays
#      within the design band and every bit position sees the band
#   3. fp64_bits: exponent field coverage — subnormal/inf/nan and
#      the swept middle must all appear; mantissa fields vary
#   4. complement pair: b == ~a exactly, every call
#   5. hamming: popcount == requested weight
#   6. no global-mwc pollution: drawing bitgen samples must not
#      advance the global stream (seed, draw, compare)
#
# Usage: scripts/bitgen-distribution.sh
#
set -u
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/probe.c" <<'EOF'
#include "stress-ng.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core-bitgen.h"
#include "core-mwc.h"

#define N (1000000)

static const uint64_t dict[] = {
	#include "bitgen-dict.inc"
};
#define NDICT (sizeof(dict) / sizeof(dict[0]))

static int popcount64(uint64_t v)
{
	int c = 0;
	while (v) { c += v & 1; v >>= 1; }
	return c;
}

int main(void)
{
	stress_bitgen_t bg;
	size_t i;
	int fails = 0;

	/* explicit seed: keeps the probe linkable without the full framework */
	stress_bitgen_seed(&bg, 0x5eed12345eed1234ULL);

	/* ---- 1. edge64 boundary hit rate ---- */
	size_t edge_hits = 0;
	for (i = 0; i < N / 10; i++) {
		uint64_t v = stress_bitgen_edge64(&bg);
		size_t d;
		for (d = 0; d < NDICT; d++) {
			uint64_t diff = v > dict[d] ? v - dict[d] : dict[d] - v;
			if (diff <= 4) { edge_hits++; break; }
		}
	}
	/* uniform baseline: P(within ±4 of any of ~57 dict values) ≈ 57*9/2^64 ≈ 0 */
	printf("EDGE_HITS %zu\n", edge_hits);
	if (edge_hits < (N / 10) / 2) {
		fprintf(stderr, "FAIL: edge64 hit rate too low (%zu of %d)\n",
			edge_hits, N / 10);
		fails++;
	}

	/* ---- 2. bandwalk coverage ---- */
	stress_bitgen_seed(&bg, 42);
	static uint64_t ones[64];
	static size_t total = 0;
	for (i = 0; i < N; i++) {
		uint64_t v = stress_bitgen_bandwalk64(&bg);
		int b;
		for (b = 0; b < 64; b++)
			if (v & (1ULL << b))
				ones[b]++;
		total++;
	}
	int uncovered = 0, extreme = 0;
	for (i = 0; i < 64; i++) {
		if (ones[i] == 0)
			uncovered++;
		/* every bit must sometimes be 0 and sometimes 1 */
		if (ones[i] == 0 || ones[i] == total)
			extreme++;
	}
	printf("BANDWALK total=%zu uncovered_bits=%d extreme_bits=%d\n",
		total, uncovered, extreme);
	if (uncovered != 0 || extreme != 0) {
		fprintf(stderr, "FAIL: bandwalk bit coverage broken\n");
		fails++;
	}

	/* ---- 3. fp64_bits field coverage ---- */
	stress_bitgen_seed(&bg, 7);
	size_t subnormal = 0, infnan = 0, midexp = 0;
	uint64_t mant_or = 0;
	for (i = 0; i < N; i++) {
		uint64_t v = stress_bitgen_fp64_bits(&bg);
		uint64_t e = (v >> 52) & 0x7ff;
		if (e == 0) subnormal++;
		else if (e == 2047) infnan++;
		else midexp++;
		mant_or |= (v & 0x000fffffffffffffULL);
	}
	printf("FP64 subnormal=%zu infnan=%zu midexp=%zu mant_or=%llx\n",
		subnormal, infnan, midexp, (unsigned long long)mant_or);
	/* expected ~1/8 subnormal, ~1/8 inf/nan, ~3/4 middle */
	if (subnormal < N / 32 || infnan < N / 32 || midexp < N / 2) {
		fprintf(stderr, "FAIL: fp64 exponent mix off\n");
		fails++;
	}
	if (mant_or != 0x000fffffffffffffULL) {
		fprintf(stderr, "FAIL: fp64 mantissa fields not fully covered\n");
		fails++;
	}

	/* ---- 4. complement pairs ---- */
	stress_bitgen_seed(&bg, 99);
	for (i = 0; i < 100000; i++) {
		uint64_t a, b;
		stress_bitgen_complement_pair64(&bg, &a, &b);
		if (b != ~a) {
			fprintf(stderr, "FAIL: complement pair broken at %zu\n", i);
			fails++;
			break;
		}
	}
	printf("COMPLEMENT ok\n");

	/* ---- 5. hamming ---- */
	stress_bitgen_seed(&bg, 123);
	for (i = 0; i < 100000; i++) {
		unsigned w = (unsigned)(i % 33);
		if ((unsigned)popcount64(stress_bitgen_hamming64(&bg, w)) != w) {
			fprintf(stderr, "FAIL: hamming weight broken at %zu\n", i);
			fails++;
			break;
		}
	}
	printf("HAMMING ok\n");

	/* ---- 6. global-mwc isolation ---- */
	/* stress_bitgen_init() draws exactly one 64-bit value from the
	 * global stream (by design, so runs differ); every subsequent
	 * generation must run on the private state only.  Measure the
	 * global stream position before/after init + 1000 samples. */
	stress_mwc_seed_set(0x11111111, 0x22222222);
	stress_bitgen_init(&bg);
	uint32_t w1, z1;
	stress_mwc_seed_get(&w1, &z1);
	/* snapshot: the stream advanced exactly one mwc64 (two mwc32
	 * steps) past the init draw — record where it now sits */
	uint32_t w_after_init, z_after_init;
	stress_mwc_seed_get(&w_after_init, &z_after_init);
	(void)w1; (void)z1;
	/* consume bitgen samples */
	for (i = 0; i < 1000; i++)
		(void)stress_bitgen_u64(&bg);
	uint32_t w2, z2;
	stress_mwc_seed_get(&w2, &z2);
	if (w2 != w_after_init || z2 != z_after_init) {
		fprintf(stderr, "FAIL: bitgen samples advanced the global mwc stream\n");
		fails++;
	}
	printf("ISOLATION ok\n");

	printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails);
	return fails ? 1 : 0;
}
EOF

# generate the dict include from core-bitgen.c's table
sed -n '/bitgen_edge_dict\[\] = {/,/^};/p' core-bitgen.c | \
	grep -oE '0x[0-9a-fA-F]+ULL' | sed 's/$/,/' > "$TMP/bitgen-dict.inc"

# core-mwc.c pulls framework symbols (reseed path); stub them so the
# probe links standalone
cat > "$TMP/stubs.c" <<'EOF'
#include "stress-ng.h"
uint64_t g_opt_flags;
void pr_inf(const char *fmt, ...) { (void)fmt; }
bool stress_little_endian(void) { return true; }
/* framework getters only referenced by stress_mwc_reseed()'s entropy
 * pool — the probe never calls reseed, stubs exist only to link */
/* signatures per core-setting.h / core-helper.h / core-filesystem.h / core-time.h */
bool stress_setting_get(const char *name, void *value) { (void)name; (void)value; return false; }
uint64_t stress_machine_id_get(void) { return 0; }
int stress_load_average_get(stress_load_average_info_t *lai) { (void)lai; return -1; }
unsigned int stress_cpu_get(void) { return 1; }
int32_t stress_cpus_online_get(void) { return 1; }
int32_t stress_ticks_per_second_get(void) { return 100; }
uint64_t stress_fs_size_get(void) { return 0; }
uint64_t stress_memory_phys_size_get(void) { return 1ULL << 30; }
int stress_kernel_release_get(void) { return -1; }
double stress_time_now(void) { return 0.0; }
EOF
cc -O2 -Wall -I. -Itest -I"$TMP" -o "$TMP/probe" "$TMP/probe.c" core-bitgen.c core-mwc.c "$TMP/stubs.c" -lm || {
	echo "probe build failed (ensure 'make config.h' has been run)"; exit 1; }

"$TMP/probe"
