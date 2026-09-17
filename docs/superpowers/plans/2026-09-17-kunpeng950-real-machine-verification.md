# Kunpeng 950 真机验证方案（port/arm64-saturation-sdc 分支，11 commits）

日期：2026-09-17
分支：`port/arm64-saturation-sdc`（aac012d56，已推 origin）
适用对象：CP1 = Kunpeng 950 7592C（2 socket × 95 核 × SMT2 = 382 CPU，SVE2 全家桶 + ls64 + rng + sm3/sm4/sha3/sha512 + lrcpc + dit）
前置事实（不得在真机上重新推导，直接引用）：
- 开发机（920，无 SVE/SMT/SM3/SM4/RNG）已完成的验证与**未能**覆盖的路径见 §1
- CP1 已有核隔离/自检失败史；node1 疑少 1 物理核（96+95）——阶段 0 先取证
- SDCShield 定位 = 校验器，stress-ng = 扰动器（D1 决策不变）
- qemu 8.2 `-cpu max` 与真机的已知差异：qemu 的 NEON HWCAP_SM4 位=0（真机预期=1）；qemu 无法验证性能/带宽形态、PMU 事件、hwmon、cpufreq、SMT 拓扑——这些**只能在真机验**

## 1. 验证目标：真机要补什么

每条 = 开发机/qemu 验证不了的点，逐条给验收标准：

| # | 验证目标 | 为什么只能真机 | 验收标准 |
|---|---|---|---|
| V1 | armcrypto 13 方法全部 passed | qemu 只证指令编码正确，不证真机加密单元时序/功耗 | `--armcrypto-method all` + 逐方法 `failed: 0`，且 bogo-ops/s 量级合理（AES 类 GB/s 级，SM4 对比 NEON 参考吞吐） |
| V2 | sve2 5 方法（fmla/gather/fcmla/bitperm/bfdot）golden verify | qemu 不证真实 VL（真机 VL 待实测，qemu 是 512-bit）与数据通路时序 | SVE2 march 构建 + 逐方法 `--verify` passed；`pr_dbg` 或 objdump 确认 SVE 路径执行 |
| V3 | fma SVE2 分发真的选中 SVE2 表 | HWCAP_SVE=1 时 `fma_sve2_supported()` 返回真——但 qemu 无法证明性能差异 | SVE2 构建下 fma bogo-ops/s 显著高于 NEON 构建基线（记录两构建对比数据） |
| V4 | regs z0-z31 全 32 寄存器绑定 | qemu 不验真机寄存器堆时序 | `--regs` passed + objdump（构建产物）含 z0-z31；运行无 SIGILL |
| V5 | rdrand RNDR 吞吐 + 熵耗尽行为 | qemu 的 RNDR 是软件模型 | passed；`--rdrand-seed` passed；记录吞吐（真机 RNDR 有 16 项缓冲，打空会掉速——观察 bogo-ops 曲线是否呈此形态） |
| V6 | DC ZVA/CIVAC/CVAC 真实带宽 | qemu 无缓存模型 | `write64zva` passed 且吞吐 ≥ write64（免 RFO 应更快）；`--cache-flush` passed |
| V7 | pair 模式 SMT 争用矩阵 | qemu 单核无 SMT | 191 核 × 4 组合全跑完 rc=0；矩阵数据落盘（fma×fma 应显著低于 2×单线程 fma——共享向量管） |
| V8 | hwmon 功率遥测 | 开发机无传感器 | `--raplstat 2` 打印真实瓦数；`--rapl` 计入 metrics |
| V9 | 全量回归：默认构建不破坏既有 | 真机指令集更全，编译面更广 | `make` 后跑 §5 冒烟清单全绿 |
| V10 | sdc-run.sh 全漏斗（full→scan→pair→path） | 依赖 SMT 拓扑/382 CPU/NUMA | `all` 模式 rc 收敛、输出目录结构完整、suspects 汇总生成 |

## 2. 阶段 0：环境取证与构建（必做，约 30 分钟）

### 0.1 基线快照（复用既有 sdc-run 阶段 0 逻辑，手工执行一次留档）

```bash
mkdir -p ~/sdc950_$(date +%m%d) && cd ~/sdc950_*
lscpu -e=CPU,CORE,SOCKET,NODE > topology.txt
cat /sys/devices/system/cpu/isolated /sys/devices/system/cpu/offline > iso_offline.txt
dmesg -T | grep -iE 'edac|mce|fault|deconfig|cpu.*err|lockup|ras' > boot_cpu_err.log 2>&1
grep -H . /sys/devices/system/edac/mc/mc*/{ce,ue}_count > edac_before.txt 2>/dev/null
# BMC SEL（有 ipmitool 则导出）
ipmitool sel list > bmc_sel.txt 2>/dev/null
# 特性快照（验证 V1-V5 的门控输入）
tr ' ' '\n' < /proc/cpuinfo | grep -xE 'sve2|sveaes|svepmull|svesha3|svesm4|svebf16|svei8mm|sm3|sm4|sha3|sha512|rng|ls64|ls64_v|lrcpc|dit' | sort -u > features.txt
cat features.txt   # 期望：全部 19 项都在（lscpu 基线已记录这些 flag）
```

