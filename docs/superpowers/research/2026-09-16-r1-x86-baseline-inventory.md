# R1: x86 专属能力基线盘点（对标基线）

> 研究日期 2026-09-16。所有 file:line 为 subagent 实际读取；"实测"行为在开发机（Kunpeng 920, gcc 12.3.1）真实运行。
> 主会话已交叉核实：port 分支 11 patch 确已合入 main（`git merge-base --is-ancestor bea490355 main` = YES），工作树即最新。

## 1. x86 专属能力总表

### 1.1 完整 x86-only stressor（ARM64 上 skipped，实测确认）

| 文件:行 | 能力 | x86 机制 | ARM64 等价物现状 | 证据 |
|---|---|---|---|---|
| stress-smi.c:37-40,296 | SMI/SMM 触发 + 寄存器完整性校验 + SMI 频率测量 | `out` 写 APM 端口 0xb2; MSR_SMI_COUNT via /dev/cpu/N/msr; SAVE_REGS 16 GPR 前后比对 | 无（ARM 无 SMM，合理跳过） | 实测 skip 消息 |
| stress-rdrand.c:46-95,448 | 硬件 RNG 压测（rdrand/rdseed，CPU 内独立熵电路） | rdrand/rdseed 指令；**PPC64 有 darn 分支(97-133)，结构上预留第三架构** | **无 ARM RNDR 分支**——目标机 950 有 RNDR | 实测 skip |
| stress-x86cpuid.c:30,292 | 80+ CPUID leaf 枚举压测 | cpuid 指令 | 无（MIDR/ID_AA64* 未被任何 stressor 枚举） | 实测 skip |
| stress-x86syscall.c:41,587 | 直接 syscall 指令走 vsyscall | x86-64 syscall 操作码 | 无（价值低） | 实测 skip |
| stress-ioport.c:80,340 | 端口 I/O | in/out + ioperm() | 无（ARM 无端口 IO，合理） | 实测 skip |
| stress-tsc.c:118-161,642 | 计数器读取微基准（rdtsc/rdtscp/lfence） | rdtsc/rdtscp；**LoongArch/RISC-V/PPC/s390 都有分支，唯独缺 ARM CNTVCT** | **无 `mrs x0, cntvct_el0` 分支** | 实测 skip |
| stress-ipsec-mb.c:66,943 | IPSec-MB 库 AESNI/AVX/AVX2/AVX512 加解密 | Intel ipsec-mb 库 | 无用户态加密直打（仅内核 af-alg 间接路径） | 实测 skip |
| stress-usersyscall.c:67 | syscall 指令调度 | x86-64 syscall | 部分（通用 raise(SIGSYS)） | 实测 skip |
| stress-kvm.c:38,113-135 | KVM 虚机内跑 cpuid/rdtsc/invd 循环 | x86 guest 机器码 | 部分：ARM 分支已有但 guest 仅 strb MMIO 5 条指令 | 已读 113-135 |

### 1.2 方法级/特性级 x86 专属

