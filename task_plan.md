# Task Plan: stress-ng × SDCShield 协同压测 Kunpeng 920 SDC 故障核

## Goal

CP1（Kunpeng 950 7592C，2 socket / 95C×2T=190 物理核 / 382 逻辑 CPU / SVE2+crypto+LSE）出现过 CPU 核隔离、自检失败现象，怀疑存在 SDC（静默数据损坏）故障核。本任务：

1. 结合设备 CPU 特性（SVE2/SVE、NEON、CRC32、atomics、ls64 等），识别 stress-ng 中能配合 SDCShield 检出 SDC 的能力与缺口；
2. 设计 stress-ng 改进方案（作为 SDCShield 的"环境扰动器 / 压力背景"，而不是重复做结果校验）；
3. 给出 stress-ng 端到端的压测命令，与 SDCShield 协同定位 SDC 故障核。

**核心分工原则**（来自 /home/sdc/wangxu/kunpeng920_sdc_plan.md 的方法论）：
- SDCShield = 校验器（golden 比对，判定 SDC 与否，报告哪个核 fail）
- stress-ng = 环境扰动器（压 di/dt、压功耗、压缓存/TLB/互联、制造边界时序条件，把"潜在弱核"逼出 SDC）

## Target machine facts (from user's lscpu, MUST NOT re-derive)

| 项 | 值 | 含义 |
|---|---|---|
| CPU | Kunpeng 950 7592C, HiSilicon, family 280 | 目标机型（注意：不是本机！本机是 920/128 核） |
| 拓扑 | 2 sockets × 95 cores × 2 threads = 382 CPU | SMT=2，**同核两超线程共享执行资源** |
| NUMA | node0=0-191, node1=192-381，每 socket 一个 node | 跨 socket 访存最远 |
| 频率 | 2.3GHz max / 1.2GHz min, boost disabled, scaling 100% | 固定频率，电压/频率诱因需靠负载形态 |
| 特性 | SVE2（sveaes/svepmull/svesha3/svesm4/svebitperm/svei8mm/svebf16）、SVE、NEON(asimd)、crc32、atomics(LSE)、ls64/ls64_v、rng、fphp/asimdhp、sm3/sm4/sha1/sha2/sha3、dit、uscat、lrcpc/ilrcpc、dgh、rpres、wfxt、hbc | ARMv9.x 级特性集 |
| Cache | L1d 64KB/核(11.9MiB/191)、L1i 128KB/核、L2 1MB/核(191MiB)、L3 546MiB/24 实例（≈22.75MB/实例，共享组） | L3 分 24 个实例 |
| CP1 现象 | CPU 核隔离 + CP1 自检失败 | 已有核被隔离的先兆 |

**注意**：本机（开发机）是 Kunpeng 920 / 128 CPU / 无 SMT / 4 NUMA node —— 与目标机不同。stress-ng 改动的验证在本机做，端到端命令面向目标机写。

## Phases

### Phase 1: 调研 stress-ng 现有能力与 SDC 相关缺口 — complete
- [x] 盘点 --verify 覆盖的 stressor（342/390 文件含 verify 逻辑）
- [x] 盘点 CPU/内存/缓存/NUMA/原子 相关 stressor 与方法列表（cpu-method × 71、vm-method × 39、cacheline-method × 13）
- [x] 确认 --taskset / --affinity-pin / --taskset-random CPU 绑定能力（实测 `--taskset 0-3 --cpu 2 -t 2` → passed: 2）
- [x] 确认 SDCShield 侧能力（273 用例、EDAC/RAS、--dump-cpu-info、-Y continue-on-error、-n 1 线程数、-e 选择、-F first-fail-stop）
- [x] 通读 kunpeng920_sdc_plan.md 的模块脆弱性模型（MMU 20% / L2C 40% / LSU 54% / OoO 56% 覆盖率最低）

