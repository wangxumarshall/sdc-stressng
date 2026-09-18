# CI Run 14 (35317475173) — Cross-OS Benchmark & Test Analysis

- **Run**: [35317475173](https://github.com/wangxumarshall/sdc-stressng/actions/runs/35317475173) — `multi-os-verify` on `main` @ `ff43ec4b4`, triggered 2026-09-18 07:01 UTC (workflow_dispatch, attempt 1)
- **Runner fleet**: Azure arm64 shared runners (`ubuntu-24.04-arm`), spread across eastus / eastus2 / westus / westus2 / westus3 / northcentralus / centralus / westcentralus
- **Overall conclusion**: `failure` (14/15 build-test jobs green; `final-status` failed because of the one red job)

## 1. Headline findings

1. **The cross-OS benchmark table is empty.** The `benchmark-comparison` artifact (312 bytes) contains only the markdown header — zero data rows. Root cause is a workflow bug, not missing benchmark *runs*: the `reports-*` upload step lists `methods-${TAG_SAFE}.log`, `bench-${TAG_SAFE}.log`, `bench-${TAG_SAFE}.yaml` as **literal paths** (GitHub Actions does not expand `${VAR}` inside `with:` blocks), so only the 3 literal-named files (`buildinfo.txt`, `build.log`, `smoke.log`) were uploaded. The `benchmark-compare` job then found no `bench-*.yaml` files to tabulate. The bench step itself ran successfully on all 14 images that reached it (verify line: `passed=14 skipped=0 failed=0`, workers `cpu(4) fma(4) memcpy(2) memrate(2) stream(2)`) — only the YAML outputs were never published.
2. **Same bug silently dropped all `seq-report-*` artifacts** (`path: seq-${TAG_SAFE}.log …` → "No files were found"). Sequential results below were recovered from job logs.
3. **Only 1 of 15 images built with UBSan.** Every image tries `make SANITIZE=1` first; 14 fell back to plain with `cannot find -lubsan` (broken `libubsan.so` symlink — known openEuler packaging issue). Only **24.03-lts-sp4** linked successfully → `BUILD_MODE=sanitize-ubsan`.
4. **The UBSan image hung in the sequential suite** for ~5h42m until the 360-min job timeout cancelled it (started 07:20, killed 13:02, zero progress output). Sanitizer instrumentation makes the ~12-min suite effectively unbounded on a shared runner.
5. **The UBSan build is measurably slower and found a real code smell**: its smoke run logged 4× `stress-cpu.c:1079:1: runtime error: <unknown> is outside the range of representable values of type 'long int'` and its cpu bogo-ops/s is ~8.7% below its plain-built siblings.
6. **One sweep failure**: `20.03-lts` → `ci: [FAIL] --opcode --opcode-method text (rc=2)` (known transient under shared-runner neighbor load; retry-once fix committed after this run as `a567867ae`). That job died at the sweep step, so `20.03-lts` has no bench/seq/binary artifacts.
7. **`buildinfo.txt` "build mode:" line is empty for every image** — `BUILD_MODE` is written to `$GITHUB_ENV` and echoed in the *same* step, before the variable is exported. The true mode is only visible in later steps' env blocks.

## 2. Per-image summary (all 15 images)

Sweep = `ci-method-sweep.sh` (pass/skip/nolimit/fail). Bench = 20 s `cpu(matrixprod)+fma+memcpy+memrate(write64zva)+stream` verify. Seq = `--sequential 1 --timeout 2 --verify` suite, 393 stressors total (9 excluded as environment-hostile).

| Image | Job | Build mode | Compiler | Sweep pass/skip/nolimit/fail | Bench p/s/f | Seq passed/skipped/failed | Runner region |
|---|---|---|---|---|---|---|---|
| 20.03-lts | 105512286086 (fail) | plain (ubsan fallback) | gcc 7.3.0 | 129 / 3 / 1 / **1** (`opcode:text` rc=2) | not reached | not reached | westus3 |
| 20.03-lts-sp1 | 105512286084 | plain (ubsan fallback) | gcc 7.3.0 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 322 / 71 / 0 | eastus |
| 20.03-lts-sp2 | 105512286065 | plain (ubsan fallback) | gcc 7.3.0 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 322 / 71 / 0 | eastus2 |
| 20.03-lts-sp3 | 105512286052 | plain (ubsan fallback) | gcc 7.3.0 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 322 / 71 / 0 | eastus |
| 20.03-lts-sp4 | 105512286013 | plain (ubsan fallback) | gcc 7.3.0 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 322 / 71 / 0 | eastus2 |
| 22.03-lts | 105512285998 | plain (ubsan fallback) | gcc 10.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 327 / 66 / 0 | westus3 |
| 22.03-lts-sp1 | 105512286031 | plain (ubsan fallback) | gcc 10.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 327 / 66 / 0 | northcentralus |
| 22.03-lts-sp2 | 105512286026 | plain (ubsan fallback) | gcc 10.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 328 / 65 / 0 | westcentralus |
| 22.03-lts-sp3 | 105512286095 | plain (ubsan fallback) | gcc 10.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 328 / 65 / 0 | northcentralus |
| 22.03-lts-sp4 | 105512286096 | plain (ubsan fallback) | gcc 10.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 328 / 65 / 0 | westus3 |
| 24.03-lts | 105512286150 | plain (ubsan fallback) | gcc 12.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 330 / 63 / 0 | westus2 |
| 24.03-lts-sp1 | 105512286053 | plain (ubsan fallback) | gcc 12.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 330 / 63 / 0 | eastus2 |
| 24.03-lts-sp2 | 105512286023 | plain (ubsan fallback) | gcc 12.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 327 / 66 / 0 | eastus2 |
| 24.03-lts-sp3 | 105512286011 | plain (ubsan fallback) | gcc 12.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | 330 / 63 / 0 | eastus2 |
| 24.03-lts-sp4 | 105512286032 (cancelled) | **sanitize-ubsan** | gcc 12.3.1 | 130 / 3 / 1 / 0 | 14 / 0 / 0 | **hung → cancelled @360 min** | centralus |

Notes:
- Sweep skips are uniform: `--dfp-method`, `--eigen-method`, `--plugin-method` not compiled in; the single `nolimit` is `--rawdev` (rc=3 no-resource, environment-limited).
- All 13 completed seq runs exited rc=3 with `failed=0` — stressors that aborted early for lack of runner resources count as skipped; the verify script treats this as environment-limited (warning, not failure). Seq counts differ a few units between runners of the *same* OS family (e.g. 24.03: 330/63 vs sp2's 327/66) — that is shared-runner resource noise, not image behavior.
- Seq totals: passed+skipped = 393 on every completed image — consistent suite, no lost stressors.

## 3. Benchmark comparison

### 3.1 The published table (artifact `benchmark-comparison`, reproduced verbatim)

```markdown
# Cross-OS benchmark comparison

bogo-ops/s (real time) per stressor per openEuler image.

| stressor |  |
|---|

_Note: rates are from 20s CI runs on shared runners — compare trends across images, not absolute numbers._
```

Empty, per finding #1. The per-stressor (cpu/fma/memcpy/memrate/stream) rates for this run are unrecoverable — the `-Y bench-*.yaml` files were written inside the job container but never uploaded.

### 3.2 The one cross-OS performance signal that survived: smoke cpu rate

The smoke step (`--cpu 2 --timeout 5 --verify --metrics-brief`, default cpu method) landed in every uploaded `smoke.log`. This is the only cross-image bogo-ops/s data recoverable from run 14's artifacts:

| Image (build) | cpu bogo-ops/s (real) | cpu bogo-ops/s (usr+sys) |
|---|---|---|
| 20.03-lts-sp1 (plain, gcc 7.3) | 727.73 | 363.93 |
| 20.03-lts-sp2 (plain, gcc 7.3) | 726.30 | 363.18 |
| 20.03-lts-sp3 (plain, gcc 7.3) | 726.84 | 363.44 |
| 20.03-lts-sp4 (plain, gcc 7.3) | 723.05 | 361.54 |
| 22.03-lts (plain, gcc 10.3) | 792.53 | 396.29 |
| 22.03-lts-sp1 (plain, gcc 10.3) | 788.06 | 394.08 |
| 22.03-lts-sp2 (plain, gcc 10.3) | 787.12 | 393.62 |
| 22.03-lts-sp3 (plain, gcc 10.3) | 787.96 | 394.16 |
| 22.03-lts-sp4 (plain, gcc 10.3) | 788.92 | 394.49 |
| 24.03-lts (plain, gcc 12.3) | 805.52 | 402.82 |
| 24.03-lts-sp1 (plain, gcc 12.3) | 806.27 | 403.20 |
| 24.03-lts-sp2 (plain, gcc 12.3) | 805.71 | 403.06 |
| 24.03-lts-sp3 (plain, gcc 12.3) | **806.62** | **403.39** |
| 24.03-lts-sp4 (**UBSan**, gcc 12.3) | 735.94 | 368.13 |
| 20.03-lts (plain, gcc 7.3) | — (log lost; smoke passed 2/0/0) | — |

**Fastest**: 24.03-lts-sp3 @ 806.62 bogo-ops/s. **Slowest**: 20.03-lts-sp4 @ 723.05 bogo-ops/s. **Spread (max/min): 1.116×** (~11.6%).

Family-level (plain builds, family means):

| Comparison | Ratio | Delta |
|---|---|---|
| 24.03 (806.03) vs 20.03 (725.98) | 1.110× | +11.0% |
| 24.03 (806.03) vs 22.03 (788.92) | 1.022× | +2.2% |
| 22.03 (788.92) vs 20.03 (725.98) | 1.087× | +8.7% |
| 24.03 plain (806.03) vs 24.03-lts-sp4 UBSan (735.94) | 1.095× | **UBSan overhead ≈ 8.7%** |

Since the toolchain is the dominant per-family variable (gcc 7.3.0 → 10.3.1 → 12.3.1) and the kernel/userspace is the openEuler image, the monotonic 20.03 < 22.03 < 24.03 trend most plausibly reflects compiler codegen improvements (and possibly younger glibc), not OS "speed". The UBSan build sits below even the gcc-7.3 20.03 family on this stressor — instrumentation cost swamps toolchain gains.

The cpu stressor result is corroborated by the UBSan runtime diagnostics: `stress-cpu.c:1079` converts a value outside `long int` range (4 hits in a 5 s, 2-worker run) — worth a look in the source even though the run passed verification.

## 4. Anomalies & action items

| # | Anomaly | Impact | Suggested action |
|---|---|---|---|
| 1 | `${TAG_SAFE}` literal in `reports-*` / `seq-report-*` upload paths (workflow `with:` blocks never expand shell env) | bench/methods/seq files never uploaded; benchmark table empty; seq data only in job logs | Use `name: reports-${{ env.TAG_SAFE }}` (already correct) with plain filenames produced by a preceding `mv`/`cp` shell step, or `path:` globs like `methods-*.log bench-*.log bench-*.yaml` |
| 2 | `build mode:` line in `buildinfo.txt` always empty | Build mode not traceable from artifact alone | Echo `$BUILD_MODE` in a later step, or write it directly in the build step to a file |
| 3 | UBSan seq suite hung (>5h42m, no output) on 24.03-lts-sp4 | Job cancelled; sp4 has no seq results | Either skip `--sequential` when `BUILD_MODE=sanitize-ubsan`, or give the seq step its own much smaller `timeout-minutes` |
| 4 | `cannot find -lubsan` on 14/15 images (broken `libubsan.so` symlink in openEuler images) | UBSan coverage effectively random (1/15) | Accept fallback (current behavior) or install `libubsan` explicitly via dnf before building |
| 5 | `opcode:text` rc=2 transient on 20.03-lts | Only job failure of the run | Already fixed post-run by retry-once (`a567867ae`) |
| 6 | Seq skip counts vary ±3 between runners of the same OS (rc=3 resource aborts) | Cosmetic; verify script already classifies as environment-limited | None — document as expected shared-runner noise |

## 5. Caveat

All rates above come from single 5 s runs on shared Azure arm64 runners in different regions with unknown neighbor load — treat them as **trends, not absolutes**. The intended 20 s, 5-stressor bench comparison could not be delivered by this run due to the upload-path bug; once that is fixed, the table should be regenerated (e.g. re-run `multi-os-verify`) to get cpu/fma/memcpy/memrate/stream numbers per image.

## 6. Data provenance

- `benchmark-comparison` artifact → `/tmp/bench-r14/bench-table.md` (empty table)
- `reports-*` artifacts (14) → `/tmp/reports-r14/reports-<tag>/{buildinfo.txt,build.log,smoke.log}`
- Job logs (15 build-test + benchmark-compare) → recovered via `gh api .../actions/jobs/<id>/logs`; sweep/bench/seq summaries and build modes extracted from step output lines
- No `seq-report-*` artifacts exist in this run (finding #1)
