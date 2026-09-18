#!/bin/bash
#
#  sdc-run.sh - unified entry point for SDC (silent data corruption)
#  stress testing with stress-ng on aarch64 SMT machines.
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This script implements the three-stage SDC diagnostic funnel:
#
#    full    stage 1 - "trigger": all-cores background load with di/dt
#            load steps (varyload) + verify self-checks + RAS/thermal
#            observation.  Machine-wide concurrency is the trigger
#            condition for SDC (single-core isolation rarely triggers),
#            but attribution is coarse.
#    scan    stage 2 - "localise": sweep every physical core one at a
#            time (SMT sibling pairs), per-core yaml metrics and
#            suspects.txt.  Delegates to scripts/sdc-scan.sh.
#    path    stage 3 - "attribute": datapath-specific golden-value
#            cross checks (sve2 vector, ls64 64-byte atomic, crc32
#            hardware-vs-software).  Run against suspect cores or the
#            whole machine; a mismatch is direct SDC evidence for that
#            datapath.
#
#  The script first reads the CPU topology (online/isolated counts,
#  SMT sibling pairs, SVE2/ls64/crc32 hardware features) and derives
#  all stress-ng worker counts and --taskset arguments from it, so the
#  same command works unchanged on e.g. Kunpeng 920 (no SMT, 128 CPUs)
#  and Kunpeng 950 (SMT2, 382 CPUs).
#
#  Usage:
#	NG=./stress-ng ./scripts/sdc-run.sh <mode> [options]
#
#	Modes:
#	  full   [-t secs] [-o dir] [--sdcshield CMD] [--preheat mins]
#	         default duration 2h
#	  scan   [-t secs-per-core] [-o dir] [--sdcshield CMD]
#	  path   [-t secs] [-c cpulist] [-o dir]        default duration 600s
#	  pair   [-t secs-per-pair] [-c cpulist] [-o dir]
#	         SMT contention matrix: for each physical core, run the
#	         sibling threads against each other in 4 combinations
#	         (fma x fma, fma x cpu, armcrypto x fma, cacheline x
#	         cacheline) and record per-combination bogo-ops rates.
#	         A rate ratio near 0.5 means the contended resource is
#	         fully shared, near 1.0 means private - this maps the
#	         SMT2 sharing topology of the chip (no public microarch
#	         data exists for ARM server SMT2 cores).
#	  all    run full -> scan -> path sequentially
#
#	Options:
#	  -t SECS      duration (full/path: total; scan: per core;
#	                pair: per physical core)
#	  -c LIST      CPU list (scan/path/pair only), default: all online
#	  -o DIR       output directory, default sdc_<mode>_<timestamp>
#	  --sdcshield CMD   SDCShield command to run alongside (full) or
#	                    per core (scan); e.g. --sdcshield "./run-sdcshield.sh"
#	  --keep-bg N  scan mode: keep N workers of background load on the
#	                non-scanned cores (recommended: half of the physical
#	                cores) because single-core isolation rarely triggers
#	                SDC - the machine-wide concurrency is part of the
#	                trigger condition
#	  --preheat MINS  full mode: run a pure heat load (all-core fma +
#	                vecfp, no verify) for MINS minutes before the verify
#	                window; residual heat from a preceding hot load is an
#	                empirically observed SDC trigger condition (the
#                failing testcase only fails when run after the
#                heat-generating one)
#
#  Examples:
#	NG=./stress-ng ./scripts/sdc-run.sh full -t 7200
#	NG=./stress-ng ./scripts/sdc-run.sh scan -t 120 --keep-bg 64
#	NG=./stress-ng ./scripts/sdc-run.sh path -t 600 -c 192-381
#	NG=./stress-ng ./scripts/sdc-run.sh all
#

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
NG=${NG:-./stress-ng}
MODE=""
DUR=""
CPU_LIST=""
OUT=""
SDCSHIELD_ARG=""
KEEP_BG=0
PREHEAT=0

usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

#  Parse arguments
while [ $# -gt 0 ]; do
	case "$1" in
	full|scan|path|pair|all)
		MODE="$1"; shift ;;
	-t)
		DUR="$2"; shift 2 ;;
	-c|--cpus)
		CPU_LIST="$2"; shift 2 ;;
	-o|--out)
		OUT="$2"; shift 2 ;;
	--sdcshield)
		SDCSHIELD_ARG="$2"; shift 2 ;;
	--keep-bg)
		KEEP_BG="$2"; shift 2 ;;
	--preheat)
		PREHEAT="$2"; shift 2 ;;
	-h|--help)
		usage 0 ;;
	*)
		echo "unknown option: $1" >&2; usage 1 ;;
	esac
done

[ -n "$MODE" ] || { echo "error: mode required (full|scan|path|all)" >&2; usage 1; }
[ -x "$NG" ] || { echo "error: stress-ng not found or not executable: $NG (set NG=...)" >&2; exit 1; }

#  ---------------------------------------------------------------------------
#  Stage 0: read the CPU topology and hardware features
#  ---------------------------------------------------------------------------
SYS_CPU=/sys/devices/system/cpu

n_online=$(nproc)
online_list=$(cat "$SYS_CPU/online" 2>/dev/null || echo 0)
isolated_list=$(cat "$SYS_CPU/isolated" 2>/dev/null || true)
offline_list=$(cat "$SYS_CPU/offline" 2>/dev/null || true)

#  Expand "0-3,8,10-11" into "0 1 2 3 8 10 11"
expand_list()
{
	local list="$1" out="" part lo hi i
	local IFS=','
	for part in $list; do
		if [[ "$part" == *-* ]]; then
			lo="${part%%-*}"; hi="${part##*-}"
			for ((i = lo; i <= hi; i++)); do out="$out $i"; done
		else
			out="$out $part"
		fi
	done
	echo "$out"
}

#  Count logical CPUs in a list without spawning a subshell per call
count_list()
{
	local n=0
	for _ in $(expand_list "$1"); do n=$((n + 1)); done
	echo "$n"
}

#  Build the SMT sibling map: representative (lowest sibling) -> pair
declare -A SIBLING_PAIR
declare -A CPU_ONLINE
for c in $(expand_list "$online_list"); do
	CPU_ONLINE[$c]=1
done
N_LOGICAL=${#CPU_ONLINE[@]}

for c in "${!CPU_ONLINE[@]}"; do
	sib=$(cat "$SYS_CPU/cpu$c/topology/thread_siblings_list" 2>/dev/null || echo "$c")
	lowest=""
	pair=""
	for s in $(expand_list "$sib"); do
		[ -n "${CPU_ONLINE[$s]}" ] || continue
		[ -z "$pair" ] && pair="$s" || pair="$pair,$s"
		[ -z "$lowest" ] && lowest=$s
		[ "$s" -lt "$lowest" ] && lowest=$s
	done
	[ -z "$pair" ] && continue
	cur="${SIBLING_PAIR[$lowest]:-}"
	if [ -z "$cur" ]; then
		SIBLING_PAIR[$lowest]="$pair"
	fi
done
N_PHYSICAL=${#SIBLING_PAIR[@]}

#  SMT or not: any pair with more than one CPU means SMT is active
SMT=0
for rep in "${!SIBLING_PAIR[@]}"; do
	if [[ "${SIBLING_PAIR[$rep]}" == *,* ]]; then
		SMT=1
		break
	fi
done

#  Hardware features from /proc/cpuinfo (single read, cached)
CPUINFO_FLAGS=""
read_cpuinfo_flags()
{
	[ -n "$CPUINFO_FLAGS" ] && { echo "$CPUINFO_FLAGS"; return; }
	CPUINFO_FLAGS=$(grep -m1 -E "^Features" /proc/cpuinfo 2>/dev/null | cut -d: -f2-)
	echo "$CPUINFO_FLAGS"
}

HAS_SVE2=0; HAS_LS64=0; HAS_CRC32=0
FLAGS=$(read_cpuinfo_flags)
for f in $FLAGS; do
	case "$f" in
	sve2)		HAS_SVE2=1 ;;
	ls64|ls64_v)	HAS_LS64=1 ;;
	crc32)		HAS_CRC32=1 ;;
	esac
