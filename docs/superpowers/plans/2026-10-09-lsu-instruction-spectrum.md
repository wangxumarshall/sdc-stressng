# LSU 全指令谱 SDC 激发引擎 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 spec 建成 arm64 LSU 全指令谱激发面：memcpy 6 个指令变体 + 新 stressor `--lsupress`（地址引擎 × 访存内核，20 方法）+ excite 2.0 时间片轮换。

**Architecture:** 三层——层 1 扩展上游 `--memcpy-method` 方法表（手写指令内核挂 `memcpy_check` 对拍）；层 2 新建 `stress-lsupress.c`（NORESERVE 最大 VA 映射 + 工作集窗口迁移的地址引擎，驱动 20 个指令谱方法内核）；层 3 重构 `sdc-run.sh excite` 为时间片轮岗。

**Tech Stack:** C99 + GCC vector extension（NEON/SVE）+ inline asm（独占对/ls64）+ arm_sve.h（target 属性隔离）；bash（编排）。

**Spec:** `docs/superpowers/specs/2026-10-09-lsu-instruction-spectrum-design.md`

## Global Constraints（每 task 隐含遵守）

- 方法表 0 基分派，`methods[0]` 必须是 "all"；方法名全小写连字符。
- SVE/SVE2 代码必须 `#if defined(STRESS_ARCH_ARM) && defined(__GNUC__) && __GNUC__ >= 10` 门控（gcc7.3 兼容），运行时 HWCAP 检查 + 诚实跳过。
- ls64 特性双查（HWCAP2 bit15 / AT_HWCAP3 bit0，复用 stress-ls64.c 的探测函数）。
- verify oracle 确定性：f(seed, addr) = splitmix64 类哈希；FP 模板内核与参考同用显式 mul+add。
- 每次 commit 前 `make -j$(nproc)` rc=0 零新告警；SVE 方法过 QEMU（`/tmp/qemu-out/qemu-aarch64 -cpu max`，需 `LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64`）；objdump 指令验收门（目标指令计数 > 0）。
- 每 task 一个 commit，message 格式 `lsupress: ...` / `memcpy: ...`（历史惯例小写域前缀）。
- man 条目进 `stress-ng.1`（不是 .in）。

## Review Focus（spec 隐含但测试未覆盖的失败模式 → 已钉进对应 task）

1. **非对齐地址上的 ldp/ld1 内核**（上游 memcpy 会传 +64/+1 偏移指针）→ 功能必须仍正确（ARM64 非对齐访问合法）→ Task 1 的对拍测试天然覆盖（上游 memcpy_check 用 misaligned 调用模式），验收命令含 `--verify`。
2. **NORESERVE 窗口迁移时 MADV_DONTNEED 失败**（非致命，页表残留）→ 不 abort，仅 debug 日志 → Task 4 显式容忍 + 验收。
3. **多 worker 同时迁移窗口 → RSS 超限 OOM**（窗口×worker 是硬顶）→ Task 4 的 RSS 验收（运行时 /proc/self/status VmRSS < 窗口+余量）。
4. **SVE 方法在 920（无 SVE）运行**→ 必须诚实跳过不 SIGILL → Task 6 验收：`--lsupress 2 --lsupress-method load-sve` 在本机 skipped。
5. **verify f() 在窗口重 touch 后数据已 DONTNEED**（校验窗口内数据在 DONTNEED 前完成）→ 窗口迁移只在窗口数据处理完后触发 → Task 9 的注入实验覆盖（注入后 verify 必须报失配，含迁移发生的运行）。

---

### Task 1: memcpy 变体 — NEON 族（ldp-stp / neon / neon-ld2）

**Files:**
- Modify: `stress-memcpy.c`（方法实现区 + 方法表）
- Modify: `stress-ng.1`（--memcpy-method 新方法名）

**Interfaces:**
- Consumes: 上游 `stress_memcpy_func` 签名 `void (*)(uint8_t *, uint8_t *, uint8_t *)`；`memcpy_check(func, dest, src, n)` 包装；`MEMCPY_LOOPS`/`MEMCPY_MEMSIZE` 常量。
- Produces: 方法表三项 `{ "ldp-stp", stress_memcpy_ldp_stp }` 等，供 `--memcpy-method` 分派。

- [ ] **Step 1: 写内核实现**（`stress-memcpy.c`，插在 `stress_memcpy_libc` 之后；整段 `#if defined(STRESS_ARCH_ARM)` 门控）

```c
#if defined(STRESS_ARCH_ARM)
/*
 *  arm64 instruction-spectrum memcpy variants (SDC fork).
 *  Each kernel is a hand-written load/store loop; correctness is
 *  checked by the shared memcpy_check() memcmp wrapper, including
 *  the misaligned offset calls the upstream loop performs.
 */
static NOINLINE OPTIMIZE3 void *memcpy_ldp_stp(
	void *dest, const void *src, size_t n)
{
	uint64_t *d = (uint64_t *)dest;
	const uint64_t *s = (const uint64_t *)src;
	size_t i, pairs = n / 16;
	const uint8_t *s8;
	uint8_t *d8;

	for (i = 0; i < pairs; i++) {
		register uint64_t a, b;
		__asm__ __volatile__ (
			"ldp %0, %1, [%2], #16"
			: "=r" (a), "=r" (b), "+r" (s)
			:
			: "memory");
		__asm__ __volatile__ (
			"stp %0, %1, [%2], #16"
			:
			: "r" (a), "r" (b), "r" (d)
			: "memory");
	}
	s8 = (const uint8_t *)s;
	d8 = (uint8_t *)d;
	for (i = 0; i < (n & 15); i++)
		*d8++ = *s8++;
	return dest;
}

static NOINLINE void stress_memcpy_ldp_stp(
	uint8_t *str1, uint8_t *str2, uint8_t *str3)
{
	int i;

	s_method_name = "ldp-stp";
	for (i = 0; memcpy_okay && (i < MEMCPY_LOOPS); i++) {
		(void)memcpy_check(memcpy_ldp_stp, str3, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_ldp_stp, str2, str3, MEMCPY_MEMSIZE / 2);
		(void)memcpy_check(memcpy_ldp_stp, str1, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_ldp_stp, str3 + 1, str3, MEMCPY_MEMSIZE - 1);
		(void)memcpy_check(memcpy_ldp_stp, str3, str1, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_ldp_stp, str3 + 64, str3, MEMCPY_MEMSIZE - 64);
	}
}

static NOINLINE OPTIMIZE3 void *memcpy_neon(
	void *dest, const void *src, size_t n)
{
	/* uint8x16_t copies compile to ldr q / str q (128-bit NEON) */
	uint8x16_t *d = (uint8x16_t *)dest;
	const uint8x16_t *s = (const uint8x16_t *)src;
	size_t i, vec = n / 16;
	const uint8_t *s8 = (const uint8_t *)(s + vec);
	uint8_t *d8 = (uint8_t *)(d + vec);

	for (i = 0; i < vec; i++)
		d[i] = s[i];
	for (i = 0; i < (n & 15); i++)
		*d8++ = *s8++;
	return dest;
}

static NOINLINE void stress_memcpy_neon(
	uint8_t *str1, uint8_t *str2, uint8_t *str3)
{
	int i;

	s_method_name = "neon";
	for (i = 0; memcpy_okay && (i < MEMCPY_LOOPS); i++) {
		(void)memcpy_check(memcpy_neon, str3, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon, str2, str3, MEMCPY_MEMSIZE / 2);
		(void)memcpy_check(memcpy_neon, str1, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon, str3 + 1, str3, MEMCPY_MEMSIZE - 1);
		(void)memcpy_check(memcpy_neon, str3, str1, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon, str3 + 64, str3, MEMCPY_MEMSIZE - 64);
	}
}

static NOINLINE OPTIMIZE3 void *memcpy_neon_ld2(
	void *dest, const void *src, size_t n)
{
	/* vld2q/vst2q: interleaved 2-register load/store (multi-issue LSU) */
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;
	size_t i, chunks = n / 32;

	for (i = 0; i < chunks; i++, s += 32, d += 32) {
		uint8x16x2_t v = vld2q_u8(s);
		vst2q_u8(d, v);
	}
	for (i = 0; i < (n & 31); i++)
		d[i] = s[i];
	return dest;
}

static NOINLINE void stress_memcpy_neon_ld2(
	uint8_t *str1, uint8_t *str2, uint8_t *str3)
{
	int i;

	s_method_name = "neon-ld2";
	for (i = 0; memcpy_okay && (i < MEMCPY_LOOPS); i++) {
		(void)memcpy_check(memcpy_neon_ld2, str3, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon_ld2, str2, str3, MEMCPY_MEMSIZE / 2);
		(void)memcpy_check(memcpy_neon_ld2, str1, str2, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon_ld2, str3 + 1, str3, MEMCPY_MEMSIZE - 1);
		(void)memcpy_check(memcpy_neon_ld2, str3, str1, MEMCPY_MEMSIZE);
		(void)memcpy_check(memcpy_neon_ld2, str3 + 64, str3, MEMCPY_MEMSIZE - 64);
	}
}
#endif
```

