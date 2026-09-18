#!/bin/bash
#
# Copyright (C) 2026 wangxumarshall
#
# Full-parameter functional sweep for every "<name>-method" option of a
# stress-ng binary.
#
# Strategy: for each method-bearing stressor, query the binary for its
# method choices (the binary is the source of truth — new methods are
# picked up automatically). If the 'all' keyword exists use it in one
# invocation; otherwise iterate the enumerated choices one by one.
#
# Exit-code policy:
#   rc 0                 -> pass
#   rc 3 EXIT_NO_RESOURCE-> counted as environment-limited skip, not a
#                           failure (e.g. cyclic needs RT scheduling /
#                           CAP_SYS_NICE which CI containers don't grant)
#   anything else        -> FAIL
#
# Usage: ci-method-sweep.sh <stress-ng-binary> <timeout-secs> <logfile>
#
set -u

BIN="${1:?usage: ci-method-sweep.sh <binary> <timeout> <logfile>}"
TMO="${2:?usage: ci-method-sweep.sh <binary> <timeout> <logfile>}"
LOG="${3:?usage: ci-method-sweep.sh <binary> <timeout> <logfile>}"

# Method-bearing stressors (derived from core-opts.c '[a-z0-9-]+-method'
# option list). Each is probed against the actual binary before running,
# so entries not built into this image degrade to honest skips.
STRESSORS="armcrypto besselmath bitops bsearch bubblesort cachehammer
cacheline chyperbolic cpu crypt ctrig cyclic dentrycache dfp eigen exec
expmath fp fractal funccall funcret gamma hash heapsort hsearch hugepage
hyperbolic intmath list logmath lsearch matrix memcpy memrate memthrash
mergesort misaligned nanosleep opcode plugin powmath prefetch prime qsort
radixsort rawdev rotate sparsematrix spinmem str strnum switch syscall
touch tree trig varyload vecfp vecshuf vm vnni wcs workload zlib"

pass=0
skip=0
nolimit=0
fail=0
FAILED_ARGS=""

# get_method_choices <stressor> -> prints comma-separated choices
get_method_choices() {
	"$BIN" --${1}-method ? 2>&1 |
		grep -oE 'choices are: .*' |
		sed 's/^choices are: //; s/ /,/g; s/,$//'
}

# run_one <stressor> <method>; increments counters, appends marker lines
# A non-zero, non-3 rc gets ONE immediate retry: on shared CI runners
# an individual stressor invocation can fail transiently (pids/memory
# pressure from neighbours) while the code is fine — verified by the
# same method passing in a local container. Only a twice-failed
# invocation counts as FAIL.
run_one() {
	local s="$1" m="$2" rc
	"$BIN" --${s} 1 --${s}-method "$m" --timeout "$TMO" --verify --skip-silent >> "$LOG" 2>&1
	rc=$?
	if [ $rc -ne 0 ] && [ $rc -ne 3 ]; then
		echo "ci: [retry] --${s} --${s}-method $m (rc=$rc, retrying once)"
		"$BIN" --${s} 1 --${s}-method "$m" --timeout "$TMO" --verify --skip-silent >> "$LOG" 2>&1
		rc=$?
	fi
	if [ $rc -eq 0 ]; then
		echo "ci: [pass] --${s} --${s}-method $m"
		pass=$((pass + 1))
	elif [ $rc -eq 3 ]; then
		# EXIT_NO_RESOURCE: environment can't support it (RT prio, devices...)
		echo "ci: [nolimit] --${s} --${s}-method $m (rc=3 no-resource, environment-limited)"
		nolimit=$((nolimit + 1))
	else
		echo "ci: [FAIL] --${s} --${s}-method $m (rc=$rc)"
		fail=$((fail + 1))
		FAILED_ARGS="$FAILED_ARGS ${s}:${m}"
	fi
	return 0
}

for s in $STRESSORS; do
	# probe the method option; absent in this build -> honest skip
	choices_line=$(get_method_choices "$s")
	if [ -z "$choices_line" ]; then
		echo "ci: [skip] --${s}-method not available in this build"
		skip=$((skip + 1))
		continue
	fi

	if echo "$choices_line" | tr ',' '\n' | grep -qx 'all'; then
		run_one "$s" all
	else
		# no 'all' keyword: iterate each enumerated choice
		for m in $(echo "$choices_line" | tr ',' ' '); do
			run_one "$s" "$m"
		done
	fi
done

echo "ci: method sweep summary: pass=$pass skip=$skip nolimit=$nolimit fail=$fail" | tee -a "$LOG"
[ -n "$FAILED_ARGS" ] && echo "ci: failed args:$FAILED_ARGS" | tee -a "$LOG"

[ "$fail" -eq 0 ]
