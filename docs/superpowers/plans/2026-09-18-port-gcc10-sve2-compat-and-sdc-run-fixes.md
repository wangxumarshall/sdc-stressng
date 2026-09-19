# Plan: 移植 sdc-stressng-sve 增量修改（GCC10 SVE2 兼容 + sdc-run.sh 修复）

## Goal

`../sdc-stressng-sve` 相对本仓库（`port/arm64-saturation-sdc`）的**真实增量 = 单提交 `f18db7245`**（`git diff aac012d56..HEAD` 仅 4 文件，6+/5-）。工作树里 1327 个 "modified" 文件是 CRLF 行尾噪声（`git diff --ignore-all-space` 为空），非内容变更。

把该增量作为**一个 patch 单元**合入本仓库，并充分验证。

## 增量内容（已核实，逐行对照）

| 文件 | 变更 |
|---|---|
| `stress-armcrypto.c:502` | `armv9-a` → `armv8.6-a`（SVE2_TARGET 宏） |
| `stress-fma.c:262` | `armv9-a` → `armv8.6-a`（FMA_SVE2_TARGET 宏） |
| `stress-regs.c:2247` | `armv9-a` → `armv8.6-a`（REGS_SVE_TARGET 宏） |
| `scripts/sdc-run.sh:106` | `SDCSHIELD_ARG="--sdcshield $2"` → `SDCSHIELD_ARG="$2"`（存裸命令，修复 rc=127） |
| `scripts/sdc-run.sh:285` | 前台 SDCShield 用 `timeout --signal=TERM --kill-after=30 $((dur+120))` 包裹（防 `-T forever` 永不退出） |

理由（来自提交说明）：GCC 10.3 拒绝 `armv9-a` target 属性（GCC 11+ 才支持），`armv8.6-a+sve2` 语义等价且 GCC10/11+/BiSheng 均接受 → 加宽工具链覆盖、新编译器无行为变化。已验证于 Kunpeng 920F 真机（608 核 / SVE2-512+SME / Kylin V10 / GCC 10.3.1）。

## Phases

### Phase 1: 应用补丁 — complete
- [x] 3 处 C 文件 `armv9-a` → `armv8.6-a`
- [x] 2 处 sdc-run.sh 修复
- 逐字节核对：4 文件与源提交 f18db7245 完全一致（git diff --no-index 全 MATCH）

### Phase 2: 构建验证 — complete
- [x] `make -j$(nproc)` 链接成功（`LD stress-ng`，产出 3.2MB ELF）。告警均为既有（missing-field-initializers/unused-parameter），与本次字符串改动无关

### Phase 3: 功能验证 — complete
- [x] `./stress-ng --regs 1 -t 5` → `passed: 1: regs (1)`
- [x] `./stress-ng --fma 1 --verify -t 5` → `passed: 1: fma (1)`
- [x] `./stress-ng --armcrypto 1 -t 5` → `passed: 1: armcrypto (1)`
- [x] `bash -n scripts/sdc-run.sh` 语法通过；`timeout` 位于 /usr/bin/timeout

### Phase 4: 回归验证 — complete
- [x] `./stress-ng --zombie 1 -t 5` → `passed: 1: zombie (1)`

### Phase 5: x86 非回归审查 — complete
- [x] 3 处改动均在 ARM-only guard 内：armcrypto/fma 在 `STRESS_ARCH_ARM && __aarch64__ && HAVE_ARM_NEON_CRYPTO`；regs 在 `HAVE_ARM_NEON_CRYPTO`（ARM 探针派生）。纯 target 字符串单 token 替换，x86 零接触

### Phase 6: 提交 + 推送 — `in_progress`
- [ ] 单 commit（one-patch-per-unit）
- [ ] push 到 `port/arm64-saturation-sdc`（非 main）

## Next Step

Phase 6：提交（单 commit）并 push 到 `port/arm64-saturation-sdc`。
