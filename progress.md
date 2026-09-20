# Progress Log

## Session 2026-09-20（第十三轮·续2）: CI 监控启动 + 抓住并修复 schedule 超时 bug

用户指令"你把这些监控起来"。**监控第一枪即抓住真 bug**：

### 监控发现（ci-monitor.sh 第一次调用）
- 今日 cron run 35501026920（head=c7e7ebf55 旧代码，08:58Z 启动）`--sequential` 步骤 16/16 镜像失败
- 昨日 cron run 35432037923 同样 16/16 失败——**连续两天每日 cron 全红**
- 而同 head 的手工 dispatch run 35449398396 全绿

### 根因（铁证）
- schedule 事件不带 workflow_dispatch inputs → `SEQ_TIMEOUT: ${{ inputs.sequential_timeout || '5' }}` 兜底为 **5**，dispatch 默认是 **2**
- 实测对照：`--timeout 2` 时 sequential 11.1 分钟完成；`--timeout 5` 时跑满 **90.0 分钟**被步骤超时保护杀掉（同一镜像 20.03-lts、同代码）
- 套件时长 ×2.5 后在共享 runner 池上撞 90m 保护窗

### 修复
- commit 6baae011d：兜底值 `'5'`→`'2'`（对齐 dispatch 默认与实测预算），YAML 校验过，已推送
- 同 commit 入库 `scripts/ci-monitor.sh`（匿名 API 无需 gh/token）：latest 总览 / `--watch` per-job 结论+失败步骤定位 / `--new-code` 找 push 后首个 run

### 监控任务清单（进行中）
1. 今日 run 35501026920 收尾状态（旧代码，已知会红——失败全归因 SEQ_TIMEOUT bug，不代表代码问题）
2. **明日 12:00（北京时间）cron**：首个含 P3-P6 新代码（d48246ebc..6baae011d）的 15 镜像 run——SEQ_TIMEOUT 修复后应全绿；rand-payload 方法会进 method sweep 自动覆盖
3. 一周后：ci-trend.sh 稳定性对比（需在可认证环境运行）

## Session 2026-09-20（第十三轮·续）: P6 实施 — cache 系列 rand-payload

用户"继续"授权 P6（三待办中唯一本机可执行项）。**全部完成，3 commits 推送 ee0ba8b04..5464927a6**：

| Commit | 内容 | 验证 |
|---|---|---|
| 48f53488a cacheline rand-payload | 每进程独占对齐字（idx×8 错峰无重叠），低 8 位 tag + 高 56 位 bitgen 载荷，写→barrier→邻居扫→读回 | 注入位 45 每次往返被抓 + P3 yaml 64 计数；all/rdwr 回归绿 |
| fc94a7a46 l1cache rand-payload | set/way 几何不变，bitgen 流填充 + per-set 种子重放 verify | 注入位 46 逐字 1-bit 诊断 + yaml 32768；forward/random 回归绿 |
| 5464927a6 cache 写路径 | 线性坡 j&0xff → bitgen 流（u64 摊 8 字节），262 flag 组合共享 | A/B 带宽 5.24/5.31 vs 5.21/5.35 零回归 |

**过程中抓出 2 个真 bug**（干净运行门卫 + grep 惯例纪律的又一次兑现）：
1. l1cache `_and_verify` 初稿假设非 verify 变体先跑过 fill——实际框架每 run 只选一个变体，verify 对着零页全失配。修正为变体内自带 fill+replay。
2. `shim_memcpy` 在本构建配置非符号 → 普通 memcpy（addrspace.c 同款用法）。

**骨架先行教训兑现**：本轮三处改动均先定数据布局再动笔，零废弃代码（对比 addrspace 第一稿 300 行）。

**环境备注**：gcc 7.3 容器本轮不可用（无容器 runtime）——C99 语法零警告 + 明日 CI 15 镜像 20.03 自动兜底。

## Session 2026-09-20（第十三轮）: README/CLAUDE.md arm64 重定位 + 第十二轮复盘改进方案

用户需求（两项）：① 修改 README 和 CLAUDE.md，核心面向 arm64 服务器芯片压测；② 研究第十二轮复盘制定改进方案。

### 过程
- 恢复项目状态（12 轮历史在案，main=c7e7ebf55）；Phase 10 注册；Phase 9 CI 项闭环（run 35449398396 全绿）
- 本机构建 rc=0，方法列表实测（operand-var 5 / addrspace 7 / memrate-pattern 4 / armcrypto 13 / sve2 5；operand-var/addrspace verify 冒烟过）
- subagent ×2 并行研究复盘遗留项代码现状（findings.md §11）：cache 三件套字节粒度 0 自由位、bitgen 参数硬编码、A/B 分界 commit 可构建、CI 缺时序工具
- 修正复盘两处记忆偏差：cacheline-method 实际 11 个（非 13）；bitgen-distribution.sh 是统计验证器非采集器

