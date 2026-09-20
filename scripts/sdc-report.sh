#!/bin/bash
#
#  sdc-report.sh - mismatch statistics report for an sdc-run.sh output
#  directory (P1 of the field-validation plan).
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or
#  modify it under the GNU License as published by the Free Software
#  Foundation; either version 2, or (at your option) any later version.
#
#  A run directory produced by "sdc-run.sh full" (or "abtest") holds:
#
#    topology.txt     mode/duration/topology snapshot
#    preheat.log      optional preheat stage log
#    A_full.log       stress-ng stdout/stderr (pr_fail mismatch lines)
#    A_full.yaml      stress-ng -Y output (verify-failures per stressor,
#                     requires --metrics; sdc-run.sh passes it)
#    sdcshield.log    optional SDCShield yaml-ish output
#
#  This script turns those raw logs into a one-page report.txt that can
#  be diffed between two run directories (A/B regression, P2):
#
#    - verify failure count per stressor (yaml first, log fallback)
#    - first mismatch wall-clock position vs the preheat boundary
#      (quantifies the residual-heat ordering effect)
#    - sdcshield per-test fail rates and cpu-mask aggregation
#
#  Usage:
#	scripts/sdc-report.sh <rundir> [rundir2 ...]
#
#  Output: <rundir>/report.txt (also echoed to stdout)
#

set -u

usage()
{
	echo "usage: $0 <rundir> [rundir2 ...]" >&2
	exit "${1:-1}"
}

report_one()
{
	local dir="$1"
	local log="$dir/A_full.log"
	local yaml="$dir/A_full.yaml"
	local shield="$dir/sdcshield.log"
	local report="$dir/report.txt"

	[ -d "$dir" ] || { echo "no such directory: $dir" >&2; return 1; }

	{
	echo "=== SDC report: $dir ==="
	echo "generated: $(date '+%Y-%m-%d %H:%M:%S')"

	#  topology snapshot as-is (mode/duration/smt/features)
	if [ -f "$dir/topology.txt" ]; then
		echo "--- topology ---"
		cat "$dir/topology.txt"
	fi

	#  preheat boundary: when did the verify window open relative to
	#  the heat load?  first-mismatch offsets are interpreted against
	#  this
	if [ -f "$dir/preheat.log" ]; then
		echo "--- preheat: yes (verify window opened hot) ---"
	else
		echo "--- preheat: no ---"
	fi

	#  stress-ng verify failures, yaml source of truth (P3 field)
	echo "--- stress-ng verify failures ---"
	local yaml_failures=0
	if [ -f "$yaml" ]; then
		#  verify-failures: N lines follow "- stressor: NAME"
		awk '/- stressor: / { name = $3 }
		     /verify-failures: / { print name, $2; total += $2 }
		     END { if (total > 0)
				printf "TOTAL stress-ng verify-failures: %d\n", total }' \
			"$yaml" | sort -k2 -rn
		yaml_failures=$(awk '/verify-failures: / { s += $2 } END { print s+0 }' "$yaml")
	fi
	if [ "$yaml_failures" -eq 0 ] && [ -f "$log" ]; then
		#  fallback: count pr_fail mismatch lines in the log
		#  ("fail:" lines that carry a difference/mismatch wording)
		local log_failures
		log_failures=$(grep -cE 'fail: .*(difference|mismatch|error detected|does not contain)' "$log" || true)
		echo "TOTAL stress-ng log mismatch lines: $log_failures"
		[ "$log_failures" -gt 0 ] && \
			grep -E 'fail: .*(difference|mismatch|error detected|does not contain)' "$log" | head -5
	fi

	#  first mismatch position: line number of the first fail line as a
	#  fraction of the log - a coarse but diff-stable "how early did it
	#  break" indicator; with --preheat runs an early fail (low
	#  fraction) right after the hot stage is the residual-heat signal
	if [ -f "$log" ] && grep -qE 'fail: .*difference' "$log"; then
		local total first frac
		total=$(wc -l < "$log")
		first=$(grep -nE 'fail: .*difference' "$log" | head -1 | cut -d: -f1)
		frac=$((first * 100 / total))
		echo "first mismatch at log line $first/$total (${frac}% into the verify window)"
	fi

	#  sdcshield summary: per-test fail ratio lines and cpu masks
	echo "--- sdcshield ---"
	if [ -f "$shield" ]; then
		local shield_fails
		shield_fails=$(grep -c 'result: fail' "$shield" || true)
		echo "tests failed: $shield_fails"
		grep -E '# Test failed [0-9]+ out of' "$shield" | head -10
		echo "cpu-masks:"
		grep -E 'cpu-mask: [^n]' "$shield" | sort -u | head -10
	else
		echo "(no sdcshield.log - golden cross-check not run)"
	fi

	#  bottom line for diffing
	echo "=== bottom line ==="
	echo "stress-ng verify-failures: $yaml_failures"
	} | tee "$report"

	return 0
}

[ $# -ge 1 ] || usage 1
rc=0
for dir in "$@"; do
	report_one "$dir" || rc=1
	echo
done
exit $rc
