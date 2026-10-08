# Findings: stress-ng × SDCShield 协同 SDC 压测（v5，2026-10-08 第十四轮更新）

## 12. 第十四轮：顶级开源项目化改造调研（2026-10-08）

### 12.1 现状盘点与顶级项目标准差距

**代码资产**（全部已在 CI 15 镜像验证）：bitgen 位形状生成器（bandwalk/edge 字典/FP 位型直合成/互补对/汉明定向）；
operand-var（5 方法）/addrspace（7 配方）变异 stressor；armcrypto 13 方法（NEON 9+SVE2 4）；sve2 5 方法；
fma SVE2 内核运行时分发；ls64/rdrand/tsc/regs；缓存维护指令（CIVAC/CVAC/ZVA）；taskset physical；
rapl hwmon；memrate 4 写模式；vm rand-offset；sdc-run 编排（full/scan/path/pair/abtest + preheat +
keep-bg + sdcshield hook）；sdc-report/sdc-flip-collect/ci-trend；multi-os-verify CI（15 镜像 +
CI-MATRIX 矩阵 + ghcr 发布）。

**工程差距表**（对照顶级开源项目标准）：

| # | 顶级标准 | 现状 | 处置 |
|---|---|---|---|
| G-1 | repo 元数据自述定位 | description/homepage=上游原文，topics 空，wiki 开启 | A1 重写 |
| G-2 | README：一句话定位+badge+快速上手+架构+文档导航 | 中文 fork 段+上游原文拼接，无 badge/架构图 | A2 重写 |
| G-3 | SECURITY.md | 无（故意压边界的工具尤其需要） | A3 |
| G-4 | CONTRIBUTING.md | 无（13 轮纪律只活在 CLAUDE.md） | A3 |
| G-5 | CHANGELOG + Release | 从未发布；版本无 fork 标识 | A4/C3 |
| G-6 | issue/PR 模板 | 无 | A5 |
| G-7 | 无上游作者信息残留 | FUNDING.yml=上游收款账号；.travis.yml；3 个上游 workflow | A6 |
| G-8 | 用户向文档（架构/方法论/集成指南） | 仅内部研发记录 superpowers/ | B1-B4 |
| G-9 | CI 状态可见（badge） | 有 CI 无 badge | D1 |
| G-10 | 独立项目叙事与架构 | "fork+补丁集" | 本轮核心 |

### 12.2 上游残留 workflow 处置分析（A6 依据）

| 文件 | 触发 | fork 下的行为 | 建议 |
|---|---|---|---|
| container-image-edge.yml | push:master + 每日 cron | push 不触发（默认分支 main）；**schedule 实际每天在跑且失败**（run 37576341125、37420821028 均 failure——E3 收尾时证伪了此前"fork schedule 默认禁用"的假设） | 删除（正确决策，2026-10-08 起停止） |
| container-image-stable.yml | release published | **一旦 A4 发 Release 会自动触发**，以上游命名推 ghcr | 改造为 fork 的 release 镜像流（D2）或先删除 |
| ci-builds.yml | workflow_dispatch 手动 | 手动跑上游多平台（ubuntu/freebsd/macos/cygwin）构建 | 删除（与 arm64 SDC 定位无关） |
| .travis.yml | - | Travis 早已废弃 | 删除 |
| FUNDING.yml | - | 仓库页面展示**上游作者**收款链接 | 删除（fork 不得保留上游收款信息） |

### 12.3 架构设计：五层激发引擎

```
┌────────────────────────────────────────────────────────────────┐
│ L5 编排层   sdc-run.sh: full / scan / path / pair / abtest /    │
│             excite · 拓扑自推导 · preheat · keep-bg · 报告       │
├────────────────────────────────────────────────────────────────┤
│ L4 杠杆层   di/dt(varyload 6 波形) · 热浸润(preheat) ·           │
│             SMT 争用(pair) · 全核并发 · 长 soak · 顺序效应        │
├────────────────────────────────────────────────────────────────┤
│ L3 通路层   ALU/branch · 向量(SVE2/NEON) · crypto(AES/SHA/SM3/  │
│             SM4) · atomics(LSE) · LSU(ls64/misalign) ·          │
│             缓存层级 · MMU/TLB(addrspace) · 互联(NUMA)           │
├────────────────────────────────────────────────────────────────┤
│ L2 数据形状层  bitgen: bandwalk 位段扫掠 · 57 边界值字典 ·        │
│             FP 位型直合成 · 互补对 · 汉明定向 · 模式混合          │
├────────────────────────────────────────────────────────────────┤
│ L1 硬件感知层  HWCAP/HWCAP2/3 探测 · 拓扑推导 · SVE2 march 注入  │
│             · 诚实跳过 · 单 binary 跨 920/950                    │
└────────────────────────────────────────────────────────────────┘
```

设计原则：
1. **激发优先**：编排默认把计算资源全部投向激发；检测角色 100% 归 SDCShield（excite 模式），
   --verify 只作搭车哨兵（full 模式可选）。
2. **确定性 oracle 纪律**：随机化只进压力路径，verify oracle 保持恒等式/字面量（12 轮纪律不变）。
3. **诚实跳过**：无硬件 skipped with reason，绝不假跑。
4. **冲突面收敛**：fork 定制优先新文件；共享文件改动收敛到标记区段；
   README 采用"fork 主文档 + 上游原文分离"策略（.gitattributes merge=ours，见 B4）。

### 12.4 改造 patch 清单（A-E 组）

**A GitHub 门面**：
- A1 元数据：description="SDC excitation engine for arm64 servers — a stress-ng fork that
  maximizes silent-data-corruption excitation …" / topics: arm64,aarch64,stress-testing,
  silicon-validation,hardware-reliability,silent-data-corruption,sve2,kunpeng,ras,cpu-diagnostics /
  homepage 置空 / 关 wiki（curl PATCH + PAT）。
- A2 README 重写（语言按 D14-1）：badge 区 / Why-SDC 问题陈述 / 五层架构图 / 快速上手 3 命令 /
  能力矩阵 / SDCShield 协作拓扑 / 上游关系与致谢 / 安全警示。
