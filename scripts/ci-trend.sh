#!/bin/bash
#
#  ci-trend.sh - bogo-ops/s time series across multi-os-verify CI runs
#  (P5 of the field-validation plan).
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or
#  modify it under the GNU License as published by the Free Software
#  Foundation; either version 2, or (at your option) any later version.
#
#  The multi-os-verify workflow emits one CI-MATRIX line per stressor
#  per image into each build-test job log:
#
#	CI-MATRIX <image-tag> <stressor> <PASS|SKIP|FAIL> <bogo-ops/s|->
#
#  Each run's results-summary renders a one-off matrix; this script
#  aggregates the SAME lines across historical runs into a time series
#  so bogo-ops stability before/after a change (e.g. the 2026-09-19
#  operand/address mutation merge fc243c784..main) becomes visible.
#
#  Requires: gh (authenticated), network.
#
#  Usage:
#	scripts/ci-trend.sh [--days N] [--stressor fma,memrate,...]
#	                    [--tag 24.03-lts-sp4] [--workflow multi-os-verify]
#

set -u

DAYS=7
STRESSORS=""
TAG=""
WORKFLOW="multi-os-verify.yml"
REPO="wangxumarshall/sdc-stressng"

while [ $# -gt 0 ]; do
	case "$1" in
	--days)		DAYS="$2"; shift 2 ;;
	--stressor)	STRESSORS="$2"; shift 2 ;;
	--tag)		TAG="$2"; shift 2 ;;
	--workflow)	WORKFLOW="$2"; shift 2 ;;
	*)		echo "unknown option: $1" >&2; exit 1 ;;
	esac
done

SINCE=$(date -d "-${DAYS} days" +%Y-%m-%d)

command -v gh > /dev/null 2>&1 || { echo "gh CLI required" >&2; exit 1; }

#  Collect every CI-MATRIX line from every run's build-test job logs.
#  Job logs are kept by GitHub for 90 days; older runs simply drop out.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

#  run list: completed runs of the workflow since $SINCE
gh api --paginate \
	"repos/${REPO}/actions/workflows/${WORKFLOW}/runs?status=success&created=>=${SINCE}" \
	--jq '.workflow_runs[].id' > "$tmp/runs" 2>/dev/null

[ -s "$tmp/runs" ] || { echo "no completed runs since $SINCE" >&2; exit 1; }
echo "# runs since $SINCE: $(wc -l < "$tmp/runs")"

n=0
while read -r run_id; do
	created=$(gh api "repos/${REPO}/actions/runs/${run_id}" \
		--jq '.created_at' 2>/dev/null)
	#  build-test jobs of this run
	for job_id in $(gh api \
		"repos/${REPO}/actions/runs/${run_id}/jobs?per_page=100" \
		--jq '.jobs[] | select(.name | startswith("build-test")) | .id' \
		2>/dev/null); do
		gh api "repos/${REPO}/actions/jobs/${job_id}/logs" 2>/dev/null |
			grep -a '^CI-MATRIX ' |
			sed "s/^/run=${run_id} date=${created} /" >> "$tmp/all"
	done
	n=$((n + 1))
	echo "# collected run $n (id $run_id)"
done < "$tmp/runs"

[ -s "$tmp/all" ] || { echo "no CI-MATRIX lines found" >&2; exit 1; }

#  Filter and normalise: keep date run-id image stressor status rate
awk -v sel="$STRESSORS" -v tag="$TAG" '
{
	# fields: run=<id> date=<iso> CI-MATRIX <tag> <stressor> <status> <rate>
	run_id = $1; sub(/^run=/, "", run_id)
	date = $2; sub(/^date=/, "", date); sub(/T.*/, "", date)
	img = $4
	stressor = $5
	status = $6
	rate = $7
	if (tag != "" && img != tag) next
	if (sel != "") {
		split(sel, want, ",")
		hit = 0
		for (w in want)
			if (stressor == want[w]) hit = 1
		if (!hit) next
	}
	print date, run_id, img, stressor, status, rate
}' "$tmp/all" | sort -k3,3 -k4,4 > "$tmp/sel"

#  Per stressor x image: time series + coefficient of variation
echo
echo "=== bogo-ops/s time series (per stressor x image) ==="
awk '{
	key = $4 " @" $3
	if (key != prev) {
		if (prev != "") printf "\n"
		printf "%-14s %-18s", $4, $3
		prev = key
	}
	if ($5 == "PASS") printf " %s:%.0f", $1, $6
	else printf " %s:%s", $1, $5
}
END { printf "\n" }' "$tmp/sel"

echo
echo "=== per stressor x image: mean / coefficient of variation over PASS rates ==="
awk '$5 == "PASS" {
	key = $4 " @" $3
	n[key]++
	sum[key] += $6
	sumsq[key] += $6 * $6
}
END {
	printf "%-33s %8s %10s %8s\n", "stressor @ image", "samples", "mean", "CoV%"
	for (key in n) {
		mean = sum[key] / n[key]
		var = (sumsq[key] - n[key] * mean * mean) / n[key]
		if (var < 0) var = 0
		cov = mean > 0 ? 100 * sqrt(var) / mean : 0
		printf "%-33s %8d %10.0f %7.1f%%\n", key, n[key], mean, cov
	}
}' "$tmp/sel" | sort -k3 -rn
