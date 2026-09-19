# Address-Space Coverage Survey — stress-ng (SDC fork)

Date: 2026-09-19
Scope: how the stressor suite varies (or fails to vary) **allocation size**, **virtual address choice**, **in-buffer offset**, **alignment**, and **page order**. Thesis under test: SDC detection has insufficient ADDRESS SPACE variation; fixed sizes / fixed alignments / sequential-only touching fail to excite DRAM row-buffer, TLB/MMU, and address-bit-pattern silicon defects.

All file:line references are against the working tree at `/home/sdc/wangxu/sdc-stressng`.

---

## 1. Stressor-by-stressor survey

| Stressor | Allocation size behavior | Address behavior | Alignment / offset behavior | Evidence |
|---|---|---|---|---|
| **malloc** | RANDOM size per alloc: `stress_mwc64modn(malloc_bytes)` capped by `--malloc-bytes` default **64KB** (`DEFAULT_MALLOC_BYTES`). calloc/realloc/aligned paths all use `stress_alloc_size()`. MAXIMIZE → MAX_32. | Kernel-chosen (malloc arena); 25% of allocations forced through `M_MMAP_THRESHOLD` (default 128KB) into mmap'd chunks. | Alignment **varies deliberately**: `MK_ALIGN(i) = 1U << (3 + (i & 7))` → 8B..1024B via posix_memalign/aligned_alloc/memalign/valloc (cases 1-4); malloc/calloc default alignment otherwise. Touches only **first byte of each page** (`stress_malloc_page_touch`, ptr += page_size) — start-of-page offset only. | stress-malloc.c:32-48, 146-152, 205-322, 494-503 |
| **bigheap** | FIXED growth quantum: `--bigheap-growth` default **64KB** per realloc, page-rounded; grows until `--bigheap-bytes` (default MAX_MEM_LIMIT = 256TB on 64-bit, practically until OOM). Aggressive adds +64B re-realloc. | Single contiguous sbrk/mmap heap; address never chosen by us. | Page-aligned by definition; writes at `stride = page_size` (or `sizeof(uintptr_t)` aggressive); sequential from start; verify rewrites full region. One shape only: contiguous growth. | stress-bigheap.c:28-34, 124-169, 211-292 |
| **vm** | FIXED per instance: `vm-bytes/instances`, default **256MB** total, page-rounded; only shrinks (×15/16) on repeated OOM. Single mapping per iteration; `vm-keep` redirties in place. | `mmap(NULL, ...)` — kernel picks (top-down mmap_base region). No hint. | 30+ data-pattern methods but nearly all touch **sequentially from buf** (fwd/rev/stride-23/prime-61/gray). Exceptions: galpat-0/1 and rowhammer pick `stress_mwc64modn(buf_sz)` random offsets (sparse, 1 bit per 4KB); `vm_swap` swaps 64B chunks at pre-shuffled random offsets. walk-1a/walk-0a XOR address bits 1..63 **within the buffer** (address-bit toggling, but buffer-relative). | stress-vm.c:40-42, 3628, 3669-3760, 660-750 (walking addr), 1651-1773 (galpat), 2546-2632 (rowhammer), 1089-1191 (swap) |
| **mmap** (base) | FIXED: `mmap-bytes` default **256MB**, one mapping, page-rounded. | 50% of iterations pass `hint = ~(uintptr_t)0` (top of VA space — exercises >48-bit hints on arm64) vs NULL; random mmap flags per iteration; slow/fast munmap variants; random-order remap of pages with MAP_FIXED at **original page addresses** (address shuffle but within same span). | Page granularity only; `stress_mmap_set_light` writes first word per page. | stress-mmap.c:38-40, 590-600, 598 (hint), 689-763 (shuffle+remap) |
| **mmapaddr** | FIXED: **one page** per mapping (`page_size`). | RANDOM probe: `stress_mwc64() & mask` where mask is full page_mask or 32-bit mask (rnd & 0x80 selects); mincore rejects mapped; 50% MAP_FIXED; also MAP_32BIT, mremap MREMAP_FIXED to new random addr, MAP_FIXED_NOREPLACE collision test. Single-page mappings only. | Page-aligned by construction; offset within page never varied (PROT_READ only, one byte read). | stress-mmapaddr.c:81-110, 127-224 |
| **mmapfixed** | RANDOM small: `page_size * (1 + stress_mwc8modn(7))` = 1..7 pages. | Halving sweep: addr starts MMAP_TOP (0x8000'0000'0000'0000) and `addr >>= 1` each iter, wrapping at MMAP_BOTTOM (0x10000) — **deterministic power-of-2 ladder, not random**. Then mremap to `newaddr = addr ^ (page_size<<3 \| page_size<<4)` plus a **random-base bit-mask ladder** (`rndaddr_base & mask` for mask >>= 1) — walks each address bit. Sizes ≤ 7 pages. | Page-aligned; verifies data survives mremap (SDC-relevant: value written before remap, checked after). | stress-mmapfixed.c:46-53, 125, 135-253 |
| **mmapmany** | FIXED: `page_size * 3` per mapping, up to `sysconf(_SC_MAPPED_FILES)` (capped 256K mappings). | Kernel-chosen; creates VMA churn. Writes pattern at offset 0 and offset `page_size*2` of each mapping; **unmaps the middle page** of each 3-page mapping (VMA splitting). | Only two fixed offsets (0, 2*page_size); middle-page hole. | stress-mmapmany.c:33, 55, 105-160 |
| **mmaprandom** | RANDOM: `pages = stress_mwc32modn(maxpages) + 1`, default maxpages **8** (option up to 1024) → 1..8 pages typical; huge TLB occasionally 1×2MB page. | Best random-address engine in the suite: 50% of mmaps use `MAP_FIXED_NOREPLACE` at `stress_mwc64() & masks_64bit[]` — 21 masks from 8MB up to **1TB span (0x3ff...)**; plus random prot/flags, shm SysV/POSIX, mremap, split/split-hole/join, unmap-first/last-page, mbind, process_madvise, fork/clone. | Page granularity; reads/writes are whole-mapping memset/strided; random file offsets (`stress_mwc32modn(maxpages) * page_size`). | stress-mmaprandom.c:72-81, 493-556 (masks), 565-587 (fixed_addr), 733-818, 863-947 |
| **mmaphuge** | FIXED menu cycling: 2MB/1GB/512MB (or arch default) via `stress_mmaphuge_settings[]` round-robin; up to 8192 mappings. | Kernel-chosen hugetlb pool; random file offset (4096*mwc8modn(16)) for file-backed. | Hugepage-aligned by definition; writes at stride `page_size*64 / 8` — every 64 base pages; toggles MADV_HUGEPAGE/NOHUGEPAGE. | stress-mmaphuge.c:89-100, 120-180 |
| **hugepage** | `--hugepage-num` default **2** hugepages; splits hugepage into base pages via MADV_DONTNEED and re-collapses. | Kernel-chosen. | **Random/forward/reverse page-order methods** for the split+rebuild (`--hugepage-method`), i.e. order churn inside one hugepage. | stress-hugepage.c:25-27, 69-89 |
| **mprotect** | FIXED: `page_size * 4` (MPROTECT_MAX=7 → (7>>1)+1 pages). | Kernel-chosen; then RANDOM page index + RANDOM length (`stress_mwc32modn(mem_pages)`, `stress_mwc32modn(max_size)`) per mprotect → **page-table churn at random spans**. | Page-granular regions; no sub-page offsets. | stress-mprotect.c:40, 106-121, 167-168 |
| **mseal** | FIXED: 2 pages (`mapping_size`). | Kernel-chosen; error-path stressor (mseal then expect EPERM on mprotect/munmap/mremap/madvise/mmap-FIXED-over). | Page-granular; halves of mapping sealed/unsealed. | stress-mseal.c:32, 37, 85-228 |
| **vm-addr** | Sweeps 8MB→64MB (`buf_sz <<= 1` from MIN_VM_ADDR_BYTES). | **Power-of-2 hint ladder**: `buf_addr = page_size; buf_addr <<= 1` up to max_addr — tries every power-of-2 base address as mmap hint. | 13 access patterns over the buffer: pwr2 strides (1,2,4..4096), gray, gray-inverse, **bit-reversed addresses**, revinv, inc/dec + inverted variants, bitposn strides (every bit position), flip. This is the **address-bit-pattern workhorse** — but all within an ≤64MB buffer, and mappings are kernel-placed (hint usually ignored). | stress-vm-addr.c:31-32, 80-466 (methods), 516-552 (ladder) |
| **vm-rw** | FIXED: `vm-rw-bytes` default **16MB**; process_vm_readv/writev in 1GB chunks. | Kernel-chosen. | Sequential pointer walk. | stress-vm-rw.c:32-34, 50, 95-130 |
| **vma** | FIXED: 32 pages region, 1-page mappings inside it. | RANDOM-ish: `stress_mwc64() & ((1 << (32..59 bits)) - 1)` random target region (rejects text/heap); random page offset within region; random prot/flags. VMA ops: mmap/munmap/mlock/madvise/mincore/mprotect/msync/pagemap_scan. | 1-page granularity at random offsets within a 32-page window. | stress-vma.c:40-41, 148-233, 239-289 |
| **pagescatter** | FIXED per round: `1 << idx` pages, idx 0..order (`--pagescatter-order` default 10 = 1024 pages, max 2^30). | RANDOM single-page addresses: `stress_mwc64() >> 18 & ~page_mask` with MAP_FIXED_NOREPLACE / MAP_FIXED fallback; mincore/madvise/pipe-probe rejection of mapped addrs. Per-page NUMA mbind option. | One page each; random VA scatter — TLB/PT churn, but **no multi-page random mappings** and no data verify. | stress-pagescatter.c:30-31, 97-169, 321-360 |
| **pagemove** | FIXED: `pagemove-bytes` default **4MB** single mapping (+1 guard page unmapped). | mremap shuffle **down by one page** deterministically (page i ↔ page i+1 rotation via temp page). | Page-aligned rotation; verifies page_num+virt_addr metadata after each move — SDC-relevant data check. | stress-pagemove.c:26-29, 104-123, 183-227 |
| **pageswap** | FIXED: 1 page per mmap, linked list up to `pageswap-pages` default **65536**. | Kernel-chosen; MADV_PAGEOUT each new + old head; walk+unmap. | Single page; linked-list traversal order (insertion order, not random). | stress-pageswap.c:22-24, 108-170 |
| **numacopy** | FIXED: 1 page per NUMA node + 1 local page. | Kernel-chosen, mbind'd per node. | Page-granular memcpy between nodes; affinity cycles next/none/node/prev/random. | stress-numacopy.c:35-44, 116-270, 511-574 |
| **tlb-shootdown** | FIXED: `page_size * 512` (2MB) anon + 4-page memfd. | Kernel-chosen. RANDOM offsets: `offset = (stress_mwc32() & mmap_mask) & page_mask` for mprotect/dontneed on random pages; cache-line strided access with random start `k` and fixed stride. Whole-region mprotect R→W→RW cycles force shootdown IPIs. | Random page offset within the 2MB region; cache-line (64B) touching. | stress-tlb-shootdown.c:44-47, 201-238 |
| **tlb-numa** | SMALL: mappings of `page_size * (2 + mwc8&0xf)` = 2..17 pages, plus two `tlb-entries`-page (default 512 → 2MB) regions with every other page unmapped. | Kernel-chosen; page arrays **shuffled randomly** (`stress_tlb_numa_shuffle_pages`); random CPU affinity churn; mbind 1-in-16 pages; MADV_PAGEOUT. | Odd/even page holes; shuffled page-order access. | stress-tlb-numa.c:38-40, 91-105, 209-352, 474-483 |
| **memrate** | FIXED: `memrate-bytes` default **256MB** single mapping (`--memrate-bytes`). | Kernel-chosen; optional `--memrate-discontiguous` (madvise dance at 1GB/16MB/2MB/256KB/64KB/8KB/4KB chunk sizes). | Strictly sequential 8/16/32/64/128/256-bit lanes from buffer start, 4096-aligned pointers; rates measured per size. No random offsets. | stress-memrate.c:43-45, 132-135, 1207-1220, 1280-1290 |
| **stream** | FIXED: 3 buffers of L3-size (default **4MB** each; auto-scaled by NUMA nodes); `--stream-index` 0-3 optional **random index shuffle** (default 0 = sequential). | Kernel-chosen; optional discontiguous. | Sequential unless `--stream-index N>0` (Fisher-Yates shuffle of indices — random offsets within buffers, **opt-in only**). | stress-stream.c:45-47, 989-1007, 1268-1300 |
| **memthrash** | FIXED: `MATRIX_SIZE^2` = 256MB (16K×16K). | Kernel-chosen single mapping. | RANDOM chunk methods: chunk sizes page/256/64/8/1 at random offsets (`stress_mwcsizemodn(chunks)`); swap method uses **two random initial offsets** advanced by +129/+65 (relatively prime strides); random matrix row/col swaps with random stride. | stress-memthrash.c:52-56, 110-170, 414-484 |
| **misaligned** | FIXED: **2 pages** (`page_size << 1`). | Kernel-chosen. | The alignment specialist: every method writes at odd offsets 1,3,5..15, `page_size-1..-15`, offset 63, and page-boundary-crossing pairs (e.g. `page_size-1`, `page_size-2`) for 8/16/32/64/128/256/float/atomic types; NUMA-randomize option. But buffer is tiny (2 pages) and offsets are a **fixed enumerated set**, not random. | stress-misaligned.c:1297, 1326-1336, 136-390 |
| **sparsematrix** | Sparse x×y matrix (default 500×500 → 1MB; max 10M×10M); `--sparsematrix-mem-method` mmap/heap/big-heap. | Kernel-chosen; random (x,y) element access → random offsets within the (potentially large) mapping. | Element-granular random access (4B); default footprint small. | stress-sparsematrix.c:49-51, 1124-1187, 1201-1241 |

### Infrastructure (`core-mmap.c`, `core-madvise.c`, `core-numa.c`)

| Helper | Behavior | Randomization? |
|---|---|---|
| `stress_mmap_populate()` | mmap + MAP_POPULATE (fallback: `stress_mmap_populate_forward` touches 1 byte/page **from the start**) | None — used by ~60 stressors for their working buffers, always at NULL hint. core-mmap.c:219-243, 600-627 |
| `stress_mmap_set/check(_light)` | Fill/verify per-page first-word, incremental val | Data random (`stress_mwc64` seed), addresses page-strided. core-mmap.c:41-205 |
| `stress_mmap_discontiguous()` | MADV_NOHUGEPAGE/UNMERGEABLE/DONTNEED then re-touch every other chunk at 1GB/16MB/2MB/256KB/64KB/8KB/4KB sizes | Fixed size menu, no random. core-mmap.c:671-712 |
| `stress_madvise_randomize()` | Picks one of 13 madvise options at random (only when `--mmap-madvise` global flag set) | Yes (flag-gated). core-madvise.c |
| `stress_numa_randomize_pages()` | mbind chunks (bounded to ≤256K/`nodes×instances` chunks) to random nodes | Yes — chunk sizes are power-of-2 halvings, not random. core-numa.c:278+ |
| `stress_munmap_force()` | Retry munmap w/ hugepage-size fallbacks | None. core-mmap.c:345-405 |
| No `stress_mmap_adopt` exists in this tree (grep: zero hits) — infrastructure name from the task prompt does not match this fork. | | |

Global random-ish options (`core-opts.c`): `--random N` (random stressor selection), `--mmap-madvise` (random madvise), `--mincore-random`, `--taskset-random`, `--getrandom/urandom` (RNG stressors). **No global "random-size" or "random-address" toggle exists**; every randomization lives inside individual stressors.

---

## 2. Stressors that already randomize well (good examples to build on)

1. **mmaprandom** — the gold standard: random size (1..maxpages), random fixed addresses across 21 mask spans up to 1TB, random prot/flags, split/join/hole/unmap-edge operations, NUMA move, fork/clone COW. Weaknesses: default maxpages=8 (tiny mappings), VERIFY_NONE (no data check), never does multi-MB random-fixed mappings.
2. **mmapaddr** — true random address probing with mincore validation, both 32-bit and full masks, MAP_FIXED / MAP_32BIT / mremap-fixed / FIXED_NOREPLACE. Weakness: one page, PROT_READ, one byte touched — no sustained data pattern at the random address.
3. **vma** — random target regions across 32..59-bit spans, random page offsets, random prot/flags, full VMA-op coverage incl. pagemap_scan.
4. **pagescatter** — random single-page MAP_FIXED scatter with three-tier mapped-rejection probing; per-page NUMA. Weakness: pages only, no verification.
5. **mmapfixed** — deterministic bit-ladder from 0x8000... downward + random-base address-bit mask walk via mremap; carries data across remaps (verify).
6. **vm-addr** — 13 address-bit-pattern access orders (gray, bit-reverse, pwr2, inverted) over its buffer; the only stressor systematically exercising address-bit transitions, albeit within ≤64MB.
7. **malloc** — random sizes up to the cap and the only stressor varying allocator alignment (8B..1KB) plus malloc/mmap-threshold crossover.
8. **memthrash (swap/chunk methods)** and **tlb-shootdown** — random in-buffer offsets with prime strides.

## 3. Worst offenders (fixed size, fixed alignment, fixed/sequential offsets)

- **vm** (256MB default): the primary SDC workhorse — 30+ data patterns but nearly all touch memory **sequentially from offset 0**; only galpat/rowhammer/swap use random offsets. Single mapping at a kernel-chosen address, fixed size all run.
- **memrate** (256MB): purely sequential lanes; no offset randomization at all; measures bandwidth, exercises one address shape.
- **stream** (3×4MB): sequential unless user opts into `--stream-index`; the random-index machinery exists but is **off by default**.
- **bigheap**: one contiguous heap, fixed 64KB growth quantum, page-stride writes — a single address shape (contiguity), by design.
- **mmapmany**: 3 pages × identical mapping, only offsets 0 and 2*page_size ever touched.
- **mprotect**: 4-page region — page-table churn only at trivial scale.
- **pagemove**: deterministic one-page rotation over a 4MB region.
- **pageswap / numacopy**: single-page allocations only; sequential list traversal.
- **misaligned**: fixed 2-page buffer and an enumerated (non-random) misalignment set — never combines misalignment with large or random mappings.
- **vm-rw**: 16MB sequential walk.
- Nearly every non-VM stressor's working buffer: `stress_mmap_populate(NULL, fixed_size, ...)` at kernel-chosen addresses (grep shows ~60 call sites).

## 4. Address-space SHAPES never exercised today

1. **Single mappings > a few hundred MB at user-chosen addresses.** The random-address engines (mmapaddr, pagescatter, mmaprandom, vma) all map **one page** or ≤17 pages. Conversely the big-buffer stressors (vm, memrate, stream, bigheap) always take kernel-chosen addresses. Nothing ever does "mmap 4GB/64GB/1TB at a random aligned address and run data patterns through it".
2. **Random offsets *within* huge mappings with verification.** vm's galpat/rowhammer do sparse random offsets in a 256MB buffer (1 bit per 4KB); memthrash does random chunks in 256MB; but no stressor does dense random-offset data-pattern + verify inside a multi-GB mapping (memrate/stream never randomize; mmaprandom's buffers are ≤8 pages by default and VERIFY_NONE).
3. **Full 52-bit / upper-VA-bit patterns.** Only stress-mmap.c:593-598 hints with `~(uintptr_t)0` (top of 64-bit space, forcing >48-bit VA on arm64) 50% of the time — but never verifies data at the resulting address, and the mapping is otherwise default-sized. mmaprandom's mask table tops out at 1TB span (0x3ff...); mmapfixed tops at 0x8000'0000'0000'0000 but only as a deterministic ladder with ≤7-page payloads. No stressor systematically walks **each VA bit 48..51 with sustained traffic**.
4. **Mixed page sizes in one run at random addresses.** mmaphuge/hugepage cycle hugepage sizes at kernel addresses; pagescatter/mmapaddr use base pages only; mmaprandom occasionally maps 1×2MB hugetlb page (only after a 32-iteration counter, stress-mmaprandom.c:756-767). Nothing mixes 4KB/2MB/1GB/16KB(base on arm64) mappings interleaved at random VAs.
5. **Deliberately non-page-aligned huge mappings + sub-page misalignment at scale.** misaligned is capped at 2 pages; no stressor combines misaligned access with large mappings (which is where DRAM row/bank + TLB entry-attribute interactions live).
6. **Sparse huge mappings (guarded holes).** vm-addr/mmap touch every page; mmaprandom's split-hole creates 1-page holes only. No multi-GB mappings with random guard gaps and cross-gap data patterns (catches off-by-one TLB/PT silicon).
7. **Physical discontiguity at random orders.** `stress_mmap_discontiguous` uses a fixed size menu (1GB..4KB) with every-other-chunk touching — deterministic, and only reachable via `--vm-discontiguous`/`--memrate-discontiguous`/`--stream-discontiguous` flags.
8. **malloc'd gigabytes.** malloc-bytes maxes at MAX_MEM_LIMIT but default 64KB per allocation and malloc-max default 64 concurrent — the OOM-oriented large path (bigheap) uses realloc, never random-sized multi-GB mallocs with random alignments.

