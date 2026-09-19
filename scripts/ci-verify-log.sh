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
# Usage: ci-verify-log.sh <logfile> <run-label> [rc] [image-tag]
#
# When <image-tag> is given, the script additionally emits one
# CI-MATRIX line per stressor into the job log (the only channel
# guaranteed to survive the post-suite container exhaustion):
#
#   CI-MATRIX <image-tag> <stressor> <status> <bogo-ops/s-real>
#
# status is PASS / SKIP / FAIL; rate is '-' for skip/fail or when the
# stressor has no metrics line. These lines are later scraped from the
# job logs by the results-summary job to render the full
# stressor x image matrix in the run's summary page.
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

LOG="${1:?usage: ci-verify-log.sh <logfile> <label> [rc] [image-tag]}"
LABEL="${2:?usage: ci-verify-log.sh <logfile> <label> [rc] [image-tag]}"
RC="${3:-}"
IMAGE_TAG="${4:-}"

if [[ ! -f "$LOG" ]]; then
	echo "::error::log file '$LOG' not found"
	exit 2
fi

skipped=0
passed=0
failed=0
completed=0
fail_seen=0

# per-stressor state for the CI-MATRIX emission (associative arrays
# are bash builtins; populated only when IMAGE_TAG is set)
declare -A m_rate=()
declare -A m_status=()

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
		if [[ -n "$IMAGE_TAG" ]]; then
			# remainder after the count: "acl (1) acct (1) ..." —
			# name tokens carry no '(' , the "(n)" counts do
			list=${line##*skipped: }
			list=${list#*:}
			for w in $list; do
				[[ "$w" != *"("* ]] && m_status[$w]=""
			done
		fi
	elif [[ "$line" == *"passed: "* ]]; then
		passed=${line##*passed: }
		passed=${passed%%[!0-9]*}
		if [[ -n "$IMAGE_TAG" ]]; then
			list=${line##*passed: }
			list=${list#*:}
			for w in $list; do
				[[ "$w" != *"("* ]] && m_status[$w]=""
			done
		fi
	elif [[ "$line" == *"failed: "* ]]; then
		failed=${line##*failed: }
		failed=${failed%%[!0-9]*}
		[[ "$failed" =~ ^[1-9] ]] && fail_seen=1
		if [[ -n "$IMAGE_TAG" && "$failed" =~ ^[1-9] ]]; then
			# only a non-zero failure line carries a "name (n)" list;
			# "failed: 0" would otherwise register a bogus stressor "0"
			list=${line##*failed: }
			list=${list#*:}
			for w in $list; do
				[[ "$w" != *"("* ]] && m_status[$w]=FAIL
			done
		fi
	elif [[ "$line" == *"stress-ng: metrc:"* && "$IMAGE_TAG" ]]; then
		# metrics table row: "... stressor  bogo-ops  real  usr  sys  ops/s(real)  ops/s(usr+sys)"
		# fields after the pid bracket: name bogo real usr sys rate_real rate_usrsys
		row=${line##*\] }
		if [[ "$row" =~ ^([a-zA-Z0-9_-]+)[[:space:]]+([0-9]+)[[:space:]]+([0-9.]+)[[:space:]]+([0-9.]+)[[:space:]]+([0-9.]+)[[:space:]]+([0-9.]+) ]]; then
			m_rate[${BASH_REMATCH[1]}]=${BASH_REMATCH[6]}
		fi
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

# --- per-stressor matrix emission (fork-free, job-log scraping) ---
if [[ -n "$IMAGE_TAG" ]]; then
	# merge: stressors seen in the metrc table but not in the summary
	# lists (e.g. excluded pathological ones) default to SKIP
	for s in "${!m_rate[@]}"; do
		[[ -z "${m_status[$s]:-}" && -z "${m_status[$s]+x}" ]] && m_status[$s]=""
	done
	n_pass=0; n_skip=0; n_fail=0
	# stable order: iterate the metrc table order is not retained, so
	# sort keys via a bounded insertion (sort would fork) — instead
	# emit unsorted; the summary job collates and sorts
	for s in "${!m_status[@]}"; do
		st=${m_status[$s]}
		if [[ "$st" == "FAIL" ]]; then
			n_fail=$((n_fail + 1))
			echo "CI-MATRIX $IMAGE_TAG $s FAIL -"
		elif [[ -n "${m_rate[$s]:-}" ]]; then
			n_pass=$((n_pass + 1))
			echo "CI-MATRIX $IMAGE_TAG $s PASS ${m_rate[$s]}"
		else
			n_skip=$((n_skip + 1))
			echo "CI-MATRIX $IMAGE_TAG $s SKIP -"
		fi
	done
	echo "CI-MATRIX-SUMMARY $IMAGE_TAG pass=$n_pass skip=$n_skip fail=$n_fail"
fi
exit 0
