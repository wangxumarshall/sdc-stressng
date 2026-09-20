# 第十三轮改进方案：SDC 实战检验闭环 + 变异质量校准（2026-09-20）

> 驱动：第十二轮复盘（9 patch 变异强化已全部落地，CI 15 镜像全绿）。复盘的核心结论：
> 所有组件在**正确性维度**全绿，但"变异质量提升 SDC 检出率"的**最终证据**要等真机。
> 本轮把闭环补上：从"代码正确"走向"检出有效"。
>
> 研究事实基础见 findings.md §11（全部带 file:line 证据）。

## 一、复盘遗留项定位与优先级

| # | 遗留 | 现状核查结论 | 本轮处置 |
|---|---|---|---|
| 4 | **SDC 实战检验**：sdc-run.sh 真机 A/B 回归，对比新旧版本 golden 失配率 | 复盘点名的下一轮主线。基建缺口：sdc-run.sh full 只 `grep ... \| head -5` 摘录，无失配统计；stress-ng -Y 无失配结构化字段；A/B 分界 commit 明确（fc243c784 / c7e7ebf55 均可构建） | **P1-P3（本轮主体）** |
| 1 | bandwalk 窗口参数校准（6-20 位、5 档密度是文献推断值） | 全部硬编码在 core-bitgen.c band_fill()，无运行时接口；bitgen-distribution.sh 是 build-time 统计验证非采集器 | **P4**（采集工具+调参接口，真机出分布后校准） |
| 5 | CI 覆盖数据累积对比（变异前后 bogo-ops 稳定性） | CI-MATRIX 行格式齐全，gh api 可拉历史 run，但无跨 run 时序聚合工具 | **P5**（小工具，纯增量） |
| 2 | cache/l1cache/cacheline 数据值随机化 | 复盘判断成立且更具体：三者均字节粒度状态机（set 号/递增/rol-ror），verify 模式 0 自由位，无 64 位 tag 设计；要复用 vm-rand-offset 的双 bitgen 模式必须升字粒度 | **P6**（工作量最大，单列） |
| 3 | pagemap PFN 导向分配 | 无任何代码；需 root + /proc/pagemap | **P7**（目标机脚本先行，代码后置） |

排序理由：P1-P3 是"证明前 12 轮工作有效"的唯一路径，且复盘明示"建议下一轮做"；
P4/P5 是它的配套度量；P6 是独立的新覆盖面，不应阻塞实战检验；P7 依赖目标机权限。

## 二、Patch 清单

### P1: sdc-run.sh 失配统计报告（纯脚本）
**问题**：full 模式结束后只 `grep -E "failed: [1-9]\|data difference\|mismatch" A_full.log | head -5`（sdc-run.sh:304）——A/B 对比需要数字，不是 5 行样例。
**方案**：新增 `scripts/sdc-report.sh`（或并入 sdc-run.sh 收尾函数）：
- 从 A_full.log 统计：verify 失配总数（pr_fail 行计数）、失配 stressor 分布、首失配时间戳（对照 preheat 结束时间 → 热浸润效应）
- 从 sdcshield.log 的 YAML 统计：`result: fail` 的 test 列表、"Test failed M out of N times (X%)" 汇总行、fail 条目的 cpu-mask 聚合
- 输出 `report.txt`：一页纸摘要（运行时长/preheat/失配计数/失配率/嫌疑核），可直接 diff 两个 run 目录
**验证**：本机跑 `sdc-run.sh full -t 60`（无失配基线）+ 人造失配日志喂给报告函数。

### P2: A/B 回归模式（sdc-run.sh abtest）
**问题**：对比新旧版本需要手动构建两个二进制、跑两轮、手工对齐结果。
**方案**：sdc-run.sh 新增 `abtest` 模式：
```
NG_A=./stress-ng-old NG_B=./stress-ng-new ./scripts/sdc-run.sh abtest -t 7200
```
- 同机同参数顺序跑 A（fc243c784 构建）/ B（当前构建），中间冷却间隔（消除热历史交叉污染）
- 参数集 = full 模式现配方（cpu+fma+operand-var+addrspace+varyload）——注意 A 版没有 operand-var/addrspace，A 侧退化为 cpu+fma+varyload，**这本身就是对比点**（变异增量带来的失配差）
- 输出 `ab_summary.txt`：A/B 失配率并排 + 每指标方向性判读
**判读规则**（写进脚本注释）：B 失配率 > A 且 SDCShield 侧同步升高 → 变异有效；两者持平 → 变异未改变检出（需要更长 soak 或负载形态调整）；B < A → 检查是否 B 的负载强度反而下降（bogo-ops 对照）。
**验证**：本机 60s 短跑跑通流程（rc=0、ab_summary.txt 生成）。

