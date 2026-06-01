# z-fleet 文档

状态：草案
最后更新：2026-05-25
关联里程碑：v0.1, v0.2, v0.3

本目录是 z-fleet 的唯一文档入口。README.md 是项目愿景和边界的设计源文件，本目录用于承接可实现的架构、协议、安全、运维、路线图和 ADR 记录。

## 阅读顺序

1. [项目 README](../README.md)：项目目标、非目标、阶段路线图和维护原则。
2. [架构](architecture.md)：组件边界、三方库选型、运行形态和模块职责。
3. [契约](contracts.md)：协议消息、任务模型、审计事件、错误码和兼容性约束。
4. [安全](security.md)：威胁模型、安全默认值、身份、授权和任务风险分级。
5. [运维](operations.md)：本地运行、配置、构建、部署、发布、升级、回滚和排障。
6. [路线图](roadmap.md)：里程碑状态和阶段性交付范围。
7. [测试分层](testing.md)：单元、组件、集成测试边界与矩阵。
8. [ADR](adr/)：关键架构决策记录。

## 文档

| 文档 | 状态 | 领域 |
| --- | --- | --- |
| [架构](architecture.md) | 草案 | 总体架构、组件边界、依赖选型 |
| [契约](contracts.md) | 草案 | 协议、任务、审计、错误码 |
| [安全](security.md) | 草案 | 威胁模型、安全边界、策略默认值 |
| [运维](operations.md) | 草案 | 构建、配置、运行、部署、发布、升级、回滚、排障 |
| [测试分层](testing.md) | 草案 | 测试边界、目录拆分、覆盖矩阵 |
| [路线图](roadmap.md) | 草案 | v0.1 / v0.2 / v0.3 和长期方向 |
| [ADR 模板](adr/template.md) | 草案 | ADR 模板 |
| [ADR 0007](adr/0007-agent-control-channel-http2-protobuf-lite.md) | 已接受 | Agent/Server 主控制通道协议决策 |
| [ADR 0008](adr/0008-server-persistence-and-connection-concurrency.md) | 已接受 | Server 持久化边界与连接并发模型 |
| [ADR 0009](adr/0009-server-async-io-and-store.md) | 已接受 | Server 异步 I/O 与异步 Store 架构 |
| [ADR 0010](adr/0010-web-management-entry.md) | 已接受 | Server 内置 Web 管理入口、安装包 channel 与注册 token |
| [ADR 0011](adr/0011-agent-package-install-upgrade.md) | 草案 | Agent 安装包发布、安装与升级模型 |
| [ADR 0012](adr/0012-cross-platform-script-boundary.md) | 草案 | 跨平台构建、打包与安装脚本边界 |
| [ADR 0013](adr/0013-web-ui-information-architecture-and-interaction-guidelines.md) | 已接受 | Web 管理界面的信息架构与交互约束 |
| [ADR 0014](adr/0014-component-versioning-and-tag-driven-release.md) | 已接受 | 组件级版本管理与标签驱动发布 |
| [ADR 0015](adr/0015-installer-owned-launcher-stub.md) | 已接受 | installer 主导的 launcher stub 与版本切换入口治理 |
| [ADR 0016](adr/0016-container-build-and-preflight-gate.md) | 已接受 | 容器化构建边界与 preflight/doctor 升级门禁 |
| [ADR 0017](adr/0017-triplet-oriented-multi-arch-build-and-cache-isolation.md) | 已接受 | 基于 triplet 的多架构构建、缓存隔离与本地/CI职责边界 |

## 文档状态枚举

本文档是项目文档状态枚举的唯一说明源。`docs/*.md` 与 `docs/adr/*.md` 应统一使用以下中文状态值：

- `草案`：内容仍可能大幅调整，不能视为最终约束。
- `已接受`：设计已确认，可以作为实现依据。
- `已实现`：对应实现已经落地，文档应与代码保持一致。
- `已废弃`：不再推荐继续采用，但仍保留历史参考价值。
- `已替代`：已被新文档或新 ADR 取代，应显式链接替代项。

## 维护规则

- 每个主题只保留一个主文档，其他位置只链接，不复制正文。
- 普通功能设计优先写入对应主文档的小节。
- 影响架构、协议、安全、兼容性或长期维护策略时新增 ADR。
- 所有文档状态头统一使用中文枚举；状态语义只在本文件维护，不在各文档重复解释。
- 文档或 ADR 的状态变更后，应同步更新本文件中的文档索引表。
- C++ 代码风格以 Google C++ Style Guide 为参考，并结合本项目现有接口约定统一收敛。
- 目录名、文件名、局部变量、函数参数和命名空间使用 `snake_case`。
- 类型名使用 `PascalCase`；成员变量使用 `snake_case_`。
- 具有创建、加载、写入、解析、更新、执行等动作或副作用的函数使用 `PascalCase`，例如 `LoadConfig`、`LoadOrCreateState`。
- 轻量只读访问函数可使用 `snake_case()`，适用于稳定值、属性读取和无副作用查询，例如 `project_name()`、`protocol_version()`、`os_name()`。
- 不默认使用 `get_*` 前缀；只有在明显提升可读性时才引入。
- 新增测试默认使用 Catch2；测试目标应统一链接 `Catch2::Catch2WithMain` 并通过 Catch2 测试发现机制接入 CTest。
- 开发节奏优先测试先行或至少测试同步提交；功能、协议、错误路径和状态持久化的改动不应长期脱离测试演进。
- 倾向小步快跑：先补最小失败用例，再落实现，再扩展覆盖面，避免把测试长期停留在手写 `main()` 的 smoke check。
- 文档正文默认使用中文；必要英文仅限术语、字段名、命令、路径、许可证原文或外部兼容要求。
- 文件名保持现有英文命名风格；核心文档使用稳定英文名，ADR 使用数字加英文短横线格式，例如 `0001-use-cpp.md`。
- 单篇文档长期超过 300-500 行，或读者群明显分离时，再考虑拆分。
- 代码、配置、协议和文档状态必须同步演进。
