# Excitation guide — levers, pathways and the coverage matrix

This is the methodology document of the excitation engine: which load
conditions raise the probability of exciting silent data corruption, which
hardware pathways they can be applied to, and where the engine still has
gaps. Evidence base and literature survey live in
[docs/superpowers/research/](superpowers/research/) (round-by-round
engineering records).

## The three-factor model

Field SDC incidents decompose into three contributing factors:

1. **Insufficient design margin** — timing/voltage margins that only fail
   under specific load shapes (di/dt, resonance, boundary values).
2. **Aging / degradation** — a marginal core that drifts over time; it does
   not fail a quick functional test, it fails under sustained, shaped load.
3. **Workload shape** — real production load rarely visits the corner cases
   (boundary operands, huge address spans, SMT contention extremes) that a
   dedicated excitor can.

The engine attacks factor 3 to expose factors 1 and 2.

## Excitation levers (the "why" table)

| Lever | Mechanism | Evidence | Entry |
|---|---|---|---|
| Machine-wide concurrency | single-core isolation rarely triggers; concurrency is the observed trigger condition | CORE179 / fleet field reports (r2 survey) | `sdc-run full` |
| Residual heat + ordering | failing cases only failed when scheduled after heat-generating ones; exponential relation with a minimum trigger threshold | field observation on the reference fleet | `--preheat MINS` |
| di/dt load steps | rapid load transients modulate current slew rate and local IR drop | ~7% unique coverage attributed to load steps in the frontier survey | `--varyload N --varyload-ms` |
| SMT sibling contention | sibling threads contend for shared execution resources, compressing per-thread margins | no public ARM server SMT2 µarch data — the `pair` matrix measures it per chip | `sdc-run pair` |
| SDC-directed data shapes | bit-segment-sensitive defects need boundary/segment coverage, not uniform random | bitgen statistical validation: 100% boundary hits vs ~0 uniform | `--operand-var`, bitgen consumers |
| Address-space shaping | TLB/page-table/LSU corner paths (huge spans, VA-bit edges, misalignment, mixed orders) are never visited by linear load | address-space survey (r13 records) | `--addrspace` |
| Long soak | event rates as low as 0.01/minute demand hours of non-repeating shaped load | fleet SDC frequency measurements | `-t 2h`+, bitgen long streams |
| Periodic repetition | aging-related marginality shows up across repeated campaigns, not single runs | ITHICA-style long-sequence findings | repeated `sdc-run` campaigns |

## Coverage matrix

Rows = levers, columns = hardware pathways. `●` covered, `◐` partial,
`○` gap (roadmap).

| Lever ↓ / Pathway → | ALU / branch | Vector (SVE2/NEON) | Crypto | Atomics (LSE) | LSU (ld/st) | Cache hierarchy | MMU / TLB | Interconnect (NUMA) |
|---|---|---|---|---|---|---|---|---|
| **Data shape** | ● `--operand-var` 5 methods; cpu seed-unlocked | ● fma/vecfp FP bit synthesis; sve2 golden operands | ● armcrypto 13 methods, live operand streams | ◐ atomic RMW jitter (oracle keeps literals) | ● memrate 4 write patterns; cacheline rand-payload | ● l1cache bitgen streams; cache write path bitgen | ◐ vm rand-offset bitgen payloads | ○ |
| **Address shape** | ◐ misalign-huge (addrspace) | — | — | — | ● addrspace misaligned-huge / dense offsets | ● addrspace malloc-giant / mixed orders | ● addrspace va-bit-walk / huge-random-fixed / guarded-holes | ◐ cross-socket `path -c <node1>` |
| **Concurrency** | ● `full`: 2 workers/core via `--taskset physical` | ● same | ● same | ● same | ● same | ● same | ● same | ● full spans all nodes |
| **SMT contention** | ● `pair` fma×cpu | ● `pair` fma×fma | ◐ `pair` armcrypto×fma | ○ | ○ | ● `pair` cacheline×cacheline | ○ | ○ |
| **Residual heat** | ◐ preheat collateral | ● preheat: all-core fma+vecfp | ◐ collateral | ◐ collateral | ◐ collateral | ◐ collateral | ◐ collateral | ◐ collateral |
| **di/dt steps** | ● varyload modulates all workers | ● same | ● same | ● same | ● same | ● same | ● same | ● same |
| **Long soak** | ● `-t` hours, non-repeating bitgen streams | ● same | ● same | ● same | ● same | ● same | ● same | ● same |
| **Power/thermal observation** | `--rapl`/`--raplstat` hwmon telemetry (measurement, not a lever) | | | | | | | |