done

#  Node list for the CP1/CP0 split (scan targets one NUMA node at a time
#  when no explicit cpu list is given and the machine has 2 nodes)
NODE_LIST=$(awk '/^NUMA node[0-9]+ CPUs/{print $4}' \
	<(lscpu 2>/dev/null) 2>/dev/null | tr '\n' ',' | sed 's/,$//')

echo "=== CPU topology ==="
echo "logical CPUs      : $N_LOGICAL (online: $online_list)"
echo "physical cores    : $N_PHYSICAL (SMT: $([ $SMT -eq 1 ] && echo yes || echo no))"
[ -n "$isolated_list" ] && echo "isolated CPUs     : $isolated_list (cannot be scanned)"
[ -n "$offline_list" ] && echo "offline CPUs      : $offline_list"
echo "features          : sve2=$HAS_SVE2 ls64=$HAS_LS64 crc32=$HAS_CRC32"
echo

#  ---------------------------------------------------------------------------
#  Worker-count derivation
#
#  full:  --taskset physical selects one thread per physical core, so
#         N_PHYSICAL cpu workers + N_PHYSICAL fma workers = 2 workers per
#         physical core, i.e. both SMT siblings of every core are busy
#         (machine-wide concurrency is the SDC trigger condition) while
#         per-core metrics stay clean for outlier detection.
#  path:  quarter of the logical CPUs by default, enough for concurrency
#         without saturating the machine while SDCShield runs alongside.
#  ---------------------------------------------------------------------------
W_FULL=$((N_PHYSICAL * 2))		# workers for stage 1 (cpu + fma pairs)
W_PATH=$((N_LOGICAL / 4))
[ "$W_PATH" -lt 1 ] && W_PATH=1

TS="sdc_$(date +%Y%m%d_%H%M%S)"

#  ---------------------------------------------------------------------------
#  Mode: full - all-cores background load (stage 1, trigger)
#  ---------------------------------------------------------------------------
run_full()
{
	local out=${OUT:-sdc_full_${TS}}
	local dur=${DUR:-7200}
	local rc=0
	mkdir -p "$out"

	echo "=== mode full: $N_PHYSICAL cpu + $N_PHYSICAL fma workers on --taskset physical, ${dur}s ==="
	echo "                (2 workers per physical core = both SMT siblings busy)"
	[ -n "$SDCSHIELD_ARG" ] && echo "                SDCShield: $SDCSHIELD_ARG"

	#  Optional preheat stage: a pure heat load before the verify
	#  window.  Residual heat is an empirically observed SDC trigger
	#  (the failing testcase only failed when run right after the
	#  heat-generating one), so the verify window opens on a hot
	#  machine by design.
	if [ "$PREHEAT" -gt 0 ]; then
		echo "=== preheat: $PREHEAT min all-core fma+vecfp heat load (no verify) ==="
		"$NG" --fma "$N_PHYSICAL" --taskset physical \
		      --vecfp "$N_PHYSICAL" \
		      -t $((PREHEAT * 60))s > "$out/preheat.log" 2>&1
		echo "    preheat done (rc=$?), verify window starts hot"
	fi

	{
		echo "mode=full duration=${dur}s physical=$N_PHYSICAL logical=$N_LOGICAL smt=$SMT"
		echo "sve2=$HAS_SVE2 ls64=$HAS_LS64 crc32=$HAS_CRC32"
		echo "isolated=$isolated_list offline=$offline_list"
	} > "$out/topology.txt"
	cp /proc/interrupts "$out/interrupts-before.txt" 2>/dev/null || true

	"$NG" --cpu "$N_PHYSICAL" --taskset physical --cpu-method all \
	      --fma "$N_PHYSICAL" --verify \
	      --varyload 64 --varyload-ms 20 \
	      --interrupts -K --thermalstat 30 \
	      --metrics-brief -Y "$out/A_full.yaml" -t "${dur}s" \
	      > "$out/A_full.log" 2>&1 &
	local ng_pid=$!
	echo "stress-ng pid $ng_pid, log $out/A_full.log"

	#  Optional SDCShield golden cross-check in the foreground
	if [ -n "$SDCSHIELD_ARG" ]; then
		# shellcheck disable=SC2086
		timeout --signal=TERM --kill-after=30 $((dur + 120)) \
			$SDCSHIELD_ARG -T forever -t "${dur}s" -Y -F \
			-e 'zstd19' -e 'zlib*' -e 'fma*' -e 'crc32' -e 'isal_crc*' \
			> "$out/sdcshield.log" 2>&1
		rc=$?
		echo "SDCShield exit: $rc"
		kill "$ng_pid" 2>/dev/null || true
		wait "$ng_pid" 2>/dev/null || true
	else
		wait "$ng_pid"
		rc=$?
	fi

	cp /proc/interrupts "$out/interrupts-after.txt" 2>/dev/null || true
	echo "=== full done (rc=$rc), results in $out ==="
	grep -E "failed: [1-9]|data difference|mismatch" "$out/A_full.log" | head -5
	return $rc
}