**判据**：若 `isolated`/`offline` 非空 → 记录哪些核被隔离，后续 scan 模式天然跳过（sdc-scan.sh 已处理），但要在报告中注明"故障历史取证"。

### 0.2 构建两种 binary（同一源码 aac012d56）

```bash
git clone -b port/arm64-saturation-sdc git@github.com:wangxumarshall/sdc-stressng.git
cd sdc-stressng

# 构建 A：默认构建（NEON 强度；分发开关全部走"跳过/NEON"路径）
make clean && make -j$(nproc) 2>&1 | tee buildA.log | grep -iE 'sve2 march|neon crypto|rndr|dc (civac|cvac|zva)'
cp stress-ng stress-ng-default

# 构建 B：SVE2 march 构建（SVE2 全开；这是 950 上的主力 binary）
make clean && MARCH_AARCH64_SVE2=1 make -j$(nproc) 2>&1 | tee buildB.log | grep -iE 'sve2 march|error|warning'
cp stress-ng stress-ng-sve2
```

**判据**：
- buildA.log：`using aarch64 sve2 march ... yes`（950 有 SVE 硬件，自动探测应通过——**这是 Patch 1 探测链在真机的首次实战**；若 no，立即停：检查 `grep -c sve features.txt` 与 gcc 版本）
- buildB 强制路径同 yes
- 两构建 0 error 0 新 warning

### 0.3 锁频（复用既有结论：--ignite-cpu 的通用 cpufreq 路径在 ARM 生效，man 已更新）

```bash
# root；先试框架自带的：
./stress-ng-sve2 --ignite-cpu --cpu 1 -t 2   # 无报错即走通
# 或手动：
for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo performance > $g; done
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
```

## 3. 阶段 1：逐项功能验证（V1-V9，约 60 分钟）

> 全部用 `stress-ng-sve2`（构建 B），除非该行注明 default。每条命令后记录 passed/failed/skipped 行与关键 metric。
> **同步观察**：全程另一终端 `dmesg -w | grep -iE 'ras|edac|mce'`，任何命中即停并取证。

### V1 armcrypto（13 方法）

```bash
for m in all aes sha1 sha256 sha512 sha3 sm3 sm4 sm4key pmull sve2-aes sve2-pmull sve2-sha3 sve2-sm4; do
  echo "=== $m ==="
  ./stress-ng-sve2 --armcrypto 4 --armcrypto-method $m --verify -t 10 --metrics-brief 2>&1 \
    | grep -E 'passed:|failed:|skipped:|difference|rounds per sec' | head -4
done
```
- **预期**：13 条全部 `passed:`，`skipped: 0`（features.txt 已确认全特性在）；aes/pmull 是 KAT 双路径，任何 `data difference` = 直接 SDC 证据 → 立即进入 §6 取证
- **记录**：每方法 bogo-ops/s（后续做核间离群对比的基线）

### V2 sve2 5 方法 golden verify

```bash
for m in all fmla gather fcmla bitperm bfdot; do
  echo "=== $m ==="
  ./stress-ng-sve2 --sve2 4 --sve2-method $m --verify -t 10 2>&1 \
    | grep -E 'passed:|failed:|mismatch' | head -2
done
# 真机 VL 实测（后续 gather/寄存器验证的解释输入）：
./stress-ng-sve2 --sve2 1 -t 3 2>&1 | head -3   # 或用 pr_dbg
```
- **预期**：全 passed。任何 mismatch → 记录 element index/翻转位数（位级诊断已内建）→ §6

### V3 fma SVE2 分发命中证明（性能形态证据）

```bash
# 构建 B（SVE2 表被 HWCAP_SVE 选中）：
./stress-ng-sve2  --fma 190 --verify -t 20 --metrics-brief 2>&1 | grep -E 'passed:|failed:' 
./stress-ng-sve2  --fma 8 -t 20 --metrics-brief -Y /tmp/fmaB.yaml
# 构建 A（NEON 基线，同一 binary 对照不成立——须用 default 构建跑同参数）：
./stress-ng-default --fma 8 -t 20 --metrics-brief -Y /tmp/fmaA.yaml
# 对比 8 worker 的 bogo ops/s：B > A 显著（VL≥256-bit 时预期 ≥1.5x）
```
- **判据**：`fmaB ≥ 1.3 × fmaA`（保守阈值；若 ≈1，说明分发未命中或 VL=128——用 V2 的 VL 记录判读）

