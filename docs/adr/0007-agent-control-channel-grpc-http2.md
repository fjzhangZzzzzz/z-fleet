# ADR 0007：Agent 控制通道采用 HTTP/2 长连接与 protobuf-lite

状态：已接受
日期：2026-05-20
修订：2026-05-22，放弃 gRPC，转向轻量 HTTP/2 + protobuf-lite

## 背景

Agent 当前以一次性 REST 请求完成注册、心跳、资产上报和任务轮询。该模型已经能验证最小协议，但不适合作为长期运行形态：

- Agent 进程执行一轮后退出，无法表达持续在线、离线重连和任务实时下发；
- 周期轮询会增加任务延迟，并把在线状态、心跳和任务获取混在短连接请求中；
- 后续安装包下载、日志上传、资产快照、任务取消、配置更新等能力需要更清晰的流式和并发模型；
- gRPC 静态链接会显著拉高构建耗时和 Agent 二进制体积，不符合轻量 Agent 的产品目标。

本决策继承 [ADR 0002：Agent 使用主动出站连接](0002-agent-outbound-connection.md)：Agent 必须主动连接 Server，正常运行形态下不暴露客户端监听端口。

## 决策

Agent 与 Server 的长期主控制通道采用 **HTTP/2 长连接 + protobuf-lite**，HTTP/2 传输层优先基于 `nghttp2`，消息 schema 使用 protobuf 的 lite runtime。

核心约束：

- Agent 主动建立到 Server 的出站 HTTP/2 TLS 连接；
- 主运行形态使用长期 HTTP/2 session，而不是短连接 REST polling；
- 控制面使用两个长期 stream：
  - `POST /v1/control/events`：Agent 上传 `AgentEvent` frame；
  - `GET /v1/control/commands`：Server 下发 `ServerCommand` frame；
- protobuf 只定义 message，不定义 gRPC service；
- `.proto` 必须使用 `option optimize_for = LITE_RUNTIME;`，C++ 链接 `protobuf::libprotobuf-lite`；
- frame 使用 length-prefixed protobuf payload，应用层负责 message envelope、错误码、重连和幂等；
- Agent 断线后负责指数退避重连；Server 必须容忍 Agent 离线、重复注册和连接重建；
- 在线状态不能只依赖 TCP 连接存在，应结合控制流 heartbeat 和 Server 侧超时判断；
- 大 payload 或可并发的数据流使用独立 HTTP/2 stream，例如安装包下载、日志上传、资产快照批量上报；
- `RunOnce` 仅作为本地诊断、测试或兼容过渡入口，不作为正式 Agent runtime 模型。

落地时可以调整 endpoint 和 message 名称，但不得偏离以下方向：

- 不把正式控制面退回为周期 REST polling；
- 不引入 gRPC、Cap'n Proto RPC 等重框架作为主控制面；
- 不手写 HTTP/2 frame 层，HTTP/2 细节由 `nghttp2` 承担；
- 不让 Server 主动连接 Agent；
- 不要求 Agent 暴露入站端口；
- 不把大文件下载、日志上传等大 payload 挤占长期控制流。

## 备选方案

- **继续 REST polling**：实现简单，便于调试，但任务延迟、连接开销、在线状态和离线恢复模型都不够清晰，已拒绝作为长期主模型。REST 可保留为诊断、兼容或过渡接口。
- **gRPC over HTTP/2**：协议和工具链成熟，但对本项目过重；静态链接和构建成本明显超过轻量 Agent 目标，已拒绝。
- **WebSocket / WSS**：全双工语义简单，适合浏览器实时通信或轻量控制通道；但多路复用、流控、schema 和错误语义需要大量应用层约定。作为特殊网络环境备选可以接受，但不作为主控制协议。
- **HTTP/2 + MessagePack**：编码轻量，但 schema 约束弱，版本演进、安全校验和跨语言生成都需要更多手写约定，暂不采用。
- **HTTP/2 + FlatBuffers**：零拷贝读取优秀，但控制面消息较小，收益有限，构造和演进成本高于 protobuf-lite，暂不采用。
- **HTTP/2 + nanopb**：体积极小，适合嵌入式 Agent；当前 C++ server/agent 共享 schema 时开发效率不如 protobuf-lite，作为极限瘦身备选保留。
- **Cap'n Proto / Cap'n Proto RPC**：性能强，但会引入另一套重框架和 RPC 体系，已拒绝。
- **Server 连接 Agent**：任务推送直接，但要求 Agent 入站可达，违反 ADR 0002，已拒绝。
- **MQTT/NATS 等消息系统**：适合大规模消息分发，但会引入额外 broker 和运维面；当前项目更需要 Agent/Server 直接控制面，暂不采用。

## 影响

- 正向：保留 HTTP/2 长连接、多 stream、流控和 TLS 边界，同时避免 gRPC 体积和构建成本。
- 正向：protobuf-lite 提供稳定 schema、跨语言能力和较好的版本演进，不需要运行时反射。
- 正向：Agent 可按 `asio + openssl + nghttp2 + protobuf-lite` 收敛依赖；Server 可继续用 Beast 承载 REST/web 管理接口。
- 负向：需要自行定义 frame、应用错误码、重连、ack 和部分可观测性字段；这些必须集中在 `libs/transport` 和 `libs/protocol`，不得散落到业务代码。
- 负向：浏览器直接调用不如 WebSocket 方便，但本项目主通信对象是 Agent/Server，不以浏览器实时通道为目标。
- 后续工作：协议文档应新增 HTTP/2 endpoint、frame 和 protobuf message 定义；Agent 应重构为持续运行 runtime；Server 应增加 HTTP/2 control listener；运维文档应补充 Agent 启动、停止、断线重连、证书配置和排障流程。

## 落地指导

1. **协议先行**
   - 先定义 `.proto` message，再生成 C++ lite 代码，避免手写 JSON 拼接或在各模块重复维护协议结构。
   - `AgentEvent` 与 `ServerCommand` 必须包含稳定 message kind、request id、agent id、occurred_at、protocol version。
   - 错误判断基于稳定 code/status，不依赖 message 文案。

2. **运行模型**
   - Agent 默认持续运行并维护 HTTP/2 session。
   - `--once` 只用于诊断和测试，不应成为 service 管理脚本的默认入口。
   - 主 loop 必须支持 SIGINT/SIGTERM 优雅退出。

3. **重连模型**
   - 连接失败、连接断开、HTTP/2 stream 错误和可恢复协议错误进入指数退避。
   - 重连成功后必须重新完成注册或恢复会话，不假设 Server 保留旧连接上下文。
   - `agent_not_registered` 等身份状态缺失错误应触发重新注册。

4. **流量分层**
   - 控制流只承载小消息和控制事件。
   - 安装包、日志、资产快照等大 payload 使用独立 HTTP/2 stream，避免阻塞控制流。
   - 需要大文件时必须依赖 HTTP/2 流控和应用层分片，不一次性加载到内存。

5. **安全边界**
   - 生产目标必须使用 TLS，后续身份认证优先演进到 mTLS 或等价的 Agent 凭据机制。
   - token、证书、trace id 等上下文通过 HTTP/2 header 或明确字段传递，不混入日志文本解析。

6. **兼容与迁移**
   - 当前 REST API 可作为过渡、诊断或测试接口保留一段时间。
   - 新功能默认优先设计为 HTTP/2 control message；除非有明确兼容理由，不再扩展 REST polling 作为主控制面。
   - 文档、测试和运维脚本必须围绕 HTTP/2 长连接 runtime 演进，避免实现方向重新偏回短连接轮询。