同时在文件头部 include 区加（arm64 门控内）：`#include <arm_neon.h>`。

- [ ] **Step 2: 方法表注册**（`stress_memcpy_methods[]`，arm64 门控下加三项）

```c
#if defined(STRESS_ARCH_ARM)
	{ "ldp-stp",	stress_memcpy_ldp_stp },
	{ "neon",	stress_memcpy_neon },
	{ "neon-ld2",	stress_memcpy_neon_ld2 },
#endif
```

- [ ] **Step 3: 构建**

Run: `make -j$(nproc) 2>&1 | tail -5`
Expected: rc=0，无新 warning。

- [ ] **Step 4: 功能验收（含 misaligned 路径——Review Focus #1）**

```bash
./stress-ng --memcpy 2 --memcpy-method ldp-stp --verify -t 10
./stress-ng --memcpy 2 --memcpy-method neon --verify -t 10
./stress-ng --memcpy 2 --memcpy-method neon-ld2 --verify -t 10
```
Expected: 三条全部 `failed: 0`（memcpy_check 的 misaligned 调用模式自动覆盖 +1/+64 偏移）。

- [ ] **Step 5: objdump 指令验收门**

```bash
objdump -d stress-ng | grep -c $'\tldp\tx' ; objdump -d stress-ng | grep -c $'\tstp\tx'
objdump -d stress-ng | grep -c 'ldr\tq' ; objdump -d stress-ng | grep -c 'ld2\t{v'
```
Expected: 各计数 > 0（ldp/stp、q 寄存器、ld2 交错指令确实生成）。

- [ ] **Step 6: man 条目 + commit**

`stress-ng.1` 的 --memcpy-method 段补三个方法名与一句话说明。然后：

```bash
git add stress-memcpy.c stress-ng.1
git commit -m "memcpy: arm64 ldp-stp/neon/neon-ld2 variants (LSU pair and multi-issue paths)"
```

---

### Task 2: memcpy 变体 — sve / sve-gather / ls64

**Files:**
- Modify: `stress-memcpy.c`、`stress-ng.1`

**Interfaces:**
- Consumes: Task 1 的门控模式；stress-ls64.c 的 ls64 `.inst` 编码与 HWCAP 探测（复制其宏）。
- Produces: 方法表三项 `sve`/`sve-gather`/`ls64`；静态探测 helper `lsupress_has_sve()`（Task 6 复用——命名约定见代码）。

- [ ] **Step 1: 写内核**（stress-memcpy.c，`#if defined(STRESS_ARCH_ARM)` 内继续追加）

```c
#if defined(__GNUC__) && defined(__GNUC__) && __GNUC__ >= 10
static inline bool stress_memcpy_has_sve(void)
{
	return (stress_get_cpuinfo_features() & STRESS_CPUINFO_SVE) != 0;
}

static bool memcpy_sve_ok;

__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE void *memcpy_sve(
	void *dest, const void *src, size_t n)
{
	/* full-VL ld1d/st1d loop; svcntd() adapts to the runtime VL */
	svuint64_t *d = (svuint64_t *)dest;
	const svuint64_t *s = (const svuint64_t *)src;
	const uint64_t vl = (uint64_t)svcntd();	/* 64-bit lanes per vector */
	size_t i, vecs = n / (vl * 8);
	const uint8_t *s8 = (const uint8_t *)(s + vecs);
	uint8_t *d8 = (uint8_t *)(d + vecs);
	size_t rem = n - vecs * vl * 8;

	for (i = 0; i < vecs; i++)
		d[i] = s[i];
	for (i = 0; i < rem; i++)
		d8[i] = s8[i];
	return dest;
}

__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE void *memcpy_sve_gather(
	void *dest, const void *src, size_t n)
{
	/*
	 *  Gather/scatter copy: read 64-bit elements at strided
	 *  addresses via ld1d gather, write them contiguously via
	 *  st1d scatter — the SVE-only non-contiguous LSU path.
	 */
	const uint64_t vl = (uint64_t)svcntd();
	const size_t elems = n / 8;
	svbool_t pg = svptrue_b64();
	svuint64_t idx;
	uint64_t base = (uint64_t)src, k = 0;
	size_t i, batches = elems / vl;
	uint64_t *d = (uint64_t *)dest;
	svuint64_t v;

	for (i = 0; i < batches; i++) {
		/* indices: elements spaced 64B apart (cross-cacheline) */
		idx = svindex_u64(0, 64 / 8 * vl * 0 + 64);	/* 64B stride */
		(void)base;
		v = svld1_gather_u64index_u64(pg, (const uint64_t *)src + k, idx);
		svst1_scatter_u64index_u64(pg, d + k, idx, v);
		k += vl;
	}
	/* tail: contiguous copy */
	for (i = batches * vl; i < elems; i++)
		d[i] = ((const uint64_t *)src)[i];
	return dest;
}
#endif
```

