# ADR 0007：Agent 控制通道采用 gRPC over HTTP/2

状态：已接受
日期：2026-05-20

## 背景

Agent 当前以一次性 REST 请求完成注册、心跳、资产上报和任务轮询。该模型已经能验证最小协议，但不适合作为长期运行形态：

- Agent 进程执行一轮后退出，无法表达持续在线、离线重连和任务实时下发；
- 周期轮询会增加任务延迟，并把在线状态、心跳和任务获取混在短连接请求中；
- 后续安装包下载、日志上传、资产快照、任务取消、配置更新等能力需要更清晰的流式和并发模型；
- 若在 WebSocket 上自定义 envelope、ack、deadline、错误语义和多路复用，长期会重复实现 RPC 和 HTTP/2 已有能力。

本决策继承 [ADR 0002：Agent 使用主动出站连接](0002-agent-outbound-connection.md)：Agent 必须主动连接 Server，正常运行形态下不暴露客户端监听端口。

## 决策

Agent 与 Server 的长期主控制通道采用 **gRPC over HTTP/2**。

核心约束：

- Agent 主动建立到 Server 的出站 HTTP/2 TLS 连接；
- 主运行形态使用长期 gRPC channel，而不是短连接 REST polling；
- 控制面优先使用 gRPC 双向流，承载注册、心跳、任务下发、任务状态、任务结果、错误事件和轻量配置更新；
- 大 payload 或可并发的数据流使用独立 RPC 或独立 stream，例如安装包下载、日志上传、资产快照批量上报；
- Agent 断线后负责指数退避重连；Server 必须容忍 Agent 离线、重复注册和连接重建；
- 在线状态不能只依赖 TCP 连接存在，应结合控制流 heartbeat 和 Server 侧超时判断；
- `RunOnce` 仅作为本地诊断、测试或兼容过渡入口，不作为正式 Agent runtime 模型。

建议的服务边界：

```proto
service AgentControl {
  rpc Connect(stream AgentEvent) returns (stream ServerCommand);
}

service AgentData {
  rpc UploadAssetSnapshot(AssetSnapshotRequest) returns (StatusResponse);
  rpc UploadLogs(stream LogChunk) returns (StatusResponse);
  rpc DownloadPackage(PackageRequest) returns (stream PackageChunk);
}
```

`AgentControl.Connect` 是长期控制流。Agent 发送 `register`、`heartbeat`、`task_running`、`task_result`、`error` 等事件；Server 发送 `task_assigned`、`cancel_task`、`config_update`、`package_update` 等命令。

落地时可以调整 service 和 message 名称，但不得偏离以下方向：

- 不把正式控制面退回为周期 REST polling；
- 不在单一 WebSocket 消息管道上重新实现 RPC 多路复用、deadline、状态码和流控；
- 不让 Server 主动连接 Agent；
- 不要求 Agent 暴露入站端口；
- 不把大文件下载、日志上传等大 payload 挤占长期控制流。

## 备选方案

- **继续 REST polling**：实现简单，便于调试，但任务延迟、连接开销、在线状态和离线恢复模型都不够清晰，已拒绝作为长期主模型。REST 可保留为诊断、兼容或过渡接口。
- **WebSocket / WSS**：全双工语义简单，适合浏览器实时通信或轻量控制通道；但多路复用、流控、deadline、错误语义、schema 和可观测性需要大量应用层约定。作为特殊网络环境或早期验证备选可以接受，但不作为主控制协议。
- **HTTP/2 自定义协议**：可使用 stream 和 flow control，但仍需自定义 IDL、状态、错误、生成代码和工具链。相比 gRPC 生态收益不足。
- **Server 连接 Agent**：任务推送直接，但要求 Agent 入站可达，违反 ADR 0002，已拒绝。
- **MQTT/NATS 等消息系统**：适合大规模消息分发，但会引入额外 broker 和运维面；当前项目更需要 Agent/Server 直接控制面，暂不采用。

## 影响

- 正向：形成清晰的长期在线、双向控制、离线重连、并发 stream、流控、deadline、状态码和强类型协议模型。
- 正向：为任务实时下发、任务取消、安装包下载、日志上传、资产上报、后续 mTLS 身份认证和可观测性预留稳定边界。
- 负向：需要引入 protobuf/gRPC 工具链，重构当前 REST 调用和部分集成测试；部署侧也需要明确 HTTP/2 TLS、证书和代理支持。
- 负向：浏览器直接调用不如 WebSocket 方便，但本项目主通信对象是 Agent/Server，不以浏览器实时通道为目标。
- 后续工作：协议文档应新增 gRPC service/message 定义；Agent 应重构为持续运行 runtime；Server 应增加 gRPC endpoint；运维文档应补充 Agent 启动、停止、断线重连、证书配置和排障流程。

## 落地指导

1. **协议先行**
   - 先定义 `.proto`，再生成 C++ 代码，避免手写 JSON 拼接或在各模块重复维护协议结构。
   - `AgentEvent` 与 `ServerCommand` 必须包含稳定 message kind、request id、agent id、occurred_at、protocol version。
   - 错误判断基于稳定 code/status，不依赖 message 文案。

2. **运行模型**
   - Agent 默认持续运行并维护 gRPC channel。
   - `--once` 只用于诊断和测试，不应成为 service 管理脚本的默认入口。
   - 主 loop 必须支持 SIGINT/SIGTERM 优雅退出。

3. **重连模型**
   - 连接失败、连接断开、HTTP/2 stream 错误、gRPC retryable 状态进入指数退避。
   - 重连成功后必须重新完成注册或恢复会话，不假设 Server 保留旧连接上下文。
   - `agent_not_registered` 等身份状态缺失错误应触发重新注册。

4. **流量分层**
   - 控制流只承载小消息和控制事件。
   - 安装包、日志、资产快照等大 payload 使用独立 RPC/stream，避免阻塞控制流。
   - 需要大文件时必须依赖 HTTP/2 流控或应用层分片，不一次性加载到内存。

5. **安全边界**
   - 生产目标必须使用 TLS，后续身份认证优先演进到 mTLS 或等价的 Agent 凭据机制。
   - token、证书、trace id 等上下文通过 gRPC metadata 或明确字段传递，不混入日志文本解析。

6. **兼容与迁移**
   - 当前 REST API 可作为过渡、诊断或测试接口保留一段时间。
   - 新功能默认优先设计为 gRPC service/message；除非有明确兼容理由，不再扩展 REST polling 作为主控制面。
   - 文档、测试和运维脚本必须围绕 gRPC 长连接 runtime 演进，避免实现方向重新偏回短连接轮询。
