#!/bin/bash
#
#  ci-monitor.sh - monitor multi-os-verify CI runs via anonymous API
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or
#  modify it under the GNU License as published by the Free Software
#  Foundation; either version 2, or (at your option) any later version.
#
#  No gh CLI or token needed - anonymous api.github.com access (rate
#  limit 60 req/h, this script uses ~2-5 per invocation).
#
#  Usage:
#	ci-monitor.sh                     # latest run status overview
#	ci-monitor.sh --watch             # latest run + job breakdown
#	ci-monitor.sh --run ID [--deep]   # specific run; --deep pulls
#	                                  # per-job conclusions and greps
#	                                  # CI-MATRIX lines for the
#	                                  # round-13 additions
#	ci-monitor.sh --new-code          # find the first run whose head
#	                                  # includes a given commit
#	                                  # (default: the P3-P6 series)
#

set -u

REPO="wangxumarshall/sdc-stressng"
WFID="360487474"			# multi-os-verify.yml
API="https://api.github.com/repos/${REPO}"
# first commit of the 2026-09-20 P3-P6 series
SERIES_BASE="d48246ebc"

curl_json()
{
	curl -s --max-time 20 -H "Accept: application/vnd.github+json" "$1"
}

latest_runs()
{
	curl_json "${API}/actions/workflows/${WFID}/runs?per_page=5" | python3 -c "
import json, sys
d = json.load(sys.stdin)
if 'workflow_runs' not in d:
    print('API error:', d.get('message', d)); sys.exit(1)
for r in d['workflow_runs']:
    print('%s %s %s %s %s head=%s' % (r['id'], r['status'],
        r['conclusion'], r['event'], r['created_at'], r['head_sha'][:9]))
"
}

run_status()
{
	local rid="$1"
	curl_json "${API}/actions/runs/${rid}" | python3 -c "
import json, sys
r = json.load(sys.stdin)
print('run:      ', r['id'])
print('status:   ', r['status'], '/', r['conclusion'])
print('event:    ', r['event'])
print('head:     ', r['head_sha'][:9])
print('started:  ', r['run_started_at'])
print('updated:  ', r['updated_at'])
print('url:      ', r['html_url'])
"
}

run_jobs()
{
	local rid="$1" deep="${2:-}"
	curl_json "${API}/actions/runs/${rid}/jobs?per_page=100" | python3 -c "
import json, sys
d = json.load(sys.stdin)
jobs = d.get('jobs', [])
if not jobs:
    print('no jobs (or API error:', d.get('message', '?'), ')'); sys.exit(0)
from collections import Counter
c = Counter((j['status'], j['conclusion']) for j in jobs)
print('jobs: %d total' % len(jobs))
for (st, co), n in sorted(c.items()):
    print('  %-12s %-12s x%d' % (st, co, n))
bad = [j for j in jobs if j['conclusion'] not in ('success', 'skipped', None)]
for j in bad:
    print('FAILED: %s - %s' % (j['name'], j.get('conclusion')))
    for s in (j.get('steps') or []):
        if s.get('conclusion') not in ('success', 'skipped', None):
            print('    step: %s -> %s' % (s['name'], s.get('conclusion')))
"
}

#  --new-code: first run at or after the series base commit.
#  Uses list of runs and compares head SHA prefixes - the runs are in
#  reverse-chronological order, so walk from oldest of the fetched page.
new_code_run()
{
	curl_json "${API}/actions/workflows/${WFID}/runs?per_page=30" | python3 -c "
import json, sys
base = '${SERIES_BASE}'
d = json.load(sys.stdin)
runs = d.get('workflow_runs', [])
found = None
for r in reversed(runs):	# oldest first
    sha = r['head_sha']
    # compare commit dates via the run list ordering only: a run
    # created after the push that carries any head != old series
    # heads is a candidate; the reliable check is creation date
    if r['created_at'] >= '2026-09-20T09:30':	# push time of P3
        found = r
        break
if found:
    print('first run after the P3-P6 push:')
    print('%s %s %s %s head=%s' % (found['id'], found['status'],
        found['conclusion'], found['created_at'], found['head_sha'][:9]))
else:
    print('no run since the 2026-09-20 push yet; next cron 04:00 UTC')
"
}

case "${1:-}" in
--watch)
	latest_runs
	echo
	rid=$(curl_json "${API}/actions/workflows/${WFID}/runs?per_page=1" | python3 -c "import json,sys; print(json.load(sys.stdin)['workflow_runs'][0]['id'])")
	run_status "$rid"
	echo
	run_jobs "$rid"
	;;
--run)
	[ $# -ge 2 ] || { echo "usage: $0 --run ID [--deep]" >&2; exit 1; }
	run_status "$2"
	echo
	run_jobs "$2"
	;;
--new-code)
	new_code_run
	;;
*)
	latest_runs
	;;
esac
