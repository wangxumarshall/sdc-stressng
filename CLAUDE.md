# CLAUDE.md — sdc-stressng 开发指南

本仓库是 stress-ng 的 **arm64 服务器芯片 SDC 压测 fork**（上游 ColinIanKing/stress-ng）。
开发、验证、文档的一切决策围绕一个目标：**在 arm64 服务器（Kunpeng 920/950 级）上把潜在 SDC（静默数据损坏）故障核逼出来**。

## 分工模型（不要越界）

| 工具 | 角色 |
|---|---|
| SDCShield（../sdcshield） | **校验器**：273 个 golden 比对用例，判定"算错了没有"、报 cpu-mask |
| 本仓库 stress-ng | **扰动器**：制造 di/dt、功耗、缓存/TLB/互联压力与边界时序，压缩弱核时序裕量 |

stress-ng 的 `--verify` 是辅助报警，不是第二校验器。改进方向永远是"更强的激发"而非"重复的校验"。

## 目标机事实（写命令/方案前先读，不要重新推导）

- **目标机 CP1**：Kunpeng 950 7592C，2 socket × 95C×2T = 382 逻辑 CPU，node0=0-191 / node1=192-381，SMT2。SVE2 全家桶（sveaes/svepmull/svesha3/svesm4/svebitperm/svei8mm/svebf16）+ NEON + crc32 + LSE atomics + ls64 + rng。2.3GHz 固频（boost disabled）。L3 分 24 实例。曾有核隔离/自检失败史。
- **开发机**：Kunpeng 920，128 CPU，无 SMT，4 NUMA node，NEON aes/sha1/sha2/dotprod/fp16 有，**无 SVE/SVE2/ls64/RNDR**。
- 推论：SVE2/ls64 类代码在本机只能验证"编译 + 诚实跳过"，功能验证靠 QEMU 用户态仿真（`-cpu max`）或目标机。命令面向目标机写，代码验证按本机做。

## 构建

```bash
make clean && make -j$(nproc)   # 本机 gcc 12 即可；make clean 在拉取后必须做（config.h 再生成）
```

- aarch64 构建自动探测 SVE2 工具链 + 硬件，两者都满足才注入 `-O3 -march=armv8.6-a+sve2+bf16+i8mm+sve2-bitperm`（`-O2` 下 GCC 仍用 NEON，SVE 必须 `-O3`）。交叉强制：`make MARCH_AARCH64_SVE2=1/0`。
- gcc 7.3（openEuler 20.03 容器 `openeuler-offline:20.03-LTS-SP4`）兼容性在本机容器一次验完，**不要**推给 CI 试错（历史上 8 轮 CI 迭代的教训）。
- 所有新行为默认关闭或与旧值兼容。

## fork 的 arm64/SDC 能力地图（改动前先查这里，避免重复造轮子）

| 能力 | 入口 |
|---|---|
| SDC 定向位模式生成器 | `core-bitgen.c/h`：bandwalk（6-20 位窗口扫掠×5 档密度）/edge 字典（57 边界值±抖动）/FP 位型直合成/互补对/汉明定向；`stress_bitgen_u64()` 模式混合；统计验证 `scripts/bitgen-distribution.sh` |
| 操作数变异压测 | `--operand-var`（5 方法：bandwalk-int/bandwalk-fp/edge-dict/complement-fma/type-matrix，golden 重放比对，VERIFY_ALWAYS） |
| 地址空间形状压测 | `--addrspace`（7 配方：huge-random-fixed/va-bit-walk/dense-random-offset/guarded-holes/malloc-giant/misalign-huge/mixed-orders，全带校验） |
| SVE2 数据通路 | `--sve2`（fmla/gather/fcmla/bitperm/bfdot，golden 比对）；fma stressor 运行时分发 SVE2 内核；armcrypto 13 方法（NEON 9 + SVE2 4，.inst 编码绕 GCC 无 intrinsic） |
| ARM 专项 | `--ls64`（64B 原子访存）、`--rdrand`（RNDR）、`--tsc`（CNTVCT_EL0）、`--cache-flush/--cache-clwb`（DC CIVAC/CVAC）、`--memrate-method write64zva`（DC ZVA）、`--taskset physical`（SMT 感知）、`--rapl`（hwmon 功率） |
| memrate 写模式 | `--memrate-write-pattern 0xaa|random|bandwalk|complement` |
| vm 随机偏移 | `--vm-method rand-offset`（无放回 Fisher-Yates + bitgen 填充/同序校验） |
| SDC 编排 | `scripts/sdc-run.sh full|scan|path|pair|all`（拓扑自推导；full 含 varyload di/dt + --preheat 热浸润；scan 逐物理核 + --keep-bg 背景压；pair SMT 争用矩阵） |
| 位级 verify 诊断 | fma/vecfp/matrix 失配输出：元素下标 + expected/actual + 翻转位数 + xor 掩码 |

## 验证纪律（每个 patch 的硬性流程）

