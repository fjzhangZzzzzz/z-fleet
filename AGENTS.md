# Repository Guidelines

## 项目结构与模块组织

`z-fleet` 是基于 C++20 的 monorepo，使用 CMake + vcpkg manifest mode。应用入口位于 `apps/agent/` 与 `apps/server/`。共享库位于 `libs/`，当前包含 `core/`、`crypto/`、`package/`、`platform/`、`protocol/`、`transport/`，各模块通常采用 `src/`、`include/`、`tests/` 结构。跨组件测试放在 `tests/integration/`，测试辅助代码放在 `tests/support/`。设计与维护文档位于 `docs/`，ADR 位于 `docs/adr/`。

## 构建、测试与开发命令

Linux 环境优先使用容器化构建脚本 `builder.sh`，其次再使用通用 `build.sh`，以保证与 CI 和发布流程一致：

- `./scripts/builder.sh linux-debug`：优先方式，使用容器化流程构建 Linux Debug 预设。
- `./scripts/builder.sh linux-release`：优先方式，使用容器化流程构建 Linux Release 预设。
- `./scripts/build.sh linux-debug`：非容器化备用方式，引导 vcpkg、配置 CMake，并构建 Linux Debug 预设。
- `./scripts/build.sh linux-release`：非容器化备用方式，构建 Linux Release 预设。
- `./scripts/test.sh linux-debug`：运行 Linux Debug 预设下的 `ctest`。
- `cmake --preset linux-debug`：仅配置，适合迭代 CMake 文件。

Windows 下使用 Git Bash 执行对应的 `windows-debug` 与 `windows-release` 预设。

## 编码风格与命名约定

生产代码与文档中的 C++ 片段默认遵循 Google C++ Style Guide。仓库内约定包括：2 空格缩进、同一行大括号、保持与现有代码一致的小型翻译单元。命名空间采用项目前缀（如 `zfleet::core`），公开头文件放在 `include/zfleet/...`。源码和测试文件使用小写蛇形命名，如 `version.cpp`、`version_test.cpp`。文档文件名保持稳定：核心文档使用英文名（如 `architecture.md`），ADR 使用 `NNNN-short-name.md`。

## 测试约定

当前测试以接入 CTest 的轻量可执行检查为主。新增单元测试请放在对应模块目录（`libs/*/tests/` 或 `apps/*/tests/`）；仅端到端流程放到 `tests/integration/`。测试文件名使用 `_test.cpp` 后缀。提交 PR 前至少运行一次 `./scripts/test.sh linux-debug`。

## 提交与 PR 约定

提交信息采用简短约定式前缀，如 `feat:`、`fix:`、`docs:`、`build:`、`test:`，主题使用祈使句并尽量聚焦。PR 需说明问题背景、变更摘要、验证命令，并关联对应 issue 或 ADR。仅在引入 UI 变更时附截图。

## 文档与安全说明

文档维护入口在 `docs/README.md`。除标识符、命令、路径或外部标准外，文档正文默认使用中文。不要引入绕过 `docs/security.md` 中既定安全模型的执行方式、可写任务或路径更新。