- A3 SECURITY.md（"故意压边界，只在专属压测机运行"政策+免责+报告渠道）、
  CONTRIBUTING.md（one-patch-per-unit + 验证 6 步 + 代码纪律文档化）、SUPPORT.md。
- A4 CHANGELOG.md（13 轮演进整理，progress.md 为素材源）+ 首个 Release v0.22.00-sdc.1
  （notes：定位/能力/快速上手/已知限制）。
- A5 .github/ISSUE_TEMPLATE/{bug_report,feature_request}.md + PULL_REQUEST_TEMPLATE.md。
- A6 残留清理（§12.2 全表执行）。

**B docs 用户文档**：
- B1 architecture.md：上图 + 各层职责表 + 设计决策记录（为何五层、verify 角色定位、诚实跳过）。
- B2 excitation-guide.md：**激发覆盖率矩阵**（行=杠杆[di/dt/热/SMT/全核/缓存/TLB/互联/数据形状/soak]，
  列=通路[ALU/向量/crypto/atomics/LSU/缓存/MMU/互联]，cell=stressor/方法/编排模式）——
  同时是"还缺什么激发"的路线图；附方法论与文献引用。
- B3 sdcshield-integration.md：双工具拓扑图 + 协同剧本（阶段 0-5）+ 报告字段对接（cpu-mask 等）。
- B4 upstream-sync.md：同步节奏（每上游 release，PR 方式，先例 PR #5）+ .gitattributes
  merge=ours 清单（README.md 等 fork 完全重写文件）+ 共享文件区段标记规范 + merge 后 6 步验证。

**C 代码（one-patch-per-unit）**：
- C1 `sdc-run.sh excite` 模式：无 --verify 组合（cpu-method all 轮换 + fma/sve2/armcrypto 向量通路 +
  operand-var/addrspace/memrate-bandwalk/vm-rand-offset 形状通路 + varyload di/dt + preheat 热浸润），
  worker 数拓扑自推导，支持 --sdcshield 并行检测；VERIFY_ALWAYS 类 stressor（operand-var）保留——
  其 verify 成本占比低且数据形状本身即激发。
- C2 激发默认值审计：对照 CI-MATRIX bogo-ops/s 分布复核各模式 stressor 配比，结论写进 excitation-guide。
- C3 版本标识：Makefile VERSION=0.22.00-sdc.1 单点定义，--version 呈现（上游 merge 时单行冲突易解）。

**D CI/发布**：D1 badge（multi-os-verify workflow badge + release badge + ghcr badge）；
D2 Release 流水线（stable 镜像流：改造 container-image-stable.yml 为 fork 命名
sdc-stressng:stable，或并入 multi-os-verify publish job 加 release 触发——二选一，实施时定）。

**E 验证**：E1 文档命令 parse-verified 全量；E2 快速上手实测；E3 ci-monitor.sh --new-code 15 镜像全绿；
E4 链接/badge 检查。

**执行顺序**：A6（先拆危险/违规残留）→ A1 → A2/A3/A5 → A4 → B1→B4 → C1→C3 → D1/D2 → E。

### 12.5 决策点（呈报用户）

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| D14-1 | README 语言 | 英文主+README.zh-CN.md / 纯中文 / 纯英文 | **英文主+中文版**（国际惯例+团队中文文档成本可控） |
| D14-2 | 纯激发模式 | excite+full 双模式 / 所有模式去 verify / 维持现状 | **双模式**（"核心=激发"落地为 excite；full 保留哨兵） |
| D14-3 | 上游同步+workflow | 定期 merge+删上游 workflow / 冻结+cherry-pick / 彻底独立 | **定期 merge+删**（上游修复有价值；B4 规范控制冲突成本） |
| D14-4 | 本轮范围 | 全部 A-E / 先 A+B 门面文档，C-E 下轮 | **全部**（"彻底改造"语义；C/D 组风险可控） |
| D14-5 | 版本呈现 | 0.22.00-sdc.1；项目名/二进制名不变 | 已定默认（可推翻） |

## 11. 第十三轮：复盘驱动改进方案研究（2026-09-20）


### 11.1 本机实测数据（文档与方案的事实基础）
| 项 | 实测值 |
|---|---|
| operand-var 方法 | all / bandwalk-int / bandwalk-fp / edge-dict / complement-fma / type-matrix（5+all） |
| addrspace 配方 | all / huge-random-fixed / va-bit-walk / dense-random-offset / guarded-holes / malloc-giant / misalign-huge / mixed-orders（7+all） |
| memrate-write-pattern | 0xaa / random / bandwalk / complement |
| vm-method | 含 rand-offset（39 方法之一） |
| armcrypto 方法 | 13（NEON 9 + SVE2 4：sve2-aes/pmull/sha3/sm4） |
| sve2 方法 | fmla / gather / fcmla / bitperm / bfdot |
| stress-*.c 文件数 | 395 |
| operand-var --verify / addrspace 冒烟 | 本机（920，gcc12）passed / failed: 0 |

### 11.2 复盘遗留项代码现状（subagent 核查，带证据）

**遗留1 bandwalk 窗口参数（core-bitgen.c:150-210）**：
- 现值硬编码：`band_width = 6 + mwc%15`（6-20 位）、density ∈ {0,25,50,75,100}%（5 档）、`band_step = 1 + mwc%7`
- 每完成 64 位全扫掠重摇 width/density；无任何运行时调参接口（无选项、无 env、无 man 条目）
- `scripts/bitgen-distribution.sh` 是 build-time 统计验证（6 项：边界命中/位覆盖/FP 指数域/complement/hamming/mwc 无污染），**不是真机位翻分布采集器**——校准数据采集需新工具（可参考其 ones[] 计数模板 + 直接调 bitgen API）