### 交付物（三项全部完成）
1. **README.md 重写**：arm64 SDC 压测定位置顶（分工表/fork 能力三张表/快速上手/CI），上游通用内容保留后半部
2. **CLAUDE.md 新建**：分工模型/目标机事实/构建/能力地图/验证纪律 6 步/**代码纪律 10 条（12 轮踩坑沉淀）**/SDC 方法论 7 条/遗留
3. **改进方案** docs/superpowers/plans/2026-09-20-sdc-field-validation-and-calibration.md：
   - 主线 = 复盘遗留4（SDC 实战检验）：P3 yaml verify-failures → P1 失配统计报告 → P2 abtest 模式（A=fc243c784/B=当前，失配率并排+判读规则）
   - 配套：P4 bandwalk 校准链、P5 ci-trend.sh（遗留5 工具化）
   - 立项不实施：P6 cache 字粒度 tag 重设计（opt-in 新方法路线）、P7 pagemap 脚本
   - 实施顺序 P3→P1→P2→P5→P4；3 个决策点待用户批准

### 执行阶段（Phase 10b，用户批准"撰写完整方案并执行"）— 全部完成，4 commits 推送 c7e7ebf55..2751f8e01

| Patch | Commit | 验证证据 |
|---|---|---|
| P3 yaml verify-failures | d48246ebc | 故障注入：yaml `verify-failures: 6` 与 log 6 条失配**精确一致**；operand-var/fma/addrspace/cpu-method-all 干净路径 rc=0 且 yaml 无字段（向后兼容字节级）；注入还原后 git diff 零差异。实现：sigalarmed 同款 per-child 指针 + __atomic_add_fetch（HAVE 门控） |
| P1 sdc-report.sh | b629c8d23 | 本机 15s full E2E（全零报告）+ 合成数据（per-stressor 6+2=8 精确、首失配 3/6=50%、preheat 标记、sdcshield 失配率 27.3%+cpu-mask）；full 模式 --metrics-brief→--metrics 修正 |
| P2 abtest 模式 | 58af7b130 | 端到端：A=fc243c784 worktree 构建 vs B=当前，45s×2+30s 冷却；B 臂 5 stressor 齐（operand-var bogo 4.7 亿），A 臂按预期退化；ab_summary 三分支判读输出；健康机 A=B=0 |
| P5 ci-trend.sh | 58af7b130 | 聚合管道离线单测：时序按 image 分组正确、CoV 按镜像分离（fma@24.03=0.2% vs fma@20.03 独立）；修了跨镜像混合 bug（44.4% 假高）；gh 采集段待 CI 主机首跑 |
| P4 bitgen 校准链 | 2751f8e01 | bitgen-distribution 第 7 项 CALIBRATION（窗口 8..12 掩码 28 均 0 违例/10 万种子）→ 8/8 全 PASS；`--bitgen-band-width 812 --bitgen-band-density 28` verify 通过；消费者回归 operand-var/addrspace/vm-rand-offset 全绿；man 条目（groff 环境警告与本条目无关） |

**执行中排掉的坑**：
- P3 故障注入第一处改错位置（diff 只改诊断文本不影响比较）→ 换比较路径注入成功；期间一次 brace 破坏当场修复
- yaml metrics 段需 --metrics 旗标——发现 full 模式一直用 --metrics-brief 导致 P3 字段根本不输出，顺手修正（这个 bug 若带上线 P1/P2 全部失明）
- P5 单测抓住 CoV 跨镜像混合 bug（20.03 的 16 万与 24.03 的 174 万混算出 44.4% 假变异）——测试通过 ≠ 测的是你以为的东西的又一实例
- bitgen probe 独立链接依赖滚雪球 → 放弃独立二进制，并入 bitgen-distribution.sh 既有 stubs 机制（仓库惯例优先）
- opts 表范围 1..64 拒绝打包值 812 → 上限改 6464

**遗留（P6/P7 立项未实施 + 真机窗口）**：cache 字粒度 tag 重设计下轮独立立项；pagemap 脚本待目标机；CP1 真机 A/B 长跑（2h×2 版本×3 轮）待用户安排——P2 的 abtest 已就绪，一条命令即可执行。



用户需求：multi-os-verify 呈现所有用例的执行结果（pass 或其他 [bogo-ops/s]）。

### 实现架构（commit abd005f56..eb6e37d7c，run 22 全绿验证）
1. **发射端**：`ci-verify-log.sh` 增加第 4 参数（image tag）→ fork-free 解析 passed/skipped/failed 列表 + metrc 表，每个 stressor 发射一行 `CI-MATRIX <tag> <stressor> <PASS|SKIP|FAIL> <bogo-ops/s>` 进 **job log**——唯一保证存活到最后的通道（sequential 之后容器报废，artifact 上传会死，job log 由 runner 在容器外保存）
2. **汇总端**：新 `results-summary` job 用 gh api 抓 15 份 build-test job log → 严格形状 grep（防 set -x 回显的注释文本）→ Python 渲染：
   - 每镜像 pass/skip/fail 汇总表
   - **393 stressor × 15 镜像完整矩阵**（PASS 显示 bogo-ops/s，SKIP/FAIL 显示状态，· 表示未报告）
   - 写入 Job Summary（截 1MB）+ 完整 artifact（90 天）

