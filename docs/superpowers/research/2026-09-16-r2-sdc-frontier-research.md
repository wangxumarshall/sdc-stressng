# R2: 静默数据损坏（SDC）前沿研究与工业实践调研

> 调研日期 2026-09-16。【实证-原文】= subagent 实际下载逐字阅读；【实证-摘要】= 经 API 核实摘要；【二手】= 搜索摘要未取原文；【推断】= 工程推理。

## 0. 任务书三处事实修正

1. **"Silent Data Corruptions at Scale"（arXiv:2102.11245）是 Meta 论文（Dixit 等 2021），不是 Google/Hochschild**。Google 对应论文是 "Cores that don't count"（HotOS 2021，提出 mercurial cores 术语）。
2. **Meta 2024 "Stealthy Saboteur" 未找到**。Meta 实际公开材料：2021-02-23 Recap、2022-03-17 silent errors（Fleetscanner/Ripple）、2024-06-19 PVF、2025-07-22 AI hardware reliable。
3. **fARM 是 BSC/UPC/清华的 HPC 工作（SC 2022），不是 Meta/Microsoft**。

## 1. SDC 根因分类表

| # | 根因 | 机理 | 激活条件 | 规模数据 | 出处 |
|---|---|---|---|---|---|
| 1 | **测试逃逸：边际缺陷** | 阻性通孔/弱单元使某条时序路径在**部分 V-T-f 工作点**违例；"测试整个 V-T-f 包络从成本上不可解"（Ryan'14 定义） | 特定 V/f/T 组合 + 特定指令/数据序列 | Google 舰队 **~5,000 DPM** 测试逃逸（工业目标 100-500，超一个数量级）；SDC 致害 ~1,000 DPM，横跨 11 平台 4 代 22-5nm | Google/Mitra arXiv:2508.01786【实证-原文】 |
| 2 | **早期寿命失效（ELF）** | 弱芯片现场数周-月劣化后出错；burn-in 不足以消除 | 时间 + 应力史 | 占 Google 送回分析芯片 **29%** | 同上【实证-原文】 |
| 3 | **电路老化（BTI/HCI）** | NBTI/HCI 抬 Vth→门延迟增→setup 违例；老化速率∝每个门**占空比（用户可控）**与温度 | 高温 + 特定输入模式维持偏置；软件可定向加速 | RISC-V FMA 流水线 **>7x** 老化加速实证 | Mashburn arXiv:2508.16868【实证-原文】 |
| 4 | 电气/微码设计缺陷 | 特定 V/f/T 下违例，"很小代码改动大幅改变可靠性" | 特定组件+操作条件 | Google ephemeral errors | HotOS'21【实证-摘要】 |
| 5 | 软错误（辐射） | 中子/α 翻转 | 随机不可重复 | 与硬缺陷不同类：Google SDC 芯片错误输出中位数 82 万/十亿核·时，远高于软错误率；Meta SDC ~1/千 vs 软错误 1/百万 | Google 2025【实证-原文】 |
| 6 | 无保护数据通路 | ECC 只覆盖 SRAM，数据通路大量逻辑无检错 | 缺陷落在无校验位置 | "CPU SDC 率比软错误 FIT 高数个数量级" | Meta 2021【实证-原文】 |

**受影响功能单元（阿里巴巴，>100 万 CPU、32 个月、SOSP'23）【实证-原文】**：
- 总检出率 3.61‱（≈1/2,770）；预生产期检出 90.36%。
- 所有在役微架构均有坏芯片（0.082‱–9.29‱），新一代不降。
- **~半数坏芯片仅单物理核受损**（运算单元等私有组件），半数全核受损（cache 等共享组件）。
- **脆弱功能：ALU、向量单元、FPU、cache 一致性、事务内存**；浮点最脆弱，位翻集中在中段/尾数段，每"(测试,芯片)"组合有固定 bitflip 掩码。
- **一致性 SDC 表现为跨线程数据不一致，单核 golden 比对抓不到，必须多线程**。

## 2. 负载设计杠杆清单（核心交付物）

### A. 并发与拓扑

- **A1 全核并发而非单核（含邻居核忙）**：①共享散热升温 ②高电流改变 VRM/供电噪声 ③一致性缺陷需跨核数据流。阿里巴巴实证：某坏核**仅当其他核忙时出错，频率随忙核数上升**；另一实验被测核温度几乎不变但 SDC 随利用率升——**电流/供电噪声独立于温度的触发因子**；Meta 案例：多线程歇性错，单线程+绑核对特定数据 100% 复现。【实证-原文】SOSP'23 + arXiv:2102.11245
- **A2 多线程一致性/共享内存压力**：cache coherency 实证五大脆弱功能之一；案例：打包线程+校验和 vs 守护线程偶发读到不一致。【实证-原文】
- **A3 SMT 兄弟线程同时压**：共享物理核的逻辑核通常**同频次失败同一测试**（阿里 Observation 4）。【实证-原文】
- **A4 逐核轮检**：半数坏芯片单核受损、各核失败率差数个量级；必须"每颗核、重复地"跑（SiliFuzz 同结论）。【实证-原文】

