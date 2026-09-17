# Progress Log

## Session 2026-09-17（第八轮）: GitHub Actions 多 OS 自动化验证工作流

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
