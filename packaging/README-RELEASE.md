# sdc-stressng — self-contained SDC excitation package

**SDC excitation engine for arm64 servers**: every CPU cycle in this
package goes into load that maximizes the probability of exciting
silent data corruption (SDC) out of marginal cores. Detection is not
this package's job — it belongs to SDCShield, the companion
golden-reference detector (run it alongside, see below).

This binary was built **inside the openEuler image named in the tarball
filename** and validated by the full CI run attached to the release
(full `--sequential --verify` suite + 62 method sweeps + benchmark
sampling). Released binaries are tested binaries.

## Quick start

```bash
tar xzf sdc-stressng-<version>.openeuler-<image>.aarch64.tar.gz
cd sdc-stressng-<version>.openeuler-<image>.aarch64/

./excite.sh 120          # 2h maximum excitation, 10-min preheat
./excite.sh 480          # 8-hour soak
./excite.sh 60 --sdcshield "./run-sdcshield.sh"   # with the detector alongside
```

No build step, no packages to install. `excite.sh` prints a safety
banner and hands over to the topology-deriving orchestrator.

## Safety

> ⚠️ **This tool deliberately drives hardware beyond its normal operating
> envelope** — sustained maximum power, die heating, di/dt load steps.
> Run it **only on machines dedicated to stress testing**. Expected
> consequences: thermal events, OOM under memory stressors, and (that is
> the point) possible SDC corrupting anything running alongside.

## What's inside

| File | Purpose |
|---|---|
| `stress-ng` | The engine (dynamically linked; non-glibc libraries bundled in `lib/`) |
| `excite.sh` | One-command maximum excitation |
| `scripts/sdc-run.sh` | Full orchestrator: `excite` / `full` / `scan` / `path` / `pair` / `abtest` |
| `scripts/sdc-report.sh`, `scripts/sdc-scan.sh` | Mismatch reporting, per-core sweeps |
| `lib/*.so` | Bundled shared libraries (auto-preferred via `LD_LIBRARY_PATH`) |
| `README.md` | This file |

Modes beyond pure excitation (run `scripts/sdc-run.sh <mode>` directly):
`full` adds verify sentinels and reports, `scan` sweeps per physical core
(keep `--keep-bg` on), `path` runs datapath golden cross-checks, `pair`
maps SMT contention topology, `abtest` A/B-compares two builds.

## Compatibility

- **glibc**: not bundled; it ships with your OS. The package records its
  minimum requirement at the bottom of this file. A tarball built on an
  older openEuler runs on newer ones (forward compatible) — when in
  doubt, pick the tarball matching (or older than) your target OS.
- **All other shared libraries** are bundled in `lib/` and preferred
  automatically.
- **Hardware features** (SVE2, ls64, RNDR, …) are detected at runtime;
  absent features skip honestly with a reason — never a fake run.
- Architecture: aarch64 only.

## Running with SDCShield

SDCShield provides the detection side: 273 golden-reference cases and a
failing-CPU report. Typical joint campaign:

```bash
# terminal 1: excitation
./excite.sh 7200 --sdcshield is handled by excite.sh itself, or:

# explicit:
NG=./stress-ng ./scripts/sdc-run.sh full -t 7200 --preheat 10 \
    --sdcshield "<path to SDCShield runner>"
```

See the project's docs/sdcshield-integration.md for the staged playbook
and report cross-correlation guide.

## Project

Source, documentation (architecture, excitation coverage matrix,
upstream sync policy) and the full changelog:
https://github.com/wangxumarshall/sdc-stressng
License: GPL-2.0 (upstream stress-ng by Colin Ian King and contributors).

---
