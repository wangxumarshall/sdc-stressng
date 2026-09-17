# ARM64 服务器芯片饱和压测强化方案（对标 x86 支持基线，激发 SDC）

日期：2026-09-16
状态：**实施中**（2026-09-17 用户批准全部 12 patch；分支 `port/arm64-saturation-sdc`）
基线：main 已含 11 个落地 patch（SVE2 march 注入、`--taskset physical`、sdc-scan.sh/sdc-run.sh、fma/vecfp/matrix 位级诊断、stress-sve2.c、stress-ls64.c、crc32 cpu-method、文档）。本方案不重复造轮子。
研究依据：`docs/superpowers/research/` 下三份报告（R1 x86 基线盘点、R2 SDC 前沿研究、R3 ARM64 饱和负载调研），所有关键断言均经本机（Kunpeng 920 开发机）实证或源码核实。

## 0. 模拟验证环境（2026-09-17 就绪，用户指令要求）

本机 Kunpeng 920 无 SVE/SM3/SM4/SHA3/RNG 硬件。按用户指令建立 QEMU 用户态仿真验证环境：

- **qemu-aarch64 8.2.0 自建**（openEuler qemu-user 包不含 aarch64→aarch64 自仿真；从 qemu.org 源码 + 提取的 glib2-devel 头文件构建）：`/tmp/qemu-out/qemu-aarch64`，运行需 `LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64`
- **`-cpu max` 暴露的 HWCAP**（实测）：sve/sve2/sve2aes/sve2pmull/sve2sha3/sve2sm4/sm3/sha3/rng/bf16/i8mm 全部=1（**注意：NEON SM4 位=0**——qemu 8.2 的 max 未设 HWCAP_SM4，NEON sm4 方法在 qemu 下走诚实跳过，SM4 验证用 sve2-sm4 方法；真机两者都有）
- **验证过的执行路径**：SVE2 FMLA intrinsic 程序 qemu 下正确运行（native SIGILL rc=132 vs qemu rc=0）；SVE2 crypto `.inst`（aese/aesd/aesmc/aesimc/sm4e/sm4ekey/eor3/xar/bcax/rax1/pmullb/pmullt）qemu 下全部执行成功
- **SVE2 crypto .inst 编码**（binutils 2.41 反汇编提取，助记符语法有兼容问题故走 .inst）：
  aese=0x4522E020|rn<<5|rd, aesd=0x4522E420|..., aesmc=0x4520E000|..., aesimc=0x4520E400|...,
  sm4e=0x4523E020|..., sm4ekey=0x4521F000|..., eor3=0x04213840 变体, xar(#7)=0x04793420 变体,
  bcax=0x04613840 变体, rax1=0x4521F400 变体, pmullb=0x45016800|..., pmullt=0x45016C00|...
  （完整寄存器编号代入公式在 stressor 源码注释中给出）
- **GCC 12 intrinsic 门控实测**：`target("+crypto")` 门 AES/SHA1/SHA2/PMULL；`target("arch=armv8.4-a+sha3")` 门 SHA3+SHA512（注意必须用完整 arch= 形式，`+sha3` 简写不生效）；`target("arch=armv8.4-a+sm4")` 门 SM3+SM4（同上，且 GCC 把 SM3 归到 sm4 修饰符下）；SM4/SM3 简写 `+sm4` 属性对 sm3 intrinsic 不生效
- **模拟验证命令模板**：`LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64 /tmp/qemu-out/qemu-aarch64 -cpu max ./stress-ng --<sve2-stressor> 1 -t 5`（stress-ng 需静态链接或 qemu 能解析动态库——qemu 用户态仿真用宿主 ld.so 路径，本机路径可用）
- openEuler 定制 qemu-system-aarch64 无法标准引导（ubios 定制卡死），全系统仿真路线放弃，用户态仿真足够覆盖指令级验证

## 1. 目标与定位

**目标**：对标本仓库对 x86 的支持水平，把 ARM64 服务器芯片（Kunpeng 950 7592C）的每个功耗/计算单元压到饱和，使潜在 SDC 故障核更容易被激发，配合 SDCShield（golden 校验器）定位故障核。

**定位不变**（沿袭 D1 决策）：stress-ng = 扰动器（压电压/热/时序/功耗裕量），SDCShield = 校验器。新增能力同时自带 KAT（已知答案测试）交叉校验——按 R2 结论，"硬件指令路径 vs 软件参考实现"的双路径比对本身就是廉价 SDC 探测器（已落地的 crc32 cpu-method 即此模式）。

## 2. 研究结论 → 设计映射（为什么是这些 patch）

### 2.1 来自 R2（SDC 前沿实证）的负载设计杠杆

| 杠杆 | 实证强度 | 本方案落点 |
|---|---|---|
| A1 全核并发 + 邻居忙（电流/供电噪声独立于温度） | 阿里 SOSP'23 原文 | 已有（sdc-run full 模式）；P6 pair 模式强化 |
| A2/A3 跨核一致性 + SMT 兄弟同压 | 阿里 Obs.4 原文 | **P6 SMT 配对争用矩阵**（FIRESTARTER 方法论移植） |
| B1 热浸润（SDC 频率与温度指数关系，有最低触发阈值） | 阿里 Obs.10 原文 | **P5 hot→verify 时序编排**（发热负载在前、校验负载紧随） |
| C1 负载阶跃制造 di/dt（Ripple 7% 覆盖仅来自模式切换） | Meta 原文 | 已有 `--varyload` 6 波形；P5 编排波形轮换 |
| D1 长序列/真实程序上下文（ITHICA：100 台坏服务器仅 1 台单指令可复现） | 原文（与阿里结论冲突→两者都要） | **P1 armcrypto**（多轮真实密码算法=天然长序列）+ 既有 cpu-method all |
| D2 FMA 密集 + 最坏翻转操作数 | 多篇 | 已有（fma/matrix/sve2）；P2 向量寄存器堆翻转补齐 |
| D3 随机化数据 | Meta 原文 | 各新 stressor 沿用 random_buffer 惯例 |
| E1/E2 长 soak + 周期重复 | 原文 | 已有（-t 小时级、sdc-run） |

### 2.2 来自 R1（x86 基线）的缺口对标

x86 用户有而 ARM64 用户没有的（且目标机有硬件的）：硬件 RNG 压测（rdrand→RNDR）、加密单元直打（ipsec-mb→armcrypto）、向量寄存器压测（XMM→v/z-regs）、计数器微基准（rdtsc→CNTVCT）、缓存维护指令（clflush 家族→DC CVAC/ZVA）、功率遥测（RAPL→hwmon）。**ARM 服务器无硬件冗余（Neoverse 无 lockstep、RAS 对无检错通路 SDC 无能为力，R2 §3），软件扰动器+golden 比对是唯一路线。**

### 2.3 来自 R3（饱和负载）的单元覆盖

950 上完全没有 stressor 能碰到的功耗/面积大户：**SVE2 加密（sveaes/svepmull/svesha3/svesm4）、SM3/SM4、SVE2 gather/scatter（LSU 最重负载）、FCMLA 复数 MAC、BFDOT/SDOT 点积**。部分覆盖的：向量寄存器堆（只有 GPR 版 regs）、DC ZVA（LSU 免 RFO 写路径）。

### 2.4 开发机验证能力（已实测）

开发机 Kunpeng 920 特性：aes/sha1/sha2/asimddp/asimdhp/asimdrdm/atomics/crc32/jscvt/dcpop（无 sve/sm3/sm4/sha3/rng/dit/lrcpc）。因此：
- **本机全功能验证**：NEON AES/SHA1/SHA2/PMULL、v0-v31 寄存器、DC ZVA/CVAC、CNTVCT、SMT 无（pair 模式退化为单线程验证脚本逻辑）
- **本机诚实跳过 + 目标机全功能**：SVE2 全系（含 SVE2 加密 .inst）、SM3/SM4、SHA3、RNDR、z/p-regs

可行性已实测：`target("+crypto")` intrinsic 编译运行通过；`.inst` 数值编码编译通过（GCC 12 arm_sve.h 无 SVE2 加密 intrinsic——**必须走 .inst**，SM4E/AESE 编码已验证可编译）；v-reg/z-reg asm 变量可用；DC ZVA/CVAC/CNTVCT EL0 可执行；HWCAP2 位内核头文件齐全（SVEAES=bit2/SVEPMULL=3/SVESHA3=5/SVESM4=6/RNG=16）。

## 3. Patch 序列（one patch per unit，每 patch 一个 commit + push）

> 通用纪律（每个 patch 必须全部满足，不再逐条重复）：
> 1. `make clean` 后 `make -j$(nproc)` 零新增警告/错误
> 2. 新 stressor 走 5 处登记（stress-*.c / Makefile STRESS_SRC / core-stressors.h MACRO / core-opts.h OPT_* / core-opts.c long_options）+ help[] + stress-ng.1
> 3. 功能验证：真实命令输出（passed / 诚实 skip 原因）
> 4. 回归：`--zombie 1 -t 5` passed + 一个未相关 stressor
> 5. x86-64 非回归：改动全部在 `STRESS_ARCH_ARM`/`__aarch64__`/`HAVE_*`（x86 上为假）守卫内，或纯新增文件
> 6. 分支：从 main 拉 `port/arm64-saturation-sdc`，逐 patch push
>
> 目标机（950）验收门在每个 patch 的"目标机验证"小节，需真机执行后回填结果。

### Wave 1：本机全功能验证（高价值/低成本，先做）

#### P1+P6: stress-armcrypto.c — 加密扩展 stressor（NEON+SVE2/SM3/SM4/SHA3 全部 13 方法）— DONE ✅ (commit on port/arm64-saturation-sdc)
- **内容**：新 stressor `armcrypto`，方法表（仿 stress-vnni.c:190/485 的 `{name, func, capable, expected, verify}` 结构）：
  - `aes`：`vaeseq_u8/vaesmcq_u8`（≥4 独立流交错隐藏延迟，R3 模式）多轮 + 软件参考 AES 比对
  - `sha1`：`vsha1h_u32/vsha1su0q_u32/vsha1su1q_u32` 压缩函数 vs 软件参考
  - `sha256`：`vsha256hq_u32/vsha256h2q_u32/vsha256su0q_u32/vsha256su1q_u32` vs 软件参考
  - `sha512`：`vsha512h_u64/vsha512h2q_u64/vsha512su0q_u64/vsha512su1q_u64` vs 软件参考
  - `pmull`：`vmull_p64/vmull_high_p64`（GHASH 乘法，多累加器交错）vs 软件参考
- **实现要点**：全部函数 `__attribute__((target("+crypto")))`（复用 stress-cpu.c:1675 hw_crc32 已验证模式，零全局 march 改动）；capable 函数查 `getauxval(AT_HWCAP) & (HWCAP_AES|HWCAP_SHA1|HWCAP_SHA2)`；KAT = 软件 C 参考实现 vs 硬件轮函数，mismatch 即 pr_fail（位级诊断格式复制 stress-fma.c 已落地模式：下标+expected/actual 十六进制+xor popcount）——**这使每个方法自带 SDC 检出能力**
- **文件**：新增 stress-armcrypto.c；Makefile/core-stressors.h/core-opts.h/core-opts.c/stress-ng.1 五处登记；`--armcrypto-method M`、`--armcrypto-ops N`
- **本机验证**：`./stress-ng --armcrypto 4 --verify -t 10` → passed: 4（aes/sha1/sha256/sha512/pmull 全部本机有硬件）；`--armcrypto-method aes --verify` 逐方法 passed；故障注入（临时翻转一字节）确认 pr_fail 输出格式
- **目标机验证**：同命令 passed（950 NEON 加密同样在）
- **SDC 价值**：密码轮函数是组合逻辑最深的单元（R1 缺口 2、R3 §1 加密行）；多轮真实算法 = D1 长序列；KAT = 自检黄金负载

#### P2+P8: stress-regs.c — NEON v0-v31 + SVE z0-z31 寄存器堆 — DONE ✅ (cd0196dd8)
- **内容**：ARM64 段（现 1922-2010 仅 x0-x30 GPR）扩展：`register uint8x16_t v0 __asm__("v0")` … v31 全绑定 + XOR/ADD 高翻转循环（操作数交替 0x55/0xAA 模式压占空比，R2-G1）+ 末尾校验已知值（寄存器堆读写错误=SDC 直接检出）
- **对标**：x86 XMM0-7 版本（stress-regs.c:147-175）先例
- **本机验证**：`./stress-ng --regs 4 -t 5` → passed（v-reg 路径实测可用）；objdump 确认 v0-v31 真被绑定
- **目标机验证**：同命令 passed
- **SDC 价值**：32×128-bit 寄存器堆 = 大面积 SRAM，高翻转读写是标准 SDC 激发手段（R1 缺口 3）

#### P3: stress-tsc.c — ARM CNTVCT 分支 — DONE ✅ (59ae24272)
- **内容**：补第五架构分支：`mrs %0, cntvct_el0`（对标 LoongArch/RISC-V/PPC/s390 四分支，stress-tsc.c:51/72/183/205）；读取延迟微基准 + 跨读单调性校验（计数器回退 = pr_fail）
- **本机验证**：`./stress-ng --tsc 1 -t 5` → passed（CNTVCT EL0 实测可读）
- **目标机验证**：同命令 passed
- **SDC 价值**：系统计数器通路压测 + 时延测量基础设施；上游友好（四架构先例）

#### P4: DC CIVAC/CVAC/ZVA — DONE ✅ (8e1874136)
- **内容**：
  - stress-cache.c：`--cache-flush` ARM 分支加 `dc civac` 内联汇编循环（对标 x86 clflush / RISC-V CBO，man 从 "x86 / RISC-V" 扩为含 ARM）
  - stress-memrate.c：新方法 `zva` —— `dc zva` 整行免 RFO 清零写（对标 x86 write64ds 的"特殊写路径"位；R3：LSU 带宽模式）
- **本机验证**：`--cache 2 --cache-flush -t 5` passed；`--memrate 1 --memrate-method zva -t 5` passed（DC ZVA/CIVAC EL0 实测可执行）
- **目标机验证**：同命令 passed + 对照 `write64` 方法看 ZVA 带宽形态
- **SDC 价值**：缓存维护通路故障是 coherence 类 SDC 直接来源（R1 缺口 7）

#### P5: sdc-run.sh pair 模式 + preheat — DONE ✅ (76c730bbc)
- **内容**（纯脚本，零 C 风险）：
  - 新模式 `pair`：对每物理核 sibling 对（`--taskset <sib>`）跑工作负载矩阵 `[fma×fma（共享向量管）、fma×cpu-matrixprod（分派端口）、armcrypto×fma（独立单元应近线性）、cacheline×cacheline（共享 L1/L2 应恶化）]`，每组合独立 yaml；吞吐比 ≈0.5 = 完全共享资源、≈1.0 = 私有——为 950 刻画 SMT 争用图谱（R3 §4：ARM SMT2 无公开资料，只能自研实测）
  - full 模式加 `--preheat MINS` 阶段：先纯发热负载（fma+vecfp 满核），再切 verify+SDCShield 窗口（R2-B2：testcase Y 只有在发热的 X 之后跑才出错的实证）
  - varyload 波形轮换建议注释（R2-C1 模式切换 7% 独有覆盖）
- **本机验证**：`NG=./stress-ng ./scripts/sdc-run.sh pair -t 30` 全核轮转 rc=0（本机无 SMT，pair 退化为单线程对，验证脚本逻辑）；full 模式 preheat 阶段实测执行
- **目标机验证**：pair 模式产出 191 核 × 4 组合争用矩阵 yaml
- **SDC 价值**：A2/A3（SMT 兄弟同压 + 一致性）+ B2（热时序）两个最强实证杠杆的直接实现

### Wave 2：目标机全功能（本机诚实跳过）

#### P6: 并入 P1 一次实现（用户指令要求模拟验证 SVE → QEMU -cpu max 下 13 方法全 passed）— DONE ✅
- **内容**：armcrypto 方法表追加（各方法独立 HWCAP2 门控）：
  - `sm3`：`vsm3ss1q_u32/vsm3partw1q_u32/vsm3partw2q_u32/vsm3tt1aq_u32...`（NEON，HWCAP_SM3=bit18）vs 软件参考
  - `sm4`：`vsm4eq_u32/vsm4ekeyq_u32`（NEON，HWCAP_SM4=bit19）vs 软件参考（GB/T 32907 测试向量）
  - `sha3`：`veor3q_u8/vxarq_u8/vbcaxq_u8`（NEON，HWCAP_SHA3=bit17）Keccak 轮 vs 软件参考
  - `sve2-aes`：`.inst 0x4482e420...`（AESE/AESD/AESMC/AESIMC z 形式，HWCAP2_SVEAES=bit2）
  - `sve2-pmull`：PMULLB/PMULLT `.inst`（HWCAP2_SVEPMULL=bit3）
  - `sve2-sha3`：EOR3/XAR/RAX1/BCAX `.inst`（HWCAP2_SVESHA3=bit5）
  - `sve2-sm4`：SM4E/SM4EKEY `.inst`（HWCAP2_SVESM4=bit6）
- **实现要点**：GCC 12 arm_sve.h 无 SVE2 加密 intrinsic（实测），**必须 .inst 数值编码**；编码从 Arm ARM 指令编码公式计算（base|Rn<<5|Rd），SM4E/AESE 编码已本机验证可编译；**KAT 软件比对本身就是编码正确性的验收门**（编码错 → SIGILL 或结果错 → 被抓）；NEON 形式助记符 binutils 2.41 需 `.arch armv8.4-a+crypto` 类指令或走 intrinsic（优先 intrinsic）
- **本机验证**：构建零警告；`--armcrypto-method sm3,sm4,sha3,sve2-aes,...` 各自诚实 skip（"CPU does not support ..."，对照 pr_inf_skip 惯例）；`--armcrypto-method all` 跳过无硬件方法、运行有硬件方法 → passed
- **目标机验证**：全部方法 passed + KAT 无 mismatch；`--armcrypto 190 --verify -t 1h` 满核
- **SDC 价值**：950 上最大且完全未被触碰的独立功耗/面积单元（sveaes/svesha3/svesm4/svepmull/sm3/sm4），KAT 自检

#### P7: stress-rdrand.c ARM RNDR/RNDRRS 分支 — DONE ✅ (0272aca00)
- **内容**：对标 x86（46-95）/ppc64 darn（97-133）分支结构加 ARM 段：`mrs %0, RNDR`（`s3_3_c2_c4_0`，编码已验证可编译）/`RNDRRS`（`s3_3_c2_c4_1`）；capable 查 `getauxval(AT_HWCAP2) & HWCAP2_RNG`（bit16）；复用 32×unroll 循环与统计校验（位卡死检测 = RNG 单元 SDC 探针）
- **本机验证**：`--rdrand 1 -t 5` → 诚实 skip（"CPU does not support the RNDR instruction"）
- **目标机验证**：`--rdrand 95 --verify -t 10m` passed；吞吐与统计 sanity
- **SDC 价值**：独立熵源 IP 的高频读取（R1 缺口 1；x86/ppc64 平行分支 = 低风险 1:1 对标）

#### P8: 并入 P2 一次实现（SVE 段 HWCAP_SVE 动态门控 + target 属性，QEMU 验证）— DONE ✅
- **内容**：`__ARM_FEATURE_SVE` 守卫段：`register svfloat64_t z0 __asm__("z0")` 全绑定 + 谓词寄存器翻转 + 校验；z 堆是 950 上 VL×256-bit×32 的大 SRAM
- **本机验证**：构建零警告（守卫编译进 SVE2 march 构建路径）+ 本机诚实 skip 路径
- **目标机验证**：`--regs 4 -t 5` passed 且 objdump 确认 z0-z31 绑定
- **SDC 价值**：SVE 寄存器堆 = 950 最大寄存器阵列，高翻转+校验

#### P9: stress-sve2.c gather/fcmla/bfdot 方法表 — DONE ✅ (79cac2355)
- **内容**：stress-sve2.c 引入 `--sve2-method` 方法表（当前 fmla+bitperm 固定双跑）：
  - `gather`：`svld1_gather_u64index` 向量索引非连续访存（R3：LSU 最重负载，跨行/页每元素 4-8+ 周期）
  - `fcmla`：`svcmla`/FCMLA #0/#90 交替（复数 MAC，1 条顶 NEON 4×FMLA+shuffle）
  - `bfdot`：`svbfdot_f32`（bf16 点积，HWCAP2_SVEBF16=bit12）+ `svdot_s32`（i8mm，HWCAP2_SVEI8MM=bit9）
  - `fmla`/`bitperm` 保留为默认（向后兼容）
- **本机验证**：构建零警告 + 诚实 skip；回归 `--sve2 1 -t 5` 行为不变
- **目标机验证**：逐方法 passed；gather 方法对照连续 LD1D 的 bogo-ops 差（LSU 压力形态证明）
- **SDC 价值**：LSU/点积/复数 MAC 三条 950 独立数据通路补齐（R3 §1）

### Wave 3：框架/生态（中价值）

#### P10: core-rapl — ARM hwmon 功率遥测 — DONE ✅ (d22364ac7)
- **内容**：`--rapl`/`--raplstat` 放宽 guard：aarch64+Linux 下读 hwmon `power*_input`（`/sys/class/hwmon/hwmon*/name` 匹配已知控制器 + power*_input 存在即启用）；无传感器 → 诚实 "no power sensors" 退出
- **本机验证**：本机无 power*_input（实测）→ 干净的不可用报告；构建零警告
- **目标机验证**：`--rapl` 打印瓦数（950 BMC hwmon 预期有；若无则诚实报告）
- **SDC 价值**：打满证明（功耗曲线）+ 热-故障相关性（R1 缺口 6）
- **风险**：openEuler hwmon 命名多样，匹配表保守起步

#### P11: fma SVE2 运行时分发（FMV 等价）— DONE ✅ (687d14b39)
- **内容**：aarch64 FMV 需 GCC 15+（实测 GCC 12.3 拒绝 target_clones），用 `target("+sve2")` 函数属性 + HWCAP 运行时分发做 stress-fma.c 热函数双版本（NEON 版 + SVE2 版）——同一 binary 在 920（NEON 路径）与 950（SVE2 路径）都打满
- **本机验证**：构建零警告 + NEON 路径 `--fma 4 --verify -t 5` passed 不变
- **目标机验证**：objdump 确认 SVE2 版本存在 + 运行时走 SVE2 路径（pr_dbg 或性能形态）
- **SDC 价值**：跨机部署单 binary 场景下 SVE 管线不打折（R1 缺口 4）
- **风险**：改动共享热路径，回归面大；只做 fma 一个试点，成功再推广

#### P12: 文档总扫尾 — DONE ✅ (7876bfd39)
- **内容**：README.md 特性表（ARM64 能力矩阵：本方案新增 vs x86 对标）、stress-ng.1 全部新选项复查、`--ignite-cpu` man 描述修正（通用 cpufreq 路径对 ARM 生效，代码 core-ignite-cpu.c:79-147 已读实——**先在目标机实测再改**，不实测定性为"文档滞后"不盲改）
- **验证**：man 渲染逐条目检；README 与实际 stressor 列表 diff 校对

### Backlog（明确缓做，含理由）

| 项 | 理由 |
|---|---|
| NEON SDOT/UDOT vnni 方法 | 价值低（P9 bfdot 覆盖点积单元更全）；开发机有 asimddp 可验证，P1-P12 后补 |
| DMB/DSB 指令风暴 stressor | membarrier（syscall）+ lockbus + atomic 已部分覆盖；独立指令风暴收益待证 |
| priv-instr ARM 扩充（at 指令等） | 中低价值，每条陷阱走完整异常注入，与 SDC 激发关系弱 |
| MIDR/ID_AA64* 枚举 stressor | 纯枚举读，翻转检出靠重复读一致性，收益薄 |
| LSE 显式指令方法 | 目标机构建带 armv8.6 march 时 __atomic 自动产 LDADD/CAS；开发机 outline-atomics 也会解析到 LSE——重叠 |
| --perf topdown/Neoverse 专有事件 | 框架级改动大，且需目标机 PMU 事件清单先行（`ls /sys/bus/event_source/devices/`）——作为目标机侦察阶段输出再定 |
| SMT 争用刻画进 C 代码（--smt-contention） | 先看 P5 pair 模式的实测数据形态再决定是否内建 |

## 4. 实施顺序与依赖

```
P1 armcrypto(NEON) ──→ P6 armcrypto(SVE2/SM3/SM4 扩展)     [方法表同文件，先后依赖]
P2 regs(v) ──────────→ P8 regs(z/p)                        [同文件两段，先后依赖]
P3 tsc ── 独立
P4 cache-flush/memrate-zva ── 独立
P5 sdc-run.sh pair/preheat ── 依赖 P1（pair 矩阵用 armcrypto 组合）可先上无 armcrypto 的 3 组合
P7 rdrand ── 独立
P9 sve2 methods ── 独立（改已有 stressor）
P10 rapl/hwmon ── 独立
P11 fma FMV 试点 ── 独立（最后做，回归面大）
P12 docs ── 最后
```

建议执行序：P1 → P2 → P3 → P4 → P5 → P6 → P7 → P8 → P9 → P10 → P11 → P12（Wave1 全部本机可验收，先攒确定性；Wave2 逐个目标机验收）。

## 5. 风险与未确证事项（诚实清单）

1. **Kunpeng 950 零公开微架构资料**（R3 §6）：所有"饱和"判定最终以目标机 PMU/吞吐实测为准；本方案的指令序列以 Neoverse V2 SOG 类比 + Arm ARM 架构语义设计。
2. **SVE2 加密 .inst 编码**：SM4E/AESE 编码公式已本机验证可编译，但**运行正确性只能在目标机证明**——KAT 比对（P6 内置）就是验收门；编码错误表现为 SIGILL（skipped 之外的硬错误）或 KAT mismatch（pr_fail），两者都可诊断。
3. **binutils 2.41 SVE2 加密助记符语法有兼容问题**（实测 "comma expected at operand 3"）：P6 统一走 .inst 数值编码，源码注释标注意图（指令名+编码出处），不赌助记符。
4. **hisi uncore PMU 在本机 perf 报 not supported**（perf_event_paranoid=2 非根用户）：目标机 root 下预期可用，待真机验证；这影响"饱和度证明"的自动化，不影响 stressor 本身。
5. **SM3/SM4 KAT 测试向量**：SM4 用 GB/T 32907 标准向量，SM3 用 GB/T 32905；SHA/AES 用 NIST 向量——软件参考实现必须先在开发机对拍通过（纯软件路径本机可验）。
6. **--ignite-cpu ARM 生效性未实测**（R1 标注）：P12 修文档前必须目标机实测。
7. **P5 pair 模式在本机只能验证脚本逻辑**（无 SMT）：争用矩阵数据本身必须 950 出。

## 6. 验收总门（方案完成的定义）

- 开发机：12 个 patch 全部构建零警告、各自验证命令输出 passed/诚实 skip、回归干净
- 目标机（950）一趟验证脚本跑完：`scripts/sdc-run.sh all`（full+scan+path）+ 逐 patch 的"目标机验证"命令，产出：满核功耗曲线（P10）、SMT 争用矩阵（P5）、逐核扫描 suspect 汇总——**多证据交叉判读表沿用 findings.md §4（≥2 条命中才确认坏核）**
- 与 SDCShield 协同：full 模式 preheat+verify 窗口内 SDCShield golden 比对无假阳性增加
