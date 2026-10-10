# sdc-stressng

[![CI](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml/badge.svg)](https://github.com/wangxumarshall/sdc-stressng/actions/workflows/multi-os-verify.yml)
[![Release](https://img.shields.io/github/v/release/wangxumarshall/sdc-stressng)](https://github.com/wangxumarshall/sdc-stressng/releases)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](COPYING)
[![Arch](https://img.shields.io/badge/arch-arm64%20%28aarch64%29-orange.svg)](https://github.com/wangxumarshall/sdc-stressng)

**[English](README.md) | 简体中文**

> **面向 arm64 服务器的 SDC 激发引擎。**
> 本仓库是 [stress-ng](https://github.com/ColinIanKing/stress-ng) 的 fork，只有一个使命：
> 把每个 CPU 周期都用于最大化把**静默数据损坏（SDC）**逼出边际核的概率。
> 检测不是本工具的职责——那是 [SDCShield](#分工模型)（配套 golden 基准检测器）的工作。
> 本仓库是激发器。

## 问题背景

Arm64 服务器 CPU 没有核级 lockstep 冗余，RAS 子系统只覆盖会主动上报的错误通路。
一个计算单元若在不上报的情况下翻转了一个比特，产生的是*静默*数据损坏：等到损坏值落盘或沿依赖计算扩散时，定位源头核已是取证难题。真实集群的观察：有的核只在其他核跑热之后才来自检失败；有的机器需要固件下线某个核——这些是边际核的先兆。

在没有硬件冗余的机器上，唯一可行的路线是：

> **软件激发**（本工具）**+ golden 基准检测**（SDCShield）。

## 分工模型

| 工具 | 角色 |
|---|---|
| **SDCShield**（配套） | **检测器**：273 个 golden 基准比对用例，判定*是否*发生 SDC，报告故障核（cpu-mask） |
| **sdc-stressng**（本仓库） | **激发器**：每个周期都用于最大化 SDC 激发概率的负载——di/dt 瞬态、余热、缓存/TLB/互联压力、SMT 争用、边界时序、SDC 定向数据形状。本工具的 `--verify` 只是搭车哨兵，不是第二检测器 |

## 快速上手

三步，每步都经新克隆验证。`NG` 是 `stress-ng` 二进制路径；每条 `sdc-run` 命令的 worker 数、SMT 配对、硬件特性全部从当前拓扑推导——同一条命令在 Kunpeng 920（128 CPU、无 SMT）和 Kunpeng 950（382 CPU、SMT2）上无需修改。

### 第 1 步 · 构建（约 1 分钟）

```bash
git clone https://github.com/wangxumarshall/sdc-stressng
cd sdc-stressng
make clean && make -j$(nproc)     # 自动探测 SVE2 工具链 + 硬件
NG=$PWD/stress-ng
```

aarch64 构建仅在工具链*和*构建主机都支持时才自动注入 SVE2 march；`make MARCH_AARCH64_SVE2=1|0` 强制任一方向。SVE 代码生成的验收门是 objdump 检查（`z` 寄存器指令数 > 0），绝不依赖编译器自动向量化的"信仰"。

### 第 2 步 · 冒烟自检（约 2 分钟）

```bash
$NG --operand-var 4 --verify -t 60   # SDC 定向操作数变异
$NG --addrspace 2 --verify -t 60     # 地址空间形状激发
$NG --llccross 2 --verify -t 60      # 跨内存域一致性乒乓
```

### 第 3 步 · 激发（sdc-run）

`sdc-run.sh` 是编排器：一条命令、一次运行一个输出目录、拓扑推导贯穿始终。按目标自行选择：

```bash
# B · 触发 + verify 哨兵——工具自身的 --verify 搭车随行
NG=$NG ./scripts/sdc-run.sh full -t 7200 --preheat 10

# C · 定位——逐物理核扫描，背景负载保持机器级并发
NG=$NG ./scripts/sdc-run.sh scan -t 120 --keep-bg 64

# D · 归因——对可疑核做数据通路 golden 交叉校验
NG=$NG ./scripts/sdc-run.sh path -t 600 -c 192-381

# E · SMT 争用矩阵——映射核内哪些资源是共享的
NG=$NG ./scripts/sdc-run.sh pair -t 30

# F · A/B 回归——这个构建改变了激发/检测能力吗？
NG_A=/tmp/stress-ng-old NG_B=$NG ./scripts/sdc-run.sh abtest -t 7200

# A · 纯激发——每个周期都给负载，检测全部交给 SDCShield
NG=$NG ./scripts/sdc-run.sh excite -t 7200 --preheat 10 \
    --sdcshield "./run-sdcshield.sh"

# 完整漏斗顺序执行：full → scan → path
NG=$NG ./scripts/sdc-run.sh all

```

结果判读：

| 战役 | rc=0 含义 | 关注的证据 |
|---|---|---|
| `excite` | 激发完成（**不是**"机器健康"） | `sdcshield.log` 中 SDCShield 的 cpu-mask |
| `full` | 无 verify 失配 | `report.txt` 中任意 `verify-failures`；日志中的位级诊断 |
| `scan` | 无可疑核 | `suspects.txt` 列出指标离群的核 |
| `path` | 无通路失配 | 失配即该数据通路 SDC 的直接证据 |
| `pair` | 矩阵跑完 | 各组合速率比：≈0.5 共享资源，≈1.0 独享 |
| `abtest` | 双臂跑完 | `ab_summary.txt` 构建间失配计数对比 |

首次调查建议配合 SDCShield 分阶段联合战役，见
[docs/sdcshield-integration.md](docs/sdcshield-integration.md)。

## 架构

五层，每层一个职责。fork 的所有增量都在这个栈里
（全文：[docs/architecture.md](docs/architecture.md)）：

```
┌────────────────────────────────────────────────────────────────┐
│ L5 编排层   sdc-run.sh: excite / full / scan / path / pair /   │
│             abtest · 拓扑自推导 · preheat · keep-bg · 站点轮换   │
├────────────────────────────────────────────────────────────────┤
│ L4 杠杆层   di/dt 负载阶跃 (varyload) · 余热 (preheat) ·        │
│             SMT 争用 (pair) · 机器级并发 · 长 soak              │
├────────────────────────────────────────────────────────────────┤
│ L3 攻击面   ALU/分支 · OoO 调度器 · 向量 (SVE2/NEON) · crypto   │
│             · 原子 (LSE, lrcpc) · LSU (lsupress, ls64) ·       │
│             缓存层级 · MMU/TLB · 互联                           │
├────────────────────────────────────────────────────────────────┤
│ L2 数据形状  bitgen: 位段扫掠 · 57 项边界值字典 · FP 位型直合成  │
│             · 互补对 · 汉明定向 · 模式混合                       │
├────────────────────────────────────────────────────────────────┤
│ L1 硬件感知  HWCAP/HWCAP2/3 探测 · 拓扑自推导 · SVE2 march 注入 │
│             · 诚实跳过 · 单 binary 跨 920 (NEON) / 950 (SVE2)  │
└────────────────────────────────────────────────────────────────┘
```

## 激发杠杆

来自集群实证与 SDC 文献的方法论
（完整矩阵：[docs/excitation-guide.md](docs/excitation-guide.md)）：

| 杠杆 | 为什么有效 | 入口 |
|---|---|---|
| 机器级并发 | 单核隔离往往不触发；全机并发是实证触发条件 | `sdc-run full` / `excite` |
| 余热 + 顺序效应 | 失败用例只在发热用例之后失败 | `--preheat MINS` |
| di/dt 负载阶跃 | 负载瞬变调制电流摆率 | `--varyload N --varyload-ms`（excite 常驻背景） |
| SMT 同核争用 | 兄弟线程争抢共享执行资源 | `sdc-run pair` |
| SDC 定向数据形状 | 位段扫掠和边界字典对位段敏感缺陷的命中比均匀随机高 >10⁶ 倍 | `--operand-var`、bitgen 全库复用 |
| 地址空间形状 | 巨大随机跨度、逐 VA 位遍历、guard 洞、混合页序——线性负载永远不触达的 MMU/TLB 角落 | `--addrspace`、`--lsupress` VA 游走 |
| OoO 调度器压力 | 依赖链与重命名突发把 ROB/重命名单元推入边界状态 | `--ooopress` |
| 长 soak | SDC 频率低至 0.01 次/分钟——需要数小时不重复的形状负载 | `-t 2h` 起步 |

## 硬件攻击面

| 通路 | 压测工具 |
|---|---|
| 向量管线 | `--sve2`（fmla/gather/fcmla/bfdot/bitperm，golden 交叉校验）、`--fma`（运行时分发 SVE2 内核）、`--vecfp` |
| Crypto 引擎 | `--armcrypto`——13 方法：NEON AES/SHA1/SHA256/SHA512/SHA3/PMULL/SM3/SM4 + SVE2 crypto（`.inst` 编码，GCC 无对应 intrinsic） |
| LSU | `--lsupress`——22 方法覆盖整数/FP/NEON/SVE/原子，跑在每 worker 100GB MAP_NORESERVE VA 映射 + 迁移工作窗口上（MADV_DONTNEED 恒定物理足迹；4 种游走模式含 bitgen TLB tag 位段扫掠）；`--memcpy-method ldp-stp/neon/neon-ld2/sve/sve-gather/ls64` 指令变体拷贝引擎；`--ls64`、`--misaligned`、缓存维护指令（`--cache-flush`/`--cache-clwb` = DC CIVAC/CVAC、`--memrate-method write64zva` = DC ZVA） |
| OoO 调度器 | `--ooopress`——dep-chain（256 步串行链）、indep-max、alt 排空-回填波形、rename-reuse、branch-mix（bitgen 方向、除法臂防 if-conversion）、load-use（队头阻塞）；形状间 42 倍可测 bogo-ops 差 |
| 互联 / L3 | `--llccross`——跨域一致性：共享行乒乓（逐拍 tag 哨兵）、远端写流、远端混合扫描；worker 跨 NUMA node（或 node 内跨 L3 实例）配对 |
| 缓存层级 | `--cacheline`（rand-payload + 所有权 tag）、`--l1cache`（set/way 几何 + bitgen 流）、`--cache`、`--memrate`（4 种写模式） |
| MMU / TLB | `--addrspace`——7 种地址形状配方，全部带校验 |
| 原子 | `--atomic`（RMW 操作数抖动）；lsupress `excl-pair`（ldxr/stxr）、`lse-rmw`（ldadd）、`lrcpc-pair`/`ilrcpc-rmw`（LDAPR/STLR acquire-release，HWCAP 门控） |
| 整数 ALU | `--cpu` 71 方法，种子解锁使操作数跨运行变化 |
| 虚拟内存 | `--vm --vm-method rand-offset`（Fisher-Yates 无放回密集随机偏移 + bitgen 填充） |
| 随机数 / 计数器 | `--rdrand`（RNDR）、`--tsc`（CNTVCT_EL0） |
| 功率遥测 | `--rapl`（arm64 hwmon：SoC/DDR rail） |

所有 SVE2/ls64/RNDR/lrcpc 特性在运行时 HWCAP 门控 + target 属性编译隔离——
单 binary 在任意 aarch64 主机上诚实运行（或诚实跳过）。

## sdc-run 编排器

各战役全表（`excite` 站点默认每 10 分钟轮换——`EXCITE_SLICE` 环境变量可调）：

| 模式 | 用途 |
|---|---|
| `excite` | **纯激发，站点轮换**（2.0）：时间片站点深压（cpu → fma → armcrypto → lsupress → operand-var → memcpy 变体 → addrspace → memrate），每站以 `N_PHYSICAL` 个 worker 独占全机，varyload di/dt 常驻背景。检测全交 SDCShield（`--sdcshield`） |
| `full` | 阶段 1 *触发*：全核负载（cpu + fma + operand-var + addrspace，verify 哨兵开启）+ varyload di/dt + 可选 `--preheat`、`--sdcshield` |
| `scan` | 阶段 2 *定位*：逐物理核扫描（SMT 对），每核 yaml 指标 + 可疑核清单；`--keep-bg N` 扫描期间保持机器级并发 |
| `path` | 阶段 3 *归因*：数据通路 golden 交叉校验（sve2 / ls64 / crc32）——失配即该通路 SDC 直接证据 |
| `pair` | SMT 争用矩阵：每物理核 8 种工作负载组合（fma×fma、fma×cpu、armcrypto×fma、cacheline×cacheline、vm×vm、addrspace×addrspace、atomic×atomic、lsupress×lsupress——执行单元、缓存、MMU/TLB、原子/LSU）；bogo-ops 速率比映射共享拓扑（≈0.5 共享、≈1.0 独享）。运行时长 = 核数 × 8 × 每对秒数；大机器建议 `-c` 抽样 |
| `abtest` | 两个 stress-ng 构建间的 A/B 回归（`NG_A=`/`NG_B=`），冷却间隔，失配计数并排判读 |
| `all` | full → scan → path 顺序执行 |

verify 失配报位级诊断（元素下标、期望/实际、翻转位数、xor 掩码），聚合成每运行目录可 diff 的 `report.txt`。

## 持续集成

每日 15 个 openEuler arm64 容器镜像（20.03 / 22.03 / 24.03 × 5 SP）：

- 全量 `--sequential --verify` 套件（每镜像 330+ stressor）
- 覆盖全部 `*-method` 选项的方法 sweep
- 基准采样（cpu-matrixprod / fma / memcpy / memrate-zva / stream）
- 完整的 stressor × 镜像结果矩阵（bogo-ops/s）发布到 job summary——
  含每镜像的诚实跳过原因

容器镜像：`ghcr.io/wangxumarshall/sdc-stressng:verify-<git-tag>`
（通过 CI 工作流的手动 `publish_image` 触发发布）。

## Release 自包含包

每个 [Release](https://github.com/wangxumarshall/sdc-stressng/releases)
携带**逐镜像自包含包**：release 触发的 CI 在全部 15 个 openEuler 镜像内各自构建
`stress-ng` 并附为 tarball（+ sha256），内容含二进制、`excite.sh` 一键最大激发
入口、`sdc-run` 编排脚本全集、以及全部非 glibc 共享库——解压即用，零构建、零依赖安装：

```bash
tar xzf sdc-stressng-<ver>.openeuler-<your-image>.aarch64.tar.gz
cd sdc-stressng-*/ && ./excite.sh 120     # 2 小时最大激发
```

## 构建

```bash
make clean && make -j$(nproc)
```

拉取后必须 `make clean`（config.h 会再生成）。SVE2 march 注入：自动
（工具链 + 硬件双门控）或 `MARCH_AARCH64_SVE2=1|0` 强制。交叉编译、静态构建、
可选库依赖与非 arm 平台行为与上游 stress-ng 一致——相关文档见
[上游项目](https://github.com/ColinIanKing/stress-ng)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 五层架构、各层职责、设计决策 |
| [docs/excitation-guide.md](docs/excitation-guide.md) | 激发覆盖率矩阵（杠杆 × 通路 × 数据形状）、方法论、gap 路线图 |
| [docs/sdcshield-integration.md](docs/sdcshield-integration.md) | 联合战役手册：拓扑、分阶段脚本、报告关联 |
| [docs/upstream-sync.md](docs/upstream-sync.md) | 上游合并策略与冲突收敛 |
| [CLAUDE.md](CLAUDE.md) | 内部开发指南（中文）：验证纪律、15 轮实战沉淀的代码规则 |
| `stress-ng.1` | man 页（上游 + fork 选项） |
| `docs/superpowers/` | 工程研究与逐轮实施记录 |

## 与上游 stress-ng 的关系

本仓库是 [ColinIanKing/stress-ng](https://github.com/ColinIanKing/stress-ng)
的 fork（基于 0.22.00 发布线）。上游 release 经 PR 周期性合并；合并策略与冲突面收敛措施见 [docs/upstream-sync.md](docs/upstream-sync.md)。所有 fork 增量默认关闭或在不使用时与上游行为完全一致。

stress-ng 是现存久经实战的系统压力测试工具之一，本 fork 站在这个地基之上——
向 [Colin Ian King](https://github.com/ColinIanKing) 与上游贡献者致以巨大敬意。

## 安全

> ⚠️ **本工具故意把硬件推到正常工作包络之外。** 激发负载的设计目标就是
> 压缩时序裕量、最大化功耗和 di/dt、加热晶片。**只在专用于压力测试的机器上运行。**
> 目标机上的预期后果：持续满功率、热事件、内存类 stressor 下 OOM，以及（这正是目的）
> 可能发生的 SDC 会损坏同时在跑的任何东西。绝不在承载生产负载或不可替代数据的机器上运行。

安全政策与报告渠道见 [SECURITY.md](SECURITY.md)。

## 许可证

GPL-2.0（见 [COPYING](COPYING)），继承自 stress-ng。fork 修改以同一许可证发布。
