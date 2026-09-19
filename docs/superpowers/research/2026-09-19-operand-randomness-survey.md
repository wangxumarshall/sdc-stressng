# Operand Randomness Survey — stress-ng SDC fork

Date: 2026-09-19
Scope: how stressors source OPERANDS (data values) — fixed constants vs loop-derived vs seeded-PRNG vs per-iteration random.
Thesis under test: SDC detection coverage is limited by insufficient operand variation; many stressors feed fixed constants or loop counters into silicon, which never excites pattern-sensitive defects.

Classification:
- **A) FIXED-CONSTANT** — same bytes every run (memset 0x00/0xaa/0xa5, fixed pattern tables, or PRNG locked to a hardcoded seed).
- **B) LOOP-DERIVED** — `i`, `i%63`, `set++`, `j&0xff` — deterministic but varying.
- **C) SEEDED-PRNG** — `stress_mwc_*()` fed by the global per-worker reseed; varies per run/worker, reproducible with `--seed`.
- **D) FULLY-RANDOM** — fresh random operands re-drawn per iteration/round.

---

## 1. Stressor-by-stressor classification

| Stressor group | Representative code path | Class | Evidence (file:line + quote) |
|---|---|---|---|
| cpu: matrixprod | `stress_cpu_matrix_prod()` | C | `stress-cpu.c:1364-1366` — `const uint32_t r1 = stress_mwc32(); const uint32_t r2 = stress_mwc32(); ... a[i][j] = (long double)r1 * v;` (init once per bogo-op call, per-element random) |
| cpu: crc32 / crc16 / fletcher16 / ipv4checksum | `random_buffer()` helper | C | `stress-cpu.c:689-692` — `for (i = 0; i < len / 4; i++) { register uint32_t v = stress_mwc32(); *data++ = ...` ; callers at 1707/1737/1771/1788 |
| cpu: fft | `stress_cpu_fft()` | B | `stress-cpu.c:647` — `cpu_data->fft_buf[i] = (double complex)(i % 63);` — pure loop-derived, identical every run |
| cpu: int8..int128, rand, int+fp | `STRESS_CPU_INT` macro | **A (locked seed)** | `stress-cpu.c:850-852` — `stress_mwc_seed_default(); a = (type)stress_mwc32(); b = (type)stress_mwc32();` — resets to the compile-time default seed on every invocation, so the "random" operands are bit-identical every run. Same at 368 (`stress_cpu_rand`) and 1229 (`STRESS_CPU_INT_FP`) |
| fma | `stress_fma_init()` + `stress_fma()` | C | `stress-fma.c:68-73` — `return (float)stress_mwc32() * fhalfpwr32;` fills `double_init[]/float_init[]` once; per-iteration `b`/`c` picked by `idx_b++; idx_c += 3;` (B-style index into C-style random array) `stress-fma.c:714-726` |
| vecfp | `stress_vecfp()` init loop | C | `stress-vecfp.c:463-477` — `r = stress_mwc32(); d = (double)i + (double)r / ((double)(1ULL << 38));` (i + random; reinit never — once per run) |
| matrix | `stress_matrix_data()` | C | `stress-matrix.c:826-829` — `const uint64_t r = stress_mwc64(); return v * (stress_matrix_type_t)r;` used at 911-912 `a[i][j] = stress_matrix_data(v); b[i][j] = stress_matrix_data(v);` (init once, matrices then recycled) |
| memcpy | `stress_rndbuf(str3)` | C (once) | `stress-memcpy.c:336` — `stress_rndbuf(str3, MEMCPY_MEMSIZE);` — source filled with PRNG bytes **once** before the loop; every copy thereafter re-copies the same random buffer |
| memrate: all write ops | `STRESS_MEMRATE_WRITE` family, stos asm, memset op | **A** | `stress-memrate.c:533` — `(void)shim_memset(&vaa, 0xaa, sizeof(vaa)); v = vaa;` (same at 580, 647, 702); asm `stress-memrate.c:858` — `"mov $0xaaaaaaaaaaaaaaaa,%%rax\n;"` (891: 0xaaaaaaaa, 924: 0xaaaa, 957: 0xaa); memset method 456/479/498 — `shim_memset(..., 0xaa, ...)` |
| memrate: read ops | `STRESS_MEMRATE_READ` family | n/a (reads whatever writes left = 0xaa) | `stress-memrate.c:141-190` — pure read loops, data depends on the 0xaa writers |
| stream | `stress_stream_init_data()` | C (once) | `stress-stream.c:838-842` — `register const double delta = (double)stress_mwc32() * divisor; ... register double v = (double)r * divisor;` then `v += delta` ramp (B-style ramp from C-style start) |
| cache | `stress_cache_write()` / `CACHE_WRITE_MOD` | B | `stress-cpu.. stress-cache.c:909` — `register const uint8_t v = j & 0xff; ... buffer[i] = v; buffer[k] = v;` ; buffer pre-zeroed at 1294 `shim_memset(buffer, 0, buffer_size)`; addend `r` is a loop counter (1097 `r = 0`, 1352 `r++`) |
| cacheline | inc/adjacent/copy/mix/bits/atomicinc methods | B (A for mix) | `stress-cacheline.c:321` — `static uint8_t tmp = 0xa5;` (mix method fixed start); inc/adjacent: `(*data8)++ ... val8 += 7;` (204, 225); bits: `val8 = (uint8_t)(1U << (i & 7)); val8 ^= 0xff;` (496-506) |
| l1cache | all methods incl. "random" | B | `stress-l1cache.c:257` — `*(ptr) = (uint8_t)set;` with `set++; if (set >= l1cache_sets) set = 0;` — the DATA is the set counter; even the `random` method writes `(uint8_t)set` at 394, only the ADDRESS is random (`stress_mwc32modn`) |
| atomic | `DO_ATOMIC_OPS` macro | **A** (operand values) | `stress-atomic.c:239-295` — all fetch ops use literals: `SHIM_ATOMIC_FETCH_ADD(var, (type)1, ...)`, `(type)2`, `(type)3`, `~1`, `~2`, `~4`, `~8`, `16`, `32`, `64`, `128` — the operand set is a fixed list of small constants; only the accumulator evolves |
| bitops | sign/abs/bswap/countbits/clz/ctz... | C | `stress-bitops.c:58` — `int32_t v = stress_mwc32();` (fresh random per call; 27 mwc call sites; some refresh mid-loop e.g. 167 `v += stress_mwc32();`) |
| armcrypto | `crypto_in[]` fill | **A (locked seed)** | `stress-armcrypto.c:786-793` — `stress_mwc_seed_set(0x5eed1234, 0xabcd9876); ... crypto_in[i] = ((uint64_t)hi << 32) | (uint64_t)lo;` — comment says "deterministic pseudo-random input data": the entire crypto input is bit-identical on every run, every machine |
| vm: moving_inversion | fill + verify + complement | C | `stress-vm.c:380-384` — `stress_mwc_reseed(); w = stress_mwc32(); z = stress_mwc32(); stress_mwc_seed_set(w, z); ... *(ptr++) = stress_mwc64();` — classic random-fill with seed save/replay |
| vm: galpat zero/one | background + sparse flips | A + C | `stress-vm.c:1658` — `shim_memset(info->buf, 0x00, ...)` / 1723 `0xff`, then C-style sparse random bit flips 1664-1676 `const size_t offset = stress_mwc64modn(info->buf_sz); const uint8_t bit = stress_mwc32() & 3;` |
| vm: checkerboard | fixed 64-bit patterns | **A** | `stress-vm.c:3406-3413` — `const uint64_t v0 = 0x5555aaaa5555aaaaULL; ... v7 = 0xa5a5a5a5a5a5a5a5ULL;` — hardcoded forever |
| vm: walking one/zero (data) | per-byte bit walk | **A** (intentional) | `stress-vm.c:583-590` — `SET_AND_TEST(ptr, 0x01, ...); SET_AND_TEST(ptr, 0x02, ...) ... 0x80` / zero variant 624-631 `0xfe, 0xfd ... 0x7f` |
| vm: walking one/zero (addr) | address-bit walk, fixed data | **A** (intentional) | `stress-vm.c:665-668` — `uint8_t d1 = 0; uint8_t d2 = (uint8_t)~d1; ... shim_memset(info->buf, d1, info->buf_sz);` |
| vm: modulo_x | every 23rd byte | C | `stress-vm.c:524-526` — `stress_mwc_reseed(); pattern = stress_mwc8(); compliment = (uint8_t)~pattern;` |
| sve2 | input arrays, re-seeded per round | C→D | `stress-sve2.c:444-448` — `data->u64s[i] = stress_mwc64(); data->gather_base[i] = stress_mwc64();` but `data->doubles[i] = (double)(i + 1) * 1.000001;` is B; re-seeded each round at 513-518 `data->doubles[i] = ... + (double)stress_mwc16(); data->u64s[i] = stress_mwc64();` — the only surveyed stressor that refreshes operands per iteration |
| hash | key buffer | C | `stress-hash.c:92` — `stress_uint8rnd4((uint8_t *)bucket->buffer, STRESS_HASH_N_KEYS);` then folded to ASCII `' '..'_'` (95) at 95-96 — random but the fold truncates to 6 bits/byte |
| crypt | phrase/setting | C | `stress-crypt.c:191,195` — `orig_setting[i] = seedchars[stress_mwc8() & 0x3f]; orig_phrase[i] = seedchars[stress_mwc8() & 0x3f];` |
| fp | `stress_fp()` giant init | C (once) | `stress-fp.c:710-713` — `r = stress_mwc32(); ld = (long double)i + (long double)r / ((long double)(1ULL << 38));` — all of ld/d/f/bf16/f16/f32/f64/f80/f128 r_init/add/add_rev/mul seeded once before the main loop |
| fp-misc | rand helpers | C | `stress-fp-misc.c:1235-1245` — `return (float)stress_mwc64() / (0.1f + (float)stress_mwc64());` re-drawn every outer loop (1289-1298) — decent |
| fp-error | fixed edge-case constants | **A** (intentional) | `stress-fp-error.c:149-197` — `log(-1.0)`, `log2(0.0)`, `sqrt(-1.0)`, `DBL_MAX + DBL_MAX / 2.0`, `exp(-1000000.0)` — this stressor is *about* fixed edge values |
| fp-subnormal | min subnormals | **A** (intentional) | `stress-fp-subnormal.c:498-515` — `stress_min_subnormal_ld(&fp_data->ld.tiny1, false); ... oneish = LDBL_ONEISH;` plus one random bit `stress_mwc1()` at 370 choosing tiny1 vs tiny2 |
| randlist | item payloads | C | `stress-randlist.c:117-122` — `uint8_t dataval = stress_mwc8(); ... shim_memset(ptr->data, dataval, randlist_size); dataval++;` — random start, incrementing fill |