### 排掉的三个坑（run 18-21）
| Run | 坑 | 修复 |
|---|---|---|
| 18 | **PR #4 合并冲突**：port 分支的 armv8.6-a 改动与主线 GCC10 guard 合并丢失 `#if defined(HAVE_ARMCRYPTO_SVE2)`（10 if vs 11 endif）→ 15 镜像编译全炸 | aabda8a42 恢复 guard（gcc12 + gcc7.3 双验证） |
| 20 | manifest-check 匿名查询撞 Docker Hub 限流（sp1 tag 假报 missing） | 8c9b70faa 每查询重试 5 次 |
| 21 | `grep -ao 'CI-MATRIX.*'` 误抓 set -x 回显的注释（"CI-MATRIX line per stressor" 4 字段）→ 渲染 IndexError | eb6e37d7c 严格形状匹配（tag+status+rate 完整结构）双向防护 |

### 首份完整矩阵的关键数据（run 22, 35428338420）
| 维度 | 数据 |
|---|---|
| 全量结果 | 15 镜像全绿：20.03=323/70/0、22.03=328-329/64-65/0、24.03=328-331/62-65/0（passed+skipped=393 恒定） |
| fma 编译器代差 | 20.03 (gcc7.3) ~61万 ops/s → 22.03+ (gcc10.3+) ~174万（2.8x） |
| armcrypto 方法数差 | 20.03 ~5.3万（gcc7.3 只编 4 方法）→ 22.03+ ~17万（13 方法含 SVE2） |
| sve2 stressor | 20.03 全系 SKIP（无 SVE2 编译支持，诚实跳过）→ 22.03+/24.03 全部运行 ~1,300 ops/s |
| ls64/rdrand | 15 镜像全 SKIP（runner ARM 核无 ls64/RNDR 硬件，诚实跳过） |
| 同 SP 版本内 | 波动普遍 <3%（如 fma 174-175万），测量一致性好 |



用户指令：继续、并行搞、用 subagent。三路并行（快跑 publish + subagent 盯 sp4 + subagent 基准分析）。

### 本轮成果
1. **首次 ghcr 发布**（run 35372724010，fast 模式 33/33 全绿）：15/15 镜像上线 `ghcr.io/wangxumarshall/sdc-stressng:verify-<tag>`；22.03-sp1 首推遇 ghcr blob 竞态（unknown blob，15 job 并发推同一基础层），rerun --failed 一次成功；registry API 验证 3 代表 tag manifest 200
2. **Subagent B 深度分析发现 3 个工作流 bug**（docs/superpowers/research/2026-09-18-ci-benchmark-analysis.md）：
   - `with: path:` 里 shell 式 `${TAG_SAFE}` 不展开 → bench yaml/methods log/seq log 全部从未上传（对比表空）
   - **UBSan 构建是 sp4 "慢 runner" 的真根因**：sanitizer 仪器化让 sequential 挂 5h42m 至超时（非 runner 慢）
   - BUILD_MODE echo 顺序（已随 UBSan 移除而消解）
   三修复 commit 5de5bdd4a（${{ env.TAG_SAFE }} 表达式 / 弃用 SANITIZE / timeout 90m 包裹 sequential）
3. **Run 16 首次全绿全量运行**（35376792396，18 success + 1 预期 skipped）：
   - 基准对比表真实数据 15 列全齐
   - sequential 330 pass / 0 failed（24.03-lts 抽查），全部 15 镜像 OK
   - reports artifact 修复后含 tag 后缀文件

### 跨 OS 基准首份真实数据（20s 采样，趋势对比用）
| stressor | 20.03 系 (gcc7.3) | 22.03 系 (gcc10.3) | 24.03 系 (gcc12.3) | 解读 |
|---|---|---|---|---|
| fma | ~16-18 万 ops/s | ~48-51 万 | ~48-54 万 | gcc10 向量化 3 倍跳变（编译器代差） |
| memrate | ~1,900-2,200 | ~1,900-2,100 | **~5,000-6,400** | 24.03 的 glibc/内核优势 2.5-3x |
| memcpy | ~70-73 | ~86-94 | ~81-96 | 22.03 起提升 |
| cpu/stream | 平坦 | 平坦 | 平坦 | 编译器不敏感 |
| 同 SP 版本内 | 波动 <10% | <10% | <10% | 测量可信 |



