# SDC 压测与激发运维手册（runbook）

> **受众**：在目标机（CP1，Kunpeng 950 级 arm64 服务器）上执行 SDC 压测的工程师。
> **定位**：中文内部操作手册——命令可照抄、每步输出有判读、发现可疑核后有明确下一步。
> README 快速上手是三步摘要，本手册是完整展开。
> 压测方法论依据（为什么这样压）见 [excitation-guide.md](excitation-guide.md)，
> 联合战役设计细节见 [sdcshield-integration.md](sdcshield-integration.md)，本文不重复。

## 分工模型（一句话）

**本工具只负责激发**——压缩时序裕量：di/dt 阶跃、功耗/余热、缓存/TLB 压力、SMT 争用、
SDC 定向数据形状。**SDCShield 负责检测**——273 个 golden 用例判定是否发生 SDC、报故障核
（cpu-mask）。两个工具并行跑在同一台机器上，事后交叉判读日志。
本工具的 `--verify` 只是搭车哨兵，不是第二检测器。

## 0. 安全红线（每次开跑前核对）

- **只在专用压测机上运行，绝不上生产负载或存有不可替代数据的机器。**
  激发负载的设计目标就是把硬件推出正常工作包络。
- 预期后果（都是正常的，不是故障）：
  - 持续满功率、热事件（CP1 已知 2.3GHz 固频，boost 关闭，但热浸润仍是触发条件）；
  - 内存类 stressor 下 OOM（full 的 addrspace 约用 1/4 物理内存，excite 约 1/8）；
  - **可能发生的 SDC 会损坏同时在跑的任何东西**——这正是目的。
- 大战役（full/scan/excite 全机负载）期间不要在同机做其他工作。

## 1. 阶段 0 · 准备

### 1.1 构建激发器

```bash
cd <sdc-stressng 仓库路径>
make clean && make -j$(nproc)      # 拉取后必须 make clean（config.h 再生成）
NG=$PWD/stress-ng
$NG --version                      # 确认版本与 aarch64
```

CP1 上 SVE2 march 自动注入（工具链 + 硬件双门控，无需手工干预）。
如需确认 SVE2 代码生成生效（验收门，不依赖编译器"信仰"）：

```bash
objdump -d stress-ng | grep -cE '\tz[0-9]+'    # z 寄存器指令数 > 0 即有 SVE 代码
```

### 1.2 SDCShield 就绪确认

SDCShield 按其仓库文档部署，确认 golden 套件可运行。本手册所有示例中
`--sdcshield "./run-sdcshield.sh"` 是占位写法——**替换为 CP1 上 SDCShield 的实际启动命令**。
`sdc-run.sh` 会自动为并行检测追加合理参数（`-T forever`、同时长、失败继续、
排除与本工具重复的用例），不需要手工传这些。

### 1.3 负载前取证（事后异常才可归因）

```bash
mkdir campaign-$(date +%Y%m%d) && cd campaign-$(date +%Y%m%d)
lscpu > topology.txt
cat /sys/devices/system/cpu/{online,isolated,offline} >> topology.txt
dmesg | grep -iE 'mce|edac|hardware error|isolat' > ras-baseline.txt 2>/dev/null
ipmitool sel list > bmc-sel.txt 2>/dev/null || true
```

**判读**：如果已有核被隔离/下线（CP1 有核隔离与自检失败史），记录在案——
固件已下线的核无法进入后续 scan 扫描，且这本身就是边际核先兆。

## 2. 冒烟自检（约 3 分钟）

大战役前确认工具链正常——坏工具带进 2 小时战役是纯浪费：

```bash
$NG --operand-var 4 --verify -t 60   # SDC 定向操作数变异
$NG --addrspace 2 --verify -t 60     # 地址空间形状激发
$NG --llccross 2 --verify -t 60      # 跨内存域一致性乒乓
```

**预期**：三条各报 `successful run completed`、`failed: 0`。
任何 verify 失配（`expected/actual/xor` 位级诊断行）或崩溃 → 停下，
先按工具本身故障排查（单机单次失配优先怀疑软件，见 §6.1 弱信号行），
向仓库报 issue，不要继续大战役。

## 3. 阶段 1 · 触发（excite / full）

### 3.1 选型

