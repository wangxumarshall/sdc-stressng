# Contributing to sdc-stressng

Thank you for improving the arm64 SDC excitation engine. Before writing any
code, read [CLAUDE.md](CLAUDE.md) (the internal development guide) — it is the
distillation of twelve-plus rounds of field lessons and it is authoritative.

## The one rule above all

**Every change must make the machine more excitable, never less verifiable.**
This fork's value is excitation; detection belongs to SDCShield. If your
change adds validation machinery instead of excitation load, it is probably in
the wrong repo.

## Patch discipline

- **One patch per unit.** Each improvement is one commit that walks
  `plan → code → verify → commit → push`.
- **Grep upstream conventions before writing code.** Macro names, helper
  functions and idioms come from the existing tree, not from memory
  (`STRESS_ARCH_ARM` not `STRESS_ARCH_ARM64`, `STRESS_MB` not `MB`,
  plain `mmap` not invented shims). The tree has no `HAVE_MMAP`.
- **Method tables are 0-based and `methods[0]` is `"all"`.** A misaligned
  dispatch makes "passing" tests run someone else's code.
- **Randomization goes into pressure paths only — verify oracles stay
  deterministic** (identity/literal oracles must never be randomized).
- **Honest skipping is a feature.** No hardware → `skipped` with reason,
  never a fake run.
- **`-march` spelling:** `armv8.6-a+sve2+bf16+i8mm+sve2-bitperm`
  (`svebf16` is rejected by GCC). SVE codegen requires `-O3`.
- **SVE2 crypto has no GCC intrinsics** → `.inst` numeric encodings, and
  target attributes use the full `arch=armv8.4-a+sm4` form.
- Commands go into documentation only after they parse and run against a real
  binary in this tree.

## Verification pipeline (mandatory per patch)

1. **Local functional verification** (whatever hardware the dev machine has)
2. **QEMU user-mode emulation** for SVE2/SVE/SM3/SM4/SHA3/RNDR paths
   (`qemu-aarch64 -cpu max`)
3. **Fault injection**: for anything with a `--verify` code path, flip one bit
   manually and prove the mismatch is caught — unverified verify code equals
   no verify code
4. **gcc 7.3 compatibility** (openEuler 20.03 container) — resolve locally,
   never via CI trial-and-error
5. **CI**: 15 openEuler images must be green (new stressors join the suite
   automatically)
6. **Zero performance regression** on modified hot paths (compare bogo-ops/s
   within noise)

## Pull requests

- Use the PR template; fill the verification checklist honestly.
- CI must be green on all 15 images before review.
- New options need a man page entry (`stress-ng.1.in`) in the same commit.
- New stressors must honestly skip on hardware without the required feature.

## Commit style

Lower-case area prefix, like the existing history:
`cache: bitgen-mutated write data in the write path (P6)`,
`ci: fix scheduled-run SEQ_TIMEOUT divergence`.

## Language

Code, comments, commit messages and user-facing docs are English. CLAUDE.md
and the engineering logs under `docs/superpowers/` are Chinese — that is
intentional (internal working memory).