### runner 环境的深层发现（run 7-14 排障沉淀）
| 现象 | 根因 | 处置 |
|---|---|---|
| sequential 之后所有 exec 失败（`OCI runtime exec failed: procReady not received`，exit 128） | ~390 个短命进程树耗尽容器 pids/OCI 运行时，且**不恢复**（本地容器无此问题——runner 特有） | 步骤重排：sweep/bench/upload 全部前置，sequential 放最后；断言脚本 fork-free |
| actions/checkout post 步骤失败污染 job 结论 | post（orphan 清理）在报废容器里跑 | 换 REST tarball 解压（无 post 步骤） |
| actions/cache post 同样失败 | 同上 | 移除 cache action（接受 ~1-2min/jobs 的 dnf 刷新开销） |
| sleep/ps/head 都 exit 128 | bash 的 sleep 是外部命令也要 fork | 断言脚本纯 bash 内建 |
| rc=3 但 failed=0 且 completed | 某 stressor 因资源 early-abort 计入 skipped，resource_success=false → rc=3 | 断言分类：rc=3+failed=0+completed = 环境受限 warning |
| opcode/text 偶发 rc=2（1/15 镜像，本地容器复现通过） | shared runner 邻居压力瞬态 | sweep 失败项重试一次 |
| sp4 sequential 被 360min 超时杀 | 极慢 runner（2.5h+ 跑不完 ~12min 的套件） | 无代码修复可做——重跑碰运气；final-status 会如实报告 |


用户新指令：触发首次 publish_image，修复 15 镜像跑路问题。PAT 已配置（~/.gh-token-pat，600 权限，gh CLI 使用）。

### 8 轮 CI 迭代史（run 35235189820 → 35302105963）
| Run | 失败 | 根因 | 修复 commit |
|---|---|---|---|
| 1 (35235189820) | 20.03×5 checkout 死；22.03×5 编译死；24.03×5 sequential 3.5h 超时被 cancel | ①20.03 镜像无 tar，checkout 的 REST 回退需要它 ②gcc 10.3 不认识 armv9-a 架构名 ③max-parallel 5 分批在共享 runner 池互相拖长尾 | 46a2bb4a3 (tar) / 35d57f7cc (armv8.2-a+sve2) / 2ad1f96c4 (max-parallel 15 + timeout 2s + 360min) |
| 2 (35295112265) | 20.03 dnf 死 | sed 把 repo 重写到 archives.openeuler.org——该站没有 20.03-LTS 树（404）；官方 repo.openeuler.org 其实活着（302→dl-cdn CDN，15 tag 全 200） | 21fb8c138（删 sed，保留官方 URL） |
| 3 (35295843012) | 20.03 dnf GPG 失败 | CDN 上 20.03 的部分 RPM 未签名（zlib-devel-1.2.11-17.oe1 "is not signed"），gpgcheck=1 中止整个事务 | 5229935e4（--nogpgcheck） |
| 4 (35296465324) | 20.03 编译死 arm_sve.h | gcc 7.3 无 arm_sve.h（GCC 8+ 才有） | f809ba3dc（include 守卫 __GNUC__>=10） |
| 5 (35297495473) | 20.03 编译死 armv8.4-a+sha3/+sm4 属性 + vsm3/vsm4/vsha512 intrinsic | gcc 7.3 不认识 armv8.4-a 修饰符（GCC 8+）、vsm3/vsm4（GCC 9+）、vsha512（GCC 10+） | edb629fba（sha512/sha3/sm3/sm4/sm4key 五方法 __GNUC__>=10 门控） |
| 6 (35298726644) | 20.03 编译死 HWCAP2_RNG | 20.03 内核头（4.19 era）无 HWCAP2_RNG（kernel 5.3+）→ **本地 20.03 容器全量验证 = 发现 pmull verify 真 bug** | a5e3aa5a8（#ifndef fallback + pmull golden 分派修复） |
| 7 (35300002914) | 14 个 job sequential 断言失败 | ①断言脚本 fork 饥饿误报（pids cgroup 被残留 worker 占满，grep/awk 全 fail→计数全 0）②numacopy/shm/wait/cgroup 在 runner 容器环境失败（本地容器验证代码正常） | 245f2f458（fork-free 断言 + 四 stressor 排除 + 清理步骤） |
| 8 (35302105963) | 待观察 | — | — |

### 修复的真软件 bug（CI 的核心价值兑现）
**pmull golden 分派 bug（stress-armcrypto.c）**：`--verify` 的软件参考分派用**方法表下标**（`n == 1` aes、`n == 9` pmull）。gcc<10 时表缩 5 项，pmull 移到下标 4，`n==9` 永不命中 → ref 保持 memset 全 0 → 每个 word 报 "expected 0x0000000000000000"。该 bug 在表结构变化（增删方法）时也会触发，条件编译只是让它可达。修复：按稳定方法名分派。20.03 容器实测修复前后对比（fail → pass）。

