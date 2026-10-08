# Task Plan: sdc-stressng 顶级开源项目化改造 — arm64 SDC 激发引擎（第十四轮）

> 前序任务（13 轮：SDCShield 协同方案→移植→饱和→变异→CI→报告）已全部完成，
> 记录归档于 docs/superpowers/{plans,research}/ 与 git 历史。未闭环项见文末"遗留转入"。

## Goal

本项目（https://github.com/wangxumarshall/sdc-stressng，上游 https://github.com/ColinIanKing/stress-ng）
是 stress-ng 的 fork，13 轮开发已沉淀完整 SDC 激发能力。本轮**彻底改造为顶级完备的 GitHub 开源项目**：

1. **定位重塑**：从"fork + 补丁集"变为独立项目叙事——**arm64 SDC 激发引擎**。
   相对 ../sdcshield（校验器：273 golden 用例判 SDC、报故障核），本项目唯一核心价值 = **激发**：
   把所有计算资源消耗在最大程度把 CPU SDC 逼出来（di/dt、功耗、缓存/TLB/互联压力、
   SMT 争用、边界时序、SDC 定向数据形状）；检测/判定/定位全部交给 SDCShield。
2. **架构设计**：五层激发引擎架构（findings §12.3），现有散装能力收编进统一分层，
   交付激发覆盖率矩阵（杠杆 × 通路 × 数据形状）。
3. **GitHub 项目信息**：repo 元数据（当前还是上游原文！）、双语 README、治理文件、
   模板、上游残留清理（FUNDING.yml 指向上游作者收款账号等）。
4. **方案实现**：sdc-run `excite` 纯激发模式 + 激发默认值审计 + fork 版本标识。
5. **验证**：文档命令全量 parse-verified + 快速上手实测 + CI 15 镜像全绿 + 链接检查。

## 现状快照（2026-10-08 调研 facts，不要重新推导）

| 项 | 现状 |
|---|---|
| repo 元数据 | **description/homepage 均为上游原文**（description="This is the stress-ng upstream project..."、homepage→上游 repo）；topics 空；wiki 开启；fork:true（GitHub network 连上游） |
| fork 代码规模 | 110 commits（自 0fd4437b5 起），100 files，+11315/-293 |
| 上游基线 | 0.22.00（2026-08-19）；上游最新 V0.22.01（2026-09-20）+ 日常高频 commit；同步先例 PR #5（8 修复无冲突） |
| README | 中文 fork 定位（第十三轮版）+ 上游英文原文拼接；无 badge/架构图/英文版 |
| 治理文件 | 无 SECURITY/CONTRIBUTING/CHANGELOG/SUPPORT；无 issue/PR 模板/CODEOWNERS |
| 上游残留 | FUNDING.yml=Colin King 收款账号（github/patreon/ko_fi/liberapay）；.travis.yml（上游已弃）；ci-builds.yml（上游多平台手动构建）；container-image-edge.yml（on: push master + 每日 cron）；container-image-stable.yml（on: release published） |
| 发布 | 从未 GitHub Release；VERSION=0.22.00 无 fork 标识 |
| docs/ | 仅 superpowers/{research×12, plans×7} 内部研发记录；无用户向架构/方法论文档 |
| CI | multi-os-verify.yml 每日 15 镜像（最近全绿 run 35583732044）；ghcr verify-<tag> 手动发布流已就绪 |
| 网络/凭据 | api.github.com 直连可达（curl 200）；github.com:443 git 协议超时；PAT=~/.bashrc 的 GITHUB_PERSONAL_ACCESS_TOKEN（repo 元数据 PATCH 可用）；MCP GitHub=用户本人（wangxumarshall）；gh CLI 未装 |

## Phases

### Phase 1: 现状调研与差距分析 — complete（2026-10-08）
- [x] repo 元数据/文档/CI/发布/上游残留全量盘点（上表）
- [x] 顶级项目标准差距表 G-1..G-10（findings §12.1）
- [x] 网络+凭据通路验证（api.github.com + PAT）

### Phase 2: 架构与改造方案设计 — complete（2026-10-08，决策点待用户）
- [x] 五层激发引擎架构（findings §12.3）
- [x] "所有计算给激发"落地设计：excite vs full 双模式（findings §12.4-C）
- [x] patch 清单 A-E 组 + 执行顺序（findings §12.4）
- [x] 决策点呈报（findings §12.5 → AskUserQuestion）

