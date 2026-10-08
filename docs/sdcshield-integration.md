# Running joint campaigns with SDCShield

SDCShield is the detector; this repo is the excitor. A campaign runs both on
the same machine: sdc-stressng compresses timing margins and shapes load,
SDCShield's 273 golden-reference cases decide whether corruption occurred
and on which CPU.

```
┌──────────────────────── target machine (arm64 server) ───────────────────────┐
│                                                                              │
│   sdc-stressng (excitor)                     SDCShield (detector)            │
│   ┌───────────────────────────┐              ┌────────────────────────────┐  │
│   │ sdc-run.sh full/excite    │              │ 273 golden-reference cases │  │
│   │  · all-cores shaped load  │   same HW    │ · verdict: SDC yes/no      │  │
│   │  · varyload di/dt steps   │  ────────►   │ · cpu-mask: failing CPUs   │  │
│   │  · preheat / keep-bg      │              │ · continue-on-error /      │  │
│   │  · verify sentinels (opt) │              │   first-fail-stop modes    │  │
│   └───────────┬───────────────┘              └──────────────┬─────────────┘  │
│               │ report.txt                                  │ shield log     │
│               │ verify-failures yaml                        │ mismatch rate  │
└───────────────┼──────────────────────────────────────────────┼────────────────┘
                └────────────── cross-correlate ───────────────┘
```

## Prerequisites

- This repo built on the target machine:
  `make clean && make -j$(nproc)` (SVE2 auto-detected where available).
- SDCShield cloned and its golden suite runnable (see its repository docs).

## Stage 0 — forensics before load

Record the machine's state so later anomalies are attributable:

```bash
lscpu > topology.txt
cat /sys/devices/system/cpu/{online,isolated,offline} >> topology.txt
dmesg | grep -iE 'mce|edac|hardware error|isolat' > ras-baseline.txt 2>/dev/null
ipmitool sel list > bmc-sel.txt 2>/dev/null || true
```

If cores are already isolated/deconfigured, capture why before stressing —
a core that firmware already took offline cannot be scanned.

## Stage 1 — trigger (full machine, both tools)

```bash
NG=./stress-ng ./scripts/sdc-run.sh full -t 7200 --preheat 10 \
    --sdcshield "./run-sdcshield.sh"
```

What happens: preheat (10 min all-core fma+vecfp, no verify) → the verify
window opens hot; the full-machine shaped load (cpu + fma + operand-var +
addrspace, verify sentinels on) runs alongside varyload di/dt steps, while
SDCShield's golden suite runs in the foreground for the same duration.
Both tools write their own artifacts into the run directory
(`A_full.yaml`, `A_full.log`, `sdcshield.log`, `report.txt`).

Notes:

- The sdc-run `--sdcshield` hook already passes the sane SDCShield flags for
  a parallel run (`-T forever`, same duration, continue past failures,
  excluding the few cases that duplicate this tool's own stressors).
- For a **pure excitation** run (zero cycles spent on this tool's own
  verification), use `excite` mode — see the README orchestrator table.

## Stage 2 — localise (per core, background load kept alive)

```bash
NG=./stress-ng ./scripts/sdc-run.sh scan -t 120 --keep-bg 64 \
    --sdcshield "./run-sdcshield.sh"
```

Sweeps every physical core (SMT sibling pair) while a background load keeps
the machine-wide concurrency alive — single-core isolation rarely triggers
SDC. Suspect cores land in `suspects.txt` (scan exits 2 when found).

## Stage 3 — attribute (datapath golden cross-checks)

```bash
NG=./stress-ng ./scripts/sdc-run.sh path -t 600 -c <suspect-cpus>
```

sve2 / ls64 / crc32 hardware-vs-software cross-checks pinned to the suspect
CPUs. A mismatch here is *direct* SDC evidence for that datapath on those
cores.

## A/B regression between two builds

```bash
NG_A=/path/to/old/stress-ng NG_B=./stress-ng \
NG=./stress-ng ./scripts/sdc-run.sh abtest -t 7200
```

Runs the full recipe with each binary, cooldown-separated, and produces
`ab_summary.txt` with side-by-side failure counts and the interpretation
rule (B > A means the new build excites more; B == A means consider longer
soak; B < A means check bogo-ops first — less load, not less excitation).

## Cross-correlating the reports

| sdc-stressng | SDCShield | Reading |
|---|---|---|
| `verify-failures > 0` | mismatch + cpu-mask names cores | **Strong**: independent oracles agree — treat cpu-mask as prime suspect list |
| `verify-failures > 0` | clean | Weak signal: re-run with longer soak; check this tool's own golden paths (could be an excitor bug — report it) |
| clean | mismatch + cpu-mask | Weak signal for this tool's coverage: the corrupted pathway is outside the current load recipe — consult the [excitation coverage matrix](excitation-guide.md) gaps |
| bogo-ops outlier on core N | any | Core N is marginal in *performance*, not necessarily SDC — keep it on the watch list |
| clean | clean | No evidence this run; aging-related marginality may need repeated campaigns across days |

The per-run `report.txt` is designed to be diffed across run directories
(verify failure counts per stressor, first-mismatch positions, preheat
marker, SDCShield summary).