### 关键方法论沉淀
- **本地容器先行**：run 6 后改用本地 openeuler-offline:20.03-LTS-SP4 镜像（gcc 7.3）全量编译+冒烟，一次性抓完剩余编译错误（HWCAP2_RNG + pmull bug），不再每轮 CI 试错
- **runner 环境受限甄别**：numacopy（单 NUMA）/shm（64MB dev/shm）/wait（pids limit）/cgroup（无 cgroup v2 挂载）在本地容器通过 → 环境问题入排除清单；软件问题修代码
- **断言脚本要防 fork 饥饿**：跑在 stressor 后面的脚本必须 fork-free（bash 内建）
- CDN 细节：archives.openeuler.org ≠ 老 LTS 存放处；repo.openeuler.org 302 dl-cdn.openeuler.openatom.cn 全系 200；20.03 部分 RPM 未签名



用户需求：15 个 openEuler LTS 镜像（20.03/22.03/24.03 × 5 SP）原生 container 模式构建 + 全量功能测试 + 基准对比 + 每日 12:00 触发 + ghcr.io 改名 sdc-stressng。

### 交付物
1. `.github/workflows/multi-os-verify.yml` — 5 jobs：
   - manifest-check：15 tag 存在性 fail-fast 预检（docker manifest inspect）
   - build-test ×15 matrix（fail-fast:false、max-parallel:5、container: openeuler/openeuler:<tag>、--user root、arm64 runner）：dnf 缓存（actions/cache 按 tag 分键）→ UBSan 构建（失败自动降级 plain 并 warning）→ smoke → --sequential 全量（timeout 5s/个 + --verify + --exclude 5 病理项）→ 62 个 *-method 全参数 sweep → 20s 基准采样（cpu-matrixprod/fma/memcpy/memrate-zva/stream）→ 二进制+报告 artifact
   - benchmark-compare：15 份 bench yaml → 跨 OS bogo-ops/s 对比表 → job summary + artifact(90d)
   - publish-image（仅手动 dispatch 且勾选）：15 个 verify-<tag> 推 ghcr.io/<repo>（container job 无 docker daemon → 独立 VM job 从 artifact 组装）
   - final-status：gh api 数 matrix 实际完成数，断言 15/15，少一个都红
2. `scripts/ci-verify-log.sh` — 日志断言（failed:>0 / 缺 completed 行 / rc≠0 三类判 fail；honest skip 不算）
3. `scripts/ci-method-sweep.sh` — 62 个 *-method 全参数遍历（二进制自枚举方法；'all' 关键字优先，无则逐个；rc=3 EXIT_NO_RESOURCE 归环境受限非失败）

### 本轮排掉的坑（全部实证）
| 坑 | 实证 | 处置 |
|---|---|---|
| STATIC=1+SANITIZE=1 链接失败 | `cannot find -lubsan`（无 libubsan.a） | 工作流只用 SANITIZE=1 |
| SANITIZE=1 也可能链接失败 | 本机 libubsan.so 是**断裂符号链接**（→ 不存在的 libubsan.so.1.0.0，openEuler 打包问题） | 构建步骤自动降级 plain + ::warning |
| `-x --exclude` 语义 | 逗号分隔**纯名**列表非正则；未知名报错退出（stress-ng.c:594-607） | 工作流列表全部为已存在 stressor |
| cyclic 全方法 rc=3 | `skipped: 1: cyclic`（非 root 无 RT 调度） | sweep 归 nolimit 非失败 |
| `--<m>-method ?` probe 输出走 stderr | `2>&1 >/dev/null` 366 字节 / stdout 0 | 脚本用 2>&1 + grep |
| while-read 管道子 shell 计数丢失 | sweep1 pass=43 但有 FAIL 行（计数 bug） | 重写为 for 循环直计数 |
| sweep2 26 个 "not available" | sweep 运行中被我 make clean 删了二进制 | 假象，非脚本 bug；sweep3 复测 |
| YAML 步骤被误删 | python yaml.safe_load 报 line 106 | 恢复 steps: 键 |
| hashFiles 绝对路径 | 仅相对 workspace 有效 | cache key 改用 tag 直接分键 |

### 全量参考数据（本机 openEuler 24.03 SP3 aarch64，非 root）
- `--sequential 1 --timeout 2 --verify --exclude <5 病理>`：**319 passed / 74 skipped / 0 failed / 15m50s / rc=0** → CI 用 timeout 5 预计 ~40min/镜像（300min job 上限，安全）
- method sweep（timeout 2）：43-50 pass / 0 fail（含逐方法枚举）
- 退出码（stress-ng.h:328-334 权威）：EXIT_NOT_SUCCESS=2、EXIT_NO_RESOURCE=3、EXIT_NOT_IMPLEMENTED=4、EXIT_SIGNALED=5、EXIT_BY_SYS_EXIT=6、EXIT_METRICS_UNTRUSTWORTHY=7；rc=3 即环境受限（cyclic 无 RT 调度实测）。断言脚本只区分 0/3/非零非 3

