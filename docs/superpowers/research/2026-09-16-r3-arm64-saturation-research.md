# R3: ARM64 服务器微架构单元饱和负载调研

> 调研日期 2026-09-16。subagent 环境限制：WebFetch 被网络策略拦截，所有 URL 来自搜索摘要交叉印证，未能逐条打开原文。周期数/吞吐数字来自 Neoverse V2/N2 公开资料，**不是 Kunpeng 950 实测值**。可信度：[中高]=多源交叉+具体文档号；[中]=单源为主；[低可信]=单源疑似重构；[本地已验证]=在本仓库/本机直接验证。

## 1. 单元 × 饱和模式 × 测量方法总表

| 单元 | 最能压满的指令/访问模式 | 饱和判定指标 | 出处 |
|---|---|---|---|
| **SVE2 全宽向量 FMA** | 背靠背独立 `FMLA z`（256-bit 全宽、≥4 独立累加器、无依赖链）；注意 Neoverse V2 类核 128-bit 物理通路，256-bit crack 成 2×128 μop [中高] | `FP_PIPE`(0xB9)/`CPU_CYCLES`(0x11) 逼近 SIMD 管数；topdown retiring→上限 | Arm V2 SOG doc 102352 [中高] |
| **SVE2 复数 MAC** | `FCMLA #0/#90` 交替（FFT 蝶形）；1 条替代 NEON 4×FMLA+shuffle，提速 1.5-3× [中] | 同上 + bogo-ops 对比 NEON 基线 | Arm 指令文档 [中] |
| **SVE2 点积/矩阵** | `BFDOT/SDOT/UDOT`、`SMMLA/UMMLA/USDOT`(i8mm)、`BFMMLA`、`BFMLALB/T`(bf16) [低可信-需查 V2 SOG 原表] | `ASE_SPEC`(0x74) | Arm ARM DDI0487 [高] |
| **SVE gather/scatter（LSU 压力）** | `LD1D {zt.d}, p/z, [xn, zm.d, LSL #3]` 向量索引 gather + ST1D scatter；跨 cacheline/页时每元素 4-8+ 周期，LSU 最重负载 [中] | `LD_SPEC`(0x70)/`ST_SPEC`(0x71)、`L1D_CACHE_REFILL`、SPE 平均 load latency | AWS multi-buffer AES 博客 [中高] |
| **AES 引擎** | SVE2 `AESE/AESD+AESMC`（256-bit 一次 2 块），≥4 独立 AES 流交错隐藏延迟；NEON 形式 ~1 inst/cycle，SVE 256-bit 约 2×（受 128-bit 通路限制）[中] | 吞吐 GB/s 对照 ~0.5-0.7 cyc/B + topdown | Arm N2/V2 SOG [中高] |
| **PMULL/GHASH** | `PMULLB/PMULLT` 背靠背；GHASH 3-4 周期依赖链必须多累加器交错（OpenSSL ghashv8-armx 做法）[中] | 吞吐 GB/s | OpenSSL [中高] |
| **SHA3** | `EOR3`(θ 三输入 XOR)+`XAR`(ρ)+`RAX1`+`BCAX`(χ)；交错 2-3 个 Keccak state；内核 SVE2 SHA3 ~5.1 cyc/B vs 标量 12.7 [中] | 吞吐 cyc/B | Ard Biesheuvel 内核 patch [中高] |
| **SM4/SM3** | `SM4E/SM4EKEY`（每条 4 轮/块）、SM3 `SM3SS1/SM3PARTW1/2/SM3TT1A/B/SM3TT2A/B`；Kunpeng 920 是最著名实现，SM4E ~4-8 GB/s/核 vs 标量 150-400 MB/s [中] | `openssl speed -evp sm4-gcm` | Tongsuo/鲲鹏社区 [中] |
| **CRC32** | `CRC32X/CRC32CX` 64-bit 多独立链交错（1/cycle 吞吐 3-4 周期延迟，单链饱和不了）[中]；fork 已有 crc stressor [本地已验证] | 吞吐实测 | Arm SOG [中] |
| **整数 ALU/MAC** | `MADD/MSUB` 链、`SMULH/UMULH`、ROR/EOR/BIC；fork intmath/cpu/bitops 已覆盖 [本地已验证] | IPC 逼近发射宽度 | Arm SOG [中高] |
| **分支预测器** | taken/not-taken 伪随机（fork branch）、远跳打 BTB（far-branch）、间接分支表（goto）；Neoverse V2 有已知 BTB 容量问题 [中] | `BR_MIS_PRED`(0x10)/`BR_RETIRED`(0x12) | Chips and Cheese [中高] |
| **L1D/LSU 带宽** | `LDP/STP` Q 对（2×16B/cyc）、SVE LD1D/ST1D 连续、`DC ZVA`（免 RFO 清零整行）[中] | `LD_SPEC/ST_SPEC`、`L1D_CACHE`(0x04)、bytes/cycle 逼近理论 | C&C Grace [中高] |
| **原子/一致性（LSE）** | `CAS/SWP/LDADD` 全速率（无竞争 ~1/cycle）；竞争时 N 核打同一行乒乓；LSE 高核数扩展性远好于 LL/SC 重试风暴 [中高] | HHA snoop 事件、CMN dvmop、`BUS_ACCESS`(0x19) | Arm LSE + 内核 outline-atomics [中高] |
| **RCpc（lrcpc）** | `LDAPR/STLR` 对（比 LDAR 便宜：N1 LDAR-after-STLR ~30 周期惩罚）[中] | SPE 延迟 | LKML LDAPR 讨论 [中] |
| **64B 原子（ls64）** | `LD64B/ST64B`（FEAT_LS64）；fork 已有 ls64 stressor [本地已验证] | bytes/cycle 对照 LDP 基线 | Arm ARM [高] |
| **DSB/DMB 屏障风暴** | 密集 `DSB SY`/`DMB ISH`（fork membarrier 是 syscall 不是指令风暴——主会话核实）背靠背强制 store buffer 排空 | `BUS_ACCESS`、CMN 事件、SPE 延迟 | 架构语义 [高] |
| **TLB/MMU** | 4K 页随机大足迹（tlb-numa）、`munmap/mprotect` 风暴触发 TLBI 广播（fork tlb-shootdown [本地已验证]）；对照 2M THP+contiguous hint（16 PTE 折叠 1 TLB 项，带宽 +5-30%）[中] | `DTLB_WALK`、`L1D_TLB_REFILL`(0x05)、CMN DVM 计数 | 内核 hisi-pmu/arm-cmn 文档 [高] |
| **Cache 层级/组冲突** | 工作集逐级跨越（ptr-chase：V2 级 L1 ~4-5cyc、L2 ~16、L3 数十、DRAM ~97ns(Grace)）[中高]；组冲突地址 stride=大小/路数 等差序列；cachehammer/llc-affinity/memthrash 已覆盖 [本地已验证] | `L1D_CACHE_REFILL`(0x03)、`L2D_CACHE_REFILL`(0x17)、`LL_CACHE_MISS`、SPE 延迟分布 | C&C Grace/Graviton4；lmbench [中高] |
| **跨 socket/内存带宽** | STREAM Triad 全核+NUMA 交错（fork stream/numa/numacopy）；Kunpeng 920 实测单 socket ~194 GB/s（8×DDR4-2933 理论 85%+，McCalpin）[中高] | `hisi_sccl*_ddrc*/flux_rd+flux_wr`×64B/时间 vs 每通道理论；核数扫描曲线平台法（<5-10% 增益即饱和） | McCalpin；hisi-pmu [中高] |
| **伪共享/真共享** | 双 worker 同 64B 行相邻字段对写（fork cacheline 即此设计 [本地已验证]） | `perf c2c`（arm64 ~v6.9 经 Leo Yan SPE data_src 可用） | lore [中高] |
| **功耗/时序** | `MSR DIT, x0` 数据无关时序（Keccak M1 ~25% 惩罚；内核评价作用有限）[中]；`WFET/WFIT` 有界等待——降压方向，可用于最大/最小功耗对照 | 功耗计、DIT 开关前后吞吐差 | mouha.be；lore [中] |
| **RNG（RNDR）** | `MRS x0, RNDR` 循环；实现定义时序（M1 ~21cyc 16 项缓冲，打空后暴跌，不能假设吞吐）[中] | 吞吐测量本身即判定 | Arm ARM + Dougall Johnson [中] |