注意：gather 版的 src/dst 都按 vl 元素一批处理；若 n 不是 8 的倍数，尾部用 memcpy 处理——保持语义等价拷贝（对拍要求）。实现时若 index 步长与 src+k 组合不能精确覆盖 n，把 gather 循环限制在 n/8/vl*vl 元素内，其余走逐元素（上面代码已按此写）。**若 gather 语义与 memcmp 对拍存在等价性疑问（gather 读 src+k 的 64B 间隔元素、scatter 写回同样间隔——只要读写索引一致，dest 内容与 src 一致）**：是的，读写同一 idx 集合 → dest[i]=src[i] 对所有被覆盖 i 成立 ✓。

ls64 内核（复用 stress-ls64.c 的 `.inst` 编码常量，此处引用其数值——执行时从 stress-ls64.c 拷贝准确的 .inst 行）：

```c
/* ld64b/st64b via .inst (no GCC intrinsics) — encodings copied
 * verbatim from stress-ls64.c */
static NOINLINE void *memcpy_ls64(
	void *dest, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;
	size_t i, blocks = n / 64;
	uint64_t t0, t1, t2, t3, t4, t5, t6, t7;

	for (i = 0; i < blocks; i++, s += 64, d += 64) {
		__asm__ __volatile__ (
			".inst 0xd9a00000 + 0"		/* ld64b t0,[x] */
			...				/* 执行时从 stress-ls64.c 拷贝完整 8 寄存器序列 */
			: "=r"(t0), "=r"(t1), "=r"(t2), "=r"(t3),
			  "=r"(t4), "=r"(t5), "=r"(t6), "=r"(t7), "+r"(s)
			:
			: "memory");
		__asm__ __volatile__ (
			".inst 0xd9800000"		/* st64b */
			...
			: : "r"(t0), "r"(t1), "r"(t2), "r"(t3),
			    "r"(t4), "r"(t5), "r"(t6), "r"(t7), "r"(d)
			: "memory");
	}
	for (i = 0; i < (n & 63); i++)
		d[i] = s[i];
	return dest;
}
```

（执行者注意：`...` 处用 stress-ls64.c 里已验证的完整 `.inst` 序列替换，两文件编码一致——这是"拷贝先例"而非占位；stress-ls64.c 是唯一权威来源。）

三个 `stress_memcpy_*` 包装函数照 Task 1 模式（s_method_name、MEMCPY_LOOPS 循环、memcpy_check 六连调用）；`sve`/`sve-gather` 包装开头加：

```c
	if (!memcpy_sve_ok) {	/* 初始化时 stress_memcpy_has_sve() 探测 */
		/* feature absent: skip this loop honestly */
		return;
	}
```

方法表加（ls64 项同门控）：

```c
#if defined(STRESS_ARCH_ARM) && __GNUC__ >= 10
	{ "sve",	stress_memcpy_sve },
	{ "sve-gather",	stress_memcpy_sve_gather },
#endif
#if defined(STRESS_ARCH_ARM)
	{ "ls64",	stress_memcpy_ls64 },
#endif
```

- [ ] **Step 2: 构建** `make -j$(nproc)` → rc=0。

- [ ] **Step 3: 本机验收（920 无 SVE/ls64 → 诚实跳过路径）**

```bash
./stress-ng --memcpy 2 --memcpy-method sve -t 5      # 本机预期 skipped or 空转
./stress-ng --memcpy 2 --memcpy-method ls64 -t 5
```
Expected: 无 SIGILL、无 unrecognised option；诚实跳过或零失配完成（Review Focus #4 同型）。

- [ ] **Step 4: QEMU 验收（SVE 全功能）**

```bash
LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64 /tmp/qemu-out/qemu-aarch64 -cpu max \
	./stress-ng --memcpy 2 --memcpy-method sve --verify -t 20
LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64 /tmp/qemu-out/qemu-aarch64 -cpu max \
	./stress-ng --memcpy 2 --memcpy-method sve-gather --verify -t 20
```
Expected: `failed: 0`（含 misaligned 对拍调用）。

- [ ] **Step 5: objdump 门**：`objdump -d stress-ng | grep -c 'ld1d'` > 0；grep st64b 计数 > 0。

- [ ] **Step 6: man + commit**

```bash
git add stress-memcpy.c stress-ng.1
git commit -m "memcpy: arm64 sve/sve-gather/ls64 variants (full-VL and gather LSU paths)"
```

---

### Task 3: lsupress 骨架 — stressor 框架 + 3 个基础方法

**Files:**
- Create: `stress-lsupress.c`
- Modify: `Makefile`（OBJ 列表加 `stress-lsupress.o`）
- Modify: `stress-ng.h`（`stressor_info_t` 声明区 + OPT 枚举：`OPT_lsupress_method` 等 5 个）
- Modify: `stress-ng.c`（stressor 表注册 `stress_lsupress_info`——按 addrspace/operand-var 先例的位置）
- Modify: `stress-ng.1`

**Interfaces:**
- Consumes: `stress_args_t`、`stress_bogo_inc`、`stress_continue`、`OPTIMIZE3`/`NOINLINE`、`stress_mwc32`。
- Produces: `stress_lsupress_info`；方法分派骨架；`lsupress_value(seed, addr)`（Task 9 verify 用）；选项 5 个（method/walk/va-size/window/huge/align——本 task 先注册 method，其余 Task 4/8 注册）；方法内核统一签名 `void (*)(stress_args_t *, uint64_t *buf, size_t buf_words)`（本 task 常规 malloc buffer，Task 4 换地址引擎窗口指针）。

- [ ] **Step 1: stressor 主文件**（核心骨架；方法实现本 task 只放 3 个基础）