### 待办
- [x] sweep3 干净复测：**pass=126 skip=2 nolimit=6 fail=0 RC=0**（dfp/plugin 诚实跳过）
- [ ] git commit + push（用户首跑验证需网页手动触发 workflow_dispatch）
- [ ] 首跑后修复 15 镜像实际暴露的问题（20.03 dnf 归档源是否可用等）



用户目标：实现方案全部 12 个 patch；本机非 950 → 用 QEMU 模拟验证 SVE；SVE 功能动态开关。

### 模拟验证环境（从零搭建）
- openEuler qemu-user 包不含 aarch64 自仿真 → 从 qemu.org 8.2.0 源码 + dnf download 提取的 glib2-devel 自建 `/tmp/qemu-out/qemu-aarch64`（需 LD_LIBRARY_PATH=/tmp/qemu-build/usr/lib64）
- `-cpu max` 暴露 sve/sve2/sveaes/svepmull/svesha3/svesm4/sm3/sm4/sha3/sha512/rng/bf16/i8mm 全部 HWCAP
- openEuler 定制 qemu-system 引导卡死（ubios）→ 放弃全系统仿真，用户态仿真足够
- SVE2 crypto 无 GCC12 intrinsic → binutils 提取 12 条 .inst 编码（aese/aesd/aesmc/aesimc/sm4e/sm4ekey/eor3/bcax/rax1/xar/pmullb/pmullt），全部 qemu 下执行验证
- 关键发现：target("arch=armv8.4-a+sm4") 属性门 SM3+SM4、target("arch=armv8.4-a+sha3") 门 SHA3/SHA512（必须完整 arch= 形式）；z 寄存器变量+target 属性在默认 march 下可用 → 无需全局 march 即可动态开关

### 11 commits（分支 port/arm64-saturation-sdc）
1. d9381762c stress-armcrypto：13 方法（NEON aes/sha1/sha256/sha512/sha3/sm3/sm4/sm4key/pmull + SVE2 sve2-aes/pmull/sha3/sm4 via .inst），aes/pmull KAT 软件参考（ARM AESE 语义 ShiftRows(SubBytes(state^key)) 与 FIPS 不同、PMULL 全 128 位 carryless，均逐字节对拍修正），逐方法 HWCAP/HWCAP2 动态门控；native passed + 诚实跳过，qemu 13 方法全 passed，故障注入（翻 1 位）被抓
2. cd0196dd8 stress-regs：NEON v0-v31 + SVE z0-z31（HWCAP_SVE 门控 + target 属性），SHUFFLE 16 轮全寄存器轮换；native+qemu 双验
3. 59ae24272 stress-tsc：CNTVCT_EL0 分支（isb 序列化）；native 从 skipped 变 passed，~10ns/read
4. 8e1874136 DC CIVAC/CVAC/ZVA：cache-flush/clwb ARM 路径 + memrate write64zva 方法（CTR_EL0 DZMinLine 探测，本机 1024B/行，实测 33.6GB/s）
5. 76c730bbc sdc-run.sh：pair 模式（4 组合 SMT 争用矩阵，双 worker bogo-ops/s 记录）+ full --preheat（发热在前验证在后，R2-B2 实证杠杆）
6. 0272aca00 stress-rdrand：RNDR/RNDRRS 分支（NZCC cset 检查熵耗尽）；native 诚实跳过、qemu passed
7. 79cac2355 stress-sve2：方法表化 fmla/gather/fcmla/bitperm/bfdot；QEMU 暴露并修复原实现 3 个潜伏缺陷（svmla 参数序、golden/hw 顺序循环论证、VL 截断）+ bf16 RNE 舍入、fcmla #90 实证语义；全方法 --verify qemu passed
8. d22364ac7 core-rapl：ARM hwmon power*_input 后端（powercap 缺失不再提前返回）；假传感器树 LD_PRELOAD E2E 验证（ddr 12.35W soc 45W 实时表）
9. 687d14b39 stress-fma：12 个 SVE2 内核（add/sub×132/213/231×double/float，svmla/svmsb/svnmsb）HWCAP_SVE 运行时分发（GCC12 无 aarch64 target_clones 的 FMV 等价）；native NEON 路径不变、qemu SVE2 路径 verify passed
10. 7876bfd39 docs：README ARM 能力矩阵 + ignite-cpu man 修正（通用 cpufreq 路径全架构生效）
11. df6f448b3 docs：plan 完成标记 + 三份研究报告入库

### 最终验收
- 默认构建：make clean+make rc=0；armcrypto/regs/tsc/cache/memrate-zva/fma/zombie native 全 passed，rdrand/sve2 诚实跳过
- QEMU 终验：armcrypto+rdrand+tsc+regs+fma --verify 30s 全 passed（10 workers），failed: 0
- SVE 动态开关贯穿全部 patch：HWCAP/HWCAP2 运行时检查 + target 属性编译隔离，单 binary 跨 920/950


