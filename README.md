# z-fleet

**z-fleet** 是一个面向个人开发者、小团队、Homelab、边缘设备和测试环境的轻量端点探针与管理框架。

当前项目处于**初始设计阶段**，尚未实现 Agent、Server 或可运行功能。本 README 作为项目愿景、边界和开发维护原则的设计源文件，后续代码、文档和 ADR 应与本文同步演进。

项目目标是构建一个**高性能、低占用、安全、可审计、可自更新、可持续演进**的多设备管理系统。当前定位不是完整 RMM、不是远控工具、不是 EDR，而是一个用于“资产可见、状态可知、变更可查、任务可控、更新可信”的端点基础设施。

z-fleet 只用于**用户拥有或获得授权管理的设备**。

## 1. 项目目标

z-fleet 的核心目标：

- 使用 C++ 作为 Agent 和 Server 的主语言，优先服务于低占用、跨平台、长期运行和可控部署；
- Agent 极致轻量，空闲时 CPU 接近 0，内存占用可控；
- Agent 主动连接 Server，不暴露客户端端口；
- 支持多系统、多架构设备统一注册、心跳、资产采集和管理；
- 所有任务可审计，默认只读，写入能力显式开启；
- 任务、协议、审计事件、配置和采集结果尽量结构化，便于测试、审查和长期维护；
- 自更新支持签名校验、灰度、回滚和失败保护；
- 项目通过文档驱动设计，代码和文档同步演进；
- AI 可作为审查、测试生成和维护辅助工具，但不作为绕过策略的执行入口。

## 2. 非目标

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

## 3. 阶段路线图

### 3.1 v0.1 最小闭环

目标是产生可运行、可验证的端到端闭环：

- Agent 启动后生成或读取本地身份；
- Agent 主动连接 Server；
- Server 接收注册和心跳；
- Agent 上报基础资产信息；
- Server 持久化设备、心跳和资产快照；
- 所有注册、心跳、资产上报和错误路径生成审计事件；
- 提供最小测试和本地运行说明。

v0.1 暂不支持 Shell、插件、自更新、复杂 UI 和高风险写入任务。

### 3.2 v0.2 任务和审计模型

目标是建立受控任务能力：

- 定义任务模型、任务状态机和错误码；
- 支持只读任务下发和结果回传；
- 建立任务审计日志；
- 引入策略开关和任务能力分级；
- 增加协议版本和兼容性约束。

### 3.3 v0.3 更新和跨平台验证

目标是提升可部署性和可靠性：

- 设计自更新签名、灰度、回滚和失败保护；
- 建立 Linux 与 Windows 的最小真实设备验证流程；
- 增加包构建、发布和回滚文档；
- 完善配置迁移和 Agent / Server 兼容策略。

### 3.4 长期方向

长期能力包括：

- 受限插件系统；
- 更完整的资产模型；
- 更丰富的策略和审批流程；
- Web UI 或轻量管理界面；
- 多设备巡检报告；
- 与 AI 辅助审查、测试生成和运维报告集成。

## 4. 开发维护规范

### 4.1 工作原则 / 指导思想

z-fleet 采用以下原则：

1. **先最小闭环，再扩展能力**  
   每个阶段都必须产生可运行、可验证的结果。

2. **默认只读，写操作显式授权**  
   资产采集、状态检查优先；修改系统、执行脚本、删除文件等操作必须经过更严格的策略、审批和审计。

3. **Agent 主动出站连接**  
   客户端不暴露端口，降低部署和安全风险。

4. **安全能力先于便利能力**  
   注册、身份、审计、任务授权、自更新签名应尽早设计，不等功能复杂后再补。

5. **威胁模型先于高风险能力**  
   注册、任务下发、自更新、密钥管理、插件系统和写入任务必须先有威胁模型，再进入实现。

6. **结构化数据优先**  
   采集结果、任务、审计事件、配置、错误码和协议消息尽量使用结构化模型，避免只返回不可解析字符串。

7. **文档即设计源文件**  
   新能力先写 mini design，再实现；架构变化必须同步更新文档或 ADR。

