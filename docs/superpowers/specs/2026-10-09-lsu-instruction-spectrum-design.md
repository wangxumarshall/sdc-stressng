# LSU 全指令谱 SDC 激发引擎 — 设计规格（spec）

日期：2026-10-09 ｜ 状态：已获用户批准的设计，待 spec 审阅
来源：第十五轮 brainstorming（用户需求：在现有stress-ng基础上，不同指令实现的 load/store 压测 + memcpy 变体；
强化需求：访存要"疯狂大、高频"，地址在**最大虚拟地址空间全范围随机游走**）

## 1. 背景与动机

此前对 `excite` 模式的压力评估结论是"广度够、深度不够"：8 进程叠加稀释单通路深度、
内存压力 worker 占比 <1%、互联/L3 跨实例空白、ls64/cache 系列缺席。本设计新建
**arm64 LSU 全指令谱激发面**，直接补强三个维度：

1. **指令谱**：整数/浮点/NEON/SVE/SVE2/原子（独占对、LSE RMW）/ls64 的 load/store
   全覆盖——此前 load/store 只有编译器生成的通用 ldr/str。
2. **数据流模板**：纯 load、纯 store、copy 之外，重点做**混合形态**
   （`2×load + ALU 计算 + load + store` 类）——打 LSU 与执行单元交互面：
   多 load 并发、load-use 转发、store buffer 合并（SDC 高发区）。
3. **VA 空间引擎**：在最大虚拟地址空间（运行时探测 48/52-bit）做全范围随机游走，
   窗口迁移制造持续页表生长/回收（MMU 压力）与 TLB 位段扫掠。

## 2. 目标与非目标

**目标**
- `--memcpy-method` 扩展 6 个 arm64 指令级变体（上游方法表框架内）
- 新 stressor `--lsupress`：地址引擎 × 访存内核双组件，~20 个精选方法
- excite 2.0：时间片轮换深压（解决叠加稀释），lsupress/memcpy 变体按特性门控入编
- 全部走仓库 6 步验证纪律（含 objdump 指令验收门、故障注入）

**非目标**
- 不做 x86 等其他架构的指令谱（arm64 专属，其他架构诚实跳过）
- 不重复 addrspace 的"映射+touch 校验"语义（它是形状校验，lsupress 是持续高频访存）
- 不做激发效率自动优化闭环（记录为 backlog，依赖真机失配数据）

## 3. 架构总览（三层）

```
层 1  --memcpy-method 扩展          层 2  --lsupress 新 stressor（核心）
  上游方法表框架直接挂                地址引擎 × 访存内核双组件
  6 个指令级 memcpy 变体               ~20 方法 × 5 选项
            │                                │
            └──────────┬─────────────────────┘
                  层 3  excite 2.0 编排
            时间片轮换深压 + 特性门控加载
```

## 4. 层 1：`--memcpy-method` 指令变体（P1-P2）

arm64-gated（`STRESS_ARCH_ARM` 门控；非 arm64 下方法不出现在表中，诚实跳过）：

| 方法名 | 内核 | 压力目标 | 门控 |
|---|---|---|---|
| `ldp-stp` | ldp/stp x 对（128b/迭代） | LSU pair 通路 | 无 |
| `neon` | ld1/st1 q 寄存器 | NEON LSU 端口 | 无 |
| `neon-ld2` | ld2/st2 交错双 q | 多发射访存 | 无 |
| `sve` | ld1d/st1d z（svlen 自适应） | SVE 全 VL 访存 | HWCAP_SVE |
| `sve-gather` | ld1d (z, index) gather + scatter | 非连续地址向量访存（SVE 独有） | HWCAP_SVE |
| `ls64` | ld64b/st64b | 64B 原子块 | HWCAP2/HWCAP3 ls64 |

- 实现：寄存器变量 + target 属性为主（fork 惯例），ls64 用已有 `.inst`/asm 先例；
  **objdump 验收门**：确认目标指令计数 > 0（纪律 #8）。