```c
/*
 *  Copyright (C) 2026
 *  ... GPL header 照 stress-addrspace.c 抄 ...
 */
#include "stress-ng.h"
#include "core-bitgen.h"

#if defined(STRESS_ARCH_ARM)

#define LSUPRESS_BUF_WORDS	(1024 * 1024)	/* 8MB per worker, Task4 前的常规缓冲 */

/* deterministic per-address value (verify oracle, spec §5.4) */
static inline uint64_t ALWAYS_INLINE lsupress_value(
	const uint64_t seed,
	const uint64_t addr)
{
	uint64_t z = seed + (addr * 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

typedef void (*stress_lsupress_func_t)(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed);

typedef struct {
	const char *name;
	const stress_lsupress_func_t func;
} stress_lsupress_method_info_t;

static const char *stress_lsupress_method(const size_t i);
static void stress_lsupress_method_list(void);

/*
 *  load-int64: 8 independent back-to-back loads per iteration,
 *  consumed into a checksum (dead-store-proof)
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_load_int64(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t sum = seed;
	size_t i;

	(void)seed;
	do {
		for (i = 0; i < buf_words; i += 8) {
			sum ^= buf[i] ^ buf[i + 1] ^ buf[i + 2] ^ buf[i + 3] ^
			       buf[i + 4] ^ buf[i + 5] ^ buf[i + 6] ^ buf[i + 7];
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero(sum);	/* prevent dead-code elimination */
}

/*  store-int64: bitgen-shaped stream of stores */
static NOINLINE OPTIMIZE3 void stress_lsupress_store_int64(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	stress_bitgen_t bg;
	size_t i;

	stress_bitgen_init(&bg, seed);
	do {
		for (i = 0; i < buf_words; i++)
			buf[i] = stress_bitgen_u64(&bg);
		stress_bogo_inc(args);
	} while (stress_continue(args));
}

/*  copy-int64: ldr+str dual */
static NOINLINE OPTIMIZE3 void stress_lsupress_copy_int64(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	uint64_t *src = buf;
	uint64_t *dst = buf + buf_words / 2;
	size_t i;

	(void)seed;
	do {
		for (i = 0; i < buf_words / 2; i++)
			dst[i] = src[i];
		stress_bogo_inc(args);
	} while (stress_continue(args));
}

#define LSUPRESS_METHOD_ALL	0

static stress_lsupress_method_info_t lsupress_methods[] = {
	{ "all",		NULL },				/* 0: rotate */
	{ "load-int64",		stress_lsupress_load_int64 },
	{ "store-int64",	stress_lsupress_store_int64 },
	{ "copy-int64",		stress_lsupress_copy_int64 },
};

static const char *stress_lsupress_method(const size_t i)
{
	return (i < SIZEOF_ARRAY(lsupress_methods)) ? lsupress_methods[i].name : NULL;
}

static void stress_lsupress_method_list(void)
{
	size_t i;

	for (i = 1; i < SIZEOF_ARRAY(lsupress_methods); i++)
		(void)fprintf(stderr, " %-22s : %s\n",
			lsupress_methods[i].name, "see stress-ng.1");
	(void)fprintf(stderr, " %-22s : %s\n",
		lsupress_methods[0].name, "all the above methods (rotating)");
}

static const stress_opt_t opts[] = {
	{ OPT_lsupress_method, "lsupress-method", TYPE_ID_SIZE_T_METHOD, 0, 0, stress_lsupress_method },
	END_OPT,
};

static const stress_help_t help[] = {
	{ NULL,	"lsupress N",	  "start N workers exercising the arm64 load/store" },
	{ NULL,	"		  unit across the full instruction spectrum" },
	{ NULL,	"lsupress-method M", "method (see --lsupress-method 0 for list)" },
	END_HELP,
};

/*
 *  "all": rotate over methods every 16M bogo-ops to cover the
 *  spectrum inside a single worker run
 */
static int stress_lsupress(const stress_args_t *args)
{
	size_t lsupress_method = 0;
	stress_lsupress_func_t func;
	uint64_t *buf;
	const uint64_t seed = stress_mwc64();
	size_t method_max;

	(void)stress_get_setting("lsupress-method", &lsupress_method);

	method_max = SIZEOF_ARRAY(lsupress_methods);
	if (lsupress_method >= method_max) {
		(void)fprintf(stderr, "%s: method must be in range [0,%zu]\n",
			args->name, method_max - 1);
		return EXIT_FAILURE;
	}
	if (lsupress_method == LSUPRESS_METHOD_ALL)
		lsupress_method = 1 + (stress_mwc32() % (method_max - 1));

	func = lsupress_methods[lsupress_method].func;

	buf = stress_mmap_populate(NULL,
		LSUPRESS_BUF_WORDS * sizeof(uint64_t),
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE,
		-1, 0);
	if (buf == MAP_FAILED) {
		pr_inf("%s: mmap of working buffer failed: %d (%s)%s\n",
			args->name, errno, strerror(errno),
			stress_get_zeroed_pages() ?
				" (try --page-min)" : "");
		return EXIT_NO_RESOURCE;
	}
	stress_set_vma_anon_name(buf, LSUPRESS_BUF_WORDS * sizeof(uint64_t), "lsupress-buffer");

	func(args, buf, LSUPRESS_BUF_WORDS, seed);

	(void)munmap(buf, LSUPRESS_BUF_WORDS * sizeof(uint64_t));
	return EXIT_SUCCESS;
}

const stressor_info_t stress_lsupress_info = {
	.stressor = stress_lsupress,
	.class = CLASS_CPU | CLASS_MEMORY,
	.opts = opts,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
#else
const stressor_info_t stress_lsupress_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_CPU | CLASS_MEMORY,
	.opts = opts,
	.help = "lsupress"
};
#endif
```

（执行者注意：`opts` 在 `#else` 分支引用不到——把 `opts` 定义移到两个分支之外（static const，未用 warning 用 stress-ng.h 惯例处理），或 `#else` 分支 `.opts = NULL`。照 stress-addrspace.c 的非 arm64 处理先例。）

- [ ] **Step 2: 注册接线**

- `stress-ng.h`：枚举 `OPT_lsupress_method` 加到 fork 选项枚举区（operand-var 先例旁）；`STRESSOR_LSUPRESS` 声明位照 addrspace。
- `stress-ng.c`：stressor 表加 `{ "lsupress",	&stress_lsupress_info,	CLASS_CPU | CLASS_MEMORY,	STRESSOR_LSUPRESS }`（照 addrspace 行）。
- `Makefile`：`stress-lsupress.o` 加进 OBJ 列表（字母序 stress-ls64.o 附近）。

- [ ] **Step 3: 构建 + 验收**

```bash
make -j$(nproc)        # rc=0
./stress-ng --lsupress 2 --lsupress-method load-int64 -t 10
./stress-ng --lsupress 2 --lsupress-method store-int64 -t 10
./stress-ng --lsupress 2 --lsupress-method copy-int64 -t 10
./stress-ng --lsupress-method 0    # 方法列表可打印
```
Expected: 全部 successful run、`--lsupress 2 --lsupress-method all` 也能跑。

- [ ] **Step 4: man + commit**

```bash
git add stress-lsupress.c stress-ng.h stress-ng.c Makefile stress-ng.1
git commit -m "lsupress: new stressor skeleton — arm64 LSU instruction spectrum, int64 load/store/copy methods"
```

---

### Task 4: lsupress 地址引擎（NORESERVE 最大 VA + 窗口迁移 + uniform 游走）

**Files:**
- Modify: `stress-lsupress.c`、`stress-ng.h`（OPT_lsupress_va_size/OPT_lsupress_window）、`stress-ng.1`

**Interfaces:**
- Consumes: Task 3 的 `stress_lsupress_func_t`（签名不变——`buf` 参数从常规 buffer 变为引擎窗口指针）。
- Produces: `lsupress_engine_t` + `lsupress_engine_init/next_window/free`；`--lsupress-va-size`/`--lsupress-window` 选项。后续所有方法内核无需感知引擎（窗口指针透明传入）。

- [ ] **Step 1: 引擎实现**（stress-lsupress.c，替换 Task 3 的静态 mmap buffer）