1. **本机全功能验证**（有硬件的部分）
2. **QEMU 用户态仿真**验证 SVE2/SVE/SM3/SM4/SHA3/RNDR（`/tmp/qemu-out/qemu-aarch64 -cpu max`，自建 8.2.0）
3. **故障注入**：每个带 verify 的新组件，人工翻 1 位验证"真的能抓"——verify 代码本身没被验证过等于没有
4. **gcc 7.3 容器**编译+冒烟
5. **CI 15 镜像**（multi-os-verify.yml，openEuler 20.03/22.03/24.03 × 5 SP，每日 cron）：`--sequential --verify` 全量 + 62 个 `*-method all` sweep + 基准采样；新 stressor 自动进套件
6. **性能零回归**：改动存储路径后对比带宽（噪声内才算过）

## 代码纪律（12 轮踩坑沉淀，违反必翻车）

1. **grep 上游惯例先于写代码**。宏名/工具函数不要臆造：`HAVE_MMAP` 不存在（上游不设门）、`MB`→`STRESS_MB`、没有 `shim_mmap`（直接 mmap）、守卫宏是 `STRESS_ARCH_ARM` 不是 `STRESS_ARCH_ARM64`。同源教训：第五轮 EBADF（臆造守卫宏导致真机 mmap 全灭）。
2. **方法表是 0 基分派，`methods[0]` 是 "all"**。新 stressor 的方法表照 stress-cpu.c:3114 的 "all" 处理写；写错不只是 SIGSEGV——错位分派让"通过"的方法实际在跑别人的代码。**测试通过 ≠ 测的是你以为的东西**。
3. **随机化只进压力路径，verify oracle 保持确定性**。oracle 是恒等式/字面量的（如 atomic 的 `~tmp-1==tmp`），随机化它就毁掉校验基准。
4. **fill/verify 流同步**：重叠随机写入会打乱流对齐。统一解法 = 预绘制决策 + 无放回采样（Fisher-Yates）+ fill/verify 双份同种子 bitgen（`shim_memcpy(&bg_verify, &bg, ...)` 先例在 stress-vm.c rand-offset）。`stress_bitgen_skip` 保持 8 字节粒度对齐。
5. **复杂 recipe 先纸上定骨架再动笔**：预绘制决策 → bitgen 流 → fill → verify，四段定好再写代码（addrspace 第一稿 300 行废弃代码的教训）。
6. **SVE2 crypto 无 GCC intrinsic** → `.inst` 数值编码（binutils 助记符也有兼容问题）；target 属性用完整 `arch=armv8.4-a+sm4` 形式；SVE 动态开关 = HWCAP/HWCAP2 运行时检查 + target 属性编译隔离，单 binary 跨 920/950。
7. **`-march` 拼写**：`svebf16` 是错的（gcc 拒绝），用 `bf16`；合法串 `armv8.6-a+sve2+bf16+i8mm+sve2-bitperm`。
8. **命令先在真编译器/二进制上验证再进文档**：`--cdouble` 是 cpu-method 不是 stressor；`--ignite-cpu` x86-only（ARM 用手写 cpufreq governor）；`-x/--exclude` 是逗号分隔纯名列表。自动向量化结果不可预测 → objdump 确认 z 寄存器指令 > 0 才算 SVE 生效。
9. **one-patch-per-unit**：每个改进点一个 commit，走 plan → code → verify → commit → push。
10. **诚实跳过机制是特性不是缺陷**：无硬件就 skipped with reason，绝不假跑。

## SDC 方法论（激发杠杆，来自文献+实证）

- **全核并发是触发条件**（CORE179/Meta 经验）：单核独占往往不触发；scan 模式必须配 `--keep-bg` 背景压。
- **热浸润 + 顺序效应**：失败用例只在发热用例之后失败（指数关系+最低触发阈值）→ `--preheat` 把 verify 窗口开在热机上。
- **di/dt 阶跃**：`--varyload` 6 波形 × `--cpu-load-slice`，7% 独有覆盖。
- **SMT 同核争用**：pair 模式 4 组合矩阵映射共享资源拓扑。
- **数据变异质量决定激发效率**（第十二轮核心认知）：不是"有没有随机"而是"随机的形状"——均匀随机对位段敏感缺陷覆盖差（52 位尾数空间撒点 vs 定向位段扫掠，边界命中差 >10⁶ 倍）；纯固定模式走向另一极端。混合生成器（模式字典 × 均匀随机）是全库复用的解。
- **SDC 频率低至 0.01 次/分钟**：变异模式必须可长期循环不重复；数千次迭代长 soak + 周期性重复。
- **ARM 服务器无硬件冗余**（无 lockstep、RAS 管不了无检错通路）→ 软件扰动器 + golden 比对是唯一路线。

## 文档结构

- `docs/superpowers/research/` — 每轮研究报告（盘点/文献）
- `docs/superpowers/plans/` — 每轮实施方案（patch 清单+验证计划）
- `findings.md` / `task_plan.md` / `progress.md` — 工作记忆（planning-with-files）
- 上游内容（README 的跨平台构建说明、bugs 清单等）保留但排在 fork 能力之后

## 当前遗留（见 findings.md §11）

bandwalk 窗口参数校准（需真机位翻分布）、cache 系列字粒度 tag 重设计、pagemap PFN 导向（需 root）、真机 A/B 回归（sdc-run + SDCShield 失配率对比）、CI 时序趋势工具。
