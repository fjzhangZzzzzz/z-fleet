# z-fleet

**z-fleet** 是一个面向个人开发者、小团队、Homelab、边缘设备和测试环境的轻量端点探针与管理框架。

项目目标是构建一个高性能、低占用、安全、可审计、可自更新、可持续演进的多设备管理系统。当前定位不是完整 RMM、不是远控工具、不是 EDR，而是用于“资产可见、状态可知、变更可查、任务可控、更新可信”的端点基础设施。

z-fleet 只用于用户拥有或获得授权管理的设备。

## 当前状态

项目处于 `v0.1` 最小闭环已打通、后续能力待扩展的阶段，当前已具备：

- C++20 / CMake / vcpkg manifest mode 基础工程；
- Agent、Server、Core、Protocol、Platform 的初始目录和 CMake 目标；
- `docs/` 文档骨架和 ADR 目录；
- v0.1 三方库依赖基线；
- Agent 启动后生成或复用本地身份，并主动连接 Server；
- Server 接收注册、心跳和资产上报并持久化到 SQLite；
- 已归类请求的成功路径和失败路径生成审计事件；
- 端到端集成测试和最小本地运行说明。

## 核心目标

- 使用 C++ 作为 Agent 和 Server 的主语言，优先服务于低占用、跨平台、长期运行和可控部署。
- Agent 主动连接 Server，不暴露客户端端口。
- 支持多系统、多架构设备统一注册、心跳、资产采集和管理。
- 所有任务可审计，默认只读，写入能力显式开启。
- 任务、协议、审计事件、配置和采集结果尽量结构化，便于测试、审查和长期维护。
- 自更新支持签名校验、灰度、回滚和失败保护。
- 项目通过文档和 ADR 驱动关键设计，代码和文档同步演进。
- AI 可作为审查、测试生成和维护辅助工具，但不作为绕过策略的执行入口。

## 非目标

早期阶段明确不做：

- 不做隐蔽远控；
- 不做绕过安全软件、免杀、提权、未授权安装；
- 不默认支持任意 Shell 命令执行；
- 不做完整 RMM；
- 不做 EDR / 杀毒 / 入侵检测；
- 不做复杂多租户商业平台；
- 不一开始追求漂亮 UI 或复杂插件市场；
- 不在安全模型完成前提供高风险写入任务；
- 不把 AI 自动决策作为任务下发或系统变更的授权来源。

## v0.1 优先级

v0.1 的目标是产生可运行、可验证的端到端最小闭环：

- Agent 启动后生成或读取本地身份；
- Agent 主动连接 Server；
- Server 接收注册和心跳；
- Agent 上报基础资产信息；
- Server 持久化设备、心跳和资产快照；
- 注册、心跳、资产上报和错误路径生成审计事件；
- 提供最小测试和本地运行说明。

v0.1 暂不支持 Shell、插件、自更新、复杂 UI 和高风险写入任务。完整路线图见 [docs/roadmap.md](docs/roadmap.md)。

## 项目结构

```text
z-fleet/
  apps/
    agent/      # Agent 可执行程序
    server/     # Server 可执行程序
  libs/
    core/       # 通用基础设施
    protocol/   # 共享协议契约
    platform/   # 平台差异封装
  tests/
    integration/
  docs/
    adr/
```

组件职责、依赖选型和运行模型见 [docs/architecture.md](docs/architecture.md)。

## 构建

项目使用 CMake + vcpkg manifest mode。vcpkg tool 版本由 `.vcpkg-version` 固定，依赖 registry baseline 由 `vcpkg-configuration.json` 固定。

本地和 CI 共用同一套脚本与 CMake presets：

```bash
./scripts/build.sh linux-debug
./scripts/test.sh linux-debug
```

Windows 本地构建要求在 Git Bash 中执行脚本，并提前安装 Visual Studio Build Tools、CMake、Ninja 和 Git。

更多构建、配置、运行和排障说明见 [docs/operations.md](docs/operations.md)。

## 文档入口

[docs/README.md](docs/README.md) 是项目文档入口，维护阅读顺序、文档状态和维护规则。

核心文档：

- [架构](docs/architecture.md)：组件边界、三方库选型、运行形态和模块职责；
- [契约](docs/contracts.md)：协议消息、任务模型、审计事件、错误码和兼容性约束；
- [安全](docs/security.md)：威胁模型、安全默认值、身份、授权和任务风险分级；
- [运维](docs/operations.md)：本地运行、配置、构建、部署、升级和排障；
- [路线图](docs/roadmap.md)：里程碑状态和阶段性交付范围；
- [ADR](docs/adr/)：关键架构决策记录。

文档正文默认使用中文；必要英文仅限术语、字段名、命令、路径、许可证原文或外部兼容要求。详细规则见 [docs/README.md](docs/README.md)。

## 开发原则

- 先最小闭环，再扩展能力。
- 默认只读，写操作显式授权。
- 安全能力先于便利能力。
- 威胁模型先于高风险能力。
- 结构化数据优先。
- 兼容性显式管理。
- 可观测性内建。
- 新能力先写设计或 ADR，再实现。

## 许可证

本项目使用 MIT License。详见 [LICENSE](LICENSE)。