**遗留2 cache 系列数据值（tag 编码现状）**：
| 文件 | 元素宽度 | verify | 记账位 | 自由位 | bitgen 调用 |
|---|---|---|---|---|---|
| stress-cache.c | uint8 | 无（读值只累加输出） | 0 | 8 | 无 |
| stress-l1cache.c | uint8 | `*ptr != (uint8_t)set` | 8（=set 号） | verify 模式 0 | 无 |
| stress-cacheline.c | uint8 | 全方法 VERIFY_ALWAYS | 8（状态机值：递增/rol/ror/拷贝） | 0 | 无 |
- **复盘结论成立且比复盘更具体**：三者均为字节粒度状态机，无"地址+计数器打包 64 位 tag"设计，无空闲高位可注入随机 payload
- cacheline-method 实际 **11 个**（all/adjacent/atomicinc/bits/copy/inc/mix/rdfwd64/rdints/rdrev64/rdwr），复盘记的 13 有误
- 正面先例：stress-vm.c:1101-1193 rand-offset——fill/verify 双份同种子 bitgen（`shim_memcpy(&bg_verify, &bg, ...)`），确定性 PRNG 流 = 隐式 tag，无需显式地址字段。**cache 系列要复用此模式必须从字节粒度升到字（64位）粒度**

**遗留3 pagemap PFN 导向**：无任何代码（P3 有意跳过）；需 root + /proc/pagemap 读 PFN

**遗留4 A/B 回归基建现状**：
- A/B 分界 commit 明确：`fc243c784`（变异前基线）→ `c7e7ebf55`（变异后 HEAD），两者均可独立构建
- sdc-run.sh full 模式失配记录：仅 `grep -E "failed: [1-9]|data difference|mismatch" A_full.log | head -5`（sdc-run.sh:304）——**无失配计数、无失配率、无位置汇总**
- 输出目录：topology.txt / A_full.log / A_full.yaml / interrupts-{before,after}.txt /（可选 preheat.log、sdcshield.log）
- stress-ng -Y yaml **无 verify 失配结构化字段**（metrics 段只有 bogo-ops 等，stress-ng.c:2537-2549）；失配只在文本流（pr_fail 文案含元素下标/expected/actual/翻转位数/xor，stress-fma.c:664-670 位级诊断最全）
- sdcshield 侧（../sdcshield，二进制已构建）：-Y YAML 每 test 带 result: pass|fail、fail 条目含 cpu-mask/time-to-fail/seed、汇总 "Test failed M out of N times (X%)"——**可直接给 A/B 对比提供失配率数字**

**遗留5 CI 数据对比基建现状**：
- CI-MATRIX 行格式：`CI-MATRIX <tag> <stressor> <PASS|SKIP|FAIL> <bogo-ops/s|->`（ci-verify-log.sh:180-189）；汇总行 CI-MATRIX-SUMMARY
- results-summary job 用 gh api 拉全部 build-test job log 渲染矩阵；留存 = Job Summary 截 1MB + artifact 90 天
- 时间序列可行：`gh api /repos/.../actions/runs?workflow_id=...&created=>=日期` → 每 run 的 jobs → 每 job 的 logs（与 summary job 同接口）；或拉历史 artifact（results-matrix）
- **缺口**：没有跨 run 的趋势对比工具（每个 run 只看自己），需要一个小脚本做时序聚合

### 11.3 复盘五条坑的流程化去向（CLAUDE.md 素材）
1. 0基/1基分派：方法表必须 grep 参照现有 stressor 的 "all" 处理（stress-cpu.c:3114）再写
2. verify oracle 确定性：随机化只进压力路径，oracle 保持字面量/恒等式（atomic P8 的教训）
3. 复杂 recipe 先纸上定骨架：fill/verify 流同步 = 预绘制决策 + 无放回采样
4. grep 上游惯例先于写代码：宏名（HAVE_MMAP 不存在、MB→STRESS_MB、shim_mmap 不存在）
5. 大改动先想清注入点：死代码当场回滚，宁可 git checkout 重来


## 10. 第十二轮：数值/地址空间变异强化研究（2026-09-19）

### 10.1 文献杠杆（R2 报告 D 族，直接支撑本轮）
| 杠杆 | 实证内容 | 对变异设计的含义 |
|---|---|---|
| D3 随机化数据输入 | Meta 核 59：`Int(1.1^53)=0` 而 `Int(1.1^52)` 正确；"3×5=15 但 3×4=10"；测试空间指数级必须随机撒点 | 均匀随机是必要不充分——数据依赖 SDC 集中在**边界值邻域**（指数边界、尾数边界） |
| D2 最坏翻转操作数 | 交替 0x5555/0xAAAA 使关键路径每拍翻转（P=αCV²f 最大化），定向老化 >7x | 需要**互补位对/翻转最大模式**生成器，不只是随机 |
| D4 尾数位段敏感 | 位翻集中在中段/尾数、每设定固定掩码 | 需要**位空间扫掠**（单 bit 翻转游走、位段掩码轮换），均匀随机对低概率位段覆盖差 |
| D5 全数据类型 | i16/i32/ui32/f32/f64/bit/byte 全受影响，浮点最多 | 类型×变异算子矩阵式覆盖 |
| E1 数千次迭代 | SDC 频率低至 0.01 次/分钟 | 变异模式必须可长期循环不重复 |

### 10.2 基建现状（主线核验）
- **PRNG 采用面广**：252 个 stressor 使用 stress_mwc_*（Marsaglia MWC，周期 ~2^60，统计质量足够但无密码学强度）
- **sdcshield 参照**：AES-PRNG（aarch64 vaeseq 硬件加速，per-thread 状态，Constant/LCG/AES 三引擎）——同机房"高质量随机"的对照标杆
- **关键洞察（预期修正）**：问题不是“没有随机”，而是“随机的形状不对”——均匀随机对位段敏感缺陷的覆盖效率低（52 位尾数空间均匀撒点 vs 定向位段扫掠）；固定模式（0x55/0xAA 类）又走向另一极端。**方案方向 = 模式字典 × 均匀随机的混合生成器**


## 9. 第八轮工程事实（2026-09-17，GitHub Actions 多 OS 验证工作流；全部本机实测）