- verify：复用上游 `STRESS_MEMCPY_NAIVE` 宏的 test 对拍机制。
- bogo-ops 保持字节语义 → 各指令实现的拷贝带宽可直接横比。

## 5. 层 2：`--lsupress`（P3-P6，核心）

### 5.1 双组件架构

```
地址引擎（address engine）                访存内核（instruction kernel）
  最大 VA 映射（NORESERVE，运行时          背靠背最大并行 load（10+ in flight）
  探测 48/52-bit 上限，--va-size 可调）     × 全 VL SVE store（svlen 自适应）
  工作集窗口（--window，默认数 GB）          × 数据流模板（load/store/copy/mix/原子）
  窗口随机迁移 + 旧窗口 MADV_DONTNEED
  （物理足迹恒定；页表持续生长=MMU 压力）
  游走模式：uniform / bitgen / va-bit / near-far
```

### 5.2 方法表（methods[0]="all"，0 基分派——纪律 #2）

| # | 方法 | 模板 | 指令谱 |
|---|---|---|---|
| 0 | all | 轮换全部 | — |
| 1 | load-int64 | 纯 load | ldr x（消费校验和防 DCE） |
| 2 | load-int128 | 纯 load | ldp x 对 |
| 3 | load-fp64 | 纯 load | ldr d |
| 4 | load-neon | 纯 load | ld1 q |
| 5 | load-sve | 纯 load | ld1d z 全 VL（HWCAP_SVE） |
| 6 | load-sve-gather | 纯 load | ld1d gather（HWCAP_SVE） |
| 7 | store-int64 | 纯 store | str x（bitgen 流） |
| 8 | store-neon | 纯 store | st1 q |
| 9 | store-sve | 纯 store | st1d z 全 VL（HWCAP_SVE） |
| 10 | store-zva | 纯 store | DC ZVA 行清零 |
| 11 | copy-int64 | copy | ldr+str |
| 12 | copy-sve | copy | ld1d+st1d（HWCAP_SVE） |
| 13 | **mix-2l-alu-1s** | **2load+ALU+load+store** | ldr×3/add×2/str（用户点名） |
| 14 | mix-1l-fpu-1s | load+FPU+store | ldr d/fmul/fadd/str |
| 15 | mix-3l-2alu-1s | 3load+2ALU+store | 深并发 load |
| 16 | mix-neon-fma | batch load q→fma→batch store | ld1×N/fmla/st1×N |
| 17 | mix-sve-fma | batch ld1d→fmla→batch st1d | 全 VL（HWCAP_SVE） |
| 18 | excl-pair | ldxr/stxr 独占循环 | 独占监视器 |
| 19 | lse-rmw | LSE ldadd/stadd | 原子 RMW（HWCAP_ATOMICS） |
| 20 | ls64-copy | ld64b/st64b | 64B 原子块（ls64 门控） |

**内核频率要求（"疯狂高频"）**：内核循环 asm 化或 NOINLINE + 寄存器变量约束；
每迭代访存条目数最大化（10+ 条独立 load in flight）；循环本体仅访存+寻址折叠
（`[x0, x1, lsl #3]` 形式），零多余指令。验收线：每核每周期 ≥1 load+1 store
（带宽 + objdump 双重计量）。

### 5.3 选项

| 选项 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `--lsupress-va-size` | 尺寸后缀 | 1T | NORESERVE 映射大小（运行时按 VA 位宽收缩） |
| `--lsupress-walk` | uniform/bitgen/va-bit/near-far | uniform | 游走模式（bitgen=VA 位段定向，打 TLB tag 位段） |
| `--lsupress-window` | 尺寸后缀 | 4G | 工作集窗口（多 worker 分区） |
| `--lsupress-huge` | 4k/2m/1g | 4k | 页粒度（4k=TLB 压力最大） |
| `--lsupress-align` | aligned/misalign/cross-line | misalign | 窗口内偏移形状（misalign=1..7 轮换） |