```c
#define LSUPRESS_DEFAULT_VA_SIZE	(100ULL << 30)	/* 100GB per worker map */
#define LSUPRESS_DEFAULT_WINDOW	(1ULL << 32)	/* 4GB working window */
#define LSUPRESS_WINDOW_SHIFT	12		/* page granularity walk */

typedef struct {
	uint8_t *map_base;	/* MAP_NORESERVE region base */
	size_t   map_size;	/* actual mapped size (probe-shrunk) */
	uint8_t *win;		/* current window (page aligned) */
	size_t   win_size;
	uint64_t seed;		/* walk RNG */
	uint64_t state;		/* xorshift state for window hops */
} lsupress_engine_t;

static inline uint64_t lsupress_next_hop(lsupress_engine_t *e)
{
	/* xorshift64*: uniform window position */
	uint64_t x = e->state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	e->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static int lsupress_engine_init(
	lsupress_engine_t *e,
	uint64_t va_req,		/* --lsupress-va-size */
	uint64_t win_req,		/* --lsupress-window */
	const uint64_t seed)
{
	size_t size = (size_t)va_req;

	e->seed = seed;
	e->state = seed | 1;
	e->win_size = (size_t)win_req;
	/* probe-shrink: 100G -> ... -> window*2 minimum */
	while (size >= e->win_size * 2) {
		e->map_base = stress_mmap_populate(NULL, size,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
			-1, 0);
		if (e->map_base != MAP_FAILED)
			break;
		size /= 2;
	}
	if (e->map_base == MAP_FAILED)
		return -1;
	e->map_size = size;
	e->win = e->map_base;
	return 0;
}

/*
 *  Migrate the working window: pick a fresh page-aligned position
 *  inside the map, MADV_DONTNEED the old window (constant RSS,
 *  page-table churn = MMU pressure).  Failure of madvise is
 *  non-fatal (Review Focus #2): pages stay resident, we log once.
 */
static void lsupress_engine_next_window(lsupress_engine_t *e)
{
	const size_t hops = e->map_size / e->win_size;
	size_t hop = (size_t)(lsupress_next_hop(e) % hops);

	if (hops > 1) {
		(void)madvise(e->win, e->win_size, MADV_DONTNEED);
		e->win = e->map_base + (hop * e->win_size);
	}
}

static void lsupress_engine_free(lsupress_engine_t *e)
{
	if (e->map_base && e->map_base != MAP_FAILED)
		(void)munmap(e->map_base, e->map_size);
}
```

`stress_lsupress()` 主流程改造：init 引擎（失败回退静态 buffer 并 pr_inf 提示）→ 内核循环改为带窗口迁移：

```c
	/* window migration cadence: every ~window/8 of bogo work */
	do {
		func(args, (uint64_t *)e.win, e.win_size / sizeof(uint64_t), seed);
		lsupress_engine_next_window(&e);
	} while (stress_continue(args));
```

（窗口迁移在内核返回之间发生 → 窗口内数据处理完成后才 DONTNEED，Review Focus #5 的时序由结构保证。）

- [ ] **Step 2: 选项注册**（va-size/window，TYPE_ID_UINT64_BYTES_SIZE + 尺寸解析照 addrspace-bytes 先例）+ man。

- [ ] **Step 3: 构建 + RSS 硬顶验收（Review Focus #3）**

```bash
make -j$(nproc)
./stress-ng --lsupress 4 --lsupress-va-size 8g --lsupress-window 256m -t 30 --metrics &
# 另一终端每 5s 采样：
while sleep 5; do grep VmRSS /proc/$(pgrep -f 'lsupress' | head -1)/status; done
```
Expected: 4 worker 各 RSS 稳定在 ~256MB+ε（窗口迁移后不涨）；run successful。

- [ ] **Step 4: madvise 容错验收**：`strace -f -e madvise ./stress-ng --lsupress 1 --lsupress-window 64m -t 20 2>&1 | grep -c MADV_DONTNEED` > 0（迁移确实发生）。

- [ ] **Step 5: commit**

```bash
git add stress-lsupress.c stress-ng.h stress-ng.1
git commit -m "lsupress: max-VA address engine — NORESERVE map, migrating working window, uniform walk"
```

---

### Task 5: lsupress mix 模板族（asm 精确内核）

**Files:** Modify `stress-lsupress.c`、`stress-ng.1`

**Interfaces:**
- Consumes: Task 4 引擎窗口（`buf` 即窗口基址，内核在窗口内以 64B 步长扫掠 + 窗口内随机偏移轮换——misalign 形状在窗口内实现，本 task 用固定 misalign 轮换（i%7+1 偏移），Task 8 才做成选项）。
- Produces: 3 个 mix 方法表项。

- [ ] **Step 1: 内核**（用户点名的模板）

```c
/*
 *  mix-2l-alu-1s: the user-named dataflow template.
 *  Per 64B block: 2 loads, ALU, 1 load, store — max load-use
 *  forwarding + store-merge pressure.  Inner loop is hand asm so
 *  the instruction stream is exactly: ldr,ldr,add,ldr,add,str
 *  with post-increment addressing (zero non-memory instructions
 *  beyond the adds).
 */
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_2l_alu_1s(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const uint64_t *a = (const uint64_t *)buf;	/* window src */
	uint64_t *b = (uint64_t *)((uint8_t *)buf + buf_words * sizeof(uint64_t) / 2);
	size_t blocks = buf_words / 16;			/* 8 src + 8 dst words per block */
	size_t i, off = (seed % 7) + 1;		/* misalign rotation seed */
	uint64_t acc = 0;

	do {
		for (i = 0; i < blocks; i++) {
			register uint64_t x0, x1, x2;
			const uint64_t *src = a + (i * 16) + off;
			uint64_t *dst = b + (i * 16) + off;

			__asm__ __volatile__ (
				"ldr %0, [%3], #8\n"
				"ldr %1, [%3], #8\n"
				"add %0, %0, %1\n"
				"ldr %2, [%3], #8\n"
				"add %0, %0, %2\n"
				"str %0, [%4], #8"
				: "=r"(x0), "=r"(x1), "=r"(x2),
				  "+r"(src), "+r"(dst)
				:
				: "memory");
			acc ^= x0;
		}
		stress_bogo_inc(args);
		off = (off % 7) + 1;			/* rotate misalignment */
	} while (stress_continue(args));
	stress_uint64_zero(acc);
}
```

mix-1l-fpu-1s（fp64，显式 mul+add——verify 位精确前提）：

```c
static NOINLINE OPTIMIZE3 void stress_lsupress_mix_1l_fpu_1s(
	stress_args_t *args,
	uint64_t *buf,
	const size_t buf_words,
	const uint64_t seed)
{
	const double *a = (const double *)buf;
	double *b = (double *)((uint8_t *)buf + buf_words * sizeof(uint64_t) / 2);
	size_t i, n = buf_words / 2;
	double c = 1.0000000000000002;	/* FP accumulate, no FMA ambiguity */
	double acc = 0.0;

	(void)seed;
	do {
		for (i = 0; i < n; i++) {
			double v = a[i] * c;	/* ldr d / fmul */
			acc += v;		/* fadd */
			b[i] = acc;		/* str d */
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_double_zero(acc);
}
```