### 9.1 stress-ng 测试基建实测（构建 rc=0，gcc 12.3.1，openEuler 24.03 SP3 aarch64）
| 事实 | 证据 |
|---|---|
| `--sequential N` 逐个跑全部 stressor，默认 60s/个，`--timeout` 覆盖 | stress-ng.1:1041-1046；实测进行中 |
| `--sequential`/`--all`/`--random`/`--permute` 互斥，`--class` 只能与后三者联用 | stress-ng.c:4684-4696 |
| `-x/--exclude` 是**逗号分隔纯名列表**（非正则）；名字必须已存在，否则报错退出 | stress-ng.c:594-607（shim_strtok_r 按 "," 切分 + stress_stressor_find 校验） |
| 病理 stressor 默认禁用并提示 `--pathological` | bad-ioctl/mlockmany/oom-pipe/sysinval/watchdog（实测日志） |
| 退出码：fail→EXIT_NOT_SUCCESS、无资源→EXIT_NO_RESOURCE、metrics 不可信→独立码；全过→EXIT_SUCCESS | stress-ng.c:5042-5050 |
| yaml 字段（-Y）：`metrics:` 下逐 stressor `bogo-ops`/`bogo-ops-per-second-real-time`/`wall-clock-time`；还有 `build-info:`/`system-info:` 头 | /tmp/probe.yaml 实测 |
| `<name>-method all` 关键字普遍支持（62 个 `*-method` 选项） | core-opts.c grep + stress-cpu.c:3114 `{ "all", stress_cpu_all }` |
| `--skip-silent` 静音"诚实跳过"消息 | core-opts.c 存在 |
| Makefile SANITIZE=1 = UBSAN 全家桶（cc_supports_flag 自动过滤老 gcc 不支持项） | Makefile:155-176 |
| `all: build_info config.h stress-ng`；`make` 自动先跑 Makefile.config 生成 config.h | Makefile:920,990 |