---

## 2. `stress_mwc_*` API inventory (core-mwc.h / core-mwc.c)

Generators (all multiply-with-carry, 32-bit core state `(w, z)`):
```c
uint8_t  stress_mwc1(void);          /* 1 bit  (bit-cached)      */
uint8_t  stress_mwc8(void);          /* 8 bits (byte-cached)     */
uint16_t stress_mwc16(void);         /* 16 bits (halfword-cached)*/
uint32_t stress_mwc32(void);         /* the core MWC generator   */
uint64_t stress_mwc64(void);         /* two mwc32 calls          */
__uint128_t stress_mwc128(void);     /* two mwc64 calls          */
```

Range reduction (Lemire multiply-shift, unbiased; mask-and-reject fallback on 32-bit-only hosts):
```c
uint8_t  stress_mwc8modn(const uint8_t max);   /* 0..max-1 */
uint16_t stress_mwc16modn(const uint16_t max);
uint32_t stress_mwc32modn(const uint32_t max);
uint64_t stress_mwc64modn(const uint64_t max); /* needs __int128 */
size_t   stress_mwcsizemodn(const size_t max);
```

Buffer/string fillers:
```c
void stress_rndbuf(void *buf, const size_t len);      /* len PRNG bytes          */
void stress_rndstr(char *str, const size_t len);      /* base64url string + NUL  */
void stress_uint8rnd4(uint8_t *data, const size_t len);/* word-at-a-time fast fill*/
```

