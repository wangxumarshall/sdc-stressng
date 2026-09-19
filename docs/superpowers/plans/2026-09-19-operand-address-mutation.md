# 数值/地址空间变异强化方案（SDC 检出率提升）

日期：2026-09-19（第十二轮）
输入：operand-randomness-survey.md + address-space-survey.md（两份全量盘点）+ R2 前沿研究 D 族杠杆
目标：把"随机撒点"升级为"SDC 定向变异"——操作数位段扫掠 + 地址空间形状覆盖，全部带数据校验

---

## 0. 核心洞察（先说结论）

现状不是"没有随机"（252 个 stressor 已用 stress_mwc_*），而是**随机的形状与 SDC 物理机理错配**：

| SDC 机理 | 需要的输入形状 | 现状 | 差距 |
|---|---|---|---|
| 数据依赖缺陷（Meta 核 59：`Int(1.1^53)=0`） | 指数/尾数**边界邻域**密集采样 | 均匀随机 32 位整数 | 均匀撒点命中边界概率 ~2^-40，实际永远采不到 |
| 翻转功率敏感（P=αCV²f，定向老化>7x） | 0x55/0xAA 互补对连续拍 | memrate 全路径硬编码 0xaa（有翻转无随机） | 有一个极端，缺另一切换 |
| 位段敏感（位翻集中在中段/尾数） | 每设定固定掩码的**位段扫掠** | 无任何位段生成器 | 完全缺失 |
| DRAM row/bank 地址敏感 | 随机**大**映射内的密集随机偏移 | vm galpat 256MB 稀疏散点；随机地址引擎只映射 1-17 页 | 规模与密度都缺 |
| TLB/MMU 上位 VA 位缺陷 | VA bit 48..51 持续流量 | mmapaddr 单页单字节；mmap 顶提示不校验 | 无持续数据流量 |

设计原则：**模式字典 × 均匀随机的混合生成**（既不是纯固定模式——那走向 memrate 现状的另一极端；也不是纯均匀——对低概率位段覆盖差）。

---

## 1. 方案总览（3 个新组件 + 5 处现有加固，按收益排序）

### Patch P1（新组件）：`core-mwc-bitgen` 位段定向生成器
**收益：全库操作数质量的地基。最高优先。**

新增 `core-mwc-bitgen.h/.c`，提供 SDC 定向的位模式生成 API：

```c
/* 位段扫掠生成器：每 N 次调用前进一个位段窗口 */
uint64_t stress_bitgen_bandwalk64(void);   /* 6..20 位宽窗口，窗口内 1 密度 0/25/50/75/100% */
uint32_t stress_bitgen_bandwalk32(void);

/* 边界值字典 + 邻域扰动 */
uint64_t stress_bitgen_edge64(void);       /* 从边界字典随机取 + ±1..4 LSB 扰动
                                             字典：0, 1, 2^k-1, 2^k, 2^k+1 (k=1..63),
                                             DBL_MAX/DBL_MIN 位型, 0x7ff.., 0x800..,
                                             1.1^52/1.1^53 位型(Meta核59), all-ones,
                                             单bit游走(hamming-1 of edge values) */

/* FP 位型直接合成（不经算术缩放——现有 FP stressor 的操作数指数/尾数强相关） */
uint64_t stress_bitgen_fp64_bits(void);    /* 指数段扫掠 × 尾数段 bandwalk 独立合成 */
float    stress_bitgen_fp32(void);

/* 互补翻转对（D2 杠杆：P=αCV²f 最大化） */
void     stress_bitgen_complement_pair64(uint64_t *a, uint64_t *b); /* 0x55/0xAA 类 + 随机化的变体 */

/* Hamming 定向 */
uint64_t stress_bitgen_hamming64(const unsigned target_weight); /* 指定翻转密度 */
```

实现底座：独立于全局 mwc 状态的**每 stressor 生成器句柄**（封装 w/z 状态 + 模式游标），避免污染现有 mwc 流。周期保证：bandwalk 全空间周期 ≥2^32 次调用（E1 杠杆：数千次迭代不重复）。

**验证**：单测式 stressor 或 scripts/bitgen-distribution.sh——统计 100 万次采样的位段直方图/边界命中数，与均匀基线对比（边界命中应 >10^6 倍提升）。