### Phase 1b: 核实外部建议的代码事实断言 — complete
- [x] Makefile 无 ARM -march：**真**（grep march/mcpu/mtune 无输出，默认 -O2）
- [x] target_clones 仅 x86/ppc64：**真**（core-target-clones.h:36 STRESS_ARCH_X86、:398 PPC64；ARM 落到空 `#define TARGET_CLONES`，stress-fma.c 的 TARGET_CLONES 在 ARM 上是 no-op）
- [x] 默认构建无 SVE 指令：**真且比建议更强**——objdump 实测 182 个 fmla 全是 NEON v 寄存器形式，`ld1d/ptrue/whilelo` 计数为 **0**（z 寄存器形式 fmla 也是 0）
- [x] fma/vecfp/matrix 是 VERIFY_OPTIONAL：**真**（stress-fma.c:625、stress-vecfp.c:518/534、stress-matrix.c:1069/1088）
- [x] fma verify = 同算两遍 memcmp：**真**（stress-fma.c:568-586，错误信息 "data difference between identical double fma computations"）
- [x] --taskset 无 physical 关键字：**真**（core-affinity.c:264-273 仅 package/cluster/die/core + even/odd/all/random；man stress-ng.1:1088-1123 同）
- [x] --ignite-cpu 仅 Intel P-State x86：**真**（stress-ng.1:561 原文）
- [x] varyload/varyload-ms/cpu-load-slice/mbind/interrupts/klog-check/thermalstat/tz/seed/-Y yaml 存在：**真**（core-opts.c:1770-1772/221/809/598/680/1711/1719/1369 + -h 确认）
- [x] **修正1**：建议给的 `-march=armv8.2-a+sve2+svebf16+i8mm` 在 gcc 12.3.1 上**编译失败**（invalid feature modifier 'svebf16'）。实测可用拼写：`armv8.6-a+sve2+bf16+i8mm` 或 `armv8.2-a+sve2+bf16+i8mm`（gcc 报错信息列出的合法修饰符：sve2/sve2-sm4/sve2-aes/sve2-sha3/sve2-bitperm/i8mm/bf16/ls64/...）
- [x] **修正2**：建议命令里 `--cdouble 95`、`--cpu-method cdouble --fma 95` 混用——cdouble 是 **cpu-method** 不是独立 stressor（实测 `--cdouble 1` 报 unrecognised option；`--cpu 1 --cpu-method cdouble` 正常）。阶段B命令已改写
- [x] **修正3**：fma/vecfp 是**纯 C 循环**（无 intrinsics/asm），ARM 上 TARGET_CLONES 为空 → 即便 -march=sve2 重编，自动向量化理论上可生成 SVE，但**必须 objdump 复核**（这是 L0 的验收门，不是可选项）
- [x] eigen 在本 fork 是 `--eigen`（stress-eigen.c，VERIFY_ALWAYS），依赖 HAVE_EIGEN（本机未配置 → 诚实跳过）。建议里 "eigen/matrixprod" 分组有效，但 eigen 需构建机装 eigen C++ 库
- [x] 建议的负载分组 stressor 全部存在且可运行（实测 regs/opcode/ptr-chase/spinmem/misaligned/vecmath/memrate/l1cache/intmath 均 successful run）

### Phase 2: 差距分析 — complete
- [x] 逐维度对照：di/dt 与电压裕量 / 缓存与互联 / TLB 与页表 / LSU 边界 / 原子与互联一致性 / SVE2 数据通路 / 温度
- [x] 明确 stress-ng 已覆盖 vs 需新增/改进的点（见 findings.md 差距表）
- [x] 定义每个改进点的"检出机理"（为什么能帮 SDCShield 检出 SDC）
- [x] 吸收外部建议的 7 个优化点（含验证结论与修正）

### Phase 3: 设计 stress-ng 改进方案（分 patch 的单元清单） — complete
- [x] 融合两轮方案（我的 G1-G9 + 建议的 L0/L1/L2 三层），按 L0 重编 → L1 编排 → L2 源码增强 重排优先级
- [x] 每个改进点 = 一个 patch 单元（符合仓库 one-patch-per-unit 纪律）
- [x] 按"检出收益 × 实现成本"排序
- [x] 标注哪些是纯命令行组合（零代码，立即可用）vs 需要代码
- [x] 方案过完整性检查：是否覆盖三因素模型（设计冗余不足/老化退化/业务负载）全部杠杆

### Phase 4: 端到端压测命令设计（面向 CP1 目标机） — complete
- [x] 采用建议的四阶段结构（A 全核背景压 → B CP1/CP0 对照 → C 逐物理核 sweep → D 复现取证），融合我原方案的侦察/基线前置阶段
- [x] 修正建议命令中的错误（svebf16 拼写、cdouble 误用、--taskset package1 与数字范围二选一）
- [x] 每阶段命令可复制执行，含 SDCShield 侧配合命令
- [x] 结果判读方法（多证据交叉：verify fail + bogo ops 离群 + sdcshield cpu-mask + EDAC/dmesg）
- [x] 安全边界（root 下 OOM 调整、不可杀进程、热失控风险提示）

### Phase 5: 撰写最终交付文档 — complete
- [x] 写入 findings.md（差距分析+方案+命令，自包含）
- [x] 在本机验证改进方案中"零代码命令"确实可用（抽样实测）
- [x] 汇报用户