mix-3l-2alu-1s 照 mix-2l 模式扩展（3 组 ldr + 2 add + str 的 asm 块——把上面 asm 改为 `ldr,ldr,ldr,add,add,str` 六行，寄存器三个输入）。

方法表追加三项（"all" 轮换自动覆盖）。

- [ ] **Step 2: 构建 + objdump 指令序列验收**

```bash
make -j$(nproc)
objdump -d stress-ng | grep -A8 'stress_lsupress_mix_2l' | grep -cE 'ldr|add|str'
```
Expected: mix_2l_alu_1s 内核体内恰好 5 条访存/ALU 指令形态（无多余 mov）。

- [ ] **Step 3: 功能验收**

```bash
./stress-ng --lsupress 2 --lsupress-method mix-2l-alu-1s -t 10
./stress-ng --lsupress 2 --lsupress-method mix-1l-fpu-1s -t 10
./stress-ng --lsupress 2 --lsupress-method mix-3l-2alu-1s -t 10
```
Expected: successful run（无 SIGSEGV——misalign 轮换在窗口内，off≤7 保证不越窗口尾；blocks 计算保守取 buf_words/16 已留 8 字余量）。

- [ ] **Step 4: commit**

```bash
git add stress-lsupress.c stress-ng.1
git commit -m "lsupress: mix dataflow templates — 2load+ALU+load+store, FPU, deep-load variants"
```

---

### Task 6: lsupress 向量方法族（neon/sve load、store、copy、fma 模板、fp128）

**Files:** Modify `stress-lsupress.c`（+`#include <arm_neon.h>`，SVE 段 target 属性）、`stress-ng.1`

**Interfaces:**
- Consumes: Task 2 的 `stress_memcpy_has_sve()` 模式（移成共享 static helper——本文件内再定义一份 `lsupress_has_sve()`，避免跨 stressor 依赖）。
- Produces: 方法表 9 项：load-int128/load-fp64/load-neon/load-sve/load-sve-gather/store-neon/store-sve/store-zva/copy-sve/mix-neon-fma/mix-sve-fma（11 项，含 fma 模板）。

- [ ] **Step 1: NEON 族内核**（无门控）

```c
static NOINLINE OPTIMIZE3 void stress_lsupress_load_neon(
	stress_args_t *args, uint64_t *buf,
	const size_t buf_words, const uint64_t seed)
{
	uint8x16x2_t acc;
	const uint8x16_t *p = (const uint8x16_t *)buf;
	size_t i, vecs = (buf_words * 8) / 32;

	acc.val[0] = vdupq_n_u8(0);
	acc.val[1] = vdupq_n_u8(0);
	(void)seed;
	do {
		for (i = 0; i < vecs; i++) {
			uint8x16x2_t v = vld2q_u8((const uint8_t *)(p + i));
			acc.val[0] = veorq_u8(acc.val[0], v.val[0]);
			acc.val[1] = veorq_u8(acc.val[1], v.val[1]);
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero((uint64_t)acc.val[0][0]);
}
```

store-neon / copy-sve / load-int128（ldp 消费）/ load-fp64（ldr d 校验和）/ mix-neon-fma（batch ld1×4 → vfma → batch st1×4，每迭代 4 q load + 4 q store 背靠背）照此模式——每内核 10-20 行，`vld1q_f64`/`vfmaq_f64`/`vst1q_f64` 组合。

- [ ] **Step 2: SVE 族内核**（`__GNUC__>=10` + target 属性 + `lsupress_has_sve()` 运行时门控，无 SVE 时内核入口 return——诚实跳过由方法包装层打印 skipped 行）

```c
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_load_sve(
	stress_args_t *args, uint64_t *buf,
	const size_t buf_words, const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	svuint64_t acc = svdup_u64(seed);
	const svuint64_t *p = (const svuint64_t *)buf;
	const uint64_t vl = (uint64_t)svcntd();
	size_t i, vecs = buf_words / vl;

	do {
		for (i = 0; i < vecs; i++)
			acc = sveor_u64_x(pg, acc, p[i]);	/* ld1d + eor */
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero(svlastb_u64(pg, acc));
}
```

load-sve-gather（gather 元素 = 窗口内 64B 步进索引）、store-sve（bitgen 流 st1d）、mix-sve-fma（batch ld1d→fmla→st1d，全 VL）同型——gather 内核：

```c
__attribute__((target("arch=armv8.2-a+sve")))
static NOINLINE OPTIMIZE3 void stress_lsupress_load_sve_gather(
	stress_args_t *args, uint64_t *buf,
	const size_t buf_words, const uint64_t seed)
{
	svbool_t pg = svptrue_b64();
	svuint64_t idx = svindex_u64(0, 64 / 8);	/* 64B stride, in elems */
	svuint64_t acc = svdup_u64(seed);
	const uint64_t vl = (uint64_t)svcntd();
	const uint64_t elems = buf_words;
	uint64_t k;
	const uint64_t lim = elems - (elems % (vl * 8));

	do {
		for (k = 0; k < lim; k += vl * 8) {
			svuint64_t v = svld1_gather_u64index_u64(
				pg, buf + k, idx);
			acc = sveor_u64_x(pg, acc, v);
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero(svlastb_u64(pg, acc));
}
```

store-zva（DC ZVA 行清零——查本机 CTR_EL0 DZMinLine，fork 的 write64zva 已有该探测，拷贝）。

- [ ] **Step 3: 构建 + 本机验收**（NEON/fp/int128 全跑；SVE 系列本机预期跳过——Review Focus #4）

```bash
./stress-ng --lsupress 2 --lsupress-method load-neon -t 10
./stress-ng --lsupress 2 --lsupress-method mix-neon-fma -t 10
./stress-ng --lsupress 2 --lsupress-method load-sve -t 5      # 920: 诚实跳过/无 SIGILL
```

- [ ] **Step 4: QEMU 验收**（sve/gather/fma 四方法 `--verify` 无关但跑通 + failed:0）

- [ ] **Step 5: objdump 门**（ld1d/st1d/gather `ld1d.*{` 计数 > 0）+ commit

```bash
git commit -m "lsupress: vector method family — neon/sve load-store-copy-fma, gather, DC ZVA"
```

---

### Task 7: lsupress 原子方法（excl-pair / lse-rmw / ls64-copy）

**Files:** Modify `stress-lsupress.c`、`stress-ng.1`

**Interfaces:**
- Consumes: stress-ls64.c 的 ls64 `.inst` 序列 + HWCAP 探测；`STRESS_CPUINFO_ATOMIC`（LSE）。
- Produces: 方法表 3 项。

- [ ] **Step 1: 内核**