### Patch P2（新组件）：`stress-operand-var` 操作数变异 stressor（VERIFY_ALWAYS）
**收益：直接可跑的 SDC 定向用例，融合 D2+D3+D4+D5 全部数据杠杆。**

新 stressor，模式字典驱动的操作数变异引擎，跑在**真实计算路径**上（不是生成后丢弃）：

- 方法集（`--operand-var-method`）：
  - `bandwalk-int`：位段扫掠整数 → ALU/乘法器路径（add/mul/shift 混合）
  - `bandwalk-fp`：位段扫掠浮点 → FPU/add-mul 链（指数×尾数独立扫）
  - `edge-dict`：边界字典+邻域 → 除法/比较路径
  - `complement-fma`：互补翻转对 → FMA 单元（D2：每拍翻转最大化）
  - `type-matrix`：i8/i16/i32/i64/ui*/f16/f32/f64/f128 × 上述模式轮换（D5 全类型）
  - `all`
- 每个方法 verify = 同输入双路径计算（软件 golden vs 硬件路径）比对，位级诊断输出（复用第七轮 fma 位级诊断代码模式）
- `--operand-var-ops N` 控制

### Patch P3（新组件）：`stress-addrspace` 地址空间形状 stressor（VERIFY_ALWAYS）
**收益：补齐"多 GB 随机地址 + 密集随机偏移 + 数据校验"这个完全缺失的形状。**

新 stressor，按"形状配方"循环：

```
配方轮换（每配方一轮 map → pattern → verify → unmap）：
1. huge-random-fixed:  mmap MAP_FIXED_NOREPLACE @ 随机对齐地址（masks_64bit 扩展
   到 4TB span），尺寸 1GB..min(可用内存/4, 64GB) 对数随机 —— 补形状缺口 1/3
2. va-bit-walk:        逐 VA 位 (含 48..51) 各映射 256MB，bandwalk 数据流过
   （上位 VA 位持续流量）—— 补缺口 3
3. guarded-holes:      多 GB 映射 + 随机 guard 洞（mprotect PROT_NONE 随机页段），
   跨洞邻域数据校验 —— 补缺口 6
4. misalign-huge:      大映射内随机子页偏移 + 枚举错位访问（扩 misaligned 到
   大规模）—— 补缺口 5
5. dense-random-offset: 大映射内密集随机偏移数据模式 + 校验（vs vm galpat 的
   稀疏）—— 补缺口 2
6. malloc-giant:       随机 1..8GB malloc + 随机对齐 posix_memalign + 随机偏移
   数据校验 —— 补缺口 8（用户点名的 malloc 大内存）
7. mixed-orders:       4KB/2MB/16KB(arm64 base)/1GB 混合页尺寸随机地址交错 ——
   补缺口 4
```

- 地址合法性探测复用 pagescatter/mmapaddr 的三级探测（mincore/madvise/pipe）
- 数据模式用 P1 bitgen（bandwalk/edge/complement）
- pagemap 反馈：报告每配方的物理连续性%（复用 stress_mmap_stats）

### Patch P4（加固）：memrate 数据模式随机化
memrate 是"每秒百万次存储只有一种位模式"的最重灾区。在所有写路径（C/asm stos/memset 11 处）引入 `--memrate-write-pattern`：
- `0xaa`（默认，向后兼容）
- `random`（每 chunk stress_rndbuf）
- `bandwalk`（P1 生成器）
- `complement`（0x55/0xaa 交替，D2）
读路径不变（读校验写在读时对比——现有 memrate 不校验，本 patch 只改写模式；校验归 P3/P2 的职责，避免 memrate 带宽语义被破坏）

### Patch P5（加固）：armcrypto / cpu-int 锁死种子解锁
- armcrypto `crypto_in[]`：verify 的 golden 参考已按软件重算（第七轮改造后按方法名分派），种子锁死不再是必需——改为 per-worker reseed + verify 期软件重算同种子重放（保存 seed，verify 前重置，跑完恢复）。**随机后 golden 逻辑不变**。
- cpu STRESS_CPU_INT 的 `stress_mwc_seed_default()`：同法——把 a/b 初值改为 per-call reseed，a_final/b_final 的 verify 在重算路径里同步（保存/恢复种子）。
- 效果：int8..int128/rand/int-fp 全家 + NEON/SVE2 加密数据通路首次见到不同操作数。

