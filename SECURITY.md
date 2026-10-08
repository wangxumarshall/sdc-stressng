# Security Policy

## Scope of this project

`sdc-stressng` is an **excitation tool**: it deliberately drives arm64 server
hardware beyond its normal operating envelope (maximum power draw, di/dt load
steps, die heating, timing-margin compression) to provoke silent data
corruption out of marginal cores. It is designed to run **only on machines
dedicated to stress testing**.

## Expected behaviour, not vulnerabilities

The following are intended features of this tool and will not be treated as
security issues:

- Sustained maximum power draw and heat generation
- Memory exhaustion (OOM) under memory stressors
- Timing-margin compression that may cause *the stress-testing machine itself*
  to produce incorrect results (that is the point — SDCShield detects it)
- Kernel log noise, thermal events, cpufreq throttling on the target machine

## Actual security issues

Report genuine vulnerabilities — e.g. privilege escalation in the tool itself,
memory-safety bugs in option parsing, unsafe handling of untrusted input,
compromise of the build/release pipeline — through
[GitHub private vulnerability reporting](https://github.com/wangxumarshall/sdc-stressng/security/advisories/new).

Please do **not** open public issues for suspected vulnerabilities.

## Supported versions

| Version | Supported |
|---|---|
| latest release line (`v0.22.00-sdc.x`) | yes |
| anything older | best effort |

## Supply chain

- CI builds run on 15 pinned openEuler LTS container images; binaries are
  built from source inside the workflow.
- Published container images (`ghcr.io/wangxumarshall/sdc-stressng`) are built
  by the same workflows from the tagged commits — no third-party binary
  artifacts are distributed.