## Session 2026-09-16（第六轮）: 对标 x86 → ARM64 饱和压测强化研究 — 完成

任务：深度研究源码，对标 x86 专属支持，设计把 ARM64 服务器芯片压力打满的强化方案（激发 SDC）。

### 研究阶段（4 subagent）
- R1 x86 基线盘点 ✅：9 个完整 x86-only stressor + 13 项方法级 + 7 项框架级；10 项 ARM64 缺口排序（RNG/加密单元/向量寄存器/FMV/tsc-CNTVCT/功率遥测/缓存维护指令）
- R2 SDC 前沿 ✅：纠正任务书 3 处引用错误（2102.11245 是 Meta 不是 Google；Stealthy Saboteur 不存在；fARM 是 BSC 不是 Meta）；负载杠杆 A1-F1/G1（全核并发、热浸润+最低触发阈值、di/dt 阶跃 7% 独有覆盖、ITHICA 长序列 vs 阿里指令压力结论冲突→两者都要、SMT 同压、周期重复）
- R3 ARM64 饱和 ✅：逐单元饱和模式表；FIRESTARTER 是最大参照物；ARM SMT2 无公开微架构文献（负面结论）；Kunpeng 950 零公开信息
- R4 subagent 死于 API 模型错误（325 次调用后）——范围由 R1+R3+主会话定向核实覆盖

### 主会话交叉核实（关键实证）
- port 分支 11 patch 已合入 main（工作树即最新）
- 开发机 920 特性：aes/sha1/sha2/asimddp/asimdhp/asimdrdm/atomics/crc32 → NEON 加密/点积/FP16 可本机全功能验证
- target("+crypto") intrinsic 编译运行通过（复用 hw_crc32 模式）
- GCC 12 arm_sve.h 无 SVE2 加密 intrinsic（grep 实证）→ SVE2 加密必须 .inst；SM4E/AESE .inst 编码已验证可编译
- binutils 2.41 SVE2 加密助记符语法有兼容问题 → 统一 .inst 路线
- DC ZVA/CVAC/CNTVCT EL0 本机可执行；v-reg asm 变量可用；HWCAP2 位内核头齐全
- hisi_sccl{1,3}_{ddrc,hha,l3c} uncore PMU 本机存在，非 root perf 不可用（目标机 root 待验）

### 交付物
- docs/superpowers/research/2026-09-16-r{1,2,3}-*.md（三份研究报告）
- docs/superpowers/plans/2026-09-16-arm64-saturation-sdc.md（实现方案：12 patch 三波 + backlog + 风险清单）
- 状态：**方案待用户批准，未写任何代码**

## Session 2026-09-14（第五轮）: EBADF mmap bug 修复 — 11/11 patches

用户报告：mmap 失败 EBADF，fd=-1 但无 MAP_ANONYMOUS。核实属实：
- `HAVE_MAP_ANONYMOUS` 在 stress-sve2.c:192 / stress-ls64.c:149 被用作守卫宏，但整个构建系统（Makefile.config/config.h/test/）从未定义它 → 守卫恒假
- 后果：真机上 `--sve2`/`--ls64` 的 stress_mmap_populate 传 fd=-1 且无 ANONYMOUS → EBADF → EXIT_NO_RESOURCE，压力测试根本没跑（本机 920 因无硬件诚实跳过所以没暴露）
- 最小复现实测：`mmap(NULL,4096,RW,MAP_PRIVATE,-1,0)` → `MAP_FAILED (errno=9 Bad file descriptor)`；加 MAP_ANONYMOUS 后 ok+写入通过
- 修复（52b1fae60）：删幻影守卫，直接 `MAP_ANONYMOUS | MAP_PRIVATE, -1, 0`——与上游惯例一致（core-mmap.c:279、stress-mmap.c 4 处调用点、stress-vm.c、stress-exec.c 全裸用）
- 教训：写新 stressor 时臆造了"规范守卫宏"而没先 grep 上游惯例

## Session 2026-09-14（第四轮）: sdc-run.sh 统一入口 — 10/10 patches

应用户要求把三条命令（全核背景压/逐核扫描/通路专项）整合为单一入口 `scripts/sdc-run.sh`（commit 待查），实现"先读 CPU 信息再推导参数"：
- 阶段 0 拓扑探测：online/isolated/offline、SMT sibling 映射（N_LOGICAL/N_PHYSICAL）、sve2/ls64/crc32 feature
- full/scan/path/all 四模式；worker 数全部从拓扑推导（full=N_PHYSICAL×2、path=N_LOGICAL/4）；无硬件通路打印 skipped 不假跑
- 实测：path/full/scan 三模式在本机全跑通（rc=0），修复 3 个 bug（bg-log 目录不存在静默失败、rc 被 kill/wait 污染、-t 单位）