## 5. Physical page-level coverage gaps

- **Pagemap feedback exists but is underused**: `stress_mmap_stats()` reads `/proc/self/pagemap` (PFN, present, swapped, exclusive, soft-dirty) and reports % physically contiguous — used by vm, memrate, stream, mmap. No stressor *steers* allocation based on PFN patterns (e.g. seek specific DRAM row/bank physical addresses or force non-contiguous PFN sequences).
- **NUMA randomization is chunk-halving** (`stress_numa_randomize_pages`), not per-page random — physical interleave patterns are coarse.
- **Page migration** only via mbind/`process_madvise` in mmaprandom and mbind in tlb-numa — no `move_pages()`-based random migration sweep (pagemove is deterministic rotation).
- **THP collapse/split** only in hugepage (per-hugepage DONTNEED+rebuild) and mmaprandom's MADV_COLLAPSE via process_madvise; no mixed-order THP churn at random offsets in big buffers.
- **Swap-path verification**: pageswap forces MADV_PAGEOUT but stores only a self-pointer per page (1 word/page); vm detects swapped pages via stats but data patterns and swap-out are not correlated at random offsets.
- **Rowhammer** (vm method) picks 2 random addresses per pass but masks them with the page mask (`& mask` on an index into uint32 elements — the two hammered words are page-locked to the same page offset class), and 1M-loop budget per pass; no addresses chosen for same-bank/different-row physical adjacency (no pagemap-guided pairing).