```c
/* excl-pair: ldxr/stxr exclusive loop on a window word */
static NOINLINE OPTIMIZE3 void stress_lsupress_excl_pair(
	stress_args_t *args, uint64_t *buf,
	const size_t buf_words, const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	do {
		for (i = 0; i < buf_words; i += 64) {	/* one per cacheline */
			register uint64_t t, ok;
			__asm__ __volatile__ (
				"1: ldxr %0, [%3]\n"
				"   add  %0, %0, %4\n"
				"   stxr %w1, %0, [%3]\n"
				"   cbnz %w1, 1b"
				: "=&r"(t), "=&r"(ok)
				: "r"(v), "r"(buf + i)
				: "memory");
			v = t;
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero(v);
}

/* lse-rmw: LSE ldadd (HWCAP_ATOMICS gated at dispatch) */
static NOINLINE OPTIMIZE3 void stress_lsupress_lse_rmw(
	stress_args_t *args, uint64_t *buf,
	const size_t buf_words, const uint64_t seed)
{
	size_t i;
	uint64_t v = seed | 1;

	do {
		for (i = 0; i < buf_words; i += 64) {
			register uint64_t t;
			__asm__ __volatile__ (
				"ldadd %1, %0, [%2]"
				: "=r"(t)
				: "r"(v), "r"(buf + i)
				: "memory");
			v = t;
		}
		stress_bogo_inc(args);
	} while (stress_continue(args));
	stress_uint64_zero(v);
}
```

ls64-copy：Task 2 的 `memcpy_ls64` 双寄存器序列改为窗口内 src→dst 循环（`.inst` 序列同样从 stress-ls64.c 拷贝）。方法包装层做 HWCAP 门控（ls64/lse 不在时打印 skip 行并 return，不进内核）。

- [ ] **Step 2: 构建 + 验收**

```bash
./stress-ng --lsupress 2 --lsupress-method excl-pair -t 10     # 本机可跑（通用独占指令）
./stress-ng --lsupress 2 --lsupress-method lse-rmw -t 10       # 920 有 LSE → 跑通
./stress-ng --lsupress 2 --lsupress-method ls64-copy -t 5      # 本机跳过；QEMU -cpu max 跑通
```

- [ ] **Step 3: commit**

```bash
git commit -m "lsupress: atomic methods — exclusive pairs, LSE RMW, ls64 copy"
```

---

### Task 8: lsupress 游走扩展 + hugepage + align 选项

**Files:** Modify `stress-lsupress.c`、`stress-ng.h`（OPT_lsupress_walk/OPT_lsupress_huge/OPT_lsupress_align）、`stress-ng.1`

**Interfaces:**
- Consumes: Task 4 的 `lsupress_next_hop`（改造为可插拔游走策略）；core-bitgen.h 的 bandwalk（`stress_bitgen_u64`）。
- Produces: `--lsupress-walk uniform|bitgen|va-bit|near-far`、`--lsupress-huge 4k|2m|1g`、`--lsupress-align aligned|misalign|cross-line`。

- [ ] **Step 1: 游走策略**

```c
static uint64_t lsupress_walk_hop(lsupress_engine_t *e, const size_t method)
{
	switch (method) {
	default:
	case WALK_UNIFORM:
		return lsupress_next_hop(e);
	case WALK_BITGEN: {
		/* bit-band targeted hop: sweep a 6..20-bit window
		 * across VA bits 12..47 — TLB tag bit-segment walk */
		const uint64_t r = stress_bitgen_u64(&e->bg);
		const unsigned width = 6 + (unsigned)((r >> 58) % 15);
		const unsigned pos = (unsigned)((r >> 48) % (36 - width));
		const uint64_t mask = ((1ULL << width) - 1) << pos;
		return lsupress_next_hop(e) & ~(mask << 12);
	}
	case WALK_VA_BIT: {
		/* single VA bit per hop, walking 12..47 */
		const uint64_t bit = 12 + (e->hops++ % 36);
		return lsupress_next_hop(e) ^ (1ULL << bit);
	}
	case WALK_NEAR_FAR: {
		/* 80% near hop (within 1/1024 of map), 20% far */
		const uint64_t r = lsupress_next_hop(e);
		if ((r & 7) != 0)
			return ((uint64_t)(uintptr_t)e->win ^
				(r & ((e->map_size >> 10) - 1)));
		return r;
	}
	}
}
```

（engine 结构体补 `stress_bitgen_t bg` 与 `uint64_t hops` 字段；hop 结果由 `next_window` 归一页对齐并 clamp 进映射范围。）huge 选项：mmap 加 `MAP_HUGETLB`+`MAP_HUGE_x`（按值选），失败回退 4K 并 warning。align 选项：内核的窗口内偏移策略（aligned=0 / misalign=off 轮换（Task 5 已内建）/ cross-line=dst=src+60 强制跨行）——以全局 setting 传给内核（方法签名加一个 `const uint32_t shape` 参数——**签名变更点**：Task 5/6/7 内核同步加参并忽略或使用）。

- [ ] **Step 2: 构建 + 验收**

```bash
./stress-ng --lsupress 2 --lsupress-walk bitgen -t 20
./stress-ng --lsupress 2 --lsupress-walk va-bit -t 20
./stress-ng --lsupress 2 --lsupress-walk near-far -t 20
./stress-ng --lsupress 2 --lsupress-huge 2m -t 10 || true   # 无 hugetlb 池则回退 warning
./stress-ng --lsupress 2 --lsupress-align cross-line -t 10
```
Expected: 全部跑通；`strace -e madvise` 确认各游走模式下窗口迁移仍在发生。

- [ ] **Step 3: commit**

```bash
git commit -m "lsupress: walk modes (bitgen/va-bit/near-far), hugepage, alignment shapes"
```

---

### Task 9: lsupress verify + 故障注入

**Files:** Modify `stress-lsupress.c`、`stress-ng.1`

**Interfaces:**
- Consumes: `lsupress_value(seed, addr)`（Task 3）；方法分派层。
- Produces: `--verify` 语义：store/copy/mix 系列窗口写 f() 值 → verify 阶段重算比对；位级失配诊断。

- [ ] **Step 1: verify 集成**（`stress_lsupress()` 主流程）

```c
	if (args->verify) {
		/* after the kernel pass, re-walk the current window and
		 * recompute f() for every word we own */
		const uint64_t *w = (const uint64_t *)e.win;
		const size_t words = e.win_size / sizeof(uint64_t);
		size_t i;
		uint32_t fails = 0;

		for (i = 0; i < words; i += 4096) {	/* sampled per page */
			const uint64_t expect = lsupress_value(seed,
				(uint64_t)(uintptr_t)(w + i));
			if (UNLIKELY(w[i] != expect)) {
				const uint64_t diff = w[i] ^ expect;
				pr_fail("%s: addr %p expected 0x%16.16" PRIx64
					" actual 0x%16.16" PRIx64
					" %i bit(s) flipped (xor 0x%16.16" PRIx64 ")\n",
					args->name, (void *)(w + i), expect, w[i],
					stress.hamming(diff), diff);
				if (++fails >= 1)	/* first-fail report */
					break;
			}
		}
		if (fails)
			return EXIT_FAILURE;
	}
```