### 9.2 网络/CI 环境实测
| 事实 | 含义 |
|---|---|
| 开发机 curl hub.docker.com SSL rc=35；auth.docker.io 也失败 | Docker Hub tag 存在性无法在开发机预验 → 工作流内加 fail-fast `manifest inspect` 预检 job |
| ghcr.io 可达但匿名 token 对 openeuler/openeuler DENIED（该 org 镜像不在 ghcr） | openEuler 基础镜像只能从 Docker Hub 拉；用户自有镜像在 ghcr.io/wangxumarshall/* |
| gh 未登录 | 推送用 git ssh；首跑验证需用户在 GitHub 网页触发或本地 gh auth login |
| 本机 = aarch64 openEuler 24.03 SP3（与 CI 目标环境同族） | 本机构建/试跑结果可代表 24.03 容器行为 |
| 容器内默认 root | stressor 可跑面最大；`--oomable` 默认即可 |

### 9.3 全量 sequential 实测（关键验收数据）
- 命令：`./stress-ng --sequential 1 --timeout 2 --metrics-brief -Y seq.yaml --exclude bad-ioctl,sysinval,watchdog,mlockmany,oom-pipe --skip-silent`
- 结果（openEuler 24.03 SP3 aarch64，**非 root** 本机）：**319 passed / 74 skipped / 0 failed / 15m50s / rc=0**
- 74 skipped 是诚实跳过（root/内核特性/库缺失：acl bpf kvm jpeg judy mpfr ls64 sve2 x86cpuid …）→ 容器内 root 会转 passed 一部分（ioport cpu-online ramfs rofs 等）
- CI 断言锚点：`failed: 0` 行 + `successful run completed` + rc==0；`--timeout 5` 预算约 40min/镜像
- pathological 五兄弟（bad-ioctl mlockmany oom-pipe sysinval watchdog）默认自动禁用，无需手工排除（但排除更干净）

### 9.4 设计结论（写入 task_plan Phase 8）
- "全量用例×全量参数"务实解：`--sequential 1 --timeout <N>s --verify`（用例全量）+ 62 个 `<name>-method all` 遍历（参数全量）+ 每镜像预算 45-60min
- 断言：`grep -E "failed: [1-9]" log`（失败>0）+ rc==0；跳过不算失败（诚实跳过机制）
- 20.03 EOL 源 → 容器内 sed 替换 archives.openeuler.org；dnf 失败不 fatal（基础镜像自带工具链）
- 15 镜像矩阵 fail-fast:false + `if: always()` 汇总 job 断言 15/15
- cron：北京时间 12:00 = UTC 04:00 → `0 4 * * *`（避开整点分钟不可行——用户明确指定时刻，按需写 0 4）
- ghcr 改名：新推 `ghcr.io/wangxumarshall/sdc-stressng:verify-<tag>`；历史 opendcdiag-arm 的删除/改名需用户在 GitHub package 设置里手动操作（API 不支持 rename，git push 新名即可生效）



> v3 变更：第六轮（对标 x86 → ARM64 饱和压测激发 SDC）研究成果并入。三份完整研究报告在 `docs/superpowers/research/`（R1 x86 基线 / R2 SDC 前沿 / R3 ARM64 饱和），实现方案在 `docs/superpowers/plans/2026-09-16-arm64-saturation-sdc.md`（12 patch）。本文件保留 v2 的历史研究与已落地方案记录；§8 起为第六轮摘要。

## 8. 第六轮研究摘要（2026-09-16，详细见 research/ 与 plans/ 文件）

### 8.1 已落地基线确认
port/kunpeng950-sdc-stress 的 11 个 patch 已全部合入 main（`git merge-base --is-ancestor bea490355 main` 实证）。后续工作以 main 为基线。

### 8.2 x86 对标缺口 Top（R1 全表见 research/r1）
1. 硬件 RNG（rdrand→RNDR，950 有硬件）
2. 加密单元直打（ipsec-mb→NEON/SVE2 crypto，全树零覆盖，**本机有 NEON aes/sha1/sha2 可全功能验证**）
3. 向量寄存器堆（XMM→v/z-regs，x86 有先例）
4. FMV（target_clones ARM 空且 GCC12 不支持，需 target 属性双版本替代）
5. tsc→CNTVCT（四架构有分支唯独缺 ARM）
6. 功率遥测（RAPL→hwmon）
7. 缓存维护指令（clflush 家族→DC CVAC/ZVA，本机 EL0 可执行）

### 8.3 SDC 激发实证杠杆（R2 全表见 research/r2）
全核并发+邻居忙（电流独立于温度）、SMT 兄弟同压、热浸润（指数关系+最低触发阈值）、高发热在前校验紧随的顺序效应、di/dt 阶跃（Ripple 7% 独有覆盖）、长序列/真实程序上下文（ITHICA：100 台坏服务器仅 1 台单指令可复现；与阿里"指令压力"结论冲突→两者都要）、随机化数据、数千次迭代长 soak、周期性重复（6 个月才显性化案例）。**ARM 服务器无硬件冗余（无 lockstep、RAS 管不了无检错通路）→ 软件扰动器+golden 比对是唯一路线。**

### 8.4 关键工程实证（决定实现路线）
- `target("+crypto")` + NEON crypto intrinsic：本机编译运行通过
- GCC 12 arm_sve.h **无 SVE2 加密 intrinsic** → 必须 .inst 数值编码（已验证可编译）
- binutils 2.41 SVE2 加密助记符有语法问题 → 统一 .inst
- 开发机可全功能验证：NEON aes/sha1/sha2/pmull、v0-v31、DC ZVA/CVAC、CNTVCT；诚实跳过：SVE2 全系/SM3/SM4/SHA3/RNDR/z-regs（目标机 950 有硬件）
- hisi uncore PMU 本机存在（ddrc/hha/l3c），perf 需 root

### 8.5 方案结构（plans/2026-09-16-arm64-saturation-sdc.md）
Wave1（本机全验）：P1 armcrypto(NEON+KAT) / P2 regs-v / P3 tsc-CNTVCT / P4 DC CIVAC+ZVA / P5 sdc-run pair+preheat
Wave2（目标机全功能）：P6 armcrypto SVE2+SM3/SM4(.inst) / P7 rdrand-RNDR / P8 regs-z/p / P9 sve2 gather/fcmla/bfdot 方法表
Wave3：P10 rapl-hwmon / P11 fma FMV 试点 / P12 文档总扫尾
Backlog：vnni-DOT、DSB 风暴、priv-instr 扩充、MIDR 枚举、LSE 显式、topdown、SMT 争用内建（各含缓做理由）

---

（以下为 v2 历史内容，2026-09-14 第五轮，保持原样）

> v2 变更：吸收外部工程建议（L0 重编 / L1 编排 / L2 源码三层结构、四阶段流程、CORE179 全核并发经验、多证据判读），并对建议做了逐条代码核实——其中 3 处技术错误已修正（见 §0）。所有代码事实断言均经本仓库真实源码/二进制验证。

## 0. 外部建议核实结论（逐条对真实代码）

| 建议断言 | 核实结果 | 证据 |
|---|---|---|
| Makefile 无 ARM -march，默认 -O2 | ✅ 真 | `grep march/mcpu/mtune Makefile*` 无输出 |
| target_clones 仅 x86，ARM 上为空 | ✅ 真 | core-target-clones.h:36 `STRESS_ARCH_X86`、:398 PPC64；ARM 落到空 `#define TARGET_CLONES`（:437） |
| 默认构建无 SVE 指令 | ✅ 真且更强 | objdump 实测：182 个 fmla **全部 NEON v 寄存器形式**，`ld1d/ptrue/whilelo` 计数 **0**，z 寄存器 fmla **0** |
| fma verify=同算两遍 memcmp | ✅ 真 | stress-fma.c:568-586，报错文案 "data difference between identical double fma computations" |
| fma/vecfp/matrix 是 VERIFY_OPTIONAL | ✅ 真 | stress-fma.c:625、stress-vecfp.c:518/534、stress-matrix.c:1069/1088 |
| --taskset 无 physical 关键字 | ✅ 真 | core-affinity.c:264-273（仅 package/cluster/die/core/even/odd/all/random）+ man:1088 |
| --ignite-cpu 仅 Intel P-State x86 | ✅ 真 | stress-ng.1:561 原文 "Currently this only works for Intel P-State enabled x86 systems" |
| varyload/mbind/interrupts/-K/thermalstat/tz/seed/-Y 存在 | ✅ 真 | core-opts.c:1770/809/598/680/1711/1719/1369 |
| `-march=armv8.2-a+sve2+svebf16+i8mm` 可用 | ❌ **错** | gcc 12.3.1 实测 `invalid feature modifier 'svebf16'`。**正确拼写：`armv8.6-a+sve2+bf16+i8mm`**（gcc 合法修饰符：sve2/sve2-sm4/sve2-aes/sve2-sha3/sve2-bitperm/i8mm/bf16/ls64…） |
| `--cdouble 95` 可作为 stressor | ❌ **错** | cdouble 是 **cpu-method** 不是 stressor（`--cdouble 1` → unrecognised option；`--cpu 1 --cpu-method cdouble` → passed:1） |
| objdump 验证 SVE 指令（建议作为可选检查） | ⚠️ **必须改为硬性验收门** | fma/vecfp 是纯 C 循环、ARM 上 TARGET_CLONES 为空——重编后自动向量化**是否**真生成 SVE 不可预测，必须 objdump 确认才算 L0 完成 |
| CP1 少 1 核（node1=95 核 vs node0=96 核） | ✅ 与 lscpu 吻合 | 382 线程 = (96+95)×2；与"CP1 核隔离/自检失败"现象一致，疑 BIOS 已 deconfigure，先取证 |

另实测确认（本机）：建议的负载分组 stressor 全部存在可运行（regs/opcode/ptr-chase/spinmem/misaligned/vecmath/memrate/l1cache/intmath 均 successful run）；`--eigen` 在本 fork 依赖 HAVE_EIGEN（VERIFY_ALWAYS，本机未配置→诚实跳过）。

## 1. 两工具的分工定位

| 维度 | SDCShield (../sdcshield) | stress-ng (本仓库) |
|---|---|---|
| 本质 | **校验器**：273 个 golden 比对用例，判定"算错了没有"、报 cpu-mask | **扰动器**：390 个负载形态，制造"容易算错的环境"（di/dt、缓存/TLB/互联压力、边界时序） |
| SDC 判定 | ✅ memcmp golden、EDAC/RAS | 辅助：342/390 stressor 有 --verify（计算类"同算两遍 memcmp"粗筛） |
| 负载形态广度 | 专（每用例一个微架构配方） | 广（CPU/VM/cache/io/sched/signal 全域 + varyload 阶跃） |
| 长稳烤机 | 弱 | ✅ 强（--timeout 小时级、--seq 全量轮跑） |

协同模型：stress-ng 制造电压/热/缓存/TLB/互联压力 → 压缩弱核时序裕量 → SDCShield golden 校验检出并定位。CORE179 经验（来自建议，与 sdcshield 的 movbe/core-179 配方呼应）：**单核独占往往不触发，必须全核并发背景** —— 这决定了阶段 A 先于阶段 C。

## 2. 目标机（CP1, Kunpeng 950 7592C）关键事实 → 压测策略

| 事实 | 值 | 策略含义 |
|---|---|---|
| SMT2，382 线程 = 191 物理核×2 | node0=0-191（96核），node1=192-381（95核） | **CP1 比 CP0 少一个物理核**——疑 BIOS deconfigure，阶段0 取证；压测区分"全线程 382"与"每物理核 191"两种形态 |
| SVE2 全家桶 | sve/sve2/sveaes/svepmull/svesha3/svesm4/svebitperm/svei8mm/svebf16 | **L0 必做**：默认构建零 SVE 指令，向量 stressor 只有 NEON 强度 |
| ls64/ls64_v | 64B 原子访存 | G6 stressor 保留（压 LSU+一致性） |
| 1200-2300MHz，boost disabled | scaling 100% | --ignite-cpu 无效（x86-only），手动锁 performance |
| CP1 已有核隔离 | 现象本身 | 隔离核不在线，压测打不到——先 `cat /sys/devices/system/cpu/{isolated,offline}` + BMC SEL 取证 |

## 3. 改进方案：三层结构（L0 → L1 → L2）

### L0 重编（零源码改动，必须最先做）

默认 Makefile 不带 ARM -march、TARGET_CLONES 在 ARM 为空 → 二进制零 SVE 指令（实测）。950 的 SVE2 管线完全没被压到。

```bash
make clean
make -j$(nproc) \
  CFLAGS="-O3 -march=armv8.6-a+sve2+bf16+i8mm" \
  CXXFLAGS="-O3 -march=armv8.6-a+sve2+bf16+i8mm"
# 硬性验收门（不是可选）：
objdump -d stress-ng | grep -cE '\b(fmla|ld1d|ptrue|whilelo)\s+z'   # 必须 > 0（z 寄存器才是 SVE）
```

注意：
- `svebf16` 拼写 gcc 12.3.1 拒绝，用 `bf16`（gcc 错误信息列出全部合法修饰符）
- fma/vecfp 是纯 C 循环，自动向量化是否生成 SVE 由编译器决定——**objdump 不过门 = L0 失败**，需退到 L2.5（显式 SVE intrinsic）
- 老编译器不认 `+bf16+i8mm` 时降级 `-march=armv8.2-a+sve2`（实测可用）
- 若构建机装了 eigen C++ 库，`--eigen`（VERIFY_ALWAYS）可用，SVE 后端由 Eigen 自带

### L1 编排层（立即可用，脚本 + 现有选项）

四阶段剧本（详见 §4 端到端命令）：
1. **阶段 A 全核背景压**：382 线程满载 + `--varyload` 制造 di/dt 阶跃 + 全部 `--verify`，SDCShield 并行全核校验
2. **阶段 B CP1 定向 / CP0 对照**：`--taskset 192-381 --mbind 1` vs `--taskset 0-191 --mbind 0` 同参数对照
3. **阶段 C 逐物理核 sweep**：每物理核取 SMT sibling 对（`thread_siblings_list`），`--cpu 2 --taskset <pair>` + 同核 taskset 绑 SDCShield
4. **阶段 D 固定 seed 复现取证**：`--seed` + `-K` klog-check + `--thermalstat`，区分热致/固有

L1 的所有选项已逐一确认存在：`--varyload/--varyload-ms/--varyload-method`（6 种波形：brown/saw_inc/saw_dec/triangle/pulse/random）、`--cpu-load-slice`、`--mbind`、`--interrupts`、`-K/--klog-check`、`--thermalstat`、`--tz`、`--seed`、`-Y yaml`、`--metrics-brief`、`--taskset-random`（400Hz 迁移）、`--taskset package/cluster/die/core`。

负载按故障通路分组（建议4，stressor 名全部实测存在）：
| 通路 | stressor 组合 |
|---|---|
| SVE/FMA 计算 | `--fma --vecfp --vecmath --vecwide --veccmp`（+`--eigen` 若可用）、`--cpu --cpu-method matrixprod/cdouble/float*` |
| load/store | `--misaligned --cacheline --l1cache --spinmem --stream --memrate --ptr-chase --tlb-numa` |
| 整数/原子/分支 | `--intmath --bitops --atomic --branch --regs --opcode` |

### L2 源码增强（本 fork 落地，one-patch-per-unit）

| Patch | 内容（建议 6 项为主干 + 我方 G 系列融合） | 文件 | 验证 |
|---|---|---|---|
| L2.1 | Makefile 增加 aarch64 条件块注入 SVE2 CFLAGS（`-march=armv8.6-a+sve2+bf16+i8mm`，编译器探测降级链），照搬 sdcshield SVE build block 模式 | Makefile / Makefile.config | 重编后 objdump z 寄存器指令 > 0；x86 构建不受影响（条件块仅 aarch64） |
| L2.2 | `--taskset physical` 关键字：core-affinity.c 的 topology 机制加 `thread_siblings_list` 支持——每物理核取首个线程构成掩码（SMT 感知） | core-affinity.c + man | 本机无 SMT → 掩码=全核；目标机 → 191 核。`--taskset physical0-1` 语义与 packageN 一致 |
| L2.3 | verify 失败路径位级诊断：首个不一致下标 + expected/actual 十六进制 + xor popcount（对齐 CORE179 方法） | stress-fma.c / stress-vecfp.c / stress-matrix.c | 人工注入错误（临时改一字节）验证输出格式；正常路径 pass 不变 |
| L2.4 | per-core sweep 模式：`--sweep-cores --sweep-duration N`，逐物理核轮换绑定、按 CPU id 输出 yaml 段（把阶段 C 的 shell 循环内建） | 新 core-sweep 组件 + stress-ng.c | 本机跑 `--sweep-cores -t 1m` 全核轮转，yaml 每核一段 |
| L2.5 | 显式 SVE intrinsic 方法（不依赖自动向量化）：predicated `fmla z`、64B `ld1d`、bf16/i8mm dot；无 SVE 硬件诚实跳过 | stress-vecfp.c 新 method 或新 stressor | 本机 920 无 SVE → skipped with reason；目标机 passed。guard: `__ARM_FEATURE_SVE` + HWCAP |
| L2.6 | `-K` klog-check 命中内核报错时用 `sched_getcpu()` 记录 worker 绑定核 | core-klog.c | 注入 dmesg 错误行验证关联输出 |
| L2.7（=G2） | 跨 socket 缓存行乒乓 stressor：两进程绑 node0/node1 对同一 cacheline 交替原子写 | 新 stress-ccxp.c | 本机 4 NUMA node 可测 → passed |
| L2.8（=G6） | ls64 64B 原子访存 stressor | 新 stress-ls64.c | 本机无 ls64 → skipped with reason；目标机 passed |
| L2.9（=G5） | cpu-method crc32：硬件 `__crc32cd` vs 软件双路径比对 | stress-cpu.c | `--cpu-method crc32 --verify` → pass |

丢弃/降级的我方原 G 项：G3（SVE2 专项 stressor）并入 L2.5；G4（L3 set stride）降级为 backlog（L2.7 乒乓已覆盖一致性主路径，先验证收益再决定）；G7（di/dt 配方）被 L1 的 `--varyload + --cpu-load-slice` 组合替代（零代码）；G9（EDAC 监视）被 L1 的 `--interrupts -K --thermalstat` 替代。

## 4. 端到端命令（面向 CP1 目标机，Kunpeng 950 7592C）

> 前置：目标机 root；SDCShield 预构建包 **SP 必须与目标机 SP 匹配**（SP3 包装 SP4 会 glibc 死结）；或源码构建。
> ⚠️ root 下 stress-ng 会调 OOM 配置使 stressor 不可杀；有核隔离/自检失败史的机器先短时基线再上长稳。

### 阶段 0：准备与取证（必做）

```bash
# 0.1 SVE2 重编（L0，命令+验收门见 §3-L0；svebf16 是错误拼写，用 bf16）
# 0.2 锁频（--ignite-cpu 在 ARM 无效，手动）：
for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo performance > $g; done

# 0.3 CP1 少核取证（96 vs 95，疑 BIOS deconfigure）：
lscpu -e=CPU,CORE,SOCKET,NODE | tee topology.txt
cat /sys/devices/system/cpu/isolated /sys/devices/system/cpu/offline
dmesg -T | grep -iE 'edac|mce|fault|deconfig|cpu.*err|lockup' | tee boot_cpu_err.log
# + BMC SEL 导出（ipmitool sel list）

# 0.4 EDAC 基线：
grep -H . /sys/devices/system/edac/mc/mc*/{ce,ue}_count 2>/dev/null

