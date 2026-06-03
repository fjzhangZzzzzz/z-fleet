# Repository Guidelines

## 项目结构与模块组织

`z-fleet` 是基于 C++20 的 monorepo，使用 CMake + vcpkg manifest mode。应用入口位于 `apps/agent/`、`apps/server/`、`apps/installer/` 与 `apps/packager/`。共享库位于 `libs/`，当前包含 `core/`、`crypto/`、`package/`、`platform/`、`protocol/`、`transport/` 等模块，各模块通常采用 `src/`、`include/`、`tests/` 结构。跨组件测试放在 `tests/integration/`，测试辅助代码放在 `tests/support/`。设计与维护文档位于 `docs/`，ADR 位于 `docs/adr/`。

## 构建、测试与开发命令

Linux 环境优先使用容器化构建脚本 `builder.sh`，其次再使用通用 `build.sh`，以保证与 CI 和发布流程一致。Preset 采用 ADR 0017 定义的 `<triplet>-<build_type>` 命名：

- `./scripts/builder.sh x64-linux-debug`：优先方式，使用容器化流程构建 Linux x64 Debug 预设。
- `./scripts/builder.sh x64-linux-release`：优先方式，使用容器化流程构建 Linux x64 Release 预设。
- `./scripts/build.sh x64-linux-debug`：非容器化备用方式，引导 vcpkg、配置 CMake，并构建 Linux x64 Debug 预设。
- `./scripts/build.sh x64-linux-release`：非容器化备用方式，构建 Linux x64 Release 预设。
- `./scripts/test.sh x64-linux-debug`：运行 Linux x64 Debug 预设下的 `ctest`。
- `./scripts/check.sh boundaries`：检查 Domain-first 边界和外部表示泄漏。
- `cmake --preset x64-linux-debug`：仅配置，适合迭代 CMake 文件。

Windows 下使用 Git Bash 执行对应的 `x64-windows-debug` 与 `x64-windows-release` 预设。

## Domain-first 边界

遵守 ADR 0018。项目内所有业务对象先以 domain type 存在；JSON、Protobuf、TOML、SQLite row、CLI args 和 HTTP DTO 都只是边界上的外部表示或存储表示。

- Domain public header 不得 include 或暴露 protobuf generated header、`nlohmann::json`、TOML node、SQLite handle 或 row 类型。
- 生产代码中的 protobuf generated type 只允许作为控制通道 wire contract 出现在 `libs/protocol` 的 codec/adapter 边界；`apps/**` 与非 protocol 库不得直接 include 或引用 generated protobuf type。
- JSON 库只允许出现在 JSON codec、manifest/config 等明确边界模块中；TOML 库只允许出现在 config/state 边界中；SQLite 类型只允许出现在 store/database 边界中。
- 新增或扩展类型时，先定义或扩展 domain type 和业务 invariant，再设计 `.proto` field、JSON key、TOML key 或 SQL column，并补齐 mapping 测试。
- 引入新的外部表示库时，必须同步更新 ADR 0018、本文档和 `scripts/check-boundaries.sh`。

## 编码风格与命名约定

生产代码与文档中的 C++ 片段默认遵循 Google C++ Style Guide。仓库内约定包括：2 空格缩进、同一行大括号、保持与现有代码一致的小型翻译单元。命名空间采用项目前缀（如 `zfleet::core`），公开头文件放在 `include/zfleet/...`。源码和测试文件使用小写蛇形命名，如 `version.cpp`、`version_test.cpp`。文档文件名保持稳定：核心文档使用英文名（如 `architecture.md`），ADR 使用 `NNNN-short-name.md`。

## 测试约定

当前测试以接入 CTest 的轻量可执行检查为主。新增单元测试请放在对应模块目录（`libs/*/tests/` 或 `apps/*/tests/`）；仅端到端流程放到 `tests/integration/`。测试文件名使用 `_test.cpp` 后缀。提交 PR 前至少运行一次 `./scripts/check.sh boundaries` 和 `./scripts/test.sh x64-linux-debug`。

## 提交与 PR 约定

提交信息采用简短约定式前缀，如 `feat:`、`fix:`、`docs:`、`build:`、`test:`，主题使用祈使句并尽量聚焦。PR 需说明问题背景、变更摘要、验证命令，并关联对应 issue 或 ADR。仅在引入 UI 变更时附截图。

## 文档与安全说明

文档维护入口在 `docs/README.md`。除标识符、命令、路径或外部标准外，文档正文默认使用中文。不要引入绕过 `docs/security.md` 中既定安全模型的执行方式、可写任务或路径更新。
