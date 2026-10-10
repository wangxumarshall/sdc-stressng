# sdc-stressng

[![CI](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml/badge.svg)](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml)
[![Release](https://img.shields.io/github/v/release/wangxumarshall/sdc-stressng)](https://github.com/wangxumarshall/sdc-stressng/releases)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](COPYING)
[![Arch](https://img.shields.io/badge/arch-arm64%20%28aarch64%29-orange.svg)](https://github.com/wangxumarshall/sdc-stressng)

**English | [简体中文](README.zh-CN.md)**

> **SDC excitation engine for arm64 servers.**
> A [stress-ng](https://github.com/ColinIanKing/stress-ng) fork with a single mission:
> spend every CPU cycle maximizing the probability of exciting **silent data
> corruption (SDC)** out of marginal cores. Detection is not this tool's job —
> it belongs to [SDCShield](#division-of-labour), the companion golden-reference
> detector. This repo is the excitor.

## The problem

Arm64 server CPUs ship without core-level lockstep, and the RAS subsystem only
covers error paths that actually raise a flag. A compute unit that flips a bit
without flagging it produces *silent* data corruption: by the time corrupted
values reach storage or propagate through dependent computation, isolating the
originating core is a forensic problem. Real fleets have observed this — cores
that only fail self-tests after other cores ran hot, machines where a core had
to be deconfigured by firmware.

On hardware without redundancy, the only viable strategy is:

> **software excitation** (this tool) **+ golden-reference detection** (SDCShield).

## Division of labour

| Tool | Role |
|---|---|
| **SDCShield** (companion) | The detector: 273 golden-reference compute cases, decides *whether* an SDC occurred, reports the failing CPU (cpu-mask). |
| **sdc-stressng** (this repo) | The excitor: every cycle goes into load that maximizes SDC excitation probability — di/dt transients, residual heat, cache/TLB/interconnect pressure, SMT contention, boundary timing, SDC-directed data shapes. Its own `--verify` is a ride-along sentinel, never a second detector. |

## Quick start

Three steps, each verified on a fresh clone. `NG` is the path to your
`stress-ng` binary; every `sdc-run` command derives worker counts, SMT pairs
and hardware features from the live topology — the same command runs unchanged
on a Kunpeng 920 (128 CPUs, no SMT) and a Kunpeng 950 (382 CPUs, SMT2).

### 1 · Build (~1 minute)

```bash
git clone https://github.com/wangxumarshall/sdc-stressng
cd sdc-stressng
make clean && make -j$(nproc)     # SVE2 toolchain + HW auto-detected
NG=$PWD/stress-ng
```

On aarch64 the build auto-injects the SVE2 march only when both the toolchain
and the build host support it; `make MARCH_AARCH64_SVE2=1|0` forces either
way. The SVE-codegen acceptance gate is an objdump check (`z`-register
instructions > 0), never faith in the autovectorizer.

### 2 · Smoke self-checks (~2 minutes)

```bash
$NG --operand-var 4 --verify -t 60   # SDC-directed operand mutation
$NG --addrspace 2 --verify -t 60     # address-shape excitation
$NG --llccross 2 --verify -t 60      # cross-domain coherence ping-pong
```

Each exits 0 with `failed: 0` on a healthy machine; mismatches are reported
with bit-level diagnostics (address, expected/actual, flip count, xor mask).
Stressors whose hardware is absent skip honestly with a reason — never a fake
run.

### 3 · The excitation campaigns (`sdc-run`)

`sdc-run.sh` is the orchestrator: one command per campaign, one output
directory per run, topology-derived everywhere. Pick the campaign that matches
your goal:

```bash
# A · Pure excitation — every cycle to load, detection delegated to SDCShield
NG=$NG ./scripts/sdc-run.sh excite -t 7200 --preheat 10 \
    --sdcshield "./run-sdcshield.sh"

# B · Trigger + verify sentinels — the tool's own --verify rides along
NG=$NG ./scripts/sdc-run.sh full -t 7200 --preheat 10

# C · Localise — sweep every physical core with background load kept alive
NG=$NG ./scripts/sdc-run.sh scan -t 120 --keep-bg 64

# D · Attribute — datapath golden cross-checks on suspect cores
NG=$NG ./scripts/sdc-run.sh path -t 600 -c 192-381

# E · SMT contention matrix — map which core resources are shared
NG=$NG ./scripts/sdc-run.sh pair -t 30

# F · A/B regression — did this build change excitation/detection power?
NG_A=/tmp/stress-ng-old NG_B=$NG ./scripts/sdc-run.sh abtest -t 7200

# The whole funnel in order: full → scan → path
NG=$NG ./scripts/sdc-run.sh all
```

How to read the results:

| Campaign | rc=0 means | Evidence of interest |
|---|---|---|
| `excite` | excitation completed (**not** "machine healthy") | SDCShield's cpu-mask in `sdcshield.log` |
| `full` | no verify mismatch | any `verify-failures` in `report.txt`; bit-level diagnostics in the log |
| `scan` | no suspects | `suspects.txt` names cores with outlier metrics |
| `path` | no datapath mismatch | a mismatch is direct SDC evidence for that datapath |
| `pair` | matrix completed | per-combination rate ratios: ≈0.5 shared resource, ≈1.0 private |
| `abtest` | both arms completed | `ab_summary.txt` compares failure counts build-to-build |

For a staged joint campaign with SDCShield (recommended for a first
investigation), see
[docs/sdcshield-integration.md](docs/sdcshield-integration.md).

## Architecture

Five layers, each with one job. Everything the fork adds lives somewhere in
this stack (full detail: [docs/architecture.md](docs/architecture.md)):

```
┌────────────────────────────────────────────────────────────────┐
│ L5  ORCHESTRATION   sdc-run.sh: excite / full / scan / path /  │
│                     pair / abtest · topology-derived counts · │
│                     preheat · keep-bg · station rotation      │
├────────────────────────────────────────────────────────────────┤
│ L4  EXCITATION      di/dt load steps (varyload) · residual     │
│     LEVERS          heat (preheat) · SMT contention (pair) ·   │
│                     machine-wide concurrency · long soak       │
├────────────────────────────────────────────────────────────────┤
│ L3  ATTACK SURFACE  ALU/branch · OoO scheduler · vectors      │
│                     (SVE2/NEON) · crypto · atomics (LSE,      │
│                     lrcpc) · LSU (lsupress, ls64) · cache     │
│                     hierarchy · MMU/TLB · interconnect        │
├────────────────────────────────────────────────────────────────┤
│ L2  DATA SHAPING    bitgen: bit-band sweeps · edge-value       │
│                     dictionary · FP bit synthesis · complement │
│                     pairs · hamming-directed · pattern mixing  │
├────────────────────────────────────────────────────────────────┤
│ L1  PLATFORM        HWCAP/HWCAP2/3 probing · topology self-    │
│     AWARENESS       derivation · SVE2 march injection ·        │
│                     honest skipping · one binary across        │
│                     Kunpeng 920 (NEON) and 950 (SVE2)          │
└────────────────────────────────────────────────────────────────┘
```

## Excitation levers

Methodology distilled from field evidence and the SDC literature
(full matrix: [docs/excitation-guide.md](docs/excitation-guide.md)):

| Lever | Why it works | Entry point |
|---|---|---|
| Machine-wide concurrency | single-core isolation rarely triggers; full-machine concurrency is the observed trigger condition | `sdc-run full` / `excite` |
| Residual heat + ordering | failing cases only failed when run after heat-generating ones | `--preheat MINS` |
| di/dt load steps | load transients modulate the current slew rate | `--varyload N --varyload-ms` (standing background in `excite`) |
| SMT sibling contention | sibling threads fight over shared execution resources | `sdc-run pair` |
| SDC-directed data shapes | band sweeps and boundary dictionaries hit bit-segment-sensitive defects >10⁶× more often than uniform random | `--operand-var`, bitgen consumers |
| Address-space shaping | huge random spans, per-VA-bit walks, guard holes, mixed page orders stress MMU/TLB paths linear addressing never touches | `--addrspace`, `--lsupress` VA walk |
| OoO scheduler pressure | dependency chains and rename bursts drive the reorder buffer / rename unit into boundary states | `--ooopress` |
| Long soak | SDC rates as low as 0.01 events/minute demand hours of non-repeating patterns | `-t 2h` and beyond |

## Hardware attack surface

| Pathway | Stressors |
|---|---|
| Vector pipelines | `--sve2` (fmla/gather/fcmla/bfdot/bitperm, golden cross-check), `--fma` (runtime-dispatched SVE2 kernels), `--vecfp` |
| Crypto engines | `--armcrypto` — 13 methods: NEON AES/SHA1/SHA256/SHA512/SHA3/PMULL/SM3/SM4 + SVE2 crypto via `.inst` encodings (GCC has no intrinsics for these) |
| Load/store unit | `--lsupress` — 22 methods across int/FP/NEON/SVE/atomics over a per-worker 100GB MAP_NORESERVE VA map with a migrating working window (MADV_DONTNEED keeps the physical footprint pinned; four walk modes including bitgen TLB-tag bit-band sweeps); `--memcpy-method ldp-stp/neon/neon-ld2/sve/sve-gather/ls64` instruction-variant copy engines; `--ls64`, `--misaligned`, cache maintenance ops (`--cache-flush`/`--cache-clwb` = DC CIVAC/CVAC, `--memrate-method write64zva` = DC ZVA) |
| OoO scheduler | `--ooopress` — dep-chain (256-step serial chains), indep-max, alt drain-refill waveform, rename-reuse, branch-mix (bitgen-shaped directions, divider arm blocks if-conversion), load-use (head-of-line blocking); 42× measurable bogo-ops spread between shapes |
| Interconnect / L3 | `--llccross` — cross-domain coherence: shared-line ping-pong with per-turn tag sentinels, remote-write streams, remote mixed scans; pairs workers across NUMA nodes (or L3 instances within a node) |
| Cache hierarchy | `--cacheline` (rand-payload with ownership tags), `--l1cache` (set/way geometry + bitgen streams), `--cache`, `--memrate` (4 write patterns) |
| MMU / TLB | `--addrspace` — 7 address-shape recipes, all verified |
| Atomics | `--atomic` with RMW operand jitter; lsupress `excl-pair` (ldxr/stxr), `lse-rmw` (ldadd), `lrcpc-pair`/`ilrcpc-rmw` (LDAPR/STLR acquire-release, HWCAP-gated) |
| Integer ALU | `--cpu` with 71 methods, seed-unlocked so operands vary across runs |
| Virtual memory | `--vm --vm-method rand-offset` (Fisher-Yates dense random offsets, bitgen payloads) |
| Randomness / counters | `--rdrand` (RNDR), `--tsc` (CNTVCT_EL0) |
| Power telemetry | `--rapl` (arm64 hwmon: SoC/DDR rails) |

All SVE2/ls64/RNDR/lrcpc features are HWCAP-gated at runtime with
target-attribute compile isolation — one binary runs honestly (or skips
honestly) on any aarch64 host.

## The sdc-run orchestrator

Everything the campaigns do, in one table (`excite` stations rotate by default
every 10 minutes — `EXCITE_SLICE` env to change):

| Mode | Purpose |
|---|---|
| `excite` | **Pure excitation, station rotation** (2.0): deep pressure via time-slice stations (cpu → fma → armcrypto → lsupress → llccross → operand-var → ooopress → memcpy variants → addrspace → memrate), each getting the whole machine at `N_PHYSICAL` workers, with varyload di/dt as the standing background. Detection fully delegated to SDCShield (`--sdcshield`). |
| `full` | Stage 1 *trigger*: all-cores load (cpu + fma + operand-var + addrspace, verify sentinels on) + varyload di/dt steps + optional `--preheat` and `--sdcshield` |
| `scan` | Stage 2 *localise*: sweep every physical core (SMT pairs), per-core yaml metrics, suspects list; `--keep-bg N` keeps machine-wide concurrency alive while sweeping |
| `path` | Stage 3 *attribute*: datapath golden cross-checks (sve2 / ls64 / crc32) — a mismatch is direct SDC evidence for that datapath |
| `pair` | SMT contention matrix: 8 workload combinations per physical core (fma×fma, fma×cpu, armcrypto×fma, cacheline×cacheline, vm×vm, addrspace×addrspace, atomic×atomic, lsupress×lsupress — execute units, caches, MMU/TLB, atomics/LSU); bogo-ops ratios map the sharing topology (≈0.5 shared, ≈1.0 private). Runtime = cores × 8 × secs-per-pair; use `-c` to sample cores on large machines |
| `abtest` | A/B regression between two stress-ng builds (`NG_A=`/`NG_B=`), cooldown-separated, with side-by-side failure-count interpretation |
| `all` | full → scan → path, sequentially |

Verify failures are reported with bit-level diagnostics (element index,
expected/actual, flip count, xor mask) and aggregated into a diffable
`report.txt` per run directory.

## Continuous integration

Daily, on 15 openEuler arm64 container images (20.03 / 22.03 / 24.03 × 5 SPs):

- full `--sequential --verify` suite (330+ stressors per image)
- method sweeps covering every `*-method` option
- benchmark sampling (cpu-matrixprod / fma / memcpy / memrate-zva / stream)
- a complete stressor × image result matrix (bogo-ops/s) published to the job
  summary — including honest-skip reasons per image

Container images: `ghcr.io/wangxumarshall/sdc-stressng:verify-<git-tag>`
(published via the manual `publish_image` dispatch of the CI workflow).

## Release packages

Every published [Release](https://github.com/wangxumarshall/sdc-stressng/releases)
carries **self-contained per-image packages**: each of the 15 openEuler
images builds `stress-ng` in its own container during the release-triggered
CI run and attaches it as a tarball (+ sha256) containing the binary, the
`excite.sh` one-command maximum-excitation entry, the `sdc-run` orchestration
scripts and every non-glibc shared library — extract and run, no build
step, no dependency installs:

```bash
tar xzf sdc-stressng-<ver>.openeuler-<your-image>.aarch64.tar.gz
cd sdc-stressng-*/ && ./excite.sh 120     # 2h maximum excitation
```

## Building

```bash
make clean && make -j$(nproc)
```

`make clean` is mandatory after pulling (config.h is regenerated). SVE2 march
injection: auto (toolchain + hardware dual-gated) or `MARCH_AARCH64_SVE2=1|0`.
Cross-compiling, static builds, optional library dependencies and non-arm
platforms are unchanged from upstream stress-ng — see the
[upstream project](https://github.com/ColinIanKing/stress-ng) for that
documentation.

## Documentation

| Document | Content |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Five-layer architecture, layer responsibilities, design decisions |
| [docs/excitation-guide.md](docs/excitation-guide.md) | Excitation coverage matrix (levers × pathways × data shapes), methodology, gap roadmap |
| [docs/sdcshield-integration.md](docs/sdcshield-integration.md) | Joint campaign playbook: topology, staged scripts, report correlation |
| [docs/upstream-sync.md](docs/upstream-sync.md) | Upstream merge policy and conflict containment |
| [CLAUDE.md](CLAUDE.md) | Internal development guide (Chinese): verification discipline, code rules from 15 rounds of field lessons |
| `stress-ng.1` | Man page (upstream + fork options) |
| `docs/superpowers/` | Engineering research and round-by-round implementation records |

## Relationship with upstream stress-ng

This is a fork of [ColinIanKing/stress-ng](https://github.com/ColinIanKing/stress-ng)
(based on the 0.22.00 release line). Upstream releases are merged periodically
by PR; the merge policy and the measures that keep the conflict surface small
live in [docs/upstream-sync.md](docs/upstream-sync.md). All fork additions
default to off or behave identically to upstream when unused.

stress-ng is one of the most battle-tested system stressors in existence, and
this fork stands on that foundation — enormous credit to
[Colin Ian King](https://github.com/ColinIanKing) and the upstream contributors.

## Safety

> ⚠️ **This tool deliberately drives hardware beyond its normal operating
> envelope.** Excitation load is designed to compress timing margins, maximize
> power draw and di/dt, and heat the die. Run it **only on machines dedicated
> to stress testing**. Expected consequences on the target machine: sustained
> max power, thermal events, OOM under memory stressors, and — that is the
> point — possible SDC that corrupts anything running alongside. Never run it
> on a machine carrying production workloads or irreplaceable data.

See [SECURITY.md](SECURITY.md) for the security policy and reporting channels.

## License

GPL-2.0 (see [COPYING](COPYING)), inherited from stress-ng. Fork
modifications are released under the same license.
