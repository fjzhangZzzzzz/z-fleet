# ADR 0004：v0.1 第三方依赖基线

状态：已接受
日期：2026-05-13

## 背景

v0.1 需要一个最小端到端闭环：Agent 身份、本地主动连接、Server 注册和心跳、资产上报、持久化、审计事件、测试，以及本地运行说明。依赖集合必须支撑这个闭环，同时避免把项目过早拉向完整 RMM、复杂 UI、插件系统或高风险任务执行。

## 决策

通过 `vcpkg` manifest mode 使用以下 v0.1 依赖基线：

| 能力 | 库 |
| --- | --- |
| 异步网络 | `boost-asio` |
| HTTP/2 | `nghttp2` |
| 控制消息编码 | `protobuf-lite` |
| ID 生成 | `boost-uuid` |
| TLS | `openssl` |
| JSON 协议 | `nlohmann-json` |
| 嵌入式数据库 | `sqlite3` |
| SQLite C++ 封装 | `sqlitecpp` |
| 日志 | `spdlog` |
| 命令行解析 | `cli11` |
| 配置解析 | `tomlplusplus` |
| 测试 | `catch2` |

Agent/Server 主控制通道由 [ADR 0007：Agent 控制通道采用 HTTP/2 长连接与 protobuf-lite](0007-agent-control-channel-http2-protobuf-lite.md) 定义。旧 HTTP/1 REST 控制路径已移除，不再以 WebSocket 或 gRPC 作为默认演进方向。

## 备选方案

- Drogon：Server 开发速度快、Web 框架功能丰富，但对最小闭环而言过宽，对 Agent 侧也没有帮助。
- `libcurl` + Drogon：客户端稳定、Server 方便，但会在早期引入两套网络栈。
- `gRPC` + `protobuf`：模式定义和流式能力强，但构建和 Agent 体积成本过高，已由 ADR 0007 拒绝。
- `nghttp2` + `protobuf-lite`：保留 HTTP/2 长连接和 schema 能力，避免重 RPC 框架，已由 ADR 0007 接受。
- PostgreSQL：更适合大规模部署，但在当前阶段会增加额外服务依赖和运维负担。
- GoogleTest：成熟且强大，但当前测试面较小，Catch2 更轻。

## 影响

- 依赖边界必须保持明确：JSON 属于 `libs/protocol`，日志 / 配置 / ID 辅助能力属于 `libs/core`，平台 API 属于 `libs/platform`。
- SQLite 是 v0.1 默认持久化方案。在引入不兼容 schema 变更前，必须先设计迁移和 schema version 管理。
- `libsodium`、`zstd`、`libarchive`、UI 框架和插件运行时延后到对应里程碑与威胁模型准备完成后再引入；`nghttp2` / `protobuf-lite` 按 ADR 0007 的控制通道落地计划引入。
- 任何重大依赖替换都应更新本 ADR，或新增一个替代它的 ADR。