### V4 regs（v + z 寄存器堆）

```bash
./stress-ng-sve2 --regs 190 -t 30 2>&1 | grep -E 'passed:|failed:'
./stress-ng-sve2 --regs 4 --regs-bitflip --verify -t 10 2>&1 | grep -E 'passed:|failed:'
```

### V5 rdrand

```bash
./stress-ng-sve2 --rdrand 95 --verify -t 30 --metrics-brief 2>&1 | grep -E 'passed:|failed:|random bits'
./stress-ng-sve2 --rdrand 4 --rdrand-seed -t 10 2>&1 | grep -E 'passed:|failed:'
```
- **观察**：`--metrics-brief` 的 random bits/s；多 worker 高并发下若吞吐骤降+间歇，符合"RNDR 缓冲打空"真机形态（V5 记录项）

### V6 缓存维护通路

```bash
./stress-ng-sve2 --memrate 8 --memrate-method write64zva -t 15 --metrics-brief 2>&1 | grep -E 'passed:|write rate'
./stress-ng-sve2 --memrate 8 --memrate-method write64    -t 15 --metrics-brief 2>&1 | grep -E 'passed:|write rate'
./stress-ng-sve2 --cache 4 --cache-flush -t 10 2>&1 | grep -E 'passed:|failed:'
./stress-ng-sve2 --cache 4 --cache-clwb  -t 10 2>&1 | grep -E 'passed:|failed:'
```
- **判据**：`write64zva 吞吐 > write64 吞吐`（DC ZVA 免 RFO；真机判据，qemu 给不出）

### V7 pair 模式（本机无 SMT，首次真跑 SMT2）

```bash
# 先小样（2 核 × 4 组合 × 60s）确认脚本在 SMT 拓扑下工作：
NG=./stress-ng-sve2 ./scripts/sdc-run.sh pair -t 60 -c 0-3 -o /tmp/pair_smoke
cat /tmp/pair_smoke/pair-matrix.txt
# 全量 191 核（~4.5 小时 @60s/组合）：
NG=./stress-ng-sve2 ./scripts/sdc-run.sh pair -t 60 -o sdc_pair_full
```
- **判据**：smoke 的 pair-matrix 每行有 **2 个** worker 速率（本机验证时是同核退化）；`fma_x_fma` 两 worker 速率之和 < 2× 单线程 fma 基线（共享向量管的直接证据）
- **产物**：`sdc_pair_full/pair-matrix.txt` = 950 的 SMT2 共享资源拓扑图谱（公开资料为零，这是第一手数据）

### V8 功率遥测

```bash
./stress-ng-sve2 --cpu 95 --taskset physical --cpu-method matrixprod --raplstat 5 -t 60 2>&1 | grep -E 'raplstat|passed:'
ls /sys/class/hwmon/*/power*_input 2>/dev/null   # 传感器清单留档
./stress-ng-sve2 --cpu 95 -t 30 --rapl 2>&1 | grep -iE 'power|rapl'
```
- **判据**：raplstat 表每 5s 出真实瓦数（BMC/SoC/DDR 轨）；满载瓦数 vs 空闲瓦数差 = "打满"佐证

### V9 回归冒烟（两种构建各跑一遍）

```bash
for BIN in stress-ng-default stress-ng-sve2; do
  echo "=== $BIN ==="
  ./$BIN --zombie 4 -t 5 2>&1 | grep -E 'passed:|failed:'
  ./$BIN --cpu 8 --cpu-method all -t 30 2>&1 | grep -E 'passed:|failed:'   # 含 crc32 方法（此前已落地的）
  ./$BIN --armcrypto 4 --verify -t 5 2>&1 | grep -E 'passed:|failed:'
  ./$BIN --ls64 4 --verify -t 5 2>&1 | grep -E 'passed:|skipped:'          # 950 有 ls64 → 应 passed（此前真机未验）
  ./$BIN --taskset physical --cpu 190 -t 5 2>&1 | grep -E 'passed:'         # 191 核物理绑定（此前真机未验）
done
```

## 4. 阶段 2：SDC 协同压测（V10，真正的目的）

> 前提：阶段 1 全绿。与 SDCShield 配合（`--sdcshield` 参数接 runner 命令；SP 版本必须匹配目标机，见 findings §4 前置警告）。

### 4.1 全漏斗

