<!-- Thank you for the PR. Everything below is the review checklist — fill it honestly. -->

## What

<!-- One paragraph: what this changes and why. Reference the issue if any. -->

## Excitation rationale

<!-- Which lever / pathway / data shape does this improve?
     If it adds verification machinery instead of excitation, it likely
     belongs in SDCShield, not here. -->

## Verification checklist (see CONTRIBUTING.md for the full pipeline)

- [ ] Local functional verification on real hardware
- [ ] QEMU user-mode (`qemu-aarch64 -cpu max`) for SVE2/SVE/SM3/SM4/SHA3/RNDR paths
- [ ] Fault injection: flipped one bit and the verify path caught it
- [ ] gcc 7.3 (openEuler 20.03 container) compile + smoke
- [ ] CI green on all 15 openEuler images
- [ ] Zero performance regression on modified hot paths (bogo-ops/s within noise)

## Conventions

- [ ] One patch per unit (this PR is a single logical change)
- [ ] Upstream idioms grepped before writing (no invented macros/shims)
- [ ] Randomization only in pressure paths; verify oracles stay deterministic
- [ ] Honest skip on missing hardware (no fake runs)
- [ ] Man page updated for any new option
- [ ] New commands in docs parse against a real binary in this tree