| 场景 | 用哪个 |
|---|---|
| 首次调查（联合战役阶段 1，[sdcshield-integration.md](sdcshield-integration.md) 剧本） | `full`——verify 哨兵 + SDCShield 双 oracle |
| 纯激发 / 扩大通路覆盖（armcrypto、memrate、vm-rand-offset 这些 full 里为 verify 预算让路的通路） | `excite`——零 verify 开销，每个周期都给负载 |
| SDCShield 未部署 | 只能 `full`（自带哨兵）；但强烈建议部署 SDCShield——本工具的 verify 覆盖远窄于 273 用例 |

### 3.2 full——触发 + verify 哨兵

```bash
NG=$NG ./scripts/sdc-run.sh full -t 7200 --preheat 10 \
    --sdcshield "./run-sdcshield.sh"
```

- `--preheat 10`：先 10 分钟全核 fma+vecfp 纯热负载（无 verify），verify 窗口在热机上打开
  ——余热 + 顺序效应是实证触发条件（失败用例只在发热用例之后失败）。
- 负载构成（脚本自动按拓扑推导 worker 数，无需手工指定）：
  cpu（71 方法全谱）+ fma（verify）+ operand-var（verify）+ addrspace（verify）
  + varyload di/dt 阶跃常驻 + 中断/温度观测。
- 产物目录 `sdc_full_sdc_<时间戳>/`（shell 里 `sdc_full_*` Tab 补全；下同，各模式目录均带 `sdc_` 前缀的时间戳）：

| 文件 | 内容 |
|---|---|
| `topology.txt` | 模式/时长/拓扑/特性快照（sve2/ls64/crc32/隔离核） |
| `preheat.log` | 热浸润阶段日志 |
| `A_full.log` / `A_full.yaml` | 主负载日志与指标（verify 失配在这里） |
| `sdcshield.log` | SDCShield 并行检测结果（cpu-mask 在这里） |
| `interrupts-before/after.txt` | 中断计数快照 |
| `report.txt` | 一页汇总（脚本结束自动生成，可跨 run 目录 diff） |

- **rc=0 = 无 verify 失配**；非 0 → 看 `report.txt` 的 verify-failures 与
  `A_full.log` 里的位级诊断（元素下标、期望/实际、翻转位数、xor 掩码）。

### 3.3 excite——纯激发，站点轮换

```bash
NG=$NG ./scripts/sdc-run.sh excite -t 7200 --preheat 10 \
    --sdcshield "./run-sdcshield.sh"
```

- 站点轮换制：每站独占全机深压一个通路，默认每 10 分钟换站
  （`EXCITE_SLICE` 环境变量可调，单位分钟）。站点表：
  cpu → fma → armcrypto → lsupress → operand-var → memcpy-ldp-stp → memcpy-neon
  → addrspace → memrate；SVE2 机器追加 memcpy-sve、lsupress-sve（CP1 命中）。
- **`-t` 预算包含 preheat 阶段**：站点轮换在 preheat 完成后的剩余时间内进行
  （`--preheat 10 -t 7200` → 站点实际 6600s；full 模式无此问题，其 `-t` 不含 preheat）。
- varyload di/dt + 中断/温度观测全程常驻背景（`bg-varyload.log`）。
- 产物 `sdc_excite_sdc_<时间戳>/`：`topology.txt`、`preheat.log`、`bg-varyload.log`、
  `station_<站名>.log`（每站一个，随轮换累积）、`sdcshield.log`。
- **rc=0 只表示激发完成，不表示机器健康**——判读看 `sdcshield.log` 的 cpu-mask。
- 长跑示例：8 小时 `-t 28800`；release 自包含包用户 `./excite.sh 480`。

### 3.4 时长与重复

- 两模式默认 2h。SDC 频率低至 0.01 次/分钟——数小时起步，**跨天重复战役**：
  老化类边际核只在重复暴露中显形，单次干净不构成证据（见 §6.3）。

## 4. 阶段 2 · 定位（scan）

```bash
NG=$NG ./scripts/sdc-run.sh scan -t 120 --keep-bg 95 \
    --sdcshield "./run-sdcshield.sh"
```

- **`--keep-bg` 必加**：单核隔离几乎不触发 SDC（全机并发是实证触发条件）。
  keep-bg 在非扫描核上保持背景 matrixprod 负载。推荐物理核数的一半
  （CP1 约 95；`all` 一键漏斗模式自动设为 N_PHYSICAL/2）。
- `-t 120` 是**每核**秒数。CP1 全核扫描 ≈ 191 核 × 120s ≈ 6.5 小时；
  急于出结果可先 `-c` 扫子集，或降低每核秒数。