### P3: verify 失配结构化输出（-Y yaml 增量字段）
**问题**：失配只在文本流，机器可读性差（findings §11.2：-Y metrics 段无失配字段，stress-ng.c:2537-2549）。
**方案**：yaml 输出每 stressor 增加可选字段（仅 --verify 失败时出现）：`verify-failures: N`（pr_fail 计数）。位级诊断细节仍在文本（避免 yaml 爆炸）。改动点在 stress-ng.c 的 yaml 渲染 + stressor 共享的 verify 计数钩子（VERIFY_FAIL 宏处递增）。
**验证**：故障注入（临时翻 1 位）→ yaml 出现字段 → 正常路径 yaml 不变（向后兼容）。
**风险**：动 stress-ng.c 主文件，最小化改动面——只加计数与渲染，不动调度。

### P4: bandwalk 校准链（采集工具 + 调参接口）
**问题**：窗口参数（6-20 位、5 档密度）是文献推断；真机位翻位段分布拿到后无接口校准。
**方案**（两半）：
1. **调参接口**：`--bitgen-band-width min:max` / `--bitgen-band-density list`（core-bitgen.c band_fill() 的重摇逻辑读全局配置，默认值=现值，行为完全向后兼容）+ man 条目
2. **采集工具**：`scripts/sdc-flip-collect.sh` —— 真机上跑 operand-var --verify 长跑，从失配的 xor 掩码（P2 已有的位级诊断）聚合 64 位直方图（哪一位段翻得多），输出校准建议（窗口参数建议值）。参考 bitgen-distribution.sh 的 ones[] 计数模板。
**真机前提**：Kunpeng 920/950 有实际失配样本才可校准；无失配 → 工具输出"样本不足"，参数维持文献值。
**验证**：接口本机统计验证（band-width 8:12 时窗口分布应只在 8-12）；采集工具用人造失配日志测试聚合逻辑。

### P5: CI 时序趋势工具
**问题**：CI-MATRIX 每 run 独立，无跨 run 对比（findings §11.2 遗留5 缺口）。
**方案**：`scripts/ci-trend.sh [--days N] [--stressor fma,memrate,...]`：
- `gh api /repos/.../actions/runs?workflow_id=multi-os-verify.yml&created=>=<date>` 拉历史 run → 每 run 的 build-test job logs → 提取 CI-MATRIX 行
- 输出：每 stressor × 每 tag 的 bogo-ops/s 时间序列（ASCII 表 + 变异系数），标出 fc243c784（9/19 变异合入）前后分段
- 用途：复盤断言"一周后可对比变异前后 stressor 的 bogo-ops 稳定性变化"——本工具就是那个对比
**验证**：本机对已有 run（22 起）跑一次，确认能拉到数据并渲染。

### P6: cache 系列字粒度 tag 重设计（独立立项，本轮不实施）
**研究结论**（findings §11.2）：三文件均字节粒度；l1cache verify 校验 `(uint8_t)set`；cacheline 全方法 VERIFY_ALWAYS 状态机字节；cache 无 verify（读值只累加）。
**方案草图**（供下轮评审）：新增 `--cacheline-method rand-payload`（不动现有 11 方法）：64 位字 = 低 8 位保留 cacheline 现状态机语义（向后兼容）+ 高 56 位 bitgen 流填充，fill/verify 双份同种子 bitgen（vm-rand-offset 先例 stress-vm.c:1101-1193）。cache/l1cache 同理加 opt-in 方法。**不改默认行为**。
**本轮动作**：仅此立项记录，不写代码（复盘教训3：复杂 recipe 先纸上定骨架；且不应阻塞 P1-P3）。

### P7: pagemap PFN 导向（目标机脚本先行）
**方案**：`scripts/sdc-pfn-map.sh`（目标机 root）：读 /proc/pagemap 把 addrspace 的 huge-random-fixed 缓冲 PFN 分布导出（物理地址连续性/bank 交错可见性），为"按 PFN 模式导向分配"提供数据。代码级导向（mbind+hugepage 策略）等数据回来再定。

## 三、验证矩阵

| Patch | 本机验证 | 真机（CP1）验证 |
|---|---|---|
| P1 | 60s full 无失配基线 + 人造失配日志 | 长跑后 report.txt 出数字 |
| P2 | 60s abtest 流程跑通 | **主战场**：A/B 失配率对比，复盘遗留4 闭环 |
| P3 | 故障注入 yaml 字段 + 正常路径回归 | CI 15 镜像自动覆盖 |
| P4 | 接口统计验证 + 聚合逻辑单测 | 有失配样本后校准参数 |
| P5 | 对历史 run 出趋势表 | 每日 cron 持续累积 |
| P6/P7 | — | 数据先行 |

## 四、实施顺序与纪律

P3 → P1 → P2 → P5 → P4（P3 的 yaml 字段是 P1/P2 报告的数据源之一；P5 独立可并行）。
每 patch 走 plan → code → verify → commit → push；P3 触碰 stress-ng.c 主文件需 CI 全绿才算过。

## 五、决策点（呈报用户）

1. **P2 的 A 版基线**：用 fc243c784（第十二轮前）还是更早（第七轮饱和压测后）？建议前者——隔离"变异增量"的单一变量。
2. **P6 是否本轮实施**：建议不实施（不阻塞实战检验主线），下轮独立立项。
3. **真机窗口**：P1-P3 落地后是否立即安排 CP1 真机 A/B 长跑（建议 2h × 2 版本 × 3 轮取稳定）。