**饱和判定通用方法论**：
1. 理论峰值对比法：事件计数/时间 vs (频率×管线数×宽度)，85-95% 即饱和
2. 核数扫描平台法：吞吐 vs 核数斜率 <5-10% 判饱和
3. topdown 归因法：`perf stat -M TopdownL1`（Neoverse JSON 已入内核 tools/perf/pmu-events/arch/arm64/，v6.2+ 有 V2）[中]
4. SPE 延迟分布法：`perf record -e arm_spe_0/load_filter=1,min_latency=N/`

## 2. 工具/基准对照表

| 工具 | 做什么 | fork 缺什么 |
|---|---|---|
| **Arm Neoverse V2/N2 SOG**（doc 102352/102245） | 每指令吞吐/延迟官方表 | fork 无按 SOG 定制的"每指令最大吞吐"序列；sve2 stressor 只有 fmla/bitperm |
| **Arm Topdown/telemetry**（doc 109741?/10757 + github ARM-software/telemetry-data） | L1/L2 topdown 公式与 PMU 映射 | fork --perf 只有通用 libperf 事件，无 topdown metric、无 Neoverse 专有事件 |
| **FIRESTARTER²**（github tud-zih-energy/FIRESTARTER, arXiv:2206.04943） | **公开最成熟 x86+aarch64 功耗病毒**：可配置指令组（NEON/SVE/INT/FP/LOAD/STORE）、高低负载交替、**SMT sibling 伴生线程** | fork 无可配置指令组、无高低负载交替、无 SMT 配对压制模式——最大参照物 |
| BabelStream/STREAM | 带宽核数扫描 | fork stream 已有（copy/scale/add/triad+stride+prefetch），基本对等 |
| lmbench lat_mem_rd | 延迟 vs 足迹曲线 | fork ptr-chase 有；缺自动分级曲线输出 |
| Arm PL | SVE 化 BLAS/FFT 微内核 | fork matrix 靠编译器向量化，非手工微内核 |
| AWS Graviton Getting Started | memcpy 2KiB 页对齐分块、SVE flag | 可吸收进 memcpy/memrate |
| Chips and Cheese 系列（Graviton4 2024-07、Grace） | 独立实测延迟/BTB/LSU 结构 | 方法学模板（AMAT、jmp 步进 BTB 探测） |
| arXiv:2501.01241（NVIDIA V2 表征） | V2 灰盒微架构 | 设计 950 序列的同行参照 |
| OpenSSL/aws-lc aarch64 汇编 | 生产级饱和序列（AES/GHASH 交错、SM4、SVE gather 多缓冲） | fork 零加密单元 stressor——最大缺口 |
| likwid | ARM PMU 事件组免 root | --perf 可借用事件组定义 |