#  ---------------------------------------------------------------------------
#  Mode: scan - per-physical-core sweep (stage 2, localise)
#  ---------------------------------------------------------------------------
run_scan()
{
	local out=${OUT:-sdc_scan_${TS}}
	local per_core=${DUR:-120}
	local scan_args=()

	mkdir -p "$out"
	scan_args+=(--cpus "${CPU_LIST:-$online_list}")
	scan_args+=(--secs "$per_core")
	scan_args+=(--out "$out")
	[ -n "$SDCSHIELD_ARG" ] && scan_args+=($SDCSHIELD_ARG)

	#  Optional background load on the non-scanned cores: single-core
	#  isolation rarely triggers SDC, so keep the machine-wide
	#  concurrency alive while sweeping.
	local bg_pid=0
	if [ "$KEEP_BG" -gt 0 ]; then
		echo "=== mode scan: per-core ${per_core}s, background load $KEEP_BG workers on non-scanned cores ==="
		#  Non-scanned cores = all online minus the sweep targets.
		#  sdc-scan.sh pins each pass to one sibling pair; the background
		#  load uses --taskset physical so it spreads one worker per
		#  physical core and the scheduler keeps it off the busy pair.
		"$NG" --cpu "$KEEP_BG" --taskset physical --cpu-method matrixprod \
			-t $((N_PHYSICAL * per_core + 600))s \
			> "$out/bg-load.log" 2>&1 &
		bg_pid=$!
		echo "background load pid $bg_pid ($KEEP_BG workers, log $out/bg-load.log)"
	else
		echo "=== mode scan: per-core ${per_core}s, no background load (single-core isolation rarely triggers; consider --keep-bg) ==="
	fi

	#  sdc-scan.sh exits 2 when suspect cores are found
	NG="$NG" "$SCRIPT_DIR/sdc-scan.sh" "${scan_args[@]}"
	local rc=$?

	if [ "$bg_pid" -ne 0 ]; then
		kill "$bg_pid" 2>/dev/null || true
		wait "$bg_pid" 2>/dev/null || true
	fi
	echo "=== scan done (rc=$rc), results in $out ==="
	if [ $rc -eq 2 ]; then
		echo "SUSPECT CORES FOUND:"
		cat "$out/suspects.txt" 2>/dev/null
	fi
	return $rc
}