## Reading the matrix

- The **strong diagonal** is data-shape × compute pathways: shaped operands
  flow through ALU, vector, crypto and LSU paths with golden replay.
- **Concurrency and di/dt are horizontal**: they apply to every worker
  regardless of pathway, which is why `full` mode composes them with the
  shaped-load stressors rather than running separately.
- The **known gaps** (the roadmap):

| Gap | Why it matters | Candidate approach |
|---|---|---|
| Interconnect-dedicated excitation (L3-instance and cross-socket coherence traffic shaping) | L3 is split in 24 instances on the reference 950; coherence corner traffic is a documented low-coverage area (L2C 40% / LSU 54% coverage in the fleet model) | NUMA-aware ping-pong stressor with bitgen payloads on shared lines |
| lrcpc/ilrcpc (RCpc atomics) pathway | available on target hardware, unused | new stressor or atomic method |
| SMT × MMU/TLB and SMT × atomics combinations | pair matrix covers vector/cpu/cache only | extend `pair` COMBOS |
| OoO scheduler pressure with shaped dependencies | reorder-buffer corner cases under operand variation | dependency-chain stressor with bitgen branch/operand shapes |
| Real-machine bitgen calibration loop | window/density parameters were tuned statistically, not against real flip distributions | run `sdc-flip-collect.sh` on a machine with observed SDC, feed back `--bitgen-band-*` |

## Mode recipe audit

Why each orchestration mode runs what it runs (recorded so future changes
are deliberate, not accidental):

| Mode | Recipe | Rationale |
|---|---|---|
| `excite` | cpu-method all + fma + armcrypto + operand-var + addrspace + memrate-bandwalk + vm-rand-offset + varyload | widest pathway coverage at zero verify cost — crypto and memory-shape stressors are affordable only where no verify budget competes |
| `full` | cpu-method all + fma + operand-var + addrspace + varyload, verify sentinels on | the trigger + verify window; adding armcrypto/memrate/vm would spend the verify budget on extra pathways — that trade is exactly what `excite` exists for |
| preheat | all-core fma + vecfp, no verify | vector-heavy heat generation is the strongest empirically observed pre-failure condition |
| `scan` | per-sibling-pair sweep + `--keep-bg` cpu matrixprod | localisation needs per-core metric cleanliness; keep-bg preserves the machine-wide concurrency trigger |
| `path` | sve2 + ls64 + crc32 (feature-gated) | datapath golden cross-checks — a mismatch attributes SDC to a pathway |
| `pair` | 4 SMT combinations (fma×fma, fma×cpu, armcrypto×fma, cacheline×cacheline) | maps the sharing topology across vector / integer / crypto / cache resource classes |

## How to compose a campaign

1. **Start hot**: `--preheat 10` before any verify window.
2. **Keep the whole machine busy**: never scan a core in isolation —
   `--keep-bg` (half the physical cores) during `scan`.
3. **Shape everything**: prefer stressors wired to bitgen (operand-var,
   addrspace, memrate patterns, vm rand-offset, cacheline/l1cache
   rand-payload) over their unshaped equivalents.
4. **Modulate**: varyload di/dt steps running alongside the compute load.
5. **Run long**: hours, repeated across days for aging-related marginality.
6. **Detect elsewhere**: SDCShield alongside (see
   [sdcshield-integration.md](sdcshield-integration.md)); treat this tool's
   `--verify` as a ride-along sentinel.
