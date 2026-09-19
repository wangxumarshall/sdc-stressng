#!/bin/bash
#
# Copyright (C) 2026 wangxumarshall
#
# CI assertion helper: classify a stress-ng run log + exit code.
#
# Exit codes:
#   0 = run is healthy (passed stressors, zero failures)
#   1 = failures detected or summary missing
#   2 = usage error
#
# Usage: ci-verify-log.sh <logfile> <run-label> [rc]
#
# This script is part of the multi-OS GitHub Actions verification
# (.github/workflows/multi-os-verify.yml). It implements the "did any
# stressor actually fail" gate so the workflow can distinguish a genuine
# functional bug (failed: N>0, rc=1) from an honest skip (skipped: N).
#
# Implementation note: deliberately fork-free. After a full
# --sequential sweep the container can be polluted with reaped-late
# worker processes and hit the cgroup pids limit, which makes every
# external command (grep/awk/tail) fail with 'fork: Resource
# temporarily unavailable' — the first CI runs then mis-classified
# healthy runs because every probe silently returned nothing. All
# parsing below uses bash builtins (read/comparison) only.
#
set -u

LOG="${1:?usage: ci-verify-log.sh <logfile> <label> [rc]}"
LABEL="${2:?usage: ci-verify-log.sh <logfile> <label> [rc]}"
RC="${3:-}"

if [[ ! -f "$LOG" ]]; then
	echo "::error::log file '$LOG' not found"
	exit 2
fi

skipped=0
passed=0
failed=0
completed=0
fail_seen=0

# single pass over the log with builtins only
while IFS= read -r line; do
	if [[ "$line" == *"successful run completed"* ]]; then
		completed=$((completed + 1))
	elif [[ "$line" == *"unsuccessful run completed"* ]]; then
		# stress-ng reports failures this way; keep completed at 0
		# so the summary check below flags it as expected
		continue
	fi
	# summary lines: "skipped: N", "passed: N", "failed: N" — the count
	# may be followed by ": name (n) name (n) ..." detail; strip to the
	# leading integer
	if [[ "$line" == *"skipped: "* ]]; then
		skipped=${line##*skipped: }
		skipped=${skipped%%[!0-9]*}
	elif [[ "$line" == *"passed: "* ]]; then
		passed=${line##*passed: }
		passed=${passed%%[!0-9]*}
	elif [[ "$line" == *"failed: "* ]]; then
		failed=${line##*failed: }
		failed=${failed%%[!0-9]*}
		[[ "$failed" =~ ^[1-9] ]] && fail_seen=1
	fi
done < "$LOG"

echo "[$LABEL] passed=${passed:-0} skipped=${skipped:-0} failed=${failed:-0} completed_runs=${completed}"

# --- failures detected ---
if [[ "$fail_seen" == 1 ]]; then
	# reprint the failing summary line(s) for the annotation
	while IFS= read -r line; do
		[[ "$line" == *"failed: "[1-9]* ]] && echo "::error::[$LABEL] $line"
	done < "$LOG"
	exit 1
fi

# --- a run that never reached its summary means it died mid-suite ---
if [[ "$completed" -lt 1 ]]; then
	echo "::error::[$LABEL] no 'successful run completed' summary line — run died before finishing (rc=${RC:-unknown})"
	# last 30 lines via builtin read (tail would fork)
	mapfile -t tail_lines < "$LOG"
	n=${#tail_lines[@]}
	start=$((n > 30 ? n - 30 : 0))
	for ((i = start; i < n; i++)); do
		echo "    ${tail_lines[i]}"
	done
	exit 1
fi

# --- explicit rc cross-check when provided ---
if [[ -n "$RC" && "$RC" != "0" ]]; then
	if [[ "$RC" == "3" && "$fail_seen" != "1" && "$completed" -ge 1 ]]; then
		# EXIT_NO_RESOURCE: at least one stressor aborted early for
		# lack of system resources (counted as skipped, run otherwise
		# complete). On constrained CI runners this is an environment
		# condition, not a code defect — the suite summary above shows
		# zero failures. Report as a warning and pass.
		echo "::warning::[$LABEL] exit code 3 (no system resources): at least one stressor aborted early for lack of resources; run completed with failed=0 — treated as environment-limited"
	else
		echo "::error::[$LABEL] stress-ng exit code $RC but no 'failed:' line — classify before ignoring"
		mapfile -t tail_lines < "$LOG"
		n=${#tail_lines[@]}
		start=$((n > 30 ? n - 30 : 0))
		for ((i = start; i < n; i++)); do
			echo "    ${tail_lines[i]}"
		done
		exit 1
	fi
fi

echo "[$LABEL] OK"
exit 0