#  ---------------------------------------------------------------------------
#  Mode: path - datapath-specific golden cross checks (stage 3, attribute)
#  ---------------------------------------------------------------------------
run_path()
{
	local out=${OUT:-sdc_path_${TS}}
	local dur=${DUR:-600}
	local tsarg=""

	mkdir -p "$out"
	#  Restrict to a CPU list (e.g. the suspect cores from scan mode)
	if [ -n "$CPU_LIST" ]; then
		tsarg="--taskset $CPU_LIST"
		echo "=== mode path: ${dur}s on CPUs $CPU_LIST ==="
	else
		echo "=== mode path: ${dur}s on all $N_LOGICAL CPUs ==="
	fi

	#  Assemble the datapath stressors that this machine actually has;
	#  sve2/ls64 stressors honestly skip themselves where the feature
	#  is absent, but skipping them explicitly keeps the report honest.
	local -a NGARGS=()
	local n=$W_PATH

	if [ "$HAS_SVE2" -eq 1 ]; then
		NGARGS+=(--sve2 "$n")
		echo "  sve2  : $n workers (SVE2 fmla/bext golden cross-check)"
	else
		echo "  sve2  : skipped, no SVE2 hardware"
	fi
	if [ "$HAS_LS64" -eq 1 ]; then
		NGARGS+=(--ls64 "$n")
		echo "  ls64  : $n workers (64-byte atomic ld64b/st64b golden cross-check)"
	else
		echo "  ls64  : skipped, no ls64 hardware"
	fi
	if [ "$HAS_CRC32" -eq 1 ]; then
		NGARGS+=(--cpu "$n" --cpu-method crc32)
		echo "  crc32 : $n workers (hardware CRC32C vs software cross-check)"
	else
		echo "  crc32 : skipped, no crc32 hardware"
	fi
	[ ${#NGARGS[@]} -eq 0 ] && { echo "error: no datapath stressors available on this machine" >&2; return 1; }

	#  shellcheck disable=SC2086
	"$NG" ${tsarg} "${NGARGS[@]}" --verify \
		--metrics-brief -Y "$out/path.yaml" -t "${dur}s" \
		> "$out/path.log" 2>&1
	local rc=$?

	echo "=== path done (rc=$rc), results in $out ==="
	grep -E "failed: [1-9]|mismatch|flipped" "$out/path.log" | head -8
	return $rc
}

#  ---------------------------------------------------------------------------
#  Mode: pair - SMT sibling contention matrix (stage 2b)
#
#  For every physical core, run the SMT sibling threads against each
#  other in 4 workload combinations and record the bogo-ops rates.
#  On non-SMT machines each "pair" degenerates to a single CPU and the
#  matrix still runs (one worker per combination, rates = single
#  thread baseline) which keeps the script testable on any machine.
#  ---------------------------------------------------------------------------
run_pair()
{
	local out=${OUT:-sdc_pair_${TS}}
	local per_pair=${DUR:-30}
	local rc=0
	mkdir -p "$out"

	#  Target cores: explicit list or every physical core
	local -a reps=()
	if [ -n "$CPU_LIST" ]; then
		for c in $(expand_list "$CPU_LIST"); do
			reps+=("$c")
		done
	else
		for rep in "${!SIBLING_PAIR[@]}"; do
			reps+=("$rep")
		done
	fi
	local n_reps=${#reps[@]}
	echo "=== mode pair: ${n_reps} physical cores x 4 combinations, ${per_pair}s each ==="
	[ $SMT -eq 0 ] && echo "                (no SMT: single worker per combination = baseline rates)"

	#  The contention matrix.  Each entry: NAME:stressor1:method1:stressor2:method2
	#  fma x fma       -> both siblings hammer the shared vector pipelines
	#  fma x cpu       -> vector vs integer dispatch port contention
	#  armcrypto x fma -> independent units, expect near-linear scaling
	#  cacheline x cacheline -> shared L1/L2 write pressure
	local -a COMBOS=(
		"fma_x_fma:fma::fma:"
		"fma_x_cpu:fma::cpu:matrixprod"
		"armcrypto_x_fma:armcrypto::fma:"
		"cacheline_x_cacheline:cacheline::cacheline:"
	)

	: > "$out/pair-matrix.txt"
	local rep combo
	for rep in "${reps[@]}"; do
		local pair="${SIBLING_PAIR[$rep]:-$rep}"
		for combo in "${COMBOS[@]}"; do
			local name="${combo%%:*}"
			local rest="${combo#*:}"
			local s1="${rest%%:*}"; rest="${rest#*:}"
			local m1="${rest%%:*}"; rest="${rest#*:}"
			local s2="${rest%%:*}"
			local m2="${rest#*:}"

			#  Build the per-sibling argument sets; on non-SMT
			#  machines both workers land on the same single CPU
			local c1 c2
			c1="${pair%%,*}"
			c2="${pair##*,}"
			[ "$c2" = "$pair" ] && c2="$c1"

			local -a A1=(--"$s1" 1 --taskset "$c1")
			[ -n "$m1" ] && A1+=(--"$s1"-method "$m1")
			local -a A2=(--"$s2" 1 --taskset "$c2")
			[ -n "$m2" ] && A2+=(--"$s2"-method "$m2")

			"$NG" "${A1[@]}" "${A2[@]}" \
				--metrics-brief -Y "$out/pair_${rep}_${name}.yaml" \
				-t "${per_pair}s" > "$out/pair_${rep}_${name}.log" 2>&1
			local prc=$?
			[ $prc -ne 0 ] && { rc=$prc; echo "  cpu $pair combo $name: rc=$prc"; }

			#  Record the bogo-ops rate lines (data rows) for the
			#  combo: lines after the metrics-brief header that
			#  start with the stressor name and carry numbers
			local ops
			ops=$(awk '/\(real time\) \(usr\+sys time\)/{seen=1; next}
				   seen && NF >= 9 {print $4 "=" $(NF-1)}
				   /^stress-ng: info/{seen=0}' \
				"$out/pair_${rep}_${name}.log" 2>/dev/null | tr '\n' ' ')
			echo "$rep ($pair) $name: $ops" >> "$out/pair-matrix.txt"
		done
		echo "  core $pair done"
	done

	echo "=== pair done (rc=$rc), matrix in $out/pair-matrix.txt ==="
	echo "    rate ratio ~0.5 = fully shared resource, ~1.0 = private"
	return $rc
}

#  ---------------------------------------------------------------------------
#  Mode: all - the full funnel, sequentially
#  ---------------------------------------------------------------------------
run_all()
{
	local out=${OUT:-sdc_all_${TS}}
	mkdir -p "$out"
	echo "=== mode all: full -> scan -> path, output root $out ==="
	OUT="$out/full"  run_full;  local rc1=$?
	OUT="$out/scan" DUR="${DUR:-120}" KEEP_BG=$((N_PHYSICAL / 2)) run_scan; local rc2=$?
	OUT="$out/path" run_path;  local rc3=$?
	echo
	echo "=== funnel summary (root $out) ==="
	echo "stage 1 full  rc=$rc1 $([ $rc1 -ne 0 ] && echo '<- verify failures present')"
	echo "stage 2 scan  rc=$rc2 $([ $rc2 -eq 2 ] && echo '<- suspect cores found')"
	echo "stage 3 path  rc=$rc3 $([ $rc3 -ne 0 ] && echo '<- datapath mismatches present')"
	#  Anything non-zero means SDC evidence was gathered; propagate
	[ $rc1 -ne 0 ] || [ $rc2 -ne 0 ] || [ $rc3 -ne 0 ]
}

case "$MODE" in
full)	run_full ;;
scan)	run_scan ;;
path)	run_path ;;
pair)	run_pair ;;
all)	run_all ;;
esac