## Session 2026-09-14（第三轮）: 移植实施 — 9/9 patches pushed

按 docs/superpowers/plans/2026-09-14-kunpeng950-sdc-port.md 执行，分支 port/kunpeng950-sdc-stress：

1. **Patch 1** SVE2 march 探测（0fd4437b5）——期间发现并修复两个关键问题：
   - -O2 下 GCC 不生成 SVE（CONFIG_CFLAGS 必须 -O3）
   - 只查编译器不查硬件 → 本机构建出的二进制 SIGILL（probe 增加 HWCAP_SVE 运行检查）
2. **Patch 2** --taskset physical（a23c294d5）
3. **Patch 3** sdc-scan.sh（7a0fc24f1，修 set -u 关联数组 bug）
4. **Patch 4** fma 位级诊断（fb9fd9cb6，故障注入验证：1 bit flipped xor 0x200000000000）
5. **Patch 5** vecfp/matrix 位级诊断（6af367f39，matrix 注入验证）
6. **Patch 6** stress-sve2.c（774d5b81d——修复 guard 宏名错误 STRESS_ARCH_ARM64→STRESS_ARCH_ARM；svbext 需 +sve2-bitperm；march 串扩展）
7. **Patch 7** stress-ls64.c（ae954f194——HWCAP3 bit0/AT_HWCAP3=29 新内核 + HWCAP2 bit15 老内核双查）
8. **Patch 8** crc32 cpu-method（39408bda9——修复忘 include arm_acle.h；CRC-32C Castagnoli vs Ethernet 多项式用错）
9. **Patch 9** 文档同步（18ab99fd4）

每 patch 均含真实验证输出（详见 plan 文件）。目标机（950）待验证项：SVE2 auto-yes、physical 选 191 核、--sve2/--ls64 passed、sdc-scan 全量 sweep。

## Session 2026-09-14（第二轮）: 外部建议核实与方案融合

- 收到外部工程建议（三层 L0/L1/L2 + 四阶段 + CORE179 经验），按仓库纪律逐条对真实代码核实
- 核实结论：12 条断言中 9 条属实；**3 处技术错误修正**：
  1. `svebf16` 拼写 gcc 12.3.1 拒绝 → 改 `bf16`（`armv8.6-a+sve2+bf16+i8mm` 实测 OK）
  2. `--cdouble` 误当 stressor（实为 cpu-method，实测 unrecognised option）
  3. objdump 验证从可选改为 L0 硬性验收门（fma/vecfp 纯 C 循环 + ARM 上 TARGET_CLONES 空，自动向量化结果不可预测）
- 关键实证：当前二进制 182 个 fmla 全 NEON、ld1d/ptrue/whilelo = 0 → **默认构建零 SVE 指令**，L0 必要性坐实
- 方案重排为 L0（重编）→ L1（编排，零代码）→ L2（9 个源码 patch），v1 的 G 系列完成映射（G3/G7/G9 被更优方案替代，G4 降级 backlog）
- findings.md 重写为 v2；task_plan.md 增 Phase 1b 核实记录 + D5-D8 决策
- 待办：用户确认后按 L0 → L2.1 起步逐 patch 实现（每 patch 走 plan→code→verify→commit→push）

## Session 2026-09-14（第一轮）: stress-ng × SDCShield 协同 SDC 压测方案

### Phase 1: 调研 — complete
- 通读 sdcshield README + CLAUDE.md：273 用例、fork 模型、质量分级、EDAC/RAS、Kunpeng 920 平台怪癖
- 通读 kunpeng920_sdc_plan.md：三因素模型、模块覆盖率（MMU 20/L2C 40/LSU 54/OoO 56）、操作数变异字典、NUMA 维度扫描
- 盘点本仓库 stress-ng：390 stressor、342 含 verify、71 cpu-method、39 vm-method、13 cacheline-method
- 实测：--taskset 绑核可用；--smi 诚实跳过机制正常

### Phase 2: 差距分析 — complete
- 9 项缺口（G1-G9）写入 findings.md §3.2，每项含检出机理
- 完整性自查：覆盖三因素模型全部杠杆

### Phase 3: 改进方案 — complete
- 9 patch（P1-P9）排序与验证命令写入 findings.md §4
- 遵守 one-patch-per-unit：每 G 点一个 commit

### Phase 4: 端到端命令 — complete
- 5 阶段剧本（侦察→基线→协同→逐核→长稳）写入 findings.md §5
- 含安全边界与判读表

### Phase 5: 交付 — complete
- 本机零代码命令抽样实测（记录在 findings.md §6）
- 向用户汇报

### 后续（用户决定是否推进）
- 按 P1→P9 顺序逐 patch 实现（每 patch 走 plan → code → verify → commit → push 流程）
- 目标机（CP1, Kunpeng 950）上的命令需要真机执行；本机仅验证"命令可运行+诚实跳过"