### 5.4 verify（确定性 oracle，纪律 #3）

- 地址→值确定函数：`value = splitmix64(seed ^ addr)` 类轻量哈希——**任何地址任何时刻
  可独立校验**，天然适配随机游走（无需 buffer 级同序重放）。
- 写入侧（store/copy/mix 模板）按 f() 生成值；verify 侧重算比对。
- FP 模板刻意避免 FMA 歧义：内核与软件参考同用显式 mul+add（或同用 fma()），
  保证位精确。
- 失配输出：地址 + expected/actual + 翻转位数 + xor 掩码（fork 位级诊断惯例）。
- excite 模式无 verify 零开销。

### 5.5 风险与控制

| 风险 | 控制 |
|---|---|
| OOM（NORESERVE 随机 touch） | 窗口硬顶物理足迹（窗口×worker 数）；worker 分区；尺寸可调 |
| 52-bit 探测失败 | mmap 试探收缩 fallback 48-bit |
| 游走+verify 开销 | f() 轻量哈希；excite 无 verify |
| gcc 7.3 兼容 | SVE/SVE2 内核 `__GNUC__>=10` 门控（既有惯例）；ls64 双查 HWCAP2/3 |

## 6. 层 3：excite 2.0 时间片轮换（P7）

`run_excite()` 重构为轮岗循环（纯 bash）：

```
每 T=10min 一岗：主力 stressor 独占全核（每核 1 worker 深压），
其余通路退背景（每核 1/8 worker 保持机器级并发触发条件）+ varyload 常驻。
岗位序列（按特性门控）：cpu → fma → armcrypto → lsupress → memcpy-<变体>
→ operand-var → 循环
```

解决评估缺陷 A1（8 进程叠加稀释 → 单通路满深）。`--preheat` 语义不变。

## 7. patch 分解（one-patch-per-unit，每 patch 6 步验证）

| # | 内容 | 主要验证 |
|---|---|---|
| P1 | memcpy 变体 NEON 族（ldp-stp/neon/neon-ld2） | 本机全功能 + objdump 门 + 上游 test 对拍 |
| P2 | memcpy 变体 sve/sve-gather/ls64 | QEMU + HWCAP 门控 + gcc7.3 守卫 |
| P3 | lsupress 骨架：方法表/选项框架 + 地址引擎 MVP（NORESERVE 映射 + uniform 游走 + 窗口管理）+ load/store/copy 基础方法 | OOM 边界实测 + 本机跑通 |
| P4 | lsupress mix 模板族（mix-2l-alu-1s 等，asm 精确内核） | objdump 指令序列验收 + 带宽验收线 |
| P5 | 向量 mix（neon-fma/sve-fma）+ 原子（excl/lse）+ ls64-copy | QEMU SVE + 本机 NEON |
| P6 | 游走扩展（bitgen/va-bit/near-far）+ hugepage + verify f() 集成 + **故障注入**（翻 1 位必被抓） | 注入实验 + 位级诊断 |
| P7 | excite 2.0 时间片轮换（sdc-run.sh 重构） | 本机短跑 E2E + 轮岗日志 |
| P8 | 文档：excitation-guide 覆盖矩阵大更新（LSU/TLB 列从 ◐→●）+ man + CHANGELOG + CI 确认 | 15 镜像 method sweep 自动覆盖 |

## 8. 与既有能力的边界

- `addrspace`：形状校验（映射+touch+比对）；lsupress 吸收其 va-bit/huge-random 思想
  进地址引擎，但定位是持续高频访存——互补不重叠。
- `memrate`：带宽模式定位保留；lsupress 是指令谱+VA 空间定位。
- `--vm rand-offset`：进程内偏移形状；lsupress 是跨 VA 全空间游走。