- 产物 `sdc_scan_sdc_<时间戳>/`：`summary.txt`（总扫描核数/可疑核数）、
  每核 `core_<N>.log` + `core_<N>.yaml` 指标、`bg-load.log`（--keep-bg 时）、
  自带取证快照（`dmesg-baseline.txt`、`edac-*.txt`、`isolated.txt`、`offline.txt`）。
  `suspects.txt` **仅当发现可疑核时生成**——干净 run 里没有这个文件是正常的。
- **rc=2 = 发现可疑核**（终端直接打印 `SUSPECT CORES FOUND:` 与清单）。
- `suspects.txt` 判读：每条 `SUSPECT: core <兄弟对>` 后跟原因行——
  stress-ng rc 非 0 / verify failures / sdcshield failure，三种证据来源独立。

## 5. 阶段 3 · 归因（path）

```bash
# <核号> 取自上一步 suspects.txt（如 192,193 这对兄弟）
NG=$NG ./scripts/sdc-run.sh path -t 600 -c 192,193
```

- sve2 / ls64 / crc32 硬件 vs 软件 golden 交叉校验，`--taskset` 钉在可疑核上。
- **失配 = 该数据通路在该核上发生 SDC 的直接证据**（终端自动 grep 出
  `failed/mismatch/flipped` 行；`path.yaml` 有完整指标）。
- 无对应硬件的项目诚实跳过（终端打印 `skipped, no ... hardware`）——
  CP1 上三项全有。

## 6. 判读与处置

### 6.1 双工具交叉判读表

| sdc-stressng | SDCShield | 判读 |
|---|---|---|
| verify-failures > 0 | 失配 + cpu-mask 点名核 | **强**：双独立 oracle 一致——cpu-mask 为首要嫌疑清单 |
| verify-failures > 0 | 干净 | 弱信号：加长 soak 复跑；同时怀疑本工具 golden 路径（可能是激发器 bug——报 issue） |
| 干净 | 失配 + cpu-mask | 弱信号：损坏通路在当前负载配方之外——查 [excitation-guide.md](excitation-guide.md) 的 gap 路线图 |
| 某核 bogo-ops 离群 | 任意 | 该核**性能**边际，不必然 SDC——列入观察名单 |
| 干净 | 干净 | 本轮无证据；老化类边际需跨天重复战役 |

### 6.2 发现 SDC 后的处置流程

1. **确认复跑**：同参数同核再跑一轮（`path -c <核>` 或 `scan -c <核>`）。
   可重复出现才升级；单次失配按 §6.1 弱信号处理。
2. **翻转位置采集**：
   ```bash
   ./scripts/sdc-flip-collect.sh sdc_full_sdc_<时间戳>
   ```
   从 verify 失配的 xor 掩码统计 64 位逐位翻转直方图 + 位段校准建议
   （按翻转密度排序的 top-8 窗口）。结果反馈给 bitgen 位段参数
   （`--bitgen-band-*`）维护者——把激发数据形状校准到这片硅真实翻转的位段，
   这是真机校准闭环。干净 run 上的预期输出是
   `no xor masks found - nothing to calibrate on`（无失配即无可校准）。
3. **生成报告**：full/abtest 结束时自动产出 `report.txt`；其余模式手工：
   ```bash
   ./scripts/sdc-report.sh <run目录>
   ```
4. **固件处置**：证据链闭合（SDCShield cpu-mask 多轮一致 + path 失配 + 可重复）
   后，按组织的 RAS/硬件处置流程申请固件下线该核；处置完成后复跑 `scan` 确认。

### 6.3 什么算"干净"

单轮 clean × clean 不说明问题。结论标准：多轮（建议 ≥3 轮跨天）联合战役
均无失配、无疑似核，才可记"本轮排查未发现 SDC 证据"。

## 7. 附加战役

### 7.1 pair——SMT 争用矩阵

```bash
# 每物理核取一个代表 CPU（兄弟对首个，如偶数号）；跨 node 抽样
NG=$NG ./scripts/sdc-run.sh pair -t 30 -c 0,16,32,192,208,224
```

- 8 组合 × 每物理核：fma×fma（向量）、fma×cpu（执行端口）、armcrypto×fma（独立单元）、
  cacheline×cacheline（L1/L2 写）、vm×vm（TLB）、addrspace×addrspace（页表遍历）、
  atomic×atomic（LSE 原子）、lsupress×lsupress（LSU 队列）。
  每侧独立进程绑定各自兄弟线程，测的是真争用。
