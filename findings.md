# Findings: stress-ng × SDCShield 协同 SDC 压测（v3，2026-09-16 第六轮更新）

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
- **关键洞察（预期修正）**：问题不是"没有随机"，而是"随机的形状不对"——均匀随机对位段敏感缺陷的覆盖效率低（52 位尾数空间均匀撒点 vs 定向位段扫掠）；固定模式（0x55/0xAA 类）又走向另一极端。**方案方向 = 模式字典 × 均匀随机的混合生成器**



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
