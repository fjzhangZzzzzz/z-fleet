# ADR 0016：容器化构建边界与 preflight/doctor 升级门禁

状态：草案

日期：2026-05-30

## 背景

当前仓库已经采用容器化构建入口（`scripts/builder.sh`），并通过 `zfleet_packager` / `zfleet_installer` 形成 manifest 驱动的安装与回滚流程（见 [ADR 0006](0006-manifest-driven-installer.md)、[ADR 0012](0012-cross-platform-script-boundary.md)）。

在跨平台发布中出现了两个长期问题：

- Linux 构建物在部分目标环境下动态依赖 `libatomic` 等运行时库，若依赖宿主机库会导致可运行边界不可控。
- Windows 构建物存在 `.dll` 运行时依赖，若仅依赖系统环境，升级后稳定性和可诊断性不足。

同时，现有发布检查脚本历史上存在临时短路，导致“构建通过但升级链路失败”的问题无法在发布前被阻断。

因此需要把以下事项收敛为统一决策：

- 容器化构建负责什么、不负责什么；
- 动态库依赖由谁负责闭包；
- 升级前后如何做强制可执行健康门禁（preflight/doctor）。

## 决策

1. **容器化构建只负责产出一致的编译结果，不负责目标机依赖满足。**

   `scripts/builder.sh` 的职责是提供稳定工具链与可复现编译环境；目标机运行时依赖闭包由打包与安装链路负责，不把“容器里能跑”视为“目标机一定能跑”。

2. **运行时依赖闭包进入发布包，且按平台使用固定加载路径。**

   - Linux：发布包在 `payload/lib/` 携带关键运行时库（至少 `libatomic.so*`、`libstdc++.so*`、`libgcc_s.so*`），主可执行使用 `$ORIGIN/../lib` 解析运行时依赖。
   - Windows：发布包携带运行时 `.dll`，并采用应用目录可解析的布局（当前为 `payload/bin`）。

3. **`doctor/preflight` 作为 installer 的正式能力，不做临时脚本替代。**

   新增 `zfleet_installer doctor`（或等价命名）作为统一健康检查入口，覆盖：

   - active-version / previous-version 结构校验；
   - manifest 文件集的存在性、size、sha256 校验；
   - 平台相关运行时依赖可解析性校验（Linux `.so`、Windows `.dll`）。

4. **升级流程必须以 preflight 为门禁，失败即中止并回滚。**

   - `apply` 前：校验当前活动安装健康状态，避免带病升级；
   - `apply` 后、切换前：校验候选版本健康状态；
   - 任一阶段失败：保持或恢复到上一个健康版本，返回非 0。

5. **发布检查脚本不得以临时短路绕过真实安装路径。**

   `scripts/inspect-release-binaries.sh` 与 `scripts/inspect-release-packages.sh` 必须长期保持可执行，作为发布前门禁的一部分。

## 备选方案

- **仅依赖容器运行，不做运行时依赖打包**：实现成本低，但目标机环境漂移会直接导致启动失败，已拒绝。
- **仅靠脚本检查，不引入 installer doctor 子命令**：短期可用，但规则容易分叉，脚本与产品行为不一致，已拒绝。
- **依赖目标机预装运行时（系统库/系统运行库）**：在公网分发和异构环境下不可控，故障定位成本高，已拒绝。

## 影响

- 正向影响：
  - 发布包可运行边界更清晰，跨环境差异显著降低；
  - 构建、打包、安装、升级共享同一套健康判定标准；
  - 发布前可提前暴露升级链路问题，而不是在线上暴露。

- 代价与约束：
  - 包体积上升；
  - 需要维护平台差异化依赖收集与校验逻辑；
  - CI 发布链路需要执行完整的 binaries/package inspection 与 doctor 门禁。

- 与既有决策关系：
  - 继承 [ADR 0006](0006-manifest-driven-installer.md) 的 manifest 驱动安装与回滚模型；
  - 细化 [ADR 0012](0012-cross-platform-script-boundary.md) 中“脚本编排 vs C++ 核心职责”的边界；
  - 补充 [ADR 0015](0015-installer-owned-launcher-stub.md) 的 installer 主导安装模型，在升级门禁层面落实“installer 是单一可信执行入口”。