| 文件:行 | 能力 | ARM64 现状 | 证据 |
|---|---|---|---|
| core-target-clones.h:36-434 | **FMV 函数多版本**（x86 28 clone / ppc64 power9-11） | 空（:436）；**根因实测：GCC 12.3.1 aarch64 报 "target does not support function version dispatcher"，aarch64 FMV 需 GCC 15+** | test/test-target-clones.c:36-41 |
| stress-vnni.c:72-155 | AVX-512/AVX-VNNI intrinsic 方法 | 部分：generic vpaddb/vpdpbusd/vpdpwssd（:493-513）ARM 可用；缺 NEON SDOT/UDOT 方法 | 实测 `--vnni-method list` |
| stress-nop.c:87-110 | 16 种 x86 nop 变体 + pause/serialize/tpause | 部分：ARM 仅 nop+yield | 实测 list |
| stress-waitcpu.c:82-169 | pause/tpause/umwait 等待指令 | 部分：ARM 仅 yield | 实测 |
| stress-prefetch.c:89-115 | prefetcht0-t2/nta | **已有等价**：prfm 6 变体（core-asm-arm.h:30-54） | 实测 list |
| stress-lockbus.c:65,152-237 | lock add 总线锁 + split-lock | 部分：ARM 走 __atomic（LSE） | 已读 |
| stress-cache.c / man:1821-1841 | clflushopt/clwb/cldemote/sfence | 部分：RISC-V 有 CBO（:1199）；**ARM 无 DC CVAC/CIVAC 直打**（man 原文 "x86 only... no-op for non-x86"） | man 原文 |
| stress-memrate.c:753-1005 | rep stosq / movdiri / NT 读写 | 无 NT/rep/ds 变体 | 实测 method list |
| stress-regs.c:147-175 vs 1922-2010 | 寄存器打满：x86 有 XMM0-7 向量版 | **ARM64 只有 x0-x30 GPR 版；缺 v0-v31/z0-z31 向量版** | 已读两段 |
| stress-priv-instr.c:40-52 vs 297-345 | 特权指令陷阱：x86 13 条 | 部分：ARM 仅 tlbi 1 条 | 已读 |
| stress-monte-carlo.c:62-95 / stress-zlib.c:582 / stress-syncload.c:130-138 | rdrand 作为 RNG 源 | 无 RNDR 源 | 已读 |
| stress-tlb-numa.c:443-459 | CPUID 枚举 DTLB 项数 | 无自动探测（默认 256） | 已读 |
| core-mmap.c:30-35,78-90 | rep stosq 填页 | ARM 走 16 次标量写 | 已读 |

### 1.3 框架级 x86 能力

| 文件:行 | 能力 | ARM64 现状 |
|---|---|---|
| core-cpu.c:36-58,432-731 | 24 个 CPUID 特性探测 + vendor 表 | **无 MIDR/HWCAP 封装层**（各 stressor 自行 getauxval） |
| core-ignite-cpu.c:66-147 | --ignite-cpu（Intel P-State + **通用 sysfs cpufreq 路径对所有 Linux 架构生效**） | 部分可用；man:563-565 描述滞后于代码（未在 ARM 实测，标注） |
| core-rapl.h:25-28 | --rapl 功率遥测 | **无**（STRESS_RAPL 仅 linux+x86）；ARM 侧 hwmon power1_input 未接 |
| core-interrupts.c:91-101 | SMI 计数纳入 --interrupts | 无（ARM 无 SMI） |
| core-cpu-cache.c:1179-1189 | CPUID leaf 2/4 缓存枚举 | **已有替代**：sysfs index 优先 + AT_L1D/I/L2/L3_CACHESIZE，ARM 走通 |
| core-helper.c:1813-1844 | x86 readmsr64 | 无（合理） |
| core-asm-x86.h（409 行） | 30+ x86 内联原语 | core-asm-arm.h 仅 11 个原语（prfm×6/yield/dmb_sy/isb） |
| test/ 探针 | 40 个 test-asm-x86-*.c | 5 个 test-asm-arm-* + test-crc32-acle + test-march-aarch64-sve2 |

### 1.4 已确认不缺（ARM64 已有等价或 port 分支已落地）

- stress-sve2.c（fmla + bitperm，HWCAP_SVE 运行检查）、stress-ls64.c（HWCAP2/3 双检查）已合入 main
- crc32 cpu-method（hw/sw 交叉校验，实测 passed）
- SVE2 march 自动注入（Makefile.config:1727-1780）已合入
- --taskset physical、scripts/sdc-scan.sh、scripts/sdc-run.sh 已合入
- fma/vecfp/matrix verify 位级诊断已合入
- prfm 预取家族、spec-rollback、sigill、icache/flushcache（ARM 在列）、easy-opcode/opcode、lockbus 原子、memthrash atomics 均已有
- af-alg 内核 crypto（含 sm3/sm4/sha3/aes defconfigs，177 条）——间接路径，用户态直打仍缺

## 2. ARM64 缺口清单（按 SDC 压测价值排序）