8. **AI 作为审查者和辅助者，不作为无约束执行者**  
   AI 可以帮助生成设计、测试、审查代码和分析报告，但不应直接绕过策略执行危险动作。

9. **真实设备验证优先**  
   涉及跨平台或系统交互能力的阶段，至少在一台 Linux 和一台 Windows 设备上验证。

10. **兼容性显式管理**  
    协议版本、Agent 最低兼容 Server 版本、配置迁移和数据库迁移必须有文档记录。

11. **可观测性内建**  
    日志、错误码、请求 ID、Agent ID、任务 ID 和审计事件应从早期阶段纳入设计。

### 4.2 任务能力分级

任务能力按风险分级管理：

- `readonly`：只读取系统状态或资产信息，不改变目标设备；
- `low_risk_write`：低风险写入，例如更新 Agent 本地配置；
- `high_risk_write`：高风险写入，例如修改系统配置、安装软件、删除文件；
- `shell`：任意命令或脚本执行，默认不启用，且不属于早期阶段目标。

每个任务类型都应定义：

- 输入和输出结构；
- 成功、失败、超时和取消路径；
- 所需权限和默认开关；
- 审计字段；
- 测试用例；
- 是否影响协议或配置兼容性。

### 4.3 文档如何参与开发流程

推荐流程：

```text
需求想法
  -> 新建或更新 docs 中的设计文档
  -> 必要时新增 ADR
  -> 拆分 Issue
  -> 进入 Milestone
  -> 编码实现
  -> 单元测试 / 集成测试 / 真实设备验证
  -> 更新文档状态
  -> 合并代码
  -> 更新 CHANGELOG
```

每个新能力至少应该回答：

- 目标是什么？
- 非目标是什么？
- 涉及哪些组件？
- 数据结构是什么？
- 成功和失败路径是什么？
- 安全边界是什么？
- 如何测试？
- 是否影响已有协议、配置、数据库或部署方式？

### 4.4 ADR 使用规则

ADR，即 Architecture Decision Record，用于记录关键架构决策。

适合写 ADR 的场景：

- 是否使用 C++；
- 是否采用 monorepo；
- Agent 是主动连接还是被动监听；
- 是否允许任意 Shell；
- 使用 SQLite 还是 PostgreSQL；
- 插件系统采用外部进程、动态库还是 WASM；
- 协议使用 HTTP、WebSocket、gRPC 还是自定义传输；
- Agent 身份、注册和密钥轮换策略；
- 自更新签名和发布链路。

ADR 状态：

- `Draft`：草案；
- `Accepted`：已接受；
- `Implemented`：已实现；
- `Deprecated`：不再推荐；
- `Superseded`：被新 ADR 替代。

### 4.5 文档状态管理

每份核心文档建议带状态头：

```markdown
# Agent 设计

Status: Draft
Last updated: 2026-05-12
Related milestones: v0.1, v0.2, v0.3
```

状态含义：

- `Draft`：草案，允许大幅调整；
- `Accepted`：设计已确认，可以实现；
- `Implemented`：对应代码已经落地；
- `Deprecated`：设计不再推荐；
- `Superseded`：被其他文档或 ADR 替代。

### 4.6 AI 友好和介入方式

z-fleet 的文档和代码应尽量方便 AI 参与：

- 所有设计文档使用 Markdown；
- 协议、任务、审计、配置尽量结构化；
- 每个模块有明确边界；
- 新功能有 mini design；
- 重要决策有 ADR；
- 测试用例覆盖状态机和错误路径；
- 目录结构稳定，命名清晰。

AI 可以介入：

- 审查架构设计；
- 根据设计生成测试计划；
- 检查代码是否违反文档约束；
- 生成错误码表；
- 分析日志和任务结果；
- 生成巡检报告；
- 检查插件 manifest 权限是否过大；
- 根据 ADR 检查实现是否偏离原则。

不建议 AI 直接：

- 绕过审批执行任务；
- 自动下发高风险任务；
- 自动修改真实设备关键配置；
- 自动发布更新包。