# 0.5 SMT 拓扑表（阶段 C 用）：
for c in $(seq 0 381); do
  echo "$c $(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null)"
done | tee smt-map.txt
```

### 阶段 A：全核背景压 + SDCShield 并行（CORE179 经验：全核并发才触发）

```bash
# 终端1：stress-ng 382 worker（191 cpu + 191 fma，全部 --verify）+ di/dt 阶跃 + 观测联动
stress-ng --cpu 191 --cpu-method matrixprod \
          --fma 191 --verify \
          --varyload 64 --varyload-ms 20 \
          --taskset all --interrupts -K --thermalstat 30 \
          --metrics-brief -Y A_full.yaml -t 2h

# 终端2：SDCShield 全核 golden 比对
./run-sdcshield.sh -T forever -t 2h -Y -F \
  -e 'eigen_svd*' -e 'eigen_gemm*' -e 'fma*' -e 'zstd*' -e 'zlib*'

# 压满验证（任一时刻抽查）：
mpstat -P ALL 1 | awk 'NR>3 && $3>1 {print $1,$3"%"}'
```

负载通路轮换（A 轮每 2h 一组，避免单一通路疲劳）：
```bash
# load/store 组（L2C/LSU/MMU 脆弱模块）
stress-ng --misaligned 96 --cacheline 96 --l1cache 96 --spinmem 48 \
          --stream 8 --memrate 32 --ptr-chase 32 --tlb-numa 32 \
          --verify --taskset all --metrics-brief -Y A_ls.yaml -t 2h