| # | 缺口 | 目标机硬件 | SDC 价值 | 状态 |
|---|---|---|---|---|
| 1 | **硬件 RNG（RNDR/RNDRRS）**：stress-rdrand.c 加第三架构分支 | 950 有 RNDR+RNDRRS | 高：独立硅 IP（熵源+DRBG），高频读取直接激发；x86/ppc64 平行分支结构 = 低风险 1:1 对标 | 待做 |
| 2 | **SVE2 密码 + NEON 密码单元直打**：全树零加密 intrinsic（grep 实证 NONE） | 950 有 sveaes/svepmull/svesha3/svesm4 + NEON aes/sha1/sha2/sm3/sm4 | 高：密码轮函数组合逻辑最深，KAT 已知答案自检=可自检的 SDC 黄金负载；x86 对标 ipsec-mb | 待做（本机有 NEON aes/sha1/sha2 → 可全功能验证！） |
| 3 | **向量寄存器文件压测 v0-v31/z0-z31**：stress-regs.c 加向量版 | 950 SVE2 256-bit | 高：向量寄存器堆是大面积 SRAM，高翻转读写=标准 SDC 激发；x86 XMM 先例（147-175） | 待做（v-reg asm 变量已验证可行） |
| 4 | **FMV/按特性函数分发**：GCC 12 无 aarch64 target_clones | 950 SVE2 | 中高：同一 binary 920/950 都打满；stress-cpu.c hw_crc32 的 `target("+crc")` 模式可复制 | 待做（模式已验证） |
| 5 | **tsc 的 CNTVCT 分支** | 任何 ARM64 | 中：系统计数器压测+时延测量基础设施；四架构先例 | 待做（CNTVCT EL0 已验证可行） |
| 6 | **ARM 功率遥测（--rapl 等价）** | 950 有 hwmon | 中：打满证明+热-故障相关性 | 待做 |
| 7 | **DC CVAC/CIVAC/ZVA 缓存维护直打** | 通用 | 中：cache 维护通路故障是 coherence SDC 直接来源；ARM 分支现为 __clear_cache 回退 | 待做（DC ZVA/CVAC EL0 已验证可行） |
| 8 | priv-instr ARM 扩充（at 指令等） | 通用 | 中低 | 待做 |
| 9 | x86cpuid 的 MIDR 枚举等价 | 通用 | 低 | 待做 |
| 10 | vnni 的 NEON SDOT/UDOT 方法 | 950 dotprod | 低 | 待做 |

**明确不做**（架构不适用，保持诚实 skip）：smi/SMM、ioport、MSR 设备、MTRR、MAP_32BIT、iopl/ioperm、split-lock、EFLAGS AC、x86 syscall 操作码。

**文档顺手修**：man:563-565 --ignite-cpu "only works for Intel P-State x86" 滞后于代码（通用 cpufreq 路径对所有架构生效）——待 ARM 实测后修。

## 3. 本机交叉核实补充（主会话 2026-09-16）

- DC ZVA / DC CVAC 在本机 EL0 可执行（探测程序实测 WORKS）——Patch "缓存维护指令" 无需内核协助
- CNTVCT EL0 可读（实测 WORKS）
- `target("+crypto")` + vaeseq/vaesmc/vsha256hq/vmull_p64 在本机 gcc 12.3.1 编译运行通过（NEON 加密 stressor 可在本机全功能验证）
- **GCC 12 arm_sve.h 无 SVE2 加密 intrinsic**（svaese/sveor3/svsm4e/svpmullb 全缺，grep 实证）→ SVE2 加密必须 `.inst` 内联汇编（SM4E `.inst 0x52e0...` 编译验证通过，运行需 HWCAP_SM4）
- RNDR `mrs s3_3_c2_c4_0` 编译通过（运行需 HWCAP2_RNG；本机无 rng → 诚实跳过路径）
- v0-v31 asm 寄存器变量编译运行通过
- 本机特性：aes/sha1/sha2/asimddp(点积)/asimdhp(FP16)/asimdrdm(RDMA)/atomics/crc32/jscvt/dcpop——NEON 加密/点积/FP16 可本机全功能验证
- hisi_sccl{1,3}_{ddrc0-3,hha,l3c0-7} uncore PMU 本机存在；`perf stat -a -e hisi_sccl1_ddrc0/flux_rd/` 报 not supported（perf_event_paranoid=2 且非 root；**目标机以 root 跑预期可用**——待目标机验证）
