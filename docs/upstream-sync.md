# Upstream sync policy

This fork tracks [ColinIanKing/stress-ng](https://github.com/ColinIanKing/stress-ng)
— the most battle-tested system stressor in existence and the foundation
this project stands on. Upstream stays healthy through frequent small
commits and roughly monthly releases; this fork merges **once per upstream
release**, by PR, and re-validates everything.

## Cadence and procedure

1. Watch upstream releases (tags `V0.xx.xx`, about monthly).
2. Merge into a branch, open a PR into `main` (precedent: PR #5 merged 8
   upstream fixes with no conflicts).
3. Resolve conflicts per the containment rules below.
4. Re-run the full verification pipeline (the 6 steps in
   [CONTRIBUTING.md](../CONTRIBUTING.md)); CI must be green on all 15
   images before merge.
5. Bump `Makefile` `VERSION` to `<upstream>-sdc.<n>` and update
   [CHANGELOG.md](../CHANGELOG.md).

## Conflict containment rules

The fork's value concentrates in **new files** (new stressors, `core-bitgen`,
scripts, docs), which merge cleanly by construction. The shared-file rules
keep the remaining surface small:

| File class | Rule |
|---|---|
| Fork-owned, fully rewritten | `README.md` is excluded from merges via `.gitattributes` (`merge=ours`). If it conflicts anyway (e.g. deleted-then-modified), keep ours. |
| Shared structural files (Makefile, core-opts.c, stress-ng.1.in) | Fork edits stay *compact and marked*: `# SDC-fork: begin ... end` comment regions where practical; new entries adjacent, no gratuitous reformatting of upstream lines. Conflicts resolve by taking upstream's structure and re-applying the fork block. |
| Makefile `VERSION` | Single line, `<upstream-version>-sdc.<n>` — a one-line conflict by design. |
| `.github/` | The fork deleted the upstream workflows (FUNDING, travis, ci-builds, container-image-*). If upstream modifies them, resolve as **deleted** (ours); the fork's CI is `multi-os-verify.yml` only. |
| Upstream-internal docs (CITATIONS.md, TODO, README.Android, …) | Take upstream's version verbatim; the fork does not edit them. |

### The `merge=ours` driver setup

`.gitattributes` marks `README.md merge=ours`. The driver must be configured
once per clone (git does not ship it as a default):

```bash
git config merge.ours.driver true
```

## What comes across, what does not

- **Wanted from upstream**: bug fixes, new stressors and methods (new attack
  surface), portability fixes, kernel-interface updates.
- **Re-applied on top if the merge drops them**: SVE2 march injection
  (`Makefile.config` / config machinery), gcc 7.3 guards on SVE2/SVE
  intrinsics, HWCAP fallbacks — these live in shared files and are the most
  likely conflict points. The verification pipeline catches any that slip.
- **Never wanted**: upstream CI/publishing workflows (replaced by
  multi-os-verify), upstream funding links.

## Post-merge verification (mandatory)

The 6-step pipeline from [CONTRIBUTING.md](../CONTRIBUTING.md), plus:

- objdump gate: SVE z-register instructions > 0 in the SVE2 build.
- `--operand-var`, `--addrspace`, `--vm-method rand-offset`,
  `--memrate-write-pattern` regression runs (bitgen consumers are the
  fork's widest shared-file surface).
- One fault-injection drill on a verify-capable stressor to prove the
  verify path survived the merge.