（前提：store/copy/mix 内核的写入值 = `lsupress_value(seed, dst_addr)`——内核中生成 store 数据时用它替代裸 bitgen 值；load 系内核 verify 校验和对照 f() 累计。每页采样 1 字：verify 开销/激发比 <1%。）

- [ ] **Step 2: 故障注入实验（纪律 #3——verify 没被验证过等于没有）**

```bash
# 临时在 stress_lsupress() verify 前注入：w[页中点] ^= 0x0000200000000000ULL;
make -j$(nproc)
./stress-ng --lsupress 1 --verify -t 10 2>&1 | grep -E 'flipped|failed:'
# 期望输出形如：
#   lsupress: addr 0x7f... expected 0x... actual 0x... 1 bit(s) flipped (xor 0x0020000000000000)
#   failed: 1: lsupress (1)
git checkout stress-lsupress.c && make -j$(nproc)   # 还原 + 干净复跑 failed:0
```

- [ ] **Step 3: 全方法 verify 回归 + commit**

```bash
for m in load-int64 store-int64 copy-int64 mix-2l-alu-1s mix-1l-fpu-1s; do
	./stress-ng --lsupress 1 --lsupress-method $m --verify -t 8
done
git add stress-lsupress.c stress-ng.1
git commit -m "lsupress: per-address deterministic verify with bit-level diagnostics"
```

---

### Task 10: excite 2.0 时间片轮换

**Files:** Modify `scripts/sdc-run.sh`（`run_excite()`）、`README.md`（模式表 excite 行）、`CLAUDE.md`（能力地图行）

**Interfaces:**
- Consumes: 现有拓扑推导（N_PHYSICAL/HAS_*）；lsupress/memcpy 新方法。
- Produces: 轮岗制 excite。

- [ ] **Step 1: run_excite 重构**（岗位表 + 循环）

```bash
#  岗位表：主力 stressor（每岗全核深压）——按特性门控
local -a STATIONS=( "cpu" "fma" "armcrypto" "lsupress" "operand-var" )
[ "$HAS_SVE2" -eq 1 ] && STATIONS+=( "memcpy-sve" )
STATIONS+=( "addrspace" "memrate" )

local station_idx=0
local slice=$(( ${EXCITE_SLICE:-10} * 60 ))	# 每岗 10 分钟
local start=$SECONDS

run_station() {			# 主力深压 + 背景保持
	local main="$1"; shift
	case "$main" in
	cpu)	"$NG" --cpu "$N_PHYSICAL" --taskset physical --cpu-method all "$@" ;;
	fma)	"$NG" --fma "$N_PHYSICAL" "$@" ;;
	armcrypto) "$NG" --armcrypto "$N_PHYSICAL" "$@" ;;
	lsupress) "$NG" --lsupress "$N_PHYSICAL" "$@" ;;
	operand-var) "$NG" --operand-var "$N_PHYSICAL" "$@" ;;
	addrspace) "$NG" --addrspace 2 --addrspace-bytes "$(( total_mem_mb / 4 ))m" "$@" ;;
	memrate)	"$NG" --memrate "$N_PHYSICAL" --memrate-write-pattern bandwalk "$@" ;;
	memcpy-sve)	"$NG" --memcpy "$N_PHYSICAL" --memcpy-method sve "$@" ;;
	esac
}

#  背景：varyload di/dt 常驻（保持机器级并发触发条件）
"$NG" --varyload 64 --varyload-ms 20 --interrupts -K --thermalstat 30 \
	-t "${dur}s" > "$out/bg.log" 2>&1 &
local bg_pid=$!

while [ $(( SECONDS - start )) -lt "$dur" ]; do
	local main="${STATIONS[station_idx % ${#STATIONS[@]}]}"
	echo "=== station: $main ($(( (SECONDS - start) / 60 ))min elapsed) ==="
	run_station "$main" -t "${slice}s" > "$out/station_${main}.log" 2>&1
	station_idx=$(( station_idx + 1 ))
done
kill "$bg_pid" 2>/dev/null
```

（preheat/--sdcshield 逻辑保持；岗位日志进 `$out/`；总时长守恒：slice 超出 dur 时截断。）

- [ ] **Step 2: 本机 E2E 验收**

```bash
EXCITE_SLICE=1 NG=./stress-ng ./scripts/sdc-run.sh excite -t 8 -o /tmp/e2e_test
ls /tmp/e2e_test/station_*.log
```
Expected: ≥4 个岗位日志（8 分钟 × 1 分钟/岗），每岗位 worker 数 = N_PHYSICAL（深压），bg.log 存在。

- [ ] **Step 3: commit**

```bash
git add scripts/sdc-run.sh README.md CLAUDE.md
git commit -m "sdc-run: excite 2.0 — time-slice station rotation for single-pathway deep pressure"
```

---

### Task 11: 文档 + CI 收尾

**Files:** Modify `docs/excitation-guide.md`、`CHANGELOG.md`、`README.md`（能力表）、`stress-ng.1`（全选项终审）

- [ ] **Step 1: excitation-guide 覆盖矩阵更新**：LSU 列与 MMU/TLB 列的 Data shape/Address shape 行从 ◐ 升 ●，新增 "instruction spectrum" 行（mix 模板/load 并发/gather/原子），gap 表删去已补项。
- [ ] **Step 2: CHANGELOG 条目**（[未发布] 节：lsupress 20 方法 + memcpy 6 变体 + excite 2.0）。
- [ ] **Step 3: CI 确认**：push 后 dispatch multi-os-verify，`--lsupress-method` 与 `--memcpy-method` 自动进 62-method sweep（脚本二进制自枚举）；15/15 绿。
- [ ] **Step 4: commit + push**

```bash
git commit -m "docs: LSU instruction-spectrum excitation — coverage matrix and changelog"
```

---

## Self-Review 记录

1. **Spec coverage**：§4→Task 1/2；§5.1-5.2→Task 3-7（20 方法全覆盖：表 #1-3→T3+T6、#4-6→T6、#7-10→T3/T6、#11-12→T3/T6、#13-15→T5、#16-17→T6、#18-20→T7）；§5.3→T4/T8；§5.4→T9；§6→T10；§7 验证→各 task 步骤内联；§8 边界→设计本身。无缺口。
2. **Placeholder scan**：Task 2 ls64 `.inst` 的 `...` 是"从 stress-ls64.c 拷贝已验证编码"的显式指针（上游唯一权威），非 TBD；其余无占位。
3. **类型一致性**：`stress_lsupress_func_t` 四参签名贯穿 T3-T7；T8 引入 `shape` 参数为**签名变更点**已显式标注同步义务；`lsupress_value` T3 定义 T9 使用一致。
4. **Review Focus 5 项**已分别钉进 T1(#1)/T4(#2#3)/T6(#4)/T9(#5) 的验收步骤。