# 整数/原子/分支组
stress-ng --intmath 128 --bitops 64 --atomic 64 --branch 64 --regs 32 --opcode 32 \
          --verify --taskset all --metrics-brief -Y A_int.yaml -t 2h
```

### 阶段 B：CP1 定向 vs CP0 对照（注意：cdouble 是 cpu-method，不是 stressor）

```bash
# CP1（node1 = 192-381，95 物理核/190 线程）
stress-ng --cpu 95 --cpu-method cdouble --fma 95 --verify --vecfp 95 --vecwide 95 \
          --taskset 192-381 --mbind 1 --metrics-brief -Y B_CP1.yaml -t 60m
# CP0 对照（node0 = 0-191，96 物理核/192 线程），参数尽量对称
stress-ng --cpu 96 --cpu-method cdouble --fma 96 --verify --vecfp 96 --vecwide 96 \
          --taskset 0-191 --mbind 0 --metrics-brief -Y B_CP0.yaml -t 60m
# 等价写法：--taskset package1（框架读 package_cpus_list，已实测存在该关键字）

# 判读：B_CP1 出 verify fail / bogo ops 离群 而 B_CP0 干净 → 故障收敛到 CP1
```

### 阶段 C：逐物理核 sweep（锁定具体坏核）

```bash
for c in $(seq 192 381); do
  # 每物理核只取代表线程（sibling 对中较小号）
  first=$(awk -F'[,-]' '{print $1}' /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list)
  [ "$c" != "$first" ] && continue
  sib=$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list)
  echo "=== physical core (CPUs $sib) ==="
  # SMT 对内双线程对打 + 自检
  stress-ng --cpu 2 --cpu-method all --fma 2 --verify --vecfp 2 \
            --taskset $sib -t 10m --metrics-brief -Y C_core$c.yaml
  # 同核绑 SDCShield 单测（注意 sib 可能是 "192" 或 "192,193" 形式）
  taskset -c $sib ./run-sdcshield.sh -t 10m -e 'eigen_svd*' -e 'eigen_gemm*' -e 'fma*' -Y \
            2>&1 | tee C_core${c}_shield.log
