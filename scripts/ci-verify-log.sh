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
set -u

LOG="${1:?usage: ci-verify-log.sh <logfile> <label> [rc]}"
LABEL="${2:?usage: ci-verify-log.sh <logfile> <label> [rc]}"
RC="${3:-}"

if [ ! -f "$LOG" ]; then
	echo "::error::log file '$LOG' not found"
	exit 2
fi

# --- summary lines (anchors verified on stress-ng 0.22.00, gcc 12.3.1) ---
SKIPPED=$(grep -oE 'skipped: [0-9]+' "$LOG" | tail -1 | awk '{print $2}')
PASSED=$(grep -oE 'passed: [0-9]+' "$LOG" | grep -v 'skipped' | tail -1 | awk '{print $2}')
FAILED=$(grep -oE 'failed: [0-9]+' "$LOG" | tail -1 | awk '{print $2}')
COMPLETED=$(grep -c 'successful run completed' "$LOG")

echo "[$LABEL] passed=${PASSED:-0} skipped=${SKIPPED:-0} failed=${FAILED:-0} completed_runs=${COMPLETED}"

# --- failure extraction for the job summary ---
FAIL_LINES=$(grep -E 'failed: [1-9]' "$LOG" | head -20)
if [ -n "$FAIL_LINES" ]; then
	echo "::error::[$LABEL] stressor failures detected:"
	echo "$FAIL_LINES"
	exit 1
fi

# --- a run that never reached its summary means it died mid-suite ---
if [ "$COMPLETED" -lt 1 ]; then
	echo "::error::[$LABEL] no 'successful run completed' summary line — run died before finishing (rc=${RC:-unknown})"
	tail -30 "$LOG" | sed 's/^/    /'
	exit 1
fi

# --- explicit rc cross-check when provided ---
if [ -n "$RC" ] && [ "$RC" -ne 0 ]; then
	echo "::error::[$LABEL] stress-ng exit code $RC but no 'failed:' line — classify before ignoring"
	tail -30 "$LOG" | sed 's/^/    /'
	exit 1
fi

echo "[$LABEL] OK"
exit 0