## 6. Infrastructure available to build on

- **RNG**: `stress_mwc32/64/modn/seed_set` everywhere; mmaprandom even reseeds per-child to avoid identical sequences (stress-mmaprandom.c:2251-2253).
- **Mapped-rejection probing**: mmapaddr's mincore loop, pagescatter's mincore+madvise+pipe triple check, vma's pipe-write EFAULT probe — reusable primitives for "pick a random unmapped address".
- **`MAP_FIXED_NOREPLACE`** adopted in mmaprandom/pagescatter/vma/mmapaddr — safe random fixed-address placement.
- **mask ladders**: mmaprandom `masks_64bit[]` (21 spans), vma's `addr_bits = 32..59`, mmapfixed's `rndaddr_base & mask` loop — ready-made bit-span walkers.
- **Verification harness**: stressor `VERIFY_*` flags + `stress_mmap_set/check` and vm's bit-error accounting (shared `bit_error_count` page) — SDC detection plumbing already exists per-stressor.
- **Metrics**: `stress_mmap_stats_report` (contiguous/swapped/dirtied %), `stress_metrics_set` — a new address-shape stressor gets reporting for free.
- **Missing/absent**: no `stress_mmap_adopt` in this tree; no global `--random-size/--random-address` option in core-opts.c; `--mmap-madvise` randomization is opt-in via global flag only.