### B. 温度

- **B1 热浸润（推到接近 TjMax 并保持）**：SDC 频率与核温**指数关系**（6/27 颗坏芯片 r>0.75）；存在**最低触发温度阈值**（实例：全部错误在 59℃ 以上，阈值以下连测数天零复现）。【实证-原文】SOSP'23 Obs.10
- **B2 测试顺序：高发热负载在前、受检负载紧随**：testcase Y 只有在发热的 X 之后跑才出错；更高效的检测工具链因发热减少而 SDC 频率下降。【实证-原文】
- **B3 冷角覆盖（低温+高频）**：温度反转（低温 Vt 升高、HVT 路径 setup 变差，最坏角可能是 0.72V@-40℃）；证据间接（EDA 共识 + Meta SOSP'23 报告频率与温度负相关的二手摘要）。**结论：热角证据强，冷角证据间接——设计热-冷双角，优先热角。**

### C. 电压/频率/电流

- **C1 负载阶跃制造 di/dt droop**：Meta Ripple **7% 覆盖是长跑连续测试永远抓不到的**，来源正是"测试指令与工作负载的频繁切换/工作模式切换"；"通过唯一种子并制造更多 transition 事件，能检测到需要数千次迭代才显现的远端故障"。【实证-原文】arXiv:2203.08989 + 2022 博客
- **C2 遍历频率/电压工作点**：边际缺陷只在部分 f-V 点违例；"某个 f、V、I 组合下正确不代表所有工作点正确"（Meta）；"时序测试模式必须随电压变化"（Google 引用）。操作含义：周期性在最大频率与中间 P-state 间迁移。【实证-原文】

### D. 指令/数据模式（对 stress-ng 最可操作）

- **D1 长指令序列/真实程序形态（最强新证据）**：ITHICA（Stanford/Google，3000+ 服务器）发现**"序列驱动的执行上下文"是错误显现的首要预测因子**；100 台坏服务器只有 1 台能用单指令测试复现，其余需要"略长序列"到"完整程序"。【实证-原文】arXiv:2605.15638。**注意：此文直接反驳阿里 SOSP'23 的"指令使用压力"结论（阿里发现失败用例比其他用例多数个量级使用缺陷指令）——两篇均一手实证，结论冲突。稳妥设计=两者都要：高密度重复目标指令 × 包裹在多样化长序列上下文。** OpenDCDiag 的构成（eigen/zlib/zstd/openssl 真实库）佐证。
- **D2 FMA/浮点密集 + 最坏翻转操作数**：FPU/向量实证最脆弱；NBTI/HCI 老化∝占空比，背靠背 FMA+交替 0x5555/0xAAAA 类操作数使关键路径每拍翻转（P=αCV²f 最大化）；定向老化 >7x。【实证-原文】
- **D3 随机化数据输入**：SDC 数据依赖（Meta 核 59：`Int(1.1^53)=0` 而 `Int(1.1^52)` 正确；"3×5=15 但 3×4=10"）；测试空间指数级必须随机化撒点。【实证-原文】
- **D4 尾数位段敏感的浮点模式**：位翻集中在中段/尾数、每设定固定掩码——校验器比对全位宽；扰动器提供扫操作数位空间的浮点内核（三角函数等复杂路径被点名）。【实证-原文】
- **D5 全数据类型覆盖**：i16/i32/ui32/f32/f64/bit/byte 全受影响，浮点最多。【实证-原文】

### E. 时长/迭代

- **E1 数千次以上迭代 + 足够长 soak**：发生频率低至 0.01 次/分钟（51.2% 设定 >1 次/分钟）；Ripple："需要数千次相同数据输入的迭代"。【实证-原文】
- **E2 周期性重复（对抗老化漂移）**：Meta 实验同一计算每天跑一次，**6 个月后设备才开始算错**。【实证-原文】

### F. 复合编排（最关键综合结论）

- **F1 "高温 + 全核并发 + 高翻转 FMA + 随机数据 + 长迭代 + 模式切换"复合编排**：所有实证 converge 到 SDC 激发是多因素对齐问题。Meta 2025 最强背书："**在非生产阶段用人工负载诱导条件使故障更可复现**"——正是本 fork 扰动器定位。【实证-原文】

### G. 老化加速

- **G1 占空比控制的输入模式**：让目标路径 PMOS 长时间导通偏置即定向加速 NBTI；温度 Arrhenius 指数加速。stress-ng 类"驻留高翻转+高温"负载本质是双向 power virus：既压时序裕量（即时激发）也累积 Vth 漂移（延迟激发）。【实证-原文】arXiv:2508.16868（门级仿真，非商用 ARM 直接复现）