done
# 汇总嫌疑核：
grep -l "data difference" C_core*.yaml
grep -lE "FAIL|fail" C_core*_shield.log
```

（L2.4 sweep 模式落地后，此循环内建为 `--sweep-cores --sweep-duration 10m -Y C.yaml`）

### 阶段 D：固定 seed 复现 + 取证（区分热致/固有）

```bash
# 疑似故障核（假设扫出 CPU 137 所在物理核）三连：
stress-ng --cpu 2 --cpu-method all --fma 2 --verify \
          --taskset 136,137 --seed 12345 --interrupts -K --thermalstat 30 \
          -Y repro.yaml -t 4h
# 健康核对照组（同参数）：
stress-ng --cpu 2 --cpu-method all --fma 2 --verify \
          --taskset 0,1 --seed 12345 --interrupts -K --thermalstat 30 \
          -Y control.yaml -t 4h
# 冷机（低温）复现一次：冷机仍 fail = 固有缺陷；仅热态 fail = 记录温度阈值
```

### 坏核判定（多证据交叉，≥2 条命中才确认）

| # | 证据 | 来源 |
|---|---|---|
| 1 | verify 报 "data difference between identical … computations" | stress-ng --verify（该 worker 当时绑定核） |
| 2 | 该核 bogo ops 显著低于同 socket 其他核（离群） | yaml --metrics-brief |
| 3 | memcmp 失败 cpu-mask | SDCShield |
| 4 | EDAC CE/UE、CPU deconfig/自检记录 | dmesg / BMC SEL / /sys/edac |
| 5 | 冷机复现 vs 仅热态触发 | 阶段 D 温度对照 |

**反例警示**（CORE179 教训）：单核独占从不触发 ≠ 该核没病——判定必须在全核并发背景下做；阶段 C 的 sweep 也要在阶段 A 的背景压同时进行（C 的循环里可另开终端保持 A 的背景负载）。

## 5. 编排脚本

建议方已产出 `sdc_sweep_kp950.sh`（bash -n 语法通过，未随本仓库入库）。本仓库落地路径（=L2.4 之前的过渡）：
- 短期：脚本入本仓库 `scripts/sdc-scan.sh`（我方 P1/G8），融合四阶段结构 + 阶段 C 的 SMT sibling 逻辑 + 嫌疑核汇总
- 中期：L2.4 `--sweep-cores` 落地后脚本退化为薄封装

脚本要点（无论哪个版本都必须有）：
1. 阶段 0 取证自动化（topology/isolated/offline/EDAC/dmesg 快照）
2. 跳过 isolated/offline 核
3. 每阶段独立输出目录 + 时间戳
4. 嫌疑核汇总（verify fail ∩ sdcshield fail ∩ bogo 离群）
5. 全程 `-K --interrupts --thermalstat` 联动观测

## 6. 本机实测记录（开发机 Kunpeng 920, 128 CPU；全部为真实命令输出）

| 命令 | 结果 |
|---|---|
| `objdump -d stress-ng \| grep -cE 'fmla\s+v'` | 182（全 NEON v 寄存器） |
| `objdump -d stress-ng \| grep -cE '\b(ld1d\|ptrue\|whilelo)\b'` | **0**（零 SVE 指令，L0 必要性实证） |
| `gcc -march=armv8.2-a+sve2+svebf16+i8mm` | **FAIL**（invalid feature modifier 'svebf16'）→ 修正为 `+bf16` |
| `gcc -march=armv8.6-a+sve2+bf16+i8mm` | OK |
| `gcc -march=armv8.2-a+sve2` | OK（降级链可用） |
| `--taskset core0 --cpu 1 -t 1` | successful run（coreN 关键字可用） |
| `--cdouble 1` | **unrecognised option**（证实 cdouble 是 cpu-method） |
| `--cpu 1 --cpu-method cdouble -t 2` | passed: 1（正确用法） |
| `--regs/--opcode/--ptr-chase/--spinmem/--misaligned/--vecmath 1 -t 1` | 全部 successful run（分组 stressor 存在） |
| `--taskset 0-3 --cpu 2 -t 2` | passed: 2（绑核可用） |
| `--smi 1 -t 1` | skipped with reason（诚实跳过机制正常） |
| `--eigen 1 -t 2` | skipped: "eigen C++ library, headers or g++ compiler not used"（HAVE_EIGEN 未配置） |
| `--cpu-method / --vm-method / --cacheline-method 列表` | 71 / 39 / 13 个方法 |
| `--cache 2 --cache-level 3 -t 2` / `--vm 2 --vm-method galpat-0 -t 2` / `--stream 2 -t 2` | 全部 failed: 0 |
| SDCShield runner | `../sdcshield/third-party/rpms/openEuler-24.03/..._SP3/built/run-sdcshield.sh` 存在（SP3 包，目标机 SP 不匹配需源码构建） |
| 本机 SMT/flags | `thread_siblings_list=0`（无 SMT）、无 sve/ls64/dit → L2.2/L2.5/L2.8 本机只能验证"诚实跳过"路径，真功能验证在目标机 |

## 7. 与第一版方案（G 系列）的映射

| v1 | v2 去向 |
|---|---|
| G1 SMT 捆绑 | 保留：阶段 C 的 sibling 对逻辑（L1）+ L2.2 `--taskset physical` |
| G2 跨 socket 乒乓 | 保留：L2.7 |
| G3 SVE2 stressor | 并入 L2.5（显式 SVE intrinsic）+ L0（重编） |
| G4 L3 set stride | 降级 backlog（L2.7 覆盖一致性主路径后重估） |
| G5 硬件 crc32 | 保留：L2.9 |
| G6 ls64 | 保留：L2.8 |
| G7 di/dt 配方 | 被 L1 的 `--varyload --varyload-ms --cpu-load-slice` 替代（零代码） |
| G8 扫描脚本 | 保留：§5，融合四阶段结构 |
| G9 EDAC 监视 | 被 L1 的 `--interrupts -K --thermalstat` 替代（零代码） |