```bash
# full 2h（含 10 分钟 preheat 热序）→ scan（每核 120s，背景压 keep-bg）→ pair 已在 V7 跑 → path 600s
NG=./stress-ng-sve2 ./scripts/sdc-run.sh all -t 7200 --preheat 10 -o sdc_funnel_$(date +%m%d)
```
（`all` 模式内部：full → scan → path；pair 单独跑过，避免漏斗总时长翻倍）

### 4.2 SDCShield 并行（full 窗口内）

```bash
# full 启动后另一终端（或用 --sdcshield 一体化）：
./run-sdcshield.sh -T forever -t 2h -Y -F \
  -e 'eigen_svd*' -e 'eigen_gemm*' -e 'fma*' -e 'zstd19' -e 'zlib*' -e 'crc32'
```

### 4.3 判读（多证据交叉，≥2 条命中才确认——沿用 findings §4 判读表）

| 证据 | 来源 |
|---|---|
| armcrypto aes/pmull `data difference`（KAT） | stress-ng（新能力：加密单元 SDC 直接检出） |
| sve2 5 方法 `mismatch`（位级诊断） | stress-ng（新能力：SVE2 通路 SDC 直接检出） |
| fma verify `data difference` | stress-ng（SVE2 分发路径下） |
| bogo-ops/s 离群核 | 各方法 metrics 基线（V1 记录的基线在此用） |
| SDCShield cpu-mask | golden 校验器 |
| EDAC CE/UE delta、BMC SEL 新增 | `grep -H . /sys/devices/system/edac/mc/mc*/{ce,ue}_count` 与阶段 0 对比 |

### 4.4 疑似核复现（发现嫌疑后）

```bash
# 假设 scan+shield 收敛到 CPU 137 所在核（sibling 对从 thread_siblings_list 取）：
NG=./stress-ng-sve2 ./scripts/sdc-run.sh path -t 3600 -c 136,137
# 对照健康核：
NG=./stress-ng-sve2 ./scripts/sdc-run.sh path -t 3600 -c 0,1
```

## 5. 结果记录模板（每条命令一行，附原始输出）

```
[ ] V1 aes       passed:4  ops/s=________   dmesg: clean
[ ] V1 sve2-aes  passed:4  ops/s=________   dmesg: clean
[ ] V2 bfdot     passed:4                   dmesg: clean
[ ] V3 fmaB/fmaA = ____/____ (ratio ____)   分发: 命中/未命中
[ ] V4 regs 190  passed:190
[ ] V5 rdrand    passed:95  bits/s=________ 形态: ________
[ ] V6 zva/write64 = ____/____ MB/s
[ ] V7 pair smoke: 2-worker 行 ✓   fma_x_fma 和 = ____ vs 2×基线 = ____
[ ] V8 raplstat: idle=____W load=____W
[ ] V9 两构建冒烟全绿
[ ] V10 漏斗 rc=____ suspects: ____ 核
```

## 6. 停机条件与取证（任一命中即停）

1. **任何 verify fail（armcrypto/sve2/fma/ls64）**：记录 stress-ng 完整输出（位级诊断行）、`cp /proc/interrupts`、EDAC 前后、BMC SEL、当时温度（`--thermalstat 30` 建议全程开着）；不要重启
2. **dmesg 新增 RAS/EDAC/MCE 行**：同上取证
3. **热失控迹象**（thermal throttle、宕机前兆）：降压负载（减 worker 或加 `--thermalstat` 监控），先取证再继续
4. **构建 A 探测失败**（`sve2 march ... no`）：停，留 config.log，反馈开发机分析（可能是 gcc 版本差异——真机 gcc 版本先 `gcc --version` 留档）

## 7. 风险与回退

| 风险 | 缓解 |
|---|---|
| SVE2 march 构建的 auto-vectorized 代码落在未门控路径 → 950 本身有 SVE，无 SIGILL 风险；但若误拷该 binary 到 920 会 SIGILL | 二进制命名隔离（-default/-sve2），跨机拷贝纪律 |
| root 下 stressor 不可杀（OOM 配置） | 已知既有风险；首跑先用短时（-t ≤ 60）小规模 |
| pair 全量 4.5h + full 2h + scan 191×120s≈6.4h | 总时长 ~14h，可拆多窗口；`all` 模式按需分步 |
| SDCShield SP 不匹配 | 源码构建（findings §4 前置） |

## 8. 完成定义

- V1-V10 全部有原始输出记录（模板 §5 全勾）
- pair-matrix.txt、scan suspects、funnel yaml 归档到 `~/sdc950_*/`
- 若检出 SDC：嫌疑核清单 + ≥2 条交叉证据 + 复现记录
- 回填本文件各节结果；开发机侧复盘是否有需要修的 patch