### Phase 3: GitHub 门面（A 组）
- [ ] A1 repo 元数据：description/homepage/topics（+关 wiki）——curl PATCH api.github.com + PAT
- [ ] A2 README 重写（语言按 D14-1）：定位/badge/架构图/快速上手/能力矩阵/SDCShield 协作/上游致谢/安全警示
- [ ] A3 SECURITY.md + CONTRIBUTING.md + SUPPORT.md
- [ ] A4 CHANGELOG.md + 首个 GitHub Release（v0.22.00-sdc.1）
- [ ] A5 .github/ISSUE_TEMPLATE（bug/feature）+ PULL_REQUEST_TEMPLATE
- [ ] A6 上游残留清理：FUNDING.yml / .travis.yml / 3 个上游 workflow 处置（联动 D14-3）

### Phase 4: docs 用户文档（B 组）
- [ ] B1 docs/architecture.md（五层架构 mermaid + 各层职责 + 设计决策记录）
- [ ] B2 docs/excitation-guide.md（激发覆盖率矩阵：杠杆×通路×数据形状 + 方法论 + 文献引用）
- [ ] B3 docs/sdcshield-integration.md（协同作战手册：双工具拓扑/剧本/报告对接）
- [ ] B4 docs/upstream-sync.md（同步策略 + .gitattributes merge=ours 冲突收敛规范）

### Phase 5: 代码层（C 组，one-patch-per-unit）
- [ ] C1 sdc-run.sh `excite` 纯激发模式（无 --verify 组合，SDCShield 并行检测）
- [ ] C2 激发默认值审计（full/excite 组合配比 vs CI-MATRIX bogo 数据，结论进 excitation-guide）
- [ ] C3 fork 版本标识（--version → 0.22.00-sdc.1，Makefile 单点定义）

### Phase 6: CI 与发布（D 组）
- [ ] D1 multi-os-verify badge 接入 README
- [ ] D2 Release 流水线：tag → Release + ghcr stable 镜像（改造 container-image-stable.yml 或并入 multi-os-verify publish）

### Phase 7: 验证与收尾（E 组）
- [ ] E1 README/docs 全部命令 parse-verified（当前二进制实测，13 轮纪律延续）
- [ ] E2 快速上手 3 命令本机实测
- [ ] E3 push 后 ci-monitor.sh --new-code 确认 15 镜像全绿
- [ ] E4 badge/交叉链接检查；findings/progress 收尾；方案入库 docs/superpowers/plans/2026-10-08-*.md

## Errors Encountered

| Error | Attempt | Resolution |
|-------|---------|------------|
| git fetch upstream 443 超时 | 直连 github.com | 网络层限制；上游数据改走 api.github.com（release/commit 查询可用），方案不受阻 |
| sed 打码正则 Invalid range end | 查 bashrc | 改 cut -d= -f1 仅列变量名 |
| search_repositories 查本 repo 返回 0 | MCP | 改用 REST API curl 查询成功（搜索索引限制，不影响读写） |

## Decisions Made

| # | Decision | Why |
|---|----------|-----|
| D14-1 | README **纯英文**（用户选定，非双语） | 国际可见性；单一语言降低维护成本 |
| D14-2 | **excite + full 双模式**（用户采纳推荐） | excite=纯激发（无 verify，检测 100% 归 SDCShield）；full=保留 verify 哨兵 |
| D14-3 | **定期 merge 上游（每 release，PR 方式）+ 删除上游 workflow/FUNDING/.travis**（用户采纳推荐） | 上游修复持续可用；B4 规范控制冲突成本；ghcr 发布由 multi-os-verify 承担 |
| D14-4 | **全部 A-E 本轮执行**（用户采纳推荐） | "彻底改造"语义；每单元独立 commit |
| D14-5 | 版本呈现 `0.22.00-sdc.1`（上游版本+fork 后缀，Makefile 单点）；项目名 sdc-stressng、二进制名 stress-ng 不改 | 兼容上游 merge 与既有 CI/文档；改名收益低、破坏面大 |
| D14-6 | Release 打 tag 时机推迟到 E3（CI 15 镜像全绿）之后，A4 阶段只写 CHANGELOG 文件 | tag 应指向最终验证过的状态 |

## 遗留转入（前 13 轮未闭环，不因本轮改造丢失）

- ci-trend.sh 一周稳定性对比（需 gh 认证环境或 workflow_run 回传方案）
- CP1 真机 A/B 长跑窗口（P2 abtest 已就绪，等真机时间）
- bandwalk 窗口真机校准（sdc-flip-collect.sh 待真机失配数据）
- （本轮开始时被打断的）上游 0.22.01+ 合并任务 → 并入 D14-3 决策统一处理
