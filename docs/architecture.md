# Architecture — the five-layer SDC excitation engine

sdc-stressng is an **excitation engine**: its only job is to raise the
probability that a marginal arm64 core produces silent data corruption (SDC)
during a stress campaign. Deciding *whether* corruption happened, and *which*
core produced it, is [SDCShield](../sdcshield)'s job (golden-reference
detection). This split is the founding constraint of the architecture.

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

Layers are a *capability taxonomy*, not runtime modules: a single
`./stress-ng --operand-var 4 --verify` invocation already exercises L1
(HWCAP probing), L2 (bitgen shaping) and L3 (ALU/multiplier/divider paths);
`sdc-run.sh full` adds L4 levers and L5 reporting.

## Layer responsibilities

### L1 — Platform awareness

Everything that lets one binary run honestly on any aarch64 host:

- **Runtime feature probing**: HWCAP / HWCAP2 / HWCAP3 (with fallbacks for
  old kernels, e.g. `AT_HWCAP3` vs `HWCAP2` bit for ls64). Features gate
  stressors; absent features produce `skipped` with a reason — never a
  fake run.
- **Build-time dual gating**: the SVE2 march
  (`-O3 -march=armv8.6-a+sve2+bf16+i8mm+sve2-bitperm`) is injected only when
  *both* the toolchain and the build host support it; `make
  MARCH_AARCH64_SVE2=1|0` forces either way. The acceptance gate is an
  objdump check (z-register instructions > 0), because the autovectorizer's
  choices are not predictable and `-O2` never emits SVE.
- **Compile isolation**: SVE2/SVE/SM3/SM4 code is fenced with GCC target
  attributes (`arch=armv8.4-a+sm4` style — the full form; short forms are
  rejected by older GCC), so a single binary carries both NEON and SVE2
  paths and dispatches at runtime.
- **Topology self-derivation** (`sdc-run.sh` stage 0): online/isolated/
  offline CPU lists, SMT sibling pairs, NUMA node CPU sets, sve2/ls64/crc32
  feature bits — all worker counts and tasksets derive from these, so one
  command works unchanged on a 920 (128 CPUs, no SMT) and a 950
  (382 CPUs, SMT2).

### L2 — Data shaping

*The shape of randomness matters more than its presence.* Uniform random
operands sample a 52-bit mantissa space almost never landing on the boundary
values that bit-segment-sensitive defects respond to; fixed patterns sit at
the opposite extreme. The bitgen family (`core-bitgen.c`) is the middle
path — a mixed generator reused across the whole tree:

| Generator | What it produces | Why |
|---|---|---|
| bandwalk | 6–20-bit windows swept across the u64, 5 densities | targets defects sensitive to specific bit segments |
| edge dictionary | 57 boundary values (0x00, 0x7f, 0x80, 0xff, … ± jitter) | boundary conditions of encodings/representations |
| FP bit synthesis | exponent and mantissa fields composed independently | hits subnormal/NaN/inf edges and mantissa-carry corners |
| complement pairs | x and ~x in sequence | differential paths that flip on complement |
| hamming-directed | values at controlled Hamming distances | defects that respond to multi-bit proximity |

`stress_bitgen_u64()` mixes these streams. Two disciplines keep it safe:

- **Deterministic oracles**: randomization only ever enters pressure paths;
  verify oracles stay identity/literal based (e.g. atomic's `~tmp-1 == tmp`).
- **Fill/verify stream sync**: wherever data is written then verified, the
  same bitgen state is replayed (dual seeded copies, Fisher-Yates
  non-replacement sampling) so verification compares against the intended
  stream, not a re-roll. `stress_bitgen_skip()` preserves 8-byte alignment.

Statistical validation lives in `scripts/bitgen-distribution.sh`;
real-machine calibration (which bit bands actually flip) in
`scripts/sdc-flip-collect.sh`.

### L3 — Attack surface

The hardware pathways the engine can drive, and the stressors that drive
them — see the README "Hardware attack surface" table and the full coverage
matrix in [excitation-guide.md](excitation-guide.md). Notable engineering
choices:

- SVE2 crypto reaches hardware GCC cannot target with intrinsics by using
  `.inst` numeric encodings (binutils mnemonic syntax has its own
  compatibility gaps).
- The cache write path (`--cache`, `--cacheline`, `--l1cache`) consumes
  bitgen streams so even "dumb" copy loops carry SDC-directed payloads;
  cacheline's rand-payload method adds per-word ownership tags so
  cross-worker aliasing is distinguishable from corruption.
- Pathway stressors ship golden cross-checks (sve2/ls64/crc32 hw-vs-sw),
  because a datapath mismatch is *direct* SDC evidence for that pathway —
  useful even though detection is primarily SDCShield's job.

### L4 — Excitation levers

Field- and literature-derived load conditions that raise SDC excitation
probability (each entry in [excitation-guide.md](excitation-guide.md) cites
its evidence):

- **Machine-wide concurrency** — single-core isolation rarely triggers;
  full-machine concurrency is the observed trigger condition.
- **Residual heat + ordering** — failing cases only failed after
  heat-generating ones; `--preheat` opens the verify window on a hot machine.
- **di/dt load steps** — `--varyload` 6 waveforms × `--cpu-load-slice`
  fine-grained duty cycling modulate current slew.
- **SMT sibling contention** — `sdc-run pair` runs the sibling threads in 4
  workload combinations; bogo-ops ratios map which resources are shared
  (≈0.5) vs private (≈1.0).
- **Long soak** — event rates as low as 0.01/min demand hours of
  non-repeating shaped load.

### L5 — Orchestration

`sdc-run.sh` turns the layers into campaigns: `full` (trigger) → `scan`
(localise) → `path` (attribute), plus `pair` (SMT topology mapping) and
`abtest` (A/B regression between two builds). It owns preheat/keep-bg
scheduling, SDCShield process coupling, per-run `report.txt` (diffable
across runs), and the suspects list. `sdc-report.sh` aggregates
`verify-failures` yaml counts, first-mismatch positions and SDCShield
summaries.

## Design decisions

| # | Decision | Rationale |
|---|---|---|
| AD-1 | Excitation, not detection | SDCShield owns detection (273 golden cases, cpu-mask). Duplicating it here burns cycles that should excite. Fork `--verify` is a ride-along sentinel. |
| AD-2 | One binary, runtime dispatch | Single artifact across Kunpeng 920 (NEON-only) and 950 (SVE2) keeps field logistics simple; HWCAP gates + target-attribute isolation make it safe. |
| AD-3 | Deterministic oracles | A randomized oracle destroys the verification baseline — the one rule that survives every change. |
| AD-4 | Honest skipping | A fake run is worse than no run: it produces green noise on the exact machines being investigated. |
| AD-5 | Mixed-shape generators, not uniform random | >10⁶ boundary-hit improvement over uniform sampling, demonstrated statistically; fixed patterns fail the opposite way. |
| AD-6 | Topology self-derivation | Campaigns must run unmodified on any target; hard-coded worker counts are the first thing that rots. |
| AD-7 | Conflict containment with upstream | Fork value concentrates in new files; shared-file edits stay in marked regions; README is fork-owned (merge=ours). See [upstream-sync.md](upstream-sync.md). |

## Data flow of a verify-capable stressor

```
        pressure path                    oracle path
  ┌────────────────────┐          ┌────────────────────┐
  │ bitgen stream ─────┼──► compute (ALU/VEC/LSU/…)   │
  │ (shaped operands)  │          │                    │
  └────────────────────┘          │  golden reference  │
                                  │  (deterministic:   │
  fill phase: write stream  ─────►│   replay / identity│
  verify phase: replay same ─────►│   / hw-vs-sw)      │
  stream, compare                 └─────────┬──────────┘
                                            │ mismatch →
                                  element index · expected/actual ·
                                  flip count · xor mask → verify-failures
```

The pressure path carries all the randomness; the oracle path carries none.
When SDCShield runs alongside, its golden cases are the primary detector and
this machinery is defense in depth.