## 3. 检测/筛选侧互补做法

| 机制 | 做法 | 与扰动器接口 |
|---|---|---|
| Golden run 比对 | SiliFuzz 模拟器语料、阿里 633 用例、OpenDCDiag 真实库 | 扰动器制造应力，golden 校验器在应力窗口内运行 |
| 双执行比对 | ITHICA 线程内复制指令比对，比原生校验 +39%、比 SiliFuzz +69% | 扰动器主动制造多样化执行上下文 |
| 在产 trickle/ripple | Meta Ripple 15 天 70% 覆盖；Intel DCDT ~15 分钟 | 扰动器提供低强度持续+瞬时尖峰两档 |
| 在产 scan（CASP 系） | Amazon DIFT、Intel IFS、NVIDIA IST | 与 stress-ng 正交（DFT 层） |
| 系统健康取证（CCKC） | Google：内核崩溃集中单核（≥80% 同核、≥5 次、≥3 栈顶、30 天）→70% 确为 SDC 核 | 扰动器异常需 per-core 归因日志 |
| Hardware Sentinel（ASPLOS'25） | 测试无关核集中性分析，+41% | 要求扰动器不制造假异常 |
| 优先级测试+温度控制（Farron） | 高可复现→长测；低可复现→压温度边界以下 | 证明温度边界是可运营控制面 |

**ARM 侧（诚实说明）**：Arm RAS/ESB 针对已上报错误的同步化，对无检错通路 SDC 无能为力；Neoverse 服务器核（V1/V2/N2）**不提供 lockstep**；AmpereOne SDC 材料域名不可达未核实。**含义：ARM64 服务器不能指望硬件冗余，"软件扰动器+golden 比对"是唯一可行路线。**

## 4. 参考文献

★=逐字阅读 ☆=API 核实摘要 ○=未核实
1. ★ Dixit 等 (Meta) "Silent Data Corruptions at Scale" arXiv:2102.11245
2. ☆ Hochschild 等 (Google) "Cores that don't count" HotOS'21 DOI:10.1145/3458336.3465297
3. ★ Dixit 等 (Meta) "Detecting silent data corruptions in the wild" arXiv:2203.08989
4. ★ Wang 等 (阿里) "Understanding Silent Data Corruptions in a Large Production CPU Population" SOSP'23 DOI:10.1145/3600006.3613149（镜像 safari.ethz.ch）
5. ☆ Wang 等 "Understanding SDC in Processors for Mitigating its Effects" ACM TACO 21(4) 2024 DOI:10.1145/3690825（Farron）
6. ★ Mitra 等 (Google/Stanford) "SDC by 10× Test Escapes Threatens Reliable Computing" arXiv:2508.01786
7. ★ Vavelidou 等 "ITHICA: Intra-Thread Instruction Checking" arXiv:2605.15638
8. ★ Mashburn 等 "Targeted Wearout Attacks in Microprocessor Cores" arXiv:2508.16868
9. ☆ Serebryany 等 (Google) "SiliFuzz: Fuzzing CPUs by proxy" arXiv:2110.11519
10. ★ Meta Engineering "silent data corruption at scale" 2021 engineering.fb.com/2021/02/23/data-infrastructure/silent-data-corruption/
11. ★ Meta Engineering "Silent errors" 2022 engineering.fb.com/2022/03/17/production-engineering/silent-errors/
12. ★ Meta "How Meta keeps its AI hardware reliable" 2025 engineering.fb.com/2025/07/22/data-infrastructure/how-meta-keeps-its-ai-hardware-reliable/
13. ★ Meta "PVF" 2024（论文 arXiv:2405.01741）
14. ★ OpenDCDiag github.com/OpenDCDiag/opendcdiag
15. ☆ Parthasarathy (Google/OCP) OCP Blog 2024（Cloudflare 拦截未读）
16. ○ Lee 等 (Meta) SOSP'23 DOI:10.1145/3600006.3613148（Scavenger，二手）
17. ○ Chatzopoulos 等 "Veritas" HPCA 2025 DOI:10.1109/HPCA61900.2025.00012
18. ○ Chatzopoulos 等 "From gates to SDCs" DATE 2025
19. ○ Dutta 等 "Hardware Sentinel" ASPLOS 2025
20. ○ Deutsch 等 "PinDrop" HPCA 2026 DOI:10.1109/HPCA68181.2026.11408620
21. ○ Wang 等 (BSC/UPC/清华) "fARM" SC'22 DOI:10.1109/SC41414.2022.00030
22. ○ Trock 等 (Amazon) "Deterministic In-fleet Scan Test" ITC 2024
23. ○ Ryan 等 "Process Defect Trends and Strategic Test Gaps" ITC 2014
24. ☆ Deutsch 等 "DelayAVF" MICRO 2024
25. ☆ "Understanding SDC in LLM Training" arXiv:2502.12340
26. ○ Van De Ven (Intel) DCDT 2021/2024