## Errors Encountered

| Error | Attempt | Resolution |
|-------|---------|------------|
| (none) | - | - |

## Decisions Made

| # | Decision | Why |
|---|----------|-----|
| D1 | stress-ng 定位为"扰动器"而非"第二校验器" | SDCShield 已有 273 个 golden 比对用例；stress-ng 重复做校验收益低。真正缺口是负载形态/环境压力。其 --verify 自校验作为"辅助报警"保留 |
| D2 | 改进优先级以 OoO/LSU/L2C/MMU 脆弱模块为纲 | kunpeng920_sdc_plan.md 实测覆盖率：MMU 20%、L2C 40%、LSU 54%、OoO 56% 最低；这些是 SDC 检出的最大杠杆 |
| D3 | 目标机与开发机分开处理 | 目标机 Kunpeng 950（SMT2/382CPU/SVE2）≠ 开发机 920（128CPU/无SMT/无SVE）；命令按目标机写，代码验证按本机做 |
| D4 | 新 stressor 命名 `sdc-*` 前缀，SDC 配方参考 sdcshield 的 movbe/core-179 思路 | 与 SDCShield 的 ARM64 SDC 专项（arm64_sdc、power_virus_dit、lsu_store_forward_arm、l2c_cross_cache_line_arm、mmu_split_tlb_arm）形成互补分工 |
| D5 | **采纳外部建议的三层结构（L0 重编 / L1 编排 / L2 源码）并置于我原 G 系列之上** | L0 是零源码改动且直接决定"负载是否真的打到 SVE2 管线"——不改它，所有向量类 stressor 在 950 上只有 NEON 强度。核实确认：Makefile 无 ARM march、TARGET_CLONES 在 ARM 为空、二进制零 SVE 指令 |
| D6 | **L2 源码增强清单以建议的 6 项为主干，融合我原 G1/G2/G6** | 两者高度重叠（SMT 感知=建议2、verify 位级诊断=建议3、sweep 模式=建议4=我 G8）；G2 跨 socket 乒乓、G6 ls64 作为补充项保留 |
| D7 | **外部建议的 3 处技术错误已修正后再采用**：svebf16 拼写（gcc 12.3.1 拒绝，改 bf16）、cdouble 误当 stressor（实为 cpu-method）、"objdump 验证"从可选改为 L0 硬性验收门 | 仓库纪律：所有命令必须先在真实编译器/二进制上验证。实测依据见 Phase 1b |
| D8 | CP1 少一核（node1=95 核 vs node0=96 核）按"疑似 BIOS 已 deconfigure"处理，阶段0 必须先取证（BMC SEL/dmesg）再压测 | 若核已被固件下线，压测打不到它；且这是故障历史的一部分，取证优先于触发 |

## Next Step

**Phase 8（2026-09-17 第八轮）进行中**：GitHub Actions 多 OS 自动化验证（15 个 openEuler LTS 镜像 + 全量功能测试）。计划见下方 Phase 8 节。当前：写工作流文件 `.github/workflows/multi-os-verify.yml`。

## Phases（Phase 8 新增）

### Phase 8: GitHub Actions 多 OS 自动化验证 — in_progress
- [x] 恢复历史规划文件（Phase 1-7 已完成并合并 main）
- [x] 盘点现有 CI（ci-builds.yml / container-image-{stable,edge}.yml；上游 docker workflow 基于分支 master，本仓 main）
- [x] 核实 stress-ng 测试基建：`--sequential N`（逐个跑全部 stressor，默认 60s/个，`--timeout` 可调）、`--permute`、`--random`、`-x/--exclude`、`--verify`、`-Y/--yaml`、62 个 `*-method` 选项（`all` 关键字普遍支持）
- [x] 核实 Makefile：SANITIZE=1 UBSAN 全家桶已内建；`all: build_info config.h stress-ng`；DEBUG=1
- [x] Docker Hub/ghcr 网络在开发机被封锁（curl SSL rc=35）→ 15 个 tag 的存在性改为工作流内 fail-fast `manifest inspect` 预检
- [x] 本机构建 + 探测 stress-ng 输出格式：**全量 sequential 实测通过**（319 passed / 74 skipped / 0 failed，`--timeout 2` 全程 15m50s）；退出码 0；`-x` 是逗号纯名列表；`failed: 0` 行是断言锚点；rc: fail=1/无资源=2
- [x] 编写 `.github/workflows/multi-os-verify.yml`（5 jobs：manifest-check → build-test×15 → benchmark-compare → publish-image → final-status）
- [x] 编写 `scripts/ci-verify-log.sh`（日志断言：failed:0/completed/rc 分类；truncate 与 fail 路径已测）
- [x] 编写 `scripts/ci-method-sweep.sh`（62 个 *-method 全参数遍历；修了 while-read 子 shell 计数 bug；rc=3 EXIT_NO_RESOURCE 归类为环境受限非失败——cyclic 需 RT 调度）
- [x] 本机 method sweep 实测：pass=50 skip=26 nolimit=6 fail=0（第一轮 pass=43/92）
- [x] **发现并修复**：STATIC=1+SANITIZE=1 链接失败（无 libubsan.a）→ 工作流只用 SANITIZE=1 动态链接
- [x] YAML 结构验证（python yaml.safe_load 通过；15+15 matrix 一致性检查通过）
- [x] sweep probe 消退问题排查：**假象**——sweep2 运行中被并行的 make clean 删了二进制；干净复测（sweep3）：**pass=126 skip=2 nolimit=6 fail=0 RC=0**（2 skip = dfp 无 libdfp、plugin 无插件目录，均诚实跳过）
- [x] SANITIZE=1 复测：本机 libubsan.so 断链（openEuler 打包问题）→ 工作流构建步骤已设计为自动降级 plain + ::warning
- [ ] 提交推送，在 GitHub 上触发首跑并修复问题