## 5. 推荐项目骨架

初始阶段建议采用 monorepo，但保持目录简单，避免过早引入插件市场、复杂 UI 或多租户平台。

```text
z-fleet/
  README.md
  LICENSE
  CHANGELOG.md
  SECURITY.md
  CONTRIBUTING.md
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  vcpkg-configuration.json
  .vcpkg-version

  docs/
    README.md
    architecture.md
    contracts.md
    security.md
    operations.md
    roadmap.md
    adr/
      template.md
      0001-use-cpp.md
      0002-agent-outbound-connection.md
      0003-default-read-only-tasks.md

  apps/
    agent/
      CMakeLists.txt
      include/
      src/
      tests/
    server/
      CMakeLists.txt
      include/
      src/
      tests/

  libs/
    core/
      CMakeLists.txt
      include/
      src/
      tests/
    protocol/
      CMakeLists.txt
      include/
      src/
      tests/
    platform/
      CMakeLists.txt
      include/
      src/
      tests/

  tests/
    integration/

  tools/
    dev/
    packaging/

  scripts/
    bootstrap-vcpkg.sh
    build.sh
    format.sh
    test.sh

  .github/
    workflows/
      ci.yml
```

建议先搭建文档和最小 C++ 工程骨架，再实现 v0.1 最小闭环。

源码目录职责：

- `apps/agent/`：Agent 可执行程序，负责生命周期、注册、心跳、任务执行调度和平台能力编排；
- `apps/server/`：Server 可执行程序，负责服务入口、设备注册、心跳接收、任务下发和持久化编排；
- `libs/core/`：通用基础设施，例如版本、日志、时间、ID、配置解析和通用错误处理；
- `libs/protocol/`：Agent / Server 共享协议契约，包括消息结构、任务模型、审计事件、错误码和协议版本；
- `libs/platform/`：Linux / Windows 平台差异封装，包括系统信息、资产采集、路径、权限和服务管理；
- `tests/integration/`：跨组件真实链路测试，例如注册、心跳、资产上报和任务状态流转。

### 5.1 文档组织原则

`docs/README.md` 是唯一文档入口，维护文档索引、阅读顺序、状态、适用阶段和最后更新日期。

核心文档保持少而稳定：

- `architecture.md`：总体架构、组件边界、应用和共享库的职责划分；
- `contracts.md`：协议、任务模型、审计事件、错误码和兼容性约束；
- `security.md`：威胁模型、权限边界、任务风险等级和安全默认值；
- `operations.md`：本地运行、构建、部署、配置、升级、回滚和排障；
- `roadmap.md`：阶段目标、里程碑和实现状态；
- `adr/`：只记录关键且不应频繁反复的架构决策。

文档维护规则：

- 每个主题只保留一个主文档，其他位置只链接，不复制内容；
- 普通功能设计优先写入对应主文档的小节；
- 只有影响架构、协议、安全、兼容性或长期维护策略时才新增 ADR；
- 单篇文档长期超过 300-500 行，或读者群明显分离时，再考虑拆分。

## 6. 构建方式

项目使用 CMake + vcpkg manifest mode。vcpkg tool 版本由 `.vcpkg-version` 固定，依赖 registry baseline 由 `vcpkg-configuration.json` 固定。

本地和 CI 共用同一套 Bash 脚本与 CMake presets：

```bash
./scripts/build.sh linux-debug
./scripts/test.sh linux-debug
```

Windows 本地构建要求在 Git Bash 中执行脚本，并提前安装 Visual Studio Build Tools、CMake、Ninja 和 Git。

标准 preset：

- `linux-debug`
- `linux-release`
- `windows-debug`
- `windows-release`

构建脚本会将 vcpkg 安装到 `.tools/vcpkg/`，并使用 `.cache/vcpkg/archives/` 作为本地二进制缓存。这些目录以及 `build/`、`vcpkg_installed/` 不应提交。

## 7. 许可证

本项目使用 MIT License。详见 [LICENSE](LICENSE)。
