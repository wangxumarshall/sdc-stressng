# sdc-stressng

[![CI](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml/badge.svg)](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml)
[![Release](https://img.shields.io/github/v/release/wangxumarshall/sdc-stressng)](https://github.com/wangxumarshall/sdc-stressng/releases)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](COPYING)
[![Arch](https://img.shields.io/badge/arch-arm64%20%28aarch64%29-orange.svg)](https://github.com/wangxumarshall/sdc-stressng)

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

## Architecture

Five layers, each with one job. Everything the fork adds lives somewhere in
this stack (full detail: [docs/architecture.md](docs/architecture.md)):

```
┌────────────────────────────────────────────────────────────────┐
│ L5  ORCHESTRATION   sdc-run.sh: full / scan / path / pair /     │
│                     abtest · topology-derived worker counts ·  │
│                     preheat · keep-bg · reports                 │
├────────────────────────────────────────────────────────────────┤
│ L4  EXCITATION      di/dt load steps (varyload) · residual     │
│     LEVERS          heat (preheat) · SMT contention (pair) ·   │
│                     machine-wide concurrency · long soak       │
├────────────────────────────────────────────────────────────────┤
│ L3  ATTACK SURFACE  ALU/branch · vectors (SVE2/NEON) · crypto  │
│                     (AES/SHA/SM3/SM4) · atomics (LSE) · LSU    │
│                     (ls64, misalign) · cache hierarchy ·       │
│                     MMU/TLB (addrspace) · NUMA interconnect    │
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
| Machine-wide concurrency | single-core isolation rarely triggers; full-machine concurrency is the observed trigger condition | `sdc-run full` |
| Residual heat + ordering | failing cases only failed when run after heat-generating ones | `--preheat MINS` |
| di/dt load steps | 6 waveforms × fine-grained load slices modulate the current slew rate | `--varyload N --varyload-ms` |
| SMT sibling contention | both threads of a core fight over shared execution resources | `sdc-run pair` |
| SDC-directed data shapes | band sweeps and boundary dictionaries hit bit-segment-sensitive defects >10⁶× more often than uniform random | `--operand-var`, bitgen everywhere |
| Address-space shaping | huge random spans, per-VA-bit walks, guard holes, mixed page orders stress MMU/TLB paths linear addressing never touches | `--addrspace` |
| Long soak | SDC rates as low as 0.01 events/minute demand hours of non-repeating patterns | `-t 2h` and beyond |

## Hardware attack surface

| Pathway | Stressors |
|---|---|
| Vector pipelines | `--sve2` (fmla/gather/fcmla/bfdot/bitperm, golden cross-check), `--fma` (runtime-dispatched SVE2 kernels), `--vecfp` |
| Crypto engines | `--armcrypto` — 13 methods: NEON AES/SHA1/SHA256/SHA512/SHA3/PMULL/SM3/SM4 + SVE2 crypto via `.inst` encodings (GCC has no intrinsics for these) |
| Load/store unit | `--ls64` (64-byte atomic ld64b/st64b), `--misaligned`, cache maintenance ops (`--cache-flush`/`--cache-clwb` = DC CIVAC/CVAC, `--memrate-method write64zva` = DC ZVA) |
| Cache hierarchy | `--cacheline` (rand-payload with ownership tags), `--l1cache` (set/way geometry + bitgen streams), `--cache`, `--memrate` (4 write patterns) |
| MMU / TLB | `--addrspace` — 7 address-shape recipes, all verified |
| Atomics | `--atomic` with RMW operand jitter (verify oracle stays deterministic) |
| Integer ALU | `--cpu` with 71 methods, seed-unlocked so operands vary across runs |
| Virtual memory | `--vm --vm-method rand-offset` (Fisher-Yates dense random offsets, bitgen payloads) |
| Randomness / counters | `--rdrand` (RNDR), `--tsc` (CNTVCT_EL0) |
| Power telemetry | `--rapl` (arm64 hwmon: SoC/DDR rails) |

All SVE2/ls64/RNDR features are HWCAP-gated at runtime with target-attribute
compile isolation — one binary runs honestly (or skips honestly) on any
aarch64 host.

## Quick start

```bash
git clone https://github.com/wangxumarshall/sdc-stressng
cd sdc-stressng
make clean && make -j$(nproc)        # aarch64: SVE2 toolchain + HW auto-detected

# SDC-directed self-checks (each takes ~1 minute)
./stress-ng --operand-var 4 --verify -t 60
./stress-ng --addrspace 2 --verify -t 60

# The full diagnostic funnel (trigger → localise → attribute)
NG=./stress-ng ./scripts/sdc-run.sh all
```

For a pure excitation run with detection delegated entirely to SDCShield
running alongside, use `sdc-run.sh full --sdcshield "<SDCShield command>"`.

## The sdc-run orchestrator

`scripts/sdc-run.sh` derives everything (worker counts, SMT sibling pairs,
hardware features) from the live topology, so the same command works unchanged
on a Kunpeng 920 (128 CPUs, no SMT) and a Kunpeng 950 (382 CPUs, SMT2):

| Mode | Purpose |
|---|---|
| `full` | Stage 1 *trigger*: all-cores load (cpu + fma + operand-var + addrspace, verify sentinels on) + varyload di/dt steps + optional `--preheat` and `--sdcshield` |
| `scan` | Stage 2 *localise*: sweep every physical core (SMT pairs), per-core yaml metrics, suspects list; `--keep-bg N` keeps machine-wide concurrency alive while sweeping |
| `path` | Stage 3 *attribute*: datapath golden cross-checks (sve2 / ls64 / crc32) — a mismatch is direct SDC evidence for that datapath |
| `pair` | SMT contention matrix: 4 workload combinations per physical core; bogo-ops ratios map the sharing topology (≈0.5 shared, ≈1.0 private) |
| `abtest` | A/B regression between two stress-ng builds (`NG_A=`/`NG_B=`), cooldown-separated, with side-by-side failure-count interpretation |
| `all` | full → scan → path, sequentially |

Verify failures are reported with bit-level diagnostics (element index,
expected/actual, flip count, xor mask) and aggregated into a diffable
`report.txt` per run directory.

## Continuous integration

Daily, on 15 openEuler arm64 container images (20.03 / 22.03 / 24.03 × 5 SPs):

- full `--sequential --verify` suite (330+ stressors per image)
- 62 `*-method all` sweeps
- benchmark sampling (cpu-matrixprod / fma / memcpy / memrate-zva / stream)
- a complete stressor × image result matrix (bogo-ops/s) published to the job
  summary — including honest-skip reasons per image

Container images: `ghcr.io/wangxumarshall/sdc-stressng:verify-<git-tag>`
(published via the manual `publish_image` dispatch of the CI workflow).

## Building

```bash
make clean && make -j$(nproc)
```

On aarch64 the build auto-detects an SVE2-capable toolchain *and* SVE2
hardware; only when both are present it injects
`-O3 -march=armv8.6-a+sve2+bf16+i8mm+sve2-bitperm` (SVE codegen needs `-O3`).
Force either way with `make MARCH_AARCH64_SVE2=1|0`. The acceptance gate for
SVE codegen is an objdump check (`z`-register instructions > 0), not faith in
the autovectorizer.

Cross-compiling, static builds, optional library dependencies and non-arm
platforms are unchanged from upstream stress-ng — see the
[upstream project](https://github.com/ColinIanKing/stress-ng) for that
documentation.

## Documentation

| Document | Content |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Five-layer architecture, layer responsibilities, design decisions |
| [docs/excitation-guide.md](docs/excitation-guide.md) | Excitation coverage matrix (levers × pathways × data shapes), methodology, references |
| [docs/sdcshield-integration.md](docs/sdcshield-integration.md) | Joint campaign playbook: topology, staged scripts, report correlation |
| [docs/upstream-sync.md](docs/upstream-sync.md) | Upstream merge policy and conflict containment |
| [CLAUDE.md](CLAUDE.md) | Internal development guide (Chinese): verification discipline, 10 code rules from 12 rounds of field lessons |
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