### Phase 8 工作流设计（决策记录）

**输入约束（用户需求，不可变）**：
1. 15 个 openEuler LTS 镜像：20.03-lts{,-sp1..sp4}、22.03-lts{,-sp1..sp4}、24.03-lts{,-sp1..sp4}（=5+5+5）
2. 每个镜像构建 1 个 stress-ng 二进制 → 15 个二进制
3. 构建后分别跑：全量测试用例全量参数功能测试（消 bug 为目标）
4. GitHub Actions **原生 `container:` 属性**模式（作业直接跑在目标镜像内）
5. 性能优化：依赖包缓存等
6. 基准测试：不同 OS 下二进制性能对比
7. 策略：确保 15 个镜像全部执行完（fail-fast 关闭）+ 并行作业
8. ghcr.io 镜像：把历史 `opendcdiag-arm` 改名为 `sdc-stressng` 并使用该镜像（登录用 GITHUB_TOKEN，无需 secrets）
9. 每天早上 12 点触发（cron 12:00 local → UTC `0 4 * * *` 北京时间正午）
10. 兼容性：openEuler 20.03/22.03/24.03 的 dnf/glibc/gcc 差异

**架构（matrix job + 汇总 job）**：
```
manifest-check (fail-fast 预检 15 tag)
  → build-test (matrix ×15, fail-fast: false, container: openeuler/openeuler:<tag>)
      ├─ dnf 缓存 (cache dnf basedir)
      ├─ make -j2 (SANITIZE=1 UBSAN 构建)
      ├─ smoke: --version/--buildinfo
      ├─ 全量功能测试: --sequential 1 --timeout 12s --verify -x <已知不可跑清单> 
      ├─ 方法级全参数: cpu-method/vm-method/... all 循环
      ├─ 基准采样: cpu/fma/memrate/memcpy 定长跑 -Y yaml
      └─ upload-artifact: 二进制 + 测试报告 + 基准 yaml
  → benchmark-compare (needs: build-test, if: always())
      下载 15 份基准 yaml → 汇总 markdown 报告表 → artifact + job summary
  → final-status (needs: [build-test, benchmark-compare], if: always())
      校验 15/15 作业完成、聚合结论（fail 不掩盖）
```

**关键兼容性决策**：
| 问题 | 决策 |
|---|---|
| 20.03 dnf 源 EOL/404 | 20.03 系列用 openEuler 归档源（archives.openeuler.org）替换 repo 文件；dnf makecache 失败不 fatal（基础镜像已带 gcc/make） |
| 20.03 gcc 7.3 vs 24.03 gcc 12 | SANITIZE=1 部分检查项 gcc7 不支持 → Makefile 的 `cc_supports_flag` 自动过滤，无需干预 |
| runner 架构 | openEuler 官方镜像同时有 x86_64/aarch64 → GitHub arm64 runner 直接原生跑 aarch64 版（与目标 Kunpeng 场景一致，且免 QEMU） |
| 权限 | container 模式默认 root → stressor 可跑面最大；`--oomable` 关 |
| "全量参数"务实解释 | 每镜像时长预算 ~45-60min：`--sequential` 全 stressor 短 timeout + 62 个 method 选项 `all` 遍历 = 用例×参数双全量 |
| 15 镜像全跑完 | matrix `fail-fast: false` + `if: always()` 链 + final-status 聚合断言 15/15 |
| ghcr 登录 | `docker/login-action@v3` + GITHUB_TOKEN（packages:write），推 `ghcr.io/wangxumarshall/sdc-stressng:verify-<tag>`（改名自 opendcdiag-arm） |
| 缓存 | `actions/cache` key=dnf-<tag>-<lockfile hash>（/var/cache/dnf + /var/lib/dnf）；源码层不缓存（make 全量重编正是测试目的） |