Seed control:
```c
void stress_mwc_reseed(void);                 /* entropy-based (or --seed / --no-rand-seed aware) */
void stress_mwc_seed_set(const uint32_t w, const uint32_t z);
void stress_mwc_seed_get(uint32_t *w, uint32_t *z);
void stress_mwc_seed_default(void);           /* resets to (521288629, 362436069) */
```

`stress_mwc_reseed()` (core-mwc.c:112-187) mixes `getauxval(AT_RANDOM)`, gettimeofday, pid/ppid, rusage, load averages, machine-id, kernel release, memory size, and double time-rotations — a strong per-worker entropy pool. It is called globally at startup (`stress-ng.c:4574`) and again in every forked worker child (`stress-ng.c:1671`), so every worker gets a distinct stream by default.

**Not provided:** no per-stressor/per-domain PRNG handle (single global state — save/restore via `seed_get`/`seed_set` is the only isolation, as used by vm moving-inversion and l1cache random), no SIMD-width bulk generator, no bit-pattern-weighted generator (e.g. "mostly-ones", "walking-bits", "Hamming-weight-n"), no float/double bit-pattern generator (all FP stressors synthesize floats via arithmetic scaling, so exponent/mantissa fields are correlated, never uniform).

---

## 3. Good examples (correct PRNG operand sourcing)