- **时长 = 核数 × 8 × 每对秒数**。CP1 全扫约 12.7 小时——做拓扑测绘时用 `-c` 抽样。
- 判读 `pair-matrix.txt`：每组合两侧 bogo-ops 速率比，**≈0.5 = 该资源两兄弟完全共享，
  ≈1.0 = 独享**——这是在测绘这片芯片的 SMT2 共享拓扑（无公开微架构数据）。
- ⚠️ **历史数据作废**：早期实现把两个 `--taskset` 放进同一进程，兄弟线程放置错误，
  旧 pair 数据全部无效。当前实现已修复（每侧独立进程），数据需重采。

### 7.2 abtest——双构建 A/B 回归

```bash
NG_A=/tmp/stress-ng-old NG_B=$NG NG=$NG ./scripts/sdc-run.sh abtest -t 7200
```

- 两臂各跑一遍 full 配方，中间冷却隔离（`COOLDOWN` 环境变量，默认 300s），
  防止 A 臂余热污染 B 臂——余热是被测的触发条件，不能泄漏到对照组。
- 产物 `sdc_abtest_<时间戳>/{A,B}/` + `ab_summary.txt`，判读规则（已写入 summary）：
  - **B > A**：新构建激发/检测能力更强（期望结果）；
  - **B == A**：无差异，考虑加长 soak；
  - **B < A**：先比对两臂 bogo-ops——可能是跑的负载变少，不是激发变弱。

## 8. 附录

### 8.1 模式速查表

| 模式 | 用途 | `-t` 语义 | 默认 | 关键产物 | rc 语义 |
|---|---|---|---|---|---|
| `excite` | 纯激发，站点轮换 | 总时长 | 7200s | `sdcshield.log`、`station_*.log` | 0 = 激发完成（**非**健康判定） |
| `full` | 触发 + verify 哨兵 | 总时长 | 7200s | `report.txt`、`A_full.*`、`sdcshield.log` | 0 = 无 verify 失配 |
| `scan` | 逐物理核定位 | 每核秒数 | 120s | `suspects.txt`、每核 yaml | 2 = 发现可疑核 |
| `path` | 通路 golden 归因 | 总时长 | 600s | `path.log/.yaml` | 非 0 = 有失配 |
| `pair` | SMT 争用矩阵 | 每对秒数 | 30s | `pair-matrix.txt` | 非 0 = 某组合失败 |
| `abtest` | 构建 A/B | 每臂时长 | 7200s | `ab_summary.txt` | 双臂完成 |
| `all` | full→scan→path 一键漏斗 | scan 每核秒数 | — | 三阶段子目录 | 任一阶段非 0 即非 0 |

### 8.2 一键漏斗

```bash
NG=$NG ./scripts/sdc-run.sh all
# 等价于 full → scan（自动 --keep-bg 物理核一半）→ path 顺序执行，
# 结束打印三阶段 rc 汇总；任何非 0 都意味着收集到了 SDC 证据
```

### 8.3 诚实跳过：920 开发机 vs 950 目标机

单 binary 跨两代芯片，无硬件的特性在运行时 HWCAP 门控并**诚实跳过**（绝不假跑）：

| 特性 | 开发机 Kunpeng 920 | 目标机 CP1 Kunpeng 950 |
|---|---|---|
| SVE2 全家桶（sveaes/svesha3/svepmull/svesm4/…） | 无 → 跳过 | 有 |
| ls64（64B 原子访存） | 无 → 跳过 | 有 |
| RNDR（--rdrand） | 无 → 跳过 | 有 |
| lrcpc/ilrcpc（RCpc 原子） | 无 → 跳过 | 待真机 HWCAP 确认 |
| NEON aes/sha1/sha2/dotprod/fp16 | 有 | 有 |

排查"为什么本机 skipped"时先对照此表；同一命令在两台机器上无需修改
（worker 数、SMT 配对、特性全部从当前拓扑推导）。

### 8.4 常用环境变量

| 变量 | 作用 | 默认 |
|---|---|---|
| `NG` | stress-ng 二进制路径 | `./stress-ng` |
| `EXCITE_SLICE` | excite 每站时长（分钟） | 10 |
| `COOLDOWN` | abtest 两臂间冷却（秒） | 300 |