**历史规划（Phase 1-7 已完成）见下方归档**

### Phase 7: 对标 x86、强化 ARM64 饱和压测研究（2026-09-16 第六轮） — complete
- [x] R1 x86 专属能力基线盘点（smi/rdrand/x86cpuid/tsc/ipsec-mb 完整 stressor + target_clones/regs/vnni/cache 方法级 + rapl/ignite-cpu 框架级；报告 docs/superpowers/research/2026-09-16-r1-x86-baseline-inventory.md）
- [x] R2 前沿 SDC 研究（Google/Meta/阿里/ITHICA 论文实证：全核并发、热浸润、di/dt 阶跃、长序列、随机数据、SMT 同压、周期重复；报告 …r2-sdc-frontier-research.md）
- [x] R3 ARM64 微架构压测文献（逐单元饱和模式表、FIRESTARTER 参照、SMT2 无公开资料负面结论、Kunpeng 950 零公开信息；报告 …r3-arm64-saturation-research.md）
- [x] R4 饱和缺口分析（subagent 死于 API 错误；范围由 R1+R3+主会话定向核实覆盖：DC ZVA/CVAC/lrcpc/DIT/RNDR 全树零命中、stress-cacheline 已有跨核共享、port 分支已合 main）
- [x] 主会话交叉核实（port 分支合入 main、开发机特性 aes/sha1/sha2/dotprod/fp16、target("+crypto") 可用、GCC12 无 SVE2 crypto intrinsic→.inst 路线、binutils SVE2 助记符问题、HWCAP2 位齐全、hisi PMU 存在但非 root 不可用）
- [x] 综合撰写实现方案（docs/superpowers/plans/2026-09-16-arm64-saturation-sdc.md，12 patch + backlog + 风险清单）

### Phase 6: 移植实施（L2 源码落地） — complete
- [x] 特性分支 port/kunpeng950-sdc-stress 建立
- [x] Patch 1 config: SVE2 march 探测注入 CONFIG_CFLAGS（0fd4437b5）
- [x] Patch 2 --taskset physical SMT 感知（a23c294d5）
- [x] Patch 3 scripts/sdc-scan.sh 逐核 sweep（7a0fc24f1）
- [x] Patch 4 fma verify 位级诊断（fb9fd9cb6）
- [x] Patch 5 vecfp/matrix 位级诊断（6af367f39）
- [x] Patch 6 stress-sve2.c（774d5b81d，编译级验证+诚实跳过）
- [x] Patch 7 stress-ls64.c（ae954f194，同上）
- [x] Patch 8 cpu-method crc32（39408bda9，全功能验证）
- [x] Patch 9 文档同步（18ab99fd4）

全部 9 patch 已推 port/kunpeng950-sdc-stress 分支。执行详情与验证证据见 docs/superpowers/plans/2026-09-14-kunpeng950-sdc-port.md。

### Phase 7: 对标 x86、强化 ARM64 饱和压测研究（2026-09-16 第六轮） — in_progress
- [ ] R1 x86 专属能力基线盘点（smi/rdrand/rdtsc/io/affinity 等 x86-guarded 代码全集 + 对应 ARM64 等价物缺口）
- [ ] R2 前沿 SDC/压力激发研究（SILENT/SICE/Google fARM/文献：什么负载形态最能激发静默数据损坏）
- [ ] R3 ARM64 微架构压测文献（SPE/Ptrauth/MTE/SVE2/LSU/一致性协议压力、Ampere/Graviton/Kunpeng 白皮书）
- [ ] R4 "打满"缺口分析：哪些 ARM64 单元（NEON/SVE2×2、LSU、L1/L2/L3/TLB、互联/NoC、ccNUMA 一致性）现有 stressor 压不饱和
- [ ] 综合撰写实现方案（结合已落地的 11 patch，避免重复；输出可执行 patch 清单）

> 注：Phase 6 已落地的能力（SVE2 march/`--taskset physical`/sdc-scan/sdc-run/fma-vecfp-matrix 位级诊断/sve2/ls64/crc32）是本轮研究的基线，方案不得重复造轮子。
