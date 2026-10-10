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

### Phase 3: GitHub 门面（A 组） — complete
- [x] A1 repo 元数据：description（SDC excitation engine…）+ homepage 置空 + 12 topics + 关 wiki —— PATCH/PUT 200 均生效（回读验证）
- [x] A2 README 重写（7a8920737 + excite 增补 2de8f41d5）：badge/问题陈述/五层架构图/杠杆表/攻击面表/快速上手/编排器/CI/构建/文档索引/上游关系/安全警示
- [x] A3 SECURITY.md + CONTRIBUTING.md + SUPPORT.md（b984d8f62）
- [x] A4 CHANGELOG.md（eb127c60a）；**GitHub Release 待 E3 CI 全绿后打 tag**（D14-6）
- [x] A5 issue forms（bug/feature/config）+ PR 模板（28bf16304，YAML 校验过）
- [x] A6 上游残留清理（158ab37c2）：FUNDING/.travis/ci-builds/container-image-{edge,stable} 五文件删除，引用检查零残留

### Phase 4: docs 用户文档（B 组） — complete
- [x] B1 docs/architecture.md（a30695743：五层 ASCII 图 + 各层职责 + bitgen 纪律 + 数据流图 + AD-1..AD-7 设计决策）
- [x] B2 docs/excitation-guide.md（a30695743 + 12197871c：三因素模型/杠杆表/**覆盖率矩阵**（●◐○）+ gap 路线图 + 模式配方审计 + campaign 组合规则）
- [x] B3 docs/sdcshield-integration.md（00a7323a8：拓扑图/阶段 0 取证/四模式协同命令/报告交叉判读表）
- [x] B4 docs/upstream-sync.md + .gitattributes（00a7323a8：每 release PR 合并节奏/文件分类冲突规则/merge=ours driver 配置/merge 后 6 步验证）

### Phase 5: 代码层（C 组） — complete
- [x] C1 sdc-run.sh `excite` 纯激发模式（2de8f41d5）：cpu all+fma+armcrypto+operand-var+addrspace+memrate-bandwalk+vm-rand-offset+varyload，无 verify 哨兵；顺带修 usage() 截断 bug（sed 2,60p → 2,/^$/p）；本机 10s 实测 rc=0/failed:0/产物齐
- [x] C2 模式配方审计（12197871c）：excite vs full 配比决策记录进 excitation-guide（full 不加 crypto/memrate 的理由 = verify 预算竞争）
- [x] C3 fork 版本标识（67050702d）：Makefile VERSION=0.22.00-sdc.1 单点；重编后 --version 实测输出 0.22.00-sdc.1

### Phase 6: CI 与发布（D 组） — complete
- [x] D1 badge 接入 README（CI/Release/License/Arch 四 badge，随 A2 完成）
- [x] D2 release-image.yml（7ea0daedd）：on release published → 从 tag 构建 → ghcr :stable+:<tag>，arm64 only；YAML 校验过；E2E 验证随 Release 触发

### Phase 7: 验证与收尾（E 组）
- [x] E1 README/docs 全部 34 个选项/命令实跑核查：30 项 --help grep 直接过；5 项（sve2/ls64/memrate-write-pattern/bitgen×2）grep 假阴性、实跑全通过（sve2/ls64 本机诚实跳过 + successful run）——以实跑为准
- [x] E2 快速上手实测：operand-var 4 --verify 10s（failed:0 skipped:0）；addrspace 2 --verify（failed:0）；excite 10s（rc=0，A_excite.{log,yaml}+topology.txt 产物齐）
- [x] E3 CI 15 镜像全绿确认：**multi-os-verify run 37723638200（head=7ea0daedd）completed+success**（03:37Z dispatch）；tag v0.22.00-sdc.1 已推、GitHub Release 406447318 已发布（201）；release-image run 37732286596 由 release 事件触发（in_progress，ghcr :stable 构建中——结论由后续 cron 检查收尾）
- [x] E4 链接检查：docs/{architecture,excitation-guide,sdcshield-integration,upstream-sync}.md、SECURITY/CONTRIBUTING/SUPPORT/CHANGELOG/COPYING/CLAUDE.md、stress-ng.1、docs/superpowers/ 全部存在；badge URL 格式正确
- [x] findings/progress 收尾 + 方案入库 docs/superpowers/plans/2026-10-08-project-overhaul.md

## Errors Encountered

| Error | Attempt | Resolution |
|-------|---------|------------|
| git fetch upstream 443 超时 | 直连 github.com | 网络层限制；上游数据改走 api.github.com（release/commit 查询可用），方案不受阻 |
| sed 打码正则 Invalid range end | 查 bashrc | 改 cut -d= -f1 仅列变量名 |
| search_repositories 查本 repo 返回 0 | MCP | 改用 REST API curl 查询成功（搜索索引限制，不影响读写） |
| 第一个 commit 误吞已暂存的 A6 删除 | A6+规划文件分离提交 | git reset HEAD~1 拆成两个干净 commit |
| --help grep 报 5 选项 MISSING | E1 选项核查 | 实跑全部通过（sve2/ls64 诚实跳过 + successful run）——--help 文本格式致 grep 假阴性，选项核查以实跑为准 |

## Decisions Made

| # | Decision | Why |
|---|----------|-----|
| D14-1 | README **纯英文**（用户选定，非双语） | 国际可见性；单一语言降低维护成本 |
| D14-2 | **excite + full 双模式**（用户采纳推荐） | excite=纯激发（无 verify，检测 100% 归 SDCShield）；full=保留 verify 哨兵 |
| D14-3 | **定期 merge 上游（每 release，PR 方式）+ 删除上游 workflow/FUNDING/.travis**（用户采纳推荐） | 上游修复持续可用；B4 规范控制冲突成本；ghcr 发布由 multi-os-verify 承担 |
| D14-4 | **全部 A-E 本轮执行**（用户采纳推荐） | "彻底改造"语义；每单元独立 commit |
| D14-5 | 版本呈现 `0.22.00-sdc.1`（上游版本+fork 后缀，Makefile 单点）；项目名 sdc-stressng、二进制名 stress-ng 不改 | 兼容上游 merge 与既有 CI/文档；改名收益低、破坏面大 |
| D14-6 | Release 打 tag 时机推迟到 E3（CI 15 镜像全绿）之后，A4 阶段只写 CHANGELOG 文件 | tag 应指向最终验证过的状态 |

## Next Step

**Phase 12 并行推进中（用户指令"并行加速，多agent"）**：5 agent 后台并行 + CI 复验收尾。
完成后由主会话合并（注册链/man 冲突处理）→ 统一强制 SVE2 构建预检 → 逐个 commit 进 main。

### Phase 12: backlog 并行推进（5 agent，2026-10-10 dispatch）— in_progress
| Agent | 任务 | 隔离 | 冲突面 |
|---|---|---|---|
| QEMU | ✅ **完成**：/tmp/qemu-out/qemu-aarch64 8.2.0（无需 LD_LIBRARY_PATH，RPATH 内嵌）；-cpu max 暴露 sve2 全家桶+sm3/sm4/sha3/rng；三条 SVE 验证全 passed failed:0（sve2 verify/lsupress load-sve/memcpy sve-gather）。关键备忘：无 SVE 硬件机上 --sve2 stressor 必须 MARCH_AARCH64_SVE2=1 构建才非跳过桩（z 指令 1146→5824）；断点续传+dnf download 提取依赖重建流程见 /tmp/qemu-out/README | /tmp | 无（只构建 stress-ng 做验证） |
| llccross | ✅ **完成**（worktree 分支 commit c364b68d9，6 文件 +1117 行）：`--llccross` 3 方法（pingpong 共享行乒乓 turn 制+每拍 tag 哨兵 / remote-write 双向整行流 / remote-stream 混合扫描）；pair 进程跨域绑定（NUMA node→L3 实例→单域诚实降级三级推导）；first-touch 放置零 mbind；**bring-up 抓出 2 个真 bug**：arm64 读提升越过自旋交接需 mfence+dmb sy 三处配对、工作行须取 min(L1D,LLC) 一致性粒度（920/950 L3=128B 但传输=64B）；故障注入 4/4 命中；实测 39789 乒乓拍/s + 3477MB/s 远端扫描；worktree 沙箱 Mems_allowed=node0（跨域页面放置待 CP1 验证） | worktree | 注册链四文件+man（合并时处理） |
| lrcpc | ✅ **完成**（worktree 分支 commit f58e6a0ef，+97/-1）：lrcpc-pair（LDAPR+STLR armv8.3-a）+ ilrcpc-rmw（LDAPUR/STLUR 立即数偏移 RMW 链 armv8.4-a，无需 .inst）；**修正任务假设：lrcpc/ilrcpc 在 AT_HWCAP 不在 HWCAP2**（HWCAP_LRCPC=bit15/ILRCPC=bit26，走现有 hwcap_req 机制+#ifndef fallback）；本机诚实 skip + QEMU 真跑 passed:2 + 强制 SVE2 构建 0 mismatch + objdump 确认 ldapr/stlr/ldapur/stlur | worktree | stress-lsupress.c+man |
| pair | ✅ **完成**（worktree 分支 commits 1a2ee1e2a+3e8cbacbf）：8 组合（vm/addrspace/atomic/lsupress ×同）+ **抓出历史真 bug：pair 兄弟线程放置从未生效**（stress-ng --taskset 进程级+命令行最后生效 → 双 worker 同落一个 CPU，"SMT 争用矩阵"实际测的是分时单超线程，所有组合恒 ~0.5 的历史数据全部无效！）修复=每侧独立 stress-ng 进程各自 taskset；lsupress 组合用 mix-2l-alu-1s 固定方法（"all"随机抽签在无 SVE 机会性跳过）；E2E 24 行矩阵 48/48 successful；CP1 全扫时长提示（~12.7h，建议 -c 抽样） | worktree | sdc-run.sh+README/CLAUDE.md |
| ooo | ✅ **完成**（worktree 分支 commits 7a4dcaaca+1efbaf372）：`--ooopress` 6 方法（dep-chain 256 条 RAW 链/indep-max/alt 波形/rename-reuse/branch-mix（bitgen 方向+udiv 臂防 if-conversion——objdump 抓出初版被 GCC 静默 csel 化）/load-use 仿置换队头阻塞）；42 倍 bogo 形态差实测；纯可移植 C 零 target 属性；强制 SVE2 构建 rc=0；man+excitation-guide 矩阵记分+CHANGELOG 已含 | worktree | 注册链四文件+man |

同时后台：CI 复验 run（c973cad25，17:07 cron 收尾）。外部 spec 编辑已 stash（不丢）。

### Phase 12b: 合并与收尾（agent 返回后主会话执行） — complete
- [x] 4 worktree 分支合并进 main（llccross→ooopress→lrcpc→pair；唯一冲突=CLAUDE.md backlog 行，合并语义解决）；QEMU agent 无源码改动
- [x] 合并后修复：lsupress 22 内核 unused-args 警告（c973cad25 引入、当时验收 grep 只查错误漏掉——教训：验收 grep 必须同时查警告）→ 签名收敛 3 参（581e2f361）
- [x] 统一验证：normal build rc=0 零 fork 文件警告 / MARCH_AARCH64_SVE2=1 rc=0 零 mismatch / llccross(pingpong+verify)/ooopress(dep-chain)/lrcpc(诚实skip)/store-int64 verify 全冒烟绿 / pair E2E rc=0 全 8 组合有速率（首跑一次 lsupress SIGSEGV 瞬态，同核双进程×5 复跑零复现，与 T5 mix-2l 瞬态同型）
- [x] 全部推送 581e2f361；CI re-dispatch 204（15 镜像验证合并树——新 stressor 自动进 sequential+method sweep）
- [x] 5 worktree + 分支清理；spec 外部编辑 stash 恢复
- 遗留观察项（CI 值守）：pair 瞬态 SIGSEGV 若在 CI 复现需深挖；llccross 跨域页面放置待 CP1；ooopress 编排接入（excite 站点+pair 组合）下轮

### Phase 11: LSU 全指令谱 SDC 激发引擎（2026-10-09，executing-plans native 模式） — 11/11 code complete, CI 复验中

### Phase 11: LSU 全指令谱 SDC 激发引擎（2026-10-09，executing-plans native 模式） — in_progress
**Ledger**（plan: docs/superpowers/plans/2026-10-09-lsu-instruction-spectrum.md，11 tasks；commit 即进度）：
- [x] T1 memcpy NEON 族（4452881d7）：ldp-stp/neon/neon-ld2；真 bug=asm "+r" post-inc 与 dest 参数寄存器 coalesce 毁返回值（改只读 "r"+C 级推进）；**教训：验收 grep 必含 fail: 行**（"failed:0" 假绿）
- [x] T2 memcpy sve/sve-gather/ls64（722d5673c）：真 bug×2=SVE sizeless 禁指针算术（普通指针推进+svld1_u64(addr)）；skip 块插在 func 赋值前（反汇编定位 stress_setting_get 顺序，移到 func 解析后）；gather=Ruling"契约+叠加 gather pass"；SVE 真硬件验证走 CI 22.03+（QEMU 环境已丢，重建记 backlog）；ld64b=0 属编译门控预期
- [x] T3 lsupress 骨架（本 commit）：方法表 4 项 + load/store/copy-int64；真 bug=**漏 include core-mmap.h → stress_mmap_populate 隐式 int 声明 → buf 符号扩展污染 SIGSEGV**（strace syscall 合法 vs C 层污染定位）；API 对齐=classifier/const/bitgen_seed/stress_setting_get；OPS 枚举为 STRESSOR_ELEM 强制；方法表经 core-opts.c/h+core-stressors.h MACRO 接线
- [x] T4 地址引擎（本 commit）：NORESERVE 100G 试探收缩 + xorshift64* 窗口迁移 + MADV_DONTNEED；**合同修正（Ruling）**：方法内核改单遍处理 buf_words 即返回（原 do-while 自循环导致 func 永不返回、迁移仅 1 次）——循环/迁移责任归主函数 do-while；实测 madvise **5950 次/20s**（~300 迁移/s 疯狂游走）；RSS 稳态 9MB<<64MB 窗口（DONTNEED 回收与写入平衡，硬顶 PASS）；load 方法零页共享（RSS~0、TLB 压力保留）
- [x] T5 mix 模板族（55be42322）：mix-2l-alu-1s 手写 asm 恰好 ldr,ldr,add,ldr,add,str
- [x] T6 向量族（92dc919c5）：11 方法（int128/fp64/neon load-store/zva/fma + SVE 5 项 HWCAP 门）；真 bug=include 块提前关主门控
- [x] T7 原子族（cba67e47e）：excl-pair（ldxr/stxr）/lse-rmw（armv8.1 target 属性）/ls64-copy（编译+运行双门）；asm 操作数号错+ldadd 需 target 属性两个编译教训
- [x] T8 游走扩展（163ba0e40）：uniform/bitgen（TLB 位段）/va-bit/near-far + hugepage 回退；align 选项 YAGNI 降级 backlog
- [x] T9 verify+注入（e8e43e24d）：store-int64=per-address 哈希、--verify 页采样+位级诊断；**故障注入位 45 被精确抓**；Ruling=精确校验仅限 store-int64（copy/zva/mix 无 f 不变式）；教训=注入还原用 git checkout 洗掉未提交工作（改用 stash-verify-drop）
- [x] T10 excite 2.0（8074f39a0）：站点轮换制（每站全机深压+varyload 常驻）；125s E2E 三站轮换验证
- [x] T11 文档/CI（260b0a89e）：excitation-guide 指令谱行+gap 更新、CLAUDE.md 能力地图、CHANGELOG；**CI dispatch 204**（15 镜像验证进行中——lsupress+memcpy 变体自动进 method sweep）

### 11-task 计划完成：11/11。CI 15 镜像结果=最终验收门（cron 检查）

### Phase 11 前置（brainstorming/spec/plan，均 complete）

### Phase 10: Release 自包含包（2026-10-09 用户需求：含依赖库 + 一键式激发脚本） — complete
**设计决策 D10-1**：不用 STATIC=1（会打破"发布=CI 全量测试的同一二进制"不变量，且静态库缺失使
judy/mpfr/xxhash 类 stressor 跳过、覆盖缩水）；用**动态二进制 + 捆绑全部非 glibc 依赖
（ldd 递归 + rpm -qf 过滤 glibc 包）+ excite.sh 设置 LD_LIBRARY_PATH**。glibc 不捆绑
（坑多），由"按目标机 OS 选对应镜像 tar"覆盖（老 glibc 构建向上兼容），README 声明。
tar 结构：stress-ng + excite.sh + scripts/{sdc-run,sdc-report,sdc-scan}.sh + lib/*.so + README.md
- [x] task_plan 记录（本条）
- [x] 取消旧格式 release runs（cancel 409=已完成/失效；release 已 DELETE 204——旧 run 上传目标不存在，自然失效）
- [x] scripts/excite.sh：警示 banner + lib/ 自动 LD_LIBRARY_PATH + 默认 120min + `--` 透传 + `--preheat 0` 可覆盖；repo/tarball 双布局自适应
- [x] packaging/README-RELEASE.md（快速上手/安全/glibc 兼容性/SDCShield 协同/包内容表）
- [x] workflow Publish binary 步骤改造：自包含包打包（ldd+rpm -qf 捆绑非 glibc so + objdump GLIBC_MIN 追加进 README）
- [x] 本地验证：捆绑 dry-run 正确（libatomic/libcrypt/libgmp/libz 捆、glibc 跳过）；全流程打包（tar 1.6MB，结构 10 文件，glibc≥2.38 标注）；**解包目录端到端实跑 `./excite.sh 1 -- --preheat 0` rc=0**（库解析/参数透传/拓扑推导全通）
- [x] 文档同步（CHANGELOG/CLAUDE.md/README 资产描述 + CLAUDE.md 记录"tag 须含最新 workflow"要点）
- [x] commit + push + **tag -f v0.22.01-sdc.1** + 重发 release
- [x] **Phase 10 闭环**：第一次发布 attempt 15 job 全死（assets_url→404→curl exit 22）；修复 upload_url+剥模板（645469811）后重发——**multi-os-verify 37892218655 + release-image 37892218673 双 success，Publish 步骤 15/15 success，30 assets 到齐**（15×sdc-stressng-0.22.01-sdc.1.openeuler-<tag>.aarch64.tar.gz ~1.8MB + 15×sha256）

### Phase 9: Release 携带 15 镜像二进制（2026-10-08 用户需求） — superseded by Phase 10
- [x] multi-os-verify.yml：on 加 release(published)；build-test 加 job 级 contents:write；"Publish binary" 步骤（tar.gz=stress-ng+sdc-run/sdc-report/sdc-scan 脚本 + .sha256，curl 上传 assets_url）插在 pre-sequential 窗口（OCI 约束）；并发安全（每 job 唯一文件名）（commit 21022d6f9）
- [x] 文档同步：CLAUDE.md 发布流程 / CHANGELOG / README CI 节
- [x] tag v0.22.01-sdc.1（21022d6f9）+ Release 发布（201，notes 含"released binaries are tested binaries"说明）
- [x] 双 workflow 触发确认：multi-os-verify 37873644976 + release-image 37873644971 均 in_progress
- [ ] cron 收尾：CI 全绿 + Release 30 个 assets（15 tar.gz + 15 sha256）到齐确认

### Phase 8: 上游同步 #1 — V0.22.01（2026-10-08，D14-3 策略首次执行） — complete
- [x] upstream remote 改 SSH（https 443 不通、SSH 通——origin push 一直走 SSH）；fetch V0.22.01，落后仅 4 commits
- [x] merge 到 sync/upstream-0.22.01：唯一冲突 = Makefile VERSION（按设计单行）→ 0.22.01-sdc.1；README 被 merge=ours driver 自动保住（**冲突收敛规范首次实战验证**）
- [x] 上游情报：b42133a70 宣布上游迁移到 stress-ng/stress-ng org（upstream remote URL 后续需跟迁）
- [x] 验证：make clean 全量 rc=0（--version=0.22.01-sdc.1）；bitgen 消费者回归 4 项（operand-var/addrspace/vm-rand-offset/memrate-bandwalk --verify）全 failed:0；**故障注入 drill**：fma double_a2[0] 注入位 45 → verify 精确抓到（element 0/expected/actual/1-bit/xor 0x2000000000000000 完整诊断）→ 还原后干净跑 + git diff 零残留
- [x] CHANGELOG [0.22.01-sdc.1] 条目；gcc 7.3 与全量 CI 由 20.03 镜像在 dispatch 中兜底
- [x] **sync #1 闭环**：PR #6 + CI run 37743339212（sync 分支 15/15 绿）→ merge → main = 0.22.01-sdc.1（53eb6a5ab）

## 遗留转入（前 13 轮未闭环，不因本轮改造丢失）

- ci-trend.sh 一周稳定性对比（需 gh 认证环境或 workflow_run 回传方案）
- CP1 真机 A/B 长跑窗口（P2 abtest 已就绪，等真机时间）
- bandwalk 窗口真机校准（sdc-flip-collect.sh 待真机失配数据）
- （本轮开始时被打断的）上游 0.22.01+ 合并任务 → 并入 D14-3 决策统一处理