### Patch P6（加固）：FP 操作数位型直合成
vecfp/fp/fma 的操作数初始化从"算术缩放"（`(double)i + r/2^38`——指数/尾数相关）改为混合：50% 算术缩放（保留）+ 50% `stress_bitgen_fp64_bits()`（位型直合成，指数/尾数独立）。fma/vecfp/matrix 的 verify 不变（双路径同算比对）。

### Patch P7（加固）：vm 大缓冲随机偏移方法
新增 `--vm-method rand-offset`：256MB+ 缓冲内**密集**随机偏移写入 bandwalk 模式 + 第二遍随机序读校验（区别于 galpat 稀疏 1bit/4KB）。复用 vm 的 bit_error 记账与 seed 保存/重放框架。

### Patch P8（加固）：atomic/cache 家族操作数随机化
- atomic：字面操作数集 {1,2,3,~1,...128} 扩为 `literal | stress_mwc32modn(8)` 随机抖动（保持 RMW 语义类型不变）
- cache/l1cache/cacheline 的数据值（j&0xff/set 计数）→ bandwalk 低 8 位 + 计数器混合

### Patch P9（文档+集成）：man 页 + sdc-run.sh 编排
- 全新选项 man 文档
- sdc-run.sh 的 full 模式加 `--operand-var --addrspace` 进默认配方
- CI multi-os-verify：新 stressor 自然进入 sequential/sweep（自动覆盖，无需改 workflow）

---

## 2. 优先级与依赖

```
P1 bitgen ──► P2 operand-var ──┐
    │                          ├──► P9 docs/编排
    ├──► P3 addrspace ─────────┤
    ├──► P4 memrate            │
    ├──► P6 fp-直合成           │
    └──► P7 vm rand-offset     │
P5 种子解锁（独立）──────────────┘
P8 atomic/cache（独立，低优先）
```

实施顺序：P1 → P5（独立快赢）→ P2 → P3 → P4/P6/P7 → P8 → P9

## 3. 验证策略（每 patch）

| Patch | 验证 |
|---|---|
| P1 | 位段直方图统计脚本（100 万采样 vs 均匀基线，边界命中提升 >10^6 倍）|
| P2/P3 | `--verify` 全方法本机通过；QEMU 交叉验证不需要（无新指令）；故障注入（翻 1 位）被抓 |
| P4 | memrate 各 write-pattern 20s 跑通 + 带宽不显著退化（<5%）|
| P5 | armcrypto/cpu-int verify 通过 + 两次运行 crypto_in 内容不同（sha256 对比）|
| P6 | fma/vecfp/fp verify 通过 |
| P7 | vm rand-offset verify 通过，bit_error=0 |
| P8 | atomic/cache verify 通过 |
| P9 | CI 15 镜像 sequential/sweep 全绿（新 stressor 自动进套件）|

## 4. 与既有工作的边界

- **不重复第七轮 12 patch**：那些是"单元饱和"（SVE2/ls64/DC ZVA/寄存器堆）；本轮是"数据变异"，正交
- **不动 SDCShield 分工**：stress-ng 仍是扰动器；但 P2/P3 的 VERIFY_ALWAYS 让扰动器自身带粗校验（第七轮已有的模式）
- **向后兼容**：所有新行为默认关闭或 `--xxx-pattern` 默认旧值；CI 套件行为不变差
- **mwc 全局状态不污染**：bitgen 独立句柄；种子保存/恢复模式照搬 vm moving-inversion 惯例

## 5. 风险

| 风险 | 缓解 |
|---|---|
| P3 大映射在 16GB runner 上 OOM | 配方尺寸上限 = min(可用/4, 64GB)；CI 上 addrspace 自动按 --pageable 收缩（stress-ng 框架 OOM 处理已有）|
| P5 种子解锁破坏 verify 可复现性 | 保存/恢复种子模式保证 verify 期重放一致；测试两轮 verify 结果稳定 |
| P4 memrate asm 路径改动引入 bug | asm 路径只加常数来源替换（寄存器装入值改为内存读取），语义零变化；带宽回归测试把关 |
| bandwalk 周期不足导致重复 | 设计保证 ≥2^32 调用周期；直方图脚本验证 |