1. **stress-bitops.c** — fresh `stress_mwc32()` per call and mid-loop refreshes; the operand IS the workload (27 call sites).
2. **stress-cpu.c matrixprod / crc32 family** — `random_buffer()`/`stress_mwc32()` per element, per call.
3. **stress-vm.c moving_inversion** — full random fill with seed save/replay for verify; the textbook pattern.
4. **stress-fp.c / stress-vecfp.c / stress-fma.c / stress-matrix.c / stress-stream.c** — random init of operand arrays (caveat: once per run, not refreshed).
5. **stress-sve2.c** — the only one that re-randomizes inputs each round (`stress-sve2.c:513-518`).
6. **stress-hash.c / stress-crypt.c / stress-randlist.c** — PRNG-derived inputs per call.
7. **stress-memcpy.c** — `stress_rndbuf()` source (caveat: once).
8. **stress-fp-misc.c** — re-draws x/y per outer iteration.

---

## 4. Worst offenders (fixed / effectively-fixed operands in data paths)

1. **stress-memrate.c** — every write path (C, asm stosq/stod/stosw/stosb, memset) emits `0xaa`-filled words: lines 456, 479, 498, 533, 580, 647, 702, 858, 891, 924, 957. Millions of stores/sec, one bit pattern. Reads then consume the same 0xaa. No PRNG anywhere in the file.
2. **stress-armcrypto.c:786** — `stress_mwc_seed_set(0x5eed1234, 0xabcd9876)` locks `crypto_in[]` to a single hardcoded 2 KiB pattern for every run on every machine (deliberate for golden-reference verify, but it means the AES/SHA1/SHA256/PMULL SVE2/NEON datapaths never see a different operand in their lifetime).
3. **stress-cpu.c int/rand/int-fp methods (850, 368, 1229)** — `stress_mwc_seed_default()` at the top of each method resets the PRNG to the compile-time seed; the "random" a/b operands and the whole 1000-iteration INT_OPS stream replay identically every call. (Needed for --verify's known a_final/b_final, but it makes int8..int128, rand, and all STRESS_CPU_INT_FP variants pattern-static.)
4. **stress-atomic.c** — every atomic RMW uses the literal set {1, 2, 3, 4, ~1, ~2, ~4, ~8, 16, 32, 64, 128}: `DO_ATOMIC_OPS` at stress-atomic.c:239-295. Only the accumulator register evolves.
5. **stress-cache.c / stress-l1cache.c / stress-cacheline.c** — data values are `j & 0xff` (cache:909), `set` counter (l1cache:257/394), `++`/`0xa5` (cacheline:321, 204, 496). Randomness is spent on ADDRESSES (`stress_mwc64modn`, `stress_mwc32modn`) while the DATA bus sees counters.
6. Honorable mentions: **stress-vm.c checkerboard** (8 hardcoded 64-bit words, 3406-3413), **cpu fft** (`i % 63`), **memcpy/memrate/stream/fp** one-shot init (operands never refreshed during the run).

Note: several class-A cases (vm walking bits, galpat background, fp-error edge values, fp-subnormal tiny values, checkerboard) are *intentional* memory/FP-pattern tests — the defect there is not intent but the absence of a random counterpart.

---

## 5. Global options affecting operand randomization today

| Option | What it actually controls |
|---|---|
| `--seed N` (`OPT_seed`, stress-ng.c:4021; flag `OPT_FLAGS_SEED` core-opts.h:54) | Sets `mwc.z = seed >> 32; mwc.w = seed & 0xffffffff` inside `stress_mwc_reseed()` (core-mwc.c:114-126). Affects ONLY stressors that use `stress_mwc_*` without calling `seed_default()`/locked `seed_set()` first. Does NOT touch memrate (0xaa), armcrypto (locked seed), cpu int methods (locked seed), atomic literals, cacheline/cache/l1cache counters. |
| `--no-rand-seed` (`OPT_no_rand_seed`, stress-ng.c:201/399; `OPT_FLAGS_NO_RAND_SEED` core-opts.h:47) | Forces the default seeds (521288629, 362436069) — reproducible PRNG streams. Mutually exclusive with `--seed` (stress-ng.c:4707-4712). |
| `--random N` (`OPT_random`, stress-ng.c:4014; handler 3487-3495) | **Red herring for this thesis**: it randomly ORDERS/SELECTS which stressors run (`stress_random_stressors_set()`), it has nothing to do with operand values. |
| `--verify` | Side effect: some stressors (`stress-hash.c:88`, and the cpu methods' `seed_default`) lock seeds for reproducible golden results. |

There is currently NO option that controls operand bit-pattern policy (e.g. random vs walking vs checkerboard vs user-supplied pattern) for any compute stressor.

---

## 6. Summary distribution (of the ~30 surveyed paths)

- A (fixed/effectively-fixed): ~13 — memrate(all writes), armcrypto, cpu int/rand/int-fp (locked seed), atomic, cache, cacheline(mix/inc/bits), l1cache, vm checkerboard, vm walking one/zero (data+addr), fp-error, fp-subnormal, cpu fft (B/A boundary)
- B (loop-derived): ~5 — cpu fft, cache write value, l1cache set value, cacheline increments, fma b/c index selection
- C (seeded-PRNG, mostly init-once): ~12 — matrixprod, crc family, fma, vecfp, matrix, memcpy, stream, vm moving-inversion/modulo_x/galpat-flips, sve2, hash, crypt, fp, fp-misc, randlist, bitops
- D (per-iteration refresh): 1 — sve2 (partial; u64s refreshed, doubles only perturbed by mwc16)
