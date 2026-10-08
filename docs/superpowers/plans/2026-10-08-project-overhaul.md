# 2026-10-08 第十四轮：顶级开源项目化改造（SDC 激发引擎）

> 完整方案与研究数据见 findings.md §12 与 task_plan.md（第十四轮版）。
> 本文件为入库快照：决策、执行清单与验证证据。

## 背景与目标

用户指令：本项目是 stress-ng 的 fork，核心定位 = arm64 SDC **激发**（相对
../sdcshield 的检测角色），把所有计算消耗在最大程度激发 CPU SDC 上；
要求彻底改造为顶级完备的 GitHub 项目（GitHub 信息/项目信息/架构设计/
方案实现/验证）。

## 用户决策（AskUserQuestion 2026-10-08）

| # | 决策 |
|---|---|
| D14-1 | README 纯英文 |
| D14-2 | excite + full 双模式（纯激发模式落地） |
| D14-3 | 定期 merge 上游 + 删上游 workflow/FUNDING/.travis |
| D14-4 | 全部 A-E 本轮执行 |
| D14-5 | 版本 0.22.00-sdc.1；项目名/二进制名不变 |
| D14-6 | Release 打 tag 推迟到 CI 15 镜像全绿后 |

## 调研核心发现

1. **repo 元数据是上游原文**（description/homepage/topics 空）——fork 无 GitHub 身份
2. **FUNDING.yml = 上游作者收款账号**；.travis 废弃；3 个上游 workflow
   （stable 会在 Release 时自动触发上游镜像流）
3. 治理文件/模板/CHANGELOG/Release/用户文档全缺（G-1..G-10 差距表，findings §12.1）
4. fork 规模：110 commits / 100 files / +11315；上游基线 0.22.00 vs 最新 V0.22.01

## 架构交付：五层激发引擎

L5 编排（sdc-run 六模式）→ L4 杠杆（di/dt/热/SMT/并发/soak）→ L3 攻击面
（ALU/向量/crypto/atomics/LSU/缓存/MMU/互联）→ L2 数据形状（bitgen 五族）→
L1 硬件感知（HWCAP/拓扑/march 注入/诚实跳过）。
配套交付：**激发覆盖率矩阵**（杠杆×通路，●◐○）+ gap 路线图（互联定向/lrcpc/
SMT×MMU/OoO/真机校准）——见 docs/excitation-guide.md。

## 执行清单（12 commits，4b74c6047..7ea0daedd）

| 组 | 内容 | Commit |
|---|---|---|
| A1 | 元数据（description/topics×12/wiki off/homepage 空） | API 200×2 |
| A2 | README 英文门面（badge/五层图/快速上手/编排器/CI/安全） | 7a8920737 |
| A3 | SECURITY/CONTRIBUTING/SUPPORT | b984d8f62 |
| A4 | CHANGELOG（Release 待 CI 绿） | eb127c60a |
| A5 | issue forms×3 + PR 模板 | 28bf16304 |
| A6 | 上游残留×5 删除 | 158ab37c2 |
| B1-B4 | architecture/excitation-guide/sdcshield-integration/upstream-sync + .gitattributes | a30695743/00a7323a8 |
| C1 | sdc-run excite 纯激发模式 + usage 截断修复 | 2de8f41d5 |
| C2 | 模式配方审计入库 | 12197871c |
| C3 | VERSION=0.22.00-sdc.1 | 67050702d |
| D1/D2 | badge×4 + release-image.yml | 7ea0daedd |

## 验证证据

- 构建：make clean 全量 rc=0（gcc 12.3.1）；--version = 0.22.00-sdc.1
- E1：34 选项/命令实跑核查全过（5 项 --help grep 假阴性，实跑排除）
- E2：operand-var/addrspace --verify 零失配；excite 10s rc=0 failed:0
- E4：全部文档相对链接目标存在；badge URL 格式正确
- E3：multi-os-verify dispatch 204（head=7ea0daedd），结果待收（绿→tag+Release）
- issue forms / release-image YAML：python yaml.safe_load 校验过

## 遗留

- E3 CI 全绿确认 → tag v0.22.00-sdc.1 + GitHub Release（notes=CHANGELOG 节）
  → release-image.yml E2E（ghcr :stable）
- 前序轮次遗留不变：ci-trend 周对比、CP1 真机 A/B、bandwalk 真机校准
