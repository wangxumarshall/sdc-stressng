#!/bin/bash
#
#  sdc-flip-collect.sh - bit-flip position histogram from verify failures
#  (P4 of the field-validation plan).
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or
#  modify it under the GNU License as published by the Free Software
#  Foundation; either version 2, or (at your option) any later version.
#
#  The bitgen bandwalk window parameters (6..20 bits, 5 densities)
#  are literature-derived.  This script collects the empirical
#  bit-flip position distribution a machine actually produces, so
#  the windows can be calibrated to the bit-bands that really flip
#  on that silicon.
#
#  Input: verify-failure xor masks.  The stressors that report them
#  print lines of the form:
#
#    ... xor 0x<16 hex digits> (<n> bits flipped)
#
#  (operand-var, fma, vecfp, matrix bit-level diagnostics).
#  Sources, in order of preference:
#    1. an sdc-run output directory (A_full.log) or any stress-ng log
#    2. stdin
#
#  Output: per-bit flip counts across all xor masks (64 rows) plus
#  the band-width calibration suggestion (bit-bands sorted by flip
#  density, top-8 windows).
#
#  Usage:
#	scripts/sdc-flip-collect.sh <rundir-or-logfile> [more...]
#	cat mismatch.log | scripts/sdc-flip-collect.sh
#

set -u

collect()
{
	local src="$1"

	if [ -d "$src" ]; then
		cat "$src"/A_full.log 2>/dev/null
	elif [ -f "$src" ]; then
		cat "$src"
	else
		echo "no such file or directory: $src" >&2
	fi
}

#  Gather all xor masks from the inputs
masks=$(for src in "${@:-}"; do collect "$src"; done |
	grep -oE 'xor 0x[0-9a-f]{16}' |
	sed 's/xor 0x//')

[ -n "$masks" ] || { echo "no xor masks found - nothing to calibrate on" >&2; exit 1; }

n_masks=$(wc -l <<< "$masks")
echo "=== bit-flip position histogram ==="
echo "xor masks collected: $n_masks"
echo
echo "bit     flips    %ofmasks   band"
echo "---------------------------------"

#  Per-bit flip count, grouped into 8-bit bands
awk '{
	v = strtonum("0x" $1)
	for (b = 0; b < 64; b++)
		if (and(v, lshift(1, b)))
			flip[b]++
}
END {
	for (band = 0; band < 8; band++) {
		band_total = 0
		for (b = band * 8; b < band * 8 + 8; b++)
			if (flip[b]) band_total += flip[b]
		for (b = band * 8; b < band * 8 + 8; b++) {
			f = flip[b] + 0
			printf "%2d      %6d    %6.2f%%   bits %d-%d\n", \
				b, f, 100 * f / n, band * 8, band * 8 + 7
		}
	}
	n = '"$n_masks"'
}' <<< "$masks" 2>/dev/null || {
	#  fallback for awks without strtonum/and/lshift (busybox)
	awk '{
		hex = $1
		#  hex string to 64-bit value via arithmetic (2^63 ceiling safe)
		v = 0
		n = length(hex)
		for (i = 1; i <= n; i++) {
			c = substr(hex, i, 1)
			d = index("0123456789abcdef", c) - 1
			v = v * 16 + d
		}
		for (b = 0; b < 64; b++) {
			if (v % 2 == 1) flip[b]++
			v = int(v / 2)
		}
	}
	END {
		for (b = 0; b < 64; b++) {
			f = flip[b] + 0
			printf "%2d      %6d\n", b, f
		}
	}' <<< "$masks"
}

echo
echo "=== band-width calibration suggestion ==="
echo "densest 8-bit bands (candidate --bitgen-band-width windows):"
awk '{
	hex = $1
	v = 0
	n = length(hex)
	for (i = 1; i <= n; i++) {
		c = substr(hex, i, 1)
		d = index("0123456789abcdef", c) - 1
		v = v * 16 + d
	}
	for (b = 0; b < 64; b++) {
		if (v % 2 == 1) flip[b]++
		v = int(v / 2)
	}
}
END {
	for (band = 0; band < 8; band++) {
		t = 0
		for (b = band * 8; b < band * 8 + 8; b++)
			t += (flip[b] + 0)
		printf "%d %d\n", t, band * 8
	}
}' <<< "$masks" | sort -rn | head -8 |
awk '{ printf "  bits %2d-%2d: %d flips\n", $2, $2 + 7, $1 }'

echo
echo "apply with: --bitgen-band-width <min*100+max>  e.g. 812 for 8..12"
echo "            --bitgen-band-density <bitmask>    e.g. 28 for densities 2,3,4"
echo "insufficient samples (< ~100 masks): keep the literature defaults"
