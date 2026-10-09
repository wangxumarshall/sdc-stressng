# Changelog

Notable changes to the **sdc-stressng** fork. Upstream changes arrive via
periodic release merges (see [docs/upstream-sync.md](docs/upstream-sync.md));
upstream-only items are not itemized here — consult the upstream git history.

## [0.22.01-sdc.1] — 2026-10-08

LSU instruction-spectrum engine (same version line):

- `--memcpy-method` gains six hand-written arm64 copy kernels:
  ldp-stp (128-bit register pairs), neon (q-register), neon-ld2
  (interleaved two-register), sve (full-VL), sve-gather (contiguous copy
  plus a strided gather pass), ls64 (64-byte atomic blocks, compile-gated)
- new `--lsupress` stressor — the LSU instruction spectrum over a
  maximum-VA random-walk address engine:
  - 20 methods across int/FP/NEON/SVE/atomics, including the mixed
    dataflow templates (mix-2l-alu-1s = the "2 loads + ALU + load +
    store" exact-instruction kernel), gather, DC ZVA, exclusive pairs
    and LSE RMW
  - address engine: per-worker 100GB (default) MAP_NORESERVE map,
    migrating working window with MADV_DONTNEED (physical footprint
    pinned near the window, page-table churn by construction), four
    walk modes (uniform / bitgen TLB-tag bit-band / va-bit / near-far),
    optional 2MB/1GB hugepage maps with fallback
  - verification: store-int64 writes per-address deterministic hash
    values; --verify samples the window per page against the oracle with
    bit-level diagnostics; fault-injection drill passed (bit 45 caught)
- excite 2.0: time-slice station rotation replaces the 8-stressor stack
  (each station gets the whole machine for its slice; varyload di/dt is
  the standing background), lsupress and memcpy variants join the
  feature-gated station table

Upstream sync #1 under the per-release policy
([docs/upstream-sync.md](docs/upstream-sync.md)): merged upstream V0.22.01
(4 commits — core-cpu-cache debug-message cleanup, version bump, debian
changelog, and an upstream README note announcing the project's migration
to the stress-ng/stress-ng organization). Conflict surface exactly as
designed: the Makefile `VERSION` line only, resolved to `0.22.01-sdc.1`;
our README was auto-preserved by the `merge=ours` driver — first live
validation of the conflict-containment setup. Post-merge verification:
full rebuild clean, bitgen-consumer regression
(operand-var / addrspace / vm-rand-offset / memrate-bandwalk) all green,
and a fault-injection drill on the fma verify path (injected bit 45 caught
with full bit-level diagnostics).

Release assets: every published Release now carries **self-contained
per-image packages** — each of the 15 openEuler images builds `stress-ng`
inside its own container during the release-triggered CI run and attaches
`sdc-stressng-<ver>.openeuler-<image>.aarch64.tar.gz` (+ sha256): the
binary, the `excite.sh` one-command maximum-excitation entry, the full
sdc-run orchestration scripts, and every non-glibc shared library the
binary needs (bundled in `lib/`, auto-preferred via `LD_LIBRARY_PATH`;
the package records its minimum glibc). Extract and run — no build step,
no dependency installs. Released binaries are tested binaries.

## [0.22.00-sdc.1] — 2026-10-08

First tagged fork release. Baseline: upstream stress-ng **0.22.00**
(2026-08-19) plus one batch of upstream fixes via PR #5. Everything below was
developed across 13 engineering rounds (records in `docs/superpowers/`) and
validated on 15 openEuler CI images.

### Project identity

- Repositioned as the **SDC excitation engine** for arm64 servers — the
  complement of SDCShield (the detector). Excitation-first philosophy: every
  cycle goes to raising the probability of exciting silent data corruption.
- English front-page README (badges, five-layer architecture, quick start);
  SECURITY / CONTRIBUTING / SUPPORT policies; structured issue forms and a PR
  template carrying the verification checklist.
- Upstream leftovers removed: FUNDING.yml (upstream author's donation
  accounts), Travis config, 3 upstream CI workflows.
- Repo metadata rewritten (description, 12 topics, wiki disabled).
- Documentation library added: architecture, excitation guide
  (levers × pathways × data-shapes coverage matrix), SDCShield integration
  playbook, upstream sync policy.

### Data shaping (layer L2)

- `core-bitgen`: SDC-directed bit-pattern generator — bit-band sweeps
  (6–20-bit windows × 5 densities), 57-entry edge-value dictionary with
  jitter, FP bit synthesis (exponent/mantissa independently), complement
  pairs, hamming-directed flips, pattern mixing via `stress_bitgen_u64()`.
  Statistically validated: 100% boundary hits vs ~0 for uniform random
  (>10⁶ improvement); reproducible via `scripts/bitgen-distribution.sh`.
- `--operand-var`: 5 methods running real compute paths (ALU / multiplier /
  divider / FMA) with golden-replay comparison.
- `--addrspace`: 7 address-shape recipes (multi-GB random fixed spans,
  per-VA-bit walks, dense random offsets, guard holes, malloc-giant,
  misaligned-huge, mixed page orders), all verified.
- `--memrate-write-pattern {0xaa,random,bandwalk,complement}` replacing 11
  hardcoded constants in the memrate write path.
- `--vm-method rand-offset`: Fisher-Yates non-replacement dense random
  offsets with bitgen payloads and same-order replay verification.
- Seed-unlocked operand streams for cpu/armcrypto; FP bit synthesis in
  fma/vecfp; atomic RMW operand jitter (verify oracles stay deterministic).
- Tunable bitgen windows (`--bitgen-band-width`, `--bitgen-band-density`)
  with a calibration chain (`scripts/sdc-flip-collect.sh`).

### ARM64 attack surface (layer L3)

- `--armcrypto`: 13 methods — NEON AES/SHA1/SHA256/SHA512/SHA3/PMULL/SM3/SM4
  plus SVE2 sveaes/svepmull/svesha3/svesm4 via `.inst` numeric encodings
  (GCC has no intrinsics for them); KAT software references for aes/pmull.
- `--sve2`: 5 datapath methods (fmla/gather/fcmla/bitperm/bfdot) with golden
  comparison; `--fma` runtime-dispatches 12 SVE2 FMLA kernels.
- `--ls64` (64-byte atomics), `--rdrand` (RNDR), `--tsc` (CNTVCT_EL0),
  `--regs` (NEON v0–v31 + SVE z0–z31 full-register rotation).
- Cache maintenance: `--cache-flush` / `--cache-clwb` (DC CIVAC/CVAC),
  `--memrate-method write64zva` (DC ZVA).
- rand-payload methods for cacheline (ownership tags) and l1cache
  (set/way geometry + bitgen streams); bitgen-mutated cache write path.
- `--taskset physical`: SMT-aware affinity (first thread per physical core).
- `--rapl` / `--raplstat`: arm64 hwmon power telemetry (SoC/DDR rails).
- Build: SVE2 march auto-injection (`-O3
  -march=armv8.6-a+sve2+bf16+i8mm+sve2-bitperm`, toolchain + hardware dual
  gated); objdump z-register acceptance gate; gcc 7.3 compatibility guards.

### Orchestration and diagnostics (layers L4/L5)

- `excite` mode: pure-excitation orchestration — the widest load mix
  (cpu/fma/armcrypto/operand-var/addrspace/memrate-bandwalk/vm-rand-offset)
  with no verify sentinels; every cycle goes to excitation, detection fully
  delegated to SDCShield. `rc=0` documents completion, not health.
- `scripts/sdc-run.sh`: topology self-deriving entry point — `full` /
  `scan` / `path` / `pair` / `abtest` modes; `--preheat` residual-heat
  window before the verify window; `--keep-bg` background concurrency during
  per-core scans; SDCShield hook for parallel detection.
- Bit-level verify diagnostics (fma/vecfp/matrix): element index,
  expected/actual hex, flip count, xor mask.
- `-Y` yaml `verify-failures` field; `scripts/sdc-report.sh` diffable
  per-run mismatch reports; `abtest` A/B regression mode between two builds.
- verify fault-injection drills: every verify-capable component had a bit
  manually flipped to prove the mismatch is caught.

### CI (quality infrastructure)

- Daily 15-image openEuler arm64 matrix (20.03 / 22.03 / 24.03 × 5 SPs):
  full `--sequential --verify` suite, 62 `*-method` sweeps, benchmark
  sampling, and a complete stressor × image result matrix (bogo-ops/s with
  honest-skip reasons) in the job summary.
- `ghcr.io/wangxumarshall/sdc-stressng:verify-<tag>` image publishing via
  manual dispatch.
- The 8-round CI bring-up caught real bugs (pmull golden dispatch by index
  vs by name, HWCAP2_RNG fallback, fork-starvation-safe assertions) —
  documented as engineering records in `docs/superpowers/research/`.