## 3. Kunpeng 特有信息

- **Kunpeng 920（TaiShan V110）**：ARMv8.2、8 通道 DDR4-2933、mesh 互联、每 SCCL 分布 L3。最佳英文分析 McCalpin 博客（单 socket Triad ~194 GB/s）。**TaiShan 核公开微架构论文：没找到**。
- **内核 hisi uncore PMU**（本机已验证存在 hisi_sccl{1,3}_{ddrc,hha,l3c}）：DDRC 事件 flux_rd/flux_wr、L3C refill/read_allocate、HHA snoop；PMU 有 SCCL 亲和性须 -a 系统态采集。Hip09 代新增 PA/SLLC PMU。perf 事件 JSON 在内核树 tools/perf/pmu-events/arch/arm64/hisilicon/hip08.json。**950 上 `ls /sys/bus/event_source/devices/ | grep -i hisi` 是唯一权威枚举**。
- **SM3/SM4 硬件**：Kunpeng 920 是最著名实现；Tongsuo/鲲鹏 BoostKit 有实测。
- **Kunpeng 950/7592C：零公开信息**（多轮中英文检索无命中）。ISA 水平最近参照 Neoverse V2 SOG/TRM。ls64_v 表明至少 Armv8.8 级。

## 4. SMT2 特有（重要负面结论）

- **Neoverse 全系（N1/N2/V1/V2/N3）无 SMT**；Graviton/Ampere/Grace/Cobalt 全单线程。"Neoverse V3 有 SMT"说法两源矛盾未确证。
- **唯一大规模部署的 ARM 服务器 SMT2 = Cavium ThunderX2**（资料久远）。
- **结论：ARM 服务器 SMT2 共享执行端口争用无公开微架构文献，950 只能自研+实测刻画。**
- 可行方法论（FIRESTARTER x86 SMT 实践移植）：sibling 配对压测矩阵 FMA×FMA（共享向量管）、FMA×LSU（分派端口）、AES×FMA（独立单元应近线性）、cacheline×cacheline（共享 L1/L2 应恶化）；吞吐比 1.0=资源私有、≈0.5=完全共享；注意 PMU 核级共享的计数归属。

## 5. 参考文献（除标注外未能打开原文，URL 来自搜索交叉印证）

官方：Arm V2 SOG 102352 / N2 SOG 102245 / Topdown 109741 / telemetry 10757 + github ARM-software/telemetry-data / kernel hisi-pmu / arm-cmn / arm64 spe / perf Neoverse JSON / hip08.json / Arm ARM DDI0487
论文：arXiv:2501.01241 (V2 表征) / arXiv:2206.04943 (FIRESTARTER) / ISCA'13 big-memory TLB shootdown / A64FX ISSCC 2020
博客：McCalpin Kunpeng 920 / C&C Graviton4 + Grace / Cloudflare AES Graviton3 / AWS multi-buffer AES-CTR + SPE perf / Leo Yan perf c2c / Ard Biesheuvel SVE2 SHA3 / AWS Graviton getting-started / mouha.be Keccak M1 / Dougall Johnson / BabelStream / likwid / Arm PL / Cobalt 100

## 6. 明确未确证事项（诚实清单）

1. Kunpeng 950 任何公开资料：不存在
2. TaiShan 核公开微架构论文：没找到
3. ARM 服务器 SMT2 公开资料（除 ThunderX2）：没有
4. 上游 stress-ng "sveaes/svesha3/svesm4 stressor"说法：搜索结果疑似后端虚构（本 fork 0.22.00 合自上游 master 却无这些）——已由主会话核实：上游合并树中无 sve*/ls64 文件（git ls-tree 实证）
5. 所有周期数/吞吐数字是 Neoverse 转述，950 上必须 perf/SOG 类比+实测为准
