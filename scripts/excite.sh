#!/bin/bash
#
#  excite.sh - one-command maximum SDC excitation.
#
#  The intended entry point of the self-contained release package: no
#  options to learn, no build step, no library installs. Extract the
#  tarball, run this script, and every CPU cycle goes into load that
#  maximizes the probability of exciting silent data corruption out of
#  marginal cores. Detection is SDCShield's job (run it alongside via
#  --sdcshield).
#
#  Usage:
#	excite.sh [minutes] [-- extra sdc-run.sh arguments]
#
#	    minutes        excitation duration, default 120
#	    --sdcshield CMD  run the SDCShield detector alongside
#
#  Examples:
#	excite.sh                     # 2 hours, 10-min preheat
#	excite.sh 480                 # 8-hour soak
#	excite.sh 60 --sdcshield "./run-sdcshield.sh"
#
#  Package layout (release tarball):
#	stress-ng            the excitation engine binary
#	excite.sh            this script
#	scripts/sdc-run.sh   full orchestrator (excite/full/scan/path/pair/abtest)
#	lib/*.so             bundled non-glibc shared libraries
#
#  Works unpacked from the tarball and directly from a repo checkout
#  (where it lives in scripts/ next to sdc-run.sh).
#
#  Copyright (C) 2026  license: GPL-2.0 (see the repo COPYING)
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

[ -x "$HERE/stress-ng" ] || {
	echo "error: stress-ng binary not found next to excite.sh ($HERE)" >&2
	exit 1
}

#  Self-contained package: prefer the bundled shared libraries over the
#  host's when lib/ exists (glibc itself comes from the host OS - pick
#  the tarball matching your openEuler version, or an older one).
if [ -d "$HERE/lib" ] && ls "$HERE"/lib/*.so* >/dev/null 2>&1; then
	export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

MINUTES=${1:-120}
case "$MINUTES" in
	''|*[!0-9]*)
		echo "usage: $0 [minutes] (default 120) [-- extra sdc-run.sh arguments]" >&2
		exit 1 ;;
esac
shift
[ "${1:-}" = "--" ] && shift

#  Locate the orchestrator: tarball layout (scripts/) or repo layout
RUN="$HERE/scripts/sdc-run.sh"
[ -f "$RUN" ] || RUN="$HERE/sdc-run.sh"
[ -f "$RUN" ] || { echo "error: sdc-run.sh not found" >&2; exit 1; }

echo "==================================================================="
echo "  SDC EXCITATION ENGINE - maximum excitation, one command"
echo "  duration: ${MINUTES} min (+10 min preheat before the window)"
echo ""
echo "  DANGER: this deliberately drives the machine beyond its normal"
echo "  operating envelope (max power, heat, di/dt). Run it ONLY on a"
echo "  machine dedicated to stress testing."
echo "==================================================================="
[ "$(id -u)" -eq 0 ] || echo "  note: not root - some stressors will honestly skip"

#  The excite recipe: widest shaped load mix (cpu-method all + fma +
#  armcrypto + operand-var + addrspace + memrate-bandwalk + vm-rand-
#  offset) + varyload di/dt steps, NO verify sentinels - detection is
#  delegated to SDCShield. rc=0 means excitation completed, not that
#  the machine is healthy.
exec env NG="$HERE/stress-ng" bash "$RUN" excite -t $((MINUTES * 60)) --preheat 10 "$@"
