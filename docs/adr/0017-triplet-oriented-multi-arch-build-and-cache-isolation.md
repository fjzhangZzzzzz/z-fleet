# ADR 0017：基于 triplet 的多架构构建与缓存隔离

状态：已接受

日期：2026-05-31

## 背景

引入本决策前，仓库在 Linux 侧已具备基础构建与 vcpkg 缓存能力，但默认路径与流程仍以单架构（`x86_64`）为隐含前提，表现为：

- preset 命名未统一体现 triplet 与构建类型；
- `VCPKG_INSTALLED_DIR` 仍是共享目录，未来多架构并行时存在互相覆盖风险；
- CI 缓存粒度偏粗，按 `os` 复用在多架构场景下有污染风险；
- 打包与校验链路存在 `x86_64` 写死项，不利于 `arm64` 扩展。

与此同时，项目希望 preset 和缓存模型具备多架构表达能力：

- `linux-x64` 机器上的本地 `x64` 构建；
- `arm64-linux` 原生环境中的 arm64 构建入口；
- 后续按需扩展其他原生 runner 或发布产物。

为避免过渡态带来的二次迁移成本，需要一次性确定“最大化复用 + 必要隔离”的构建与缓存模型。

## 决策

1. **采用 triplet 导向的 preset 命名规范。**

   统一使用 `<triplet>-<build_type>`，例如：

   - `x64-linux-debug`
   - `x64-linux-release`
   - `arm64-linux-debug`
   - `arm64-linux-release`

2. **vcpkg 安装目录按 triplet 硬隔离。**

   `VCPKG_INSTALLED_DIR` 必须按 triplet 拆分，例如：

   - `build/vcpkg_installed/x64-linux`
   - `build/vcpkg_installed/arm64-linux`

   禁止不同 triplet 共享同一 installed tree。

3. **vcpkg binary cache 在安全边界内复用，CI cache key 按 triplet 隔离。**

   可复用缓存（如 `.cache/vcpkg/archives`）允许使用共享物理目录，由 vcpkg ABI 包名区分具体二进制产物；CI cache key 至少包含 OS 与 triplet 维度，避免不同平台或不同 triplet 的缓存归档互相污染。
   
   CI 的 vcpkg cache key 应以依赖图相关输入为主（如 `.vcpkg-version`、`vcpkg.json`、`vcpkg-configuration.json`），避免把与依赖无关且高频变更的构建编排文件（如 `CMakeLists.txt`、`CMakePresets.json`）作为主要失效因子。

4. **同 triplet 下不按 debug/release 拆分 vcpkg 依赖缓存。**

   在同一 OS、同一 triplet 场景下，debug/release 共享同一套 vcpkg 依赖缓存键空间，避免首次构建阶段重复编译依赖。

5. **本地与 CI 的平台职责分离。**

   当前不把 `arm64-linux-*` preset 作为 `linux-x64` 宿主上的跨架构构建入口。`arm64-linux-*` preset 保留为 arm64 Linux 原生环境入口；若后续重新引入跨架构构建，应新增独立 ADR 或更新本 ADR 的工具链、host tool 和验证边界。

6. **清理硬编码架构字段并统一参数化。**

   打包、校验、工作流中的 `x86_64` 字面量改为参数化，统一由 triplet/arch 映射驱动。

7. **一次切换到目标模型，不保留过渡方案。**

   不采用“先按 os、后按 triplet”的阶段性方案，直接落地 triplet 隔离与命名统一。

## 备选方案

- **维持共享 installed tree，仅新增 arm64 preset**：改动最小，但缓存与安装目录污染风险高，后续返工不可避免，已拒绝。
- **先按 `os + arch` 过渡，再升级到 triplet**：短期可行，但会形成二次迁移与双套规则维护成本，已拒绝。
- **把 arm64 preset 设计为 x64 Linux 跨架构构建入口**：会引入 host tool、目标 sysroot、工具链文件和发布可信度问题，当前范围内已拒绝。

## 影响

- 正向影响：
  - 多架构构建边界清晰，避免缓存和 installed tree 污染；
  - 本地验证与 CI 发布职责明确，发布链路可信度提升；
  - 命名、目录、缓存键统一后，后续扩展新 triplet 成本降低。
  - CI 主链路与发布链路共享 vcpkg binary archives 后，冷启动之外的依赖重复编译减少。

- 代价与约束：
  - 需要同步调整 `CMakePresets`、构建脚本、CI workflow 与打包校验参数；
  - CI 缓存按 OS 与 triplet 拆分，短期内缓存命中率可能下降；
  - 需要一次性完成历史 `x86_64` 硬编码清理。

- 与既有决策关系：
  - 继承 [ADR 0012](0012-cross-platform-script-boundary.md) 的跨平台脚本边界约束；
  - 与 [ADR 0014](0014-component-versioning-and-tag-driven-release.md) 的发布策略协同，确保多架构产物命名与校验可追踪。

## 当前实现

当前仓库按以下方式落地本决策：

- `CMakePresets.json` 使用 `<triplet>-<build_type>` 命名，覆盖 `x64-linux-*`、`arm64-linux-*` 和 `x64-windows-*`。
- 每个 triplet 使用独立 `VCPKG_INSTALLED_DIR`：`build/vcpkg_installed/<triplet>`；该目录用于本地构建输出，不进入 CI cache。
- `VCPKG_BINARY_SOURCES` 由 CMake preset environment 固定为 `.cache/vcpkg/archives`，本地与 CI 复用 vcpkg binary archives。
- CI cache 仅覆盖 `.cache/vcpkg/archives` 与 `.tools/vcpkg/downloads`，key 使用 `runner.os + triplet + .vcpkg-version/vcpkg.json/vcpkg-configuration.json hash`，restore key 保持在同 OS、同 triplet 前缀内。
- CI 主链路与 release-packaging 均使用 warmup job 先执行 `vcpkg.sh bootstrap` 和 `cmake --preset`，再让构建/打包 job restore 同一缓存前缀。
- `vcpkg-configuration.json` 使用 builtin registry baseline 固定官方 vcpkg ports；当前不使用 `overlay-ports` 或 `overlay-triplets`。
- 构建入口统一为 Bash 脚本。`scripts/build.sh` 负责 bootstrap 固定版本 vcpkg 并调用 CMake preset；`scripts/vcpkg.sh` 仅保留 `bootstrap` 与 `exec` 辅助命令。
- 当前 CI 仅验证 `x64-linux-release` 与 `x64-windows-release`。`debug` preset 保留给本地开发与排障；`arm64-linux-*` preset 保留给 arm64 Linux 原生环境，不作为 x64 Linux 宿主上的跨架构构建入口。

若后续重新引入 `arm64` 正式 runner、跨架构构建、debug CI job、自定义 triplet 或 overlay port，应更新本 ADR 或新增 ADR，明确新的工具链、host tool、缓存隔离和验证边界。
