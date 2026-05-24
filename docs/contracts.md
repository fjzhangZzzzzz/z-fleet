# 契约

状态：草案
最后更新：2026-05-24
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录 Agent / Server 之间的共享契约，包括协议版本、消息结构、任务模型、审计事件、错误码和兼容性约束。

Web 管理 API 属于 Server 管理面契约，不属于 Agent 控制通道。管理 API 的架构决策见 [ADR 0010](adr/0010-web-management-entry.md)。本文件先记录 API 边界和稳定字段方向；具体响应 schema 在实现对应接口时同步补齐。

## 协议版本管理

v0.1 约定如下：

- 当前协议版本固定为 `v1`。
- v0.1 的 Agent 和 Server 只要求同协议版本互通，不承诺跨主版本兼容。
- 所有请求和响应都必须包含 `protocol_version` 字段，值必须为 `v1`。
- 新增字段时只能新增可选字段，且接收方必须忽略未知字段。
- 删除字段、改变字段含义、改变错误码语义或改变审计事件语义，视为破坏性变更，必须更新相关设计文档，必要时新增 ADR。

字段约束：

- 时间统一使用 UTC 的 RFC 3339 字符串，例如 `2026-05-13T10:15:30Z`。
- ID 字段统一使用字符串类型；v0.1 推荐 UUID。
- `request_id` 由请求发起方生成，用于串联请求、响应、日志和审计事件。
- `agent_id` 是 Agent 稳定身份，首次生成后本地持久化，除非显式重置，否则重启后必须保持不变。

## 管理 API 边界

浏览器和运维集成使用 `/api/v1/...` 管理 API。管理 API 不复用 Agent 控制通道，不读取或写入 `/v1/control/events`、`/v1/control/commands` 的 protobuf frame。

管理 API 最低范围：

```text
GET  /api/v1/install/options
POST /api/v1/install/tokens
GET  /api/v1/packages/agent/{package_id}/download

GET /api/v1/agents
GET /api/v1/agents/{agent_id}
GET /api/v1/agents/{agent_id}/assets/latest
GET /api/v1/agents/{agent_id}/assets

GET  /api/v1/admin/packages
POST /api/v1/admin/packages
GET  /api/v1/admin/packages/{package_id}
POST /api/v1/admin/packages/{package_id}/validate
POST /api/v1/admin/packages/{package_id}/publish
POST /api/v1/admin/packages/{package_id}/retire
```

### 安装选项

`GET /api/v1/install/options` 返回当前可用于安装页的发布信息。建议字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `server_url` | `string` | yes | Agent 连接 Server 的地址 |
| `channel` | `string` | yes | 默认安装 channel，例如 `stable` |
| `platform` | `string` | yes | 平台，例如 `linux`、`windows` |
| `arch` | `string` | yes | 架构，例如 `x86_64`、`arm64` |
| `agent_version` | `string` | yes | 当前发布的 Agent 版本 |
| `package_id` | `string` | yes | 安装包 ID |
| `sha256` | `string` | yes | 安装包摘要 |
| `download_url` | `string` | yes | 安装包下载路径 |

### 注册 token

`POST /api/v1/install/tokens` 生成 Agent 首次安装或 bootstrap 使用的注册 token。token 明文只在创建响应中返回一次，Server 持久化层只保存哈希。

请求字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `purpose` | `string` | yes | 固定用途，例如 `agent_register` |
| `expires_at` | `string` | yes | 过期时间，UTC RFC 3339 |
| `channel` | `string` | no | 限定安装 channel |
| `platform` | `string` | no | 限定平台 |
| `arch` | `string` | no | 限定架构 |
| `max_uses` | `integer` | no | 最大使用次数 |

成功响应字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `token_id` | `string` | yes | token 标识 |
| `token` | `string` | yes | token 明文，只返回一次 |
| `expires_at` | `string` | yes | 过期时间 |

### 安装包管理

安装包状态：

| 状态 | 含义 |
| --- | --- |
| `uploaded` | 已上传到 staging 或包仓库，尚未完成校验 |
| `validated` | manifest、路径、大小和 SHA-256 校验通过 |
| `published` | 已发布到至少一个 channel |
| `retired` | 不再推荐安装，保留审计和回滚参考 |
| `rejected` | 校验失败，不允许发布 |

发布 channel：

| channel | 含义 |
| --- | --- |
| `stable` | 默认推荐版本 |
| `candidate` | 灰度验证版本 |
| `dev` | 开发和测试版本 |

约束：

- channel 是 Server 侧发布指针，不改变 package manifest、安装包摘要或安装目录中的 `releases/<version>` 语义。
- 同一 `component + platform + arch + channel` 同一时间只能有一个默认发布包。
- 上传、校验、发布、退役和下载失败路径必须使用稳定错误码，并写入审计事件。

## v0.1 消息

v0.1 最小闭环定义以下接口：

```text
POST /v1/agents/register
POST /v1/agents/{agent_id}/heartbeat
POST /v1/agents/{agent_id}/assets
```

### 通用字段

除非另有说明，所有请求体和响应体都应包含以下追踪字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `request_id` | `string` | yes | 用于请求链路追踪 |
| `agent_id` | `string` | yes | Agent 稳定身份；注册请求中的 `agent_id` 也必须存在 |
| `occurred_at` | `string` | yes | 事件在发送方发生的时间，UTC RFC 3339 |

约束：

- 路径参数中的 `agent_id` 必须与请求体中的 `agent_id` 一致。
- 如果请求体缺少必填字段、字段类型错误或路径与请求体冲突，Server 必须返回结构化错误响应。
- v0.1 所有成功响应的 `Content-Type` 为 `application/json`。

### 注册请求

用途：

- Agent 启动后首次向 Server 声明自身身份和最小基础信息。
- 当 Agent 已有本地身份时，重复注册视为幂等更新，而不是创建新身份。

请求体：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 请求 ID |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | Agent 发起注册时间 |
| `agent_version` | `string` | yes | Agent 二进制版本，例如 `0.1.0` |
| `hostname` | `string` | yes | 设备主机名 |
| `os` | `string` | yes | 操作系统名称，例如 `linux`、`windows` |
| `arch` | `string` | yes | CPU 架构，例如 `x86_64`、`arm64` |

请求示例：

```json
{
  "protocol_version": "v1",
  "request_id": "2ca4b70f-b21a-4c44-9fea-7f42cfb1f0ad",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "occurred_at": "2026-05-13T10:15:30Z",
  "agent_version": "0.1.0",
  "hostname": "devbox-01",
  "os": "linux",
  "arch": "x86_64"
}
```

成功响应：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 原请求的 `request_id` |
| `agent_id` | `string` | yes | 已登记的 Agent ID |
| `occurred_at` | `string` | yes | Server 处理完成时间 |
| `status` | `string` | yes | 固定为 `accepted` |
| `server_time` | `string` | yes | Server 当前时间，UTC RFC 3339 |

成功响应示例：

```json
{
  "protocol_version": "v1",
  "request_id": "2ca4b70f-b21a-4c44-9fea-7f42cfb1f0ad",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "occurred_at": "2026-05-13T10:15:31Z",
  "status": "accepted",
  "server_time": "2026-05-13T10:15:31Z"
}
```

### 心跳请求

用途：

- Agent 周期性声明自己仍在线。
- 心跳只用于运行期在线判断，不写入数据库或审计事件；数据库仅记录上线、离线等状态变化时间。

请求体：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 请求 ID |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | 心跳发送时间 |
| `agent_version` | `string` | yes | 当前 Agent 版本 |

成功响应：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 原请求的 `request_id` |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | Server 处理完成时间 |
| `status` | `string` | yes | 固定为 `ok` |
| `server_time` | `string` | yes | Server 当前时间 |

### 资产快照请求

用途：

- Agent 上报最小资产信息快照。
- v0.1 只定义基础字段，不定义复杂硬件、软件或进程清单。

请求体：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 请求 ID |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | 快照采集完成时间 |
| `hostname` | `string` | yes | 设备主机名 |
| `os` | `string` | yes | 操作系统名称 |
| `os_version` | `string` | no | 操作系统版本；未知时可省略 |
| `arch` | `string` | yes | CPU 架构 |
| `agent_version` | `string` | yes | Agent 版本 |

请求示例：

```json
{
  "protocol_version": "v1",
  "request_id": "5f14e364-3b32-4be1-aed9-e4d5a73b2357",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "occurred_at": "2026-05-13T10:16:00Z",
  "hostname": "devbox-01",
  "os": "linux",
  "os_version": "ubuntu-24.04",
  "arch": "x86_64",
  "agent_version": "0.1.0"
}
```

成功响应：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 原请求的 `request_id` |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | Server 处理完成时间 |
| `status` | `string` | yes | 固定为 `stored` |
| `server_time` | `string` | yes | Server 当前时间 |

### 错误响应

v0.1 所有失败场景统一返回结构化错误响应。

字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 固定为 `v1` |
| `request_id` | `string` | yes | 若请求中可解析，则回显原值；否则由 Server 生成 |
| `agent_id` | `string` | no | 若可确定，则返回 |
| `occurred_at` | `string` | yes | Server 生成错误响应时间 |
| `error_code` | `string` | yes | 稳定错误码 |
| `message` | `string` | yes | 面向日志和调试的简短说明 |
| `retryable` | `boolean` | yes | 是否建议 Agent 重试 |

错误响应示例：

```json
{
  "protocol_version": "v1",
  "request_id": "2ca4b70f-b21a-4c44-9fea-7f42cfb1f0ad",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "occurred_at": "2026-05-13T10:15:31Z",
  "error_code": "agent_id_mismatch",
  "message": "path agent_id does not match body agent_id",
  "retryable": false
}
```

### 最小错误码

v0.1 先固定以下错误码：

| 错误码 | HTTP 状态码 | 含义 | `retryable` |
| --- | --- | --- | --- |
| `invalid_json` | `400` | 请求体不是合法 JSON | `false` |
| `missing_required_field` | `400` | 缺少必填字段 | `false` |
| `invalid_field_type` | `400` | 字段类型不正确 | `false` |
| `unsupported_protocol_version` | `400` | 协议版本不是 `v1` | `false` |
| `agent_id_mismatch` | `400` | 路径与请求体中的 `agent_id` 不一致 | `false` |
| `agent_not_registered` | `404` | 心跳或资产上报时找不到已注册 Agent | `true` |
| `internal_error` | `500` | Server 内部错误 | `true` |

`message` 字段用于辅助排障，但 Agent 逻辑判断必须基于 `error_code` 和 HTTP 状态码，而不是依赖 `message` 文案。

## v0.1 审计事件

v0.1 的注册、心跳、资产上报，以及已归类到这些事件类型的失败路径都必须生成审计事件。

审计事件统一结构：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `audit_id` | `string` | yes | 审计事件唯一 ID |
| `occurred_at` | `string` | yes | 事件发生时间，UTC RFC 3339 |
| `agent_id` | `string` | no | 若可确定，则填写 |
| `request_id` | `string` | yes | 关联的请求 ID |
| `event_type` | `string` | yes | 事件类型 |
| `result` | `string` | yes | `success` 或 `failure` |
| `payload_json` | `string` | yes | 事件补充上下文的 JSON 摘要 |

v0.1 固定事件类型：

- `agent.register`
- `agent.asset_snapshot`

约束：

- 除心跳外，成功路径和已归类失败路径都必须落审计；心跳只更新 Agent 当前状态。
- 对于无法解析请求体、无法识别事件类型、无法提取稳定 `request_id` 或 `agent_id` 的请求，`v0.1` 不写入 `audit_events`。
- 上述未归类请求不属于 `v0.1` 业务审计范围，可通过运行日志观测；后续版本再纳入独立安全异常事件模型。
- `payload_json` 应包含足够排障的信息，例如错误码、HTTP 状态码、Agent 基础字段摘要，但不应存放敏感密钥材料。
- `payload_json` 是面向排障和展示的审计摘要，不是 HTTP/2 控制协议 payload；协议原文和任务类型专属 payload 在持久化层使用 protobuf blob 保存。
- v0.1 不要求审计事件暴露独立外部接口；落库是最低要求。

## v0.2 任务模型

v0.2 目标是在不引入任意 shell 或高风险写入的前提下，建立最小只读任务闭环。

v0.2 Agent/Server 控制面使用 [ADR 0007](adr/0007-agent-control-channel-http2-protobuf-lite.md)
定义的 HTTP/2 长连接与 protobuf-lite frame。正式控制面接口为：

```text
POST /v1/tasks
POST /v1/control/events
GET  /v1/control/commands
```

HTTP/2 控制面约束：

- `POST /v1/control/events` 接收 Agent 上传的 length-prefixed `AgentEvent`
  protobuf frame，`content-type` 必须为 `application/x-protobuf`。
- `GET /v1/control/commands` 返回 Server 下发的 length-prefixed
  `ServerCommand` protobuf frame，`accept` 必须为 `application/x-protobuf`。
- `GET /v1/control/commands` 是长期 stream。Server 在当前无任务时保持 stream
  打开，不返回“空任务”响应；后续任务入队后通过同一 stream 下发
  `ServerCommand.task_assigned` frame。
- Agent 通过 `AgentEvent.register` 和 `AgentEvent.heartbeat` 建立并维持在线状态。
- Agent 开始执行任务时通过 `AgentEvent.task_running` 上报。
- Agent 完成、失败或过期任务时通过 `AgentEvent.task_result` 上报。
- 控制面迁移和非目标协议边界由 ADR 0007 统一维护；本文只定义当前正式契约。

约束：

- `POST /v1/tasks` 用于创建最小只读任务；v0.2 不涉及复杂调度策略。
- Agent 在任一时刻只要求串行执行一个任务；v0.2 不要求并发任务调度。
- Server 只通过 command stream 下发能力等级为 `readonly` 的任务。
- 无可执行任务时 Server 保持 command stream 打开。
- 任务下发、领取、开始、完成、失败和过期都必须产生审计事件。
- v0.2 不定义取消、暂停、恢复和任务优先级抢占。

### 任务能力等级

v0.2 任务契约沿用安全文档中的能力分级，初始只开放 `readonly`：

| `capability_level` | 含义 | v0.2 默认值 |
| --- | --- | --- |
| `readonly` | 只读取系统状态或资产信息，不改变目标设备 | enabled |
| `low_risk_write` | 低风险写入，例如更新 Agent 本地配置 | disabled |
| `high_risk_write` | 修改系统配置、安装软件、删除文件等 | disabled |
| `shell` | 任意命令或脚本执行 | disabled |

任何非 `readonly` 任务进入协议前，都必须先补齐威胁模型、策略开关、审计字段和测试。

当前 `v0.2` 的 Server 只接受 `readonly` 能力等级的任务创建请求。
任何 `low_risk_write`、`high_risk_write` 或 `shell` 任务都必须返回
`403 capability_not_allowed`，且不得入队。

### 任务类型

v0.2 先固定一个最小只读任务类型：

| `task_type` | 含义 | 输入 |
| --- | --- | --- |
| `collect_basic_inventory` | 读取基础主机信息并回传结果 | 无或空对象 |

说明：

- `collect_basic_inventory` 用于验证任务下发、Agent 执行、结果回传和审计链路。
- 该任务只允许读取 Agent 已具备或低风险可获取的信息，例如 `hostname`、`os`、`arch`、`agent_version`、时间戳。
- v0.2 不将“执行任意命令”包装成只读任务。

### 任务对象

Server 持久化和任务创建请求中的任务对象使用以下结构；下发时映射到
`ServerCommand.task_assigned` protobuf payload：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `task_id` | `string` | yes | 任务唯一 ID |
| `agent_id` | `string` | yes | 目标 Agent ID |
| `task_type` | `string` | yes | 任务类型 |
| `capability_level` | `string` | yes | 所需能力等级 |
| `created_at` | `string` | yes | Server 创建任务时间 |
| `expires_at` | `string` | yes | 任务过期时间 |
| `input` | `object` | yes | 任务输入；无输入时使用空对象 |

约束：

- `task_id` 在全局范围内必须唯一。
- `agent_id` 必须与 command stream 绑定的 Agent 身份一致。
- `expires_at` 必须晚于 `created_at`。
- `input` 的字段由 `task_type` 决定；接收方必须忽略未知可选字段。

示例：

```json
{
  "protocol_version": "v1",
  "task_id": "3ea33f7a-bde7-4d1c-9cc7-1a0a0e15f501",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "task_type": "collect_basic_inventory",
  "capability_level": "readonly",
  "created_at": "2026-05-16T10:00:00Z",
  "expires_at": "2026-05-16T10:05:00Z",
  "input": {}
}
```

### Command Stream 下发

`GET /v1/control/commands` 成功后返回 HTTP `200` 响应头并保持 stream 打开。
当 Server 为该 Agent 领取到 queued 任务时，下发一个
`ServerCommand.task_assigned` frame。

`ServerCommand` envelope 字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `message_id` | `string` | yes | Server 生成的命令消息 ID |
| `correlation_id` | `string` | no | 关联请求或 trace ID |
| `agent_id` | `string` | yes | Agent 稳定身份 |
| `occurred_at` | `string` | yes | Server 生成命令时间 |
| `task_assigned` | `object` | no | 任务下发 payload |
| `error` | `object` | no | 可恢复或不可恢复的控制面错误 payload |

`task_assigned` 字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `task_id` | `string` | yes | 任务唯一 ID |
| `task_type` | enum | yes | 当前为 `TASK_TYPE_COLLECT_BASIC_INVENTORY` |
| `capability_level` | enum | yes | 当前为 `CAPABILITY_LEVEL_READONLY` |
| `created_at` | `string` | yes | Server 创建任务时间 |
| `expires_at` | `string` | yes | 任务过期时间 |
| `collect_basic_inventory` | message | no | 对应任务类型的输入 payload，当前为空 message |

说明：

- frame 使用 4 字节大端长度前缀加 protobuf payload。
- 没有任务时不发送 idle frame，stream 持续保持打开。
- Agent 收到 `task_assigned` 后，先上报 `AgentEvent.task_running`，再执行任务并上报
  `AgentEvent.task_result`。
- command stream 返回非 `200` 时，Agent 必须将其视为控制面拒绝并进入重连流程。
- command stream 正常关闭、HTTP/2 reset、TCP 断开或 protobuf frame 解析失败时，
  Agent 必须关闭当前 session，并按指数退避重新注册。
- 收到 `ServerCommand.error` 时，Agent 必须记录稳定错误码和消息，关闭当前
  session，并按指数退避重新注册。

### 任务创建请求

Server 通过以下结构接收最小任务创建请求：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `request_id` | `string` | yes | 任务创建请求 ID |
| `occurred_at` | `string` | yes | 请求发生时间 |
| `task` | `object` | yes | 完整任务对象 |

示例：

```json
{
  "protocol_version": "v1",
  "request_id": "task-create-1",
  "occurred_at": "2026-05-17T10:00:00Z",
  "task": {
    "protocol_version": "v1",
    "task_id": "3ea33f7a-bde7-4d1c-9cc7-1a0a0e15f501",
    "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
    "task_type": "collect_basic_inventory",
    "capability_level": "readonly",
    "created_at": "2026-05-17T10:00:00Z",
    "expires_at": "2026-05-17T10:05:00Z",
    "input": {}
  }
}
```

### 任务开始执行上报

Agent 在本地开始执行任务前，通过 `POST /v1/control/events` 发送
`AgentEvent.task_running` frame，向 Server 上报 `running` 状态。payload 字段为：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `request_id` | `string` | yes | 本次上报请求 ID |
| `task_id` | `string` | yes | 任务 ID |
| `agent_id` | `string` | yes | 执行任务的 Agent ID |
| `task_type` | `string` | yes | 任务类型 |
| `occurred_at` | `string` | yes | Agent 开始执行任务的时间 |

### 任务结果回传

Agent 完成或失败后，通过 `POST /v1/control/events` 发送
`AgentEvent.task_result` frame 回传统一结构。

字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `protocol_version` | `string` | yes | 当前固定为 `v1` |
| `request_id` | `string` | yes | 结果回传请求 ID |
| `task_id` | `string` | yes | 任务 ID |
| `agent_id` | `string` | yes | 执行任务的 Agent ID |
| `task_type` | `string` | yes | 任务类型；必须与下发任务一致 |
| `occurred_at` | `string` | yes | Agent 完成任务的时间 |
| `status` | `string` | yes | `succeeded`、`failed` 或 `expired` |
| `result` | `object` | no | 成功结果对象；失败或过期时可省略 |
| `error` | `object` | no | 失败或过期时返回；成功时省略 |

约束：

- `task_id` 必须与已下发的任务一致。
- `agent_id` 必须与任务归属一致。
- `task_type` 必须与原始任务类型一致。
- `result` 和 `error` 不能同时为空；至少一方应与 `status` 对应。
- `status = succeeded` 时必须省略 `error`。
- `status = failed` 或 `expired` 时必须提供 `error`。
- 当前实现已对上述形状约束做服务端校验，并返回
  `400 task_result_invalid`。

成功示例：

```json
{
  "protocol_version": "v1",
  "request_id": "result-req-1",
  "task_id": "3ea33f7a-bde7-4d1c-9cc7-1a0a0e15f501",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "task_type": "collect_basic_inventory",
  "occurred_at": "2026-05-16T10:00:30Z",
  "status": "succeeded",
  "result": {
    "hostname": "devbox-01",
    "os": "linux",
    "arch": "x86_64",
    "agent_version": "0.1.0"
  }
}
```

失败示例：

```json
{
  "protocol_version": "v1",
  "request_id": "result-req-2",
  "task_id": "3ea33f7a-bde7-4d1c-9cc7-1a0a0e15f501",
  "agent_id": "ab3f327d-7e9c-4ef2-8d7b-92ba0dbe1c59",
  "task_type": "collect_basic_inventory",
  "occurred_at": "2026-05-16T10:00:30Z",
  "status": "failed",
  "error": {
    "error_code": "task_execution_failed",
    "message": "inventory collection returned unexpected empty hostname",
    "retryable": false
  }
}
```

### 任务状态机

v0.2 最小状态机如下：

| 状态 | 含义 |
| --- | --- |
| `queued` | 任务已创建，等待 Agent 轮询领取 |
| `assigned` | 任务已通过轮询返回给目标 Agent |
| `running` | Agent 已开始执行任务 |
| `succeeded` | Agent 成功完成并回传结果 |
| `failed` | Agent 执行失败并回传错误 |
| `expired` | 任务在执行前或执行中超过 `expires_at` |

状态流转约束：

- `queued -> assigned`
- `assigned -> running`
- `running -> succeeded`
- `running -> failed`
- `queued -> expired`
- `assigned -> expired`
- `running -> expired`

v0.2 不允许：

- `succeeded` 或 `failed` 再次回到非终态；
- 不经过 `assigned` 直接进入 `running`；
- 同一 `task_id` 多次成功回传结果。

### 最小任务错误码

v0.2 先固定以下任务错误码：

| 错误码 | HTTP 状态码 | 含义 | `retryable` |
| --- | --- | --- | --- |
| `task_not_found` | `404` | 指定 `task_id` 不存在 | `false` |
| `task_agent_mismatch` | `400` | 结果回传中的 `agent_id` 与任务归属不一致 | `false` |
| `task_already_finished` | `409` | 任务已进入终态，不接受重复结果 | `false` |
| `task_expired` | `409` | 任务已过期 | `false` |
| `unsupported_task_type` | `400` | Agent 或 Server 不支持该任务类型 | `false` |
| `capability_not_allowed` | `403` | 任务能力等级未被当前策略允许 | `false` |
| `task_execution_failed` | `200` in result payload | Agent 执行任务失败的稳定错误码 | `false` |
| `task_result_invalid` | `400` | 结果回传结构非法或缺少必填字段 | `false` |

说明：

- `task_execution_failed` 是任务结果中的业务错误码，不是结果接口本身的 HTTP 错误。
- 如果 Agent 无法执行一个已分配任务，应尽量回传 `failed` 结果，而不是直接沉默超时。

### 任务审计字段

v0.2 任务相关审计事件建议至少包含以下字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `audit_id` | `string` | 审计事件 ID |
| `occurred_at` | `string` | 事件时间 |
| `agent_id` | `string` | 目标 Agent |
| `task_id` | `string` | 关联任务 |
| `request_id` | `string` | 关联请求 |
| `event_type` | `string` | 例如 `task.queued`、`task.assigned`、`task.running`、`task.succeeded`、`task.failed`、`task.expired` |
| `result` | `string` | `success` 或 `failure` |
| `payload_json` | `string` | 任务类型、状态、错误码、能力等级等审计摘要字段 |

约束：

- `task.running` 对应 Server 接受 `AgentEvent.task_running`，表示 Agent 已开始执行。
- `task.succeeded`、`task.failed` 和 `task.expired` 对应 Server 接受
  `AgentEvent.task_result` 后按结果状态写入的审计事件。
- `task_id` 应成为任务相关审计的首要检索键之一。
- 审计中不得存放敏感密钥、凭据原文或高基数大体积原始输出。
- 若任务结果体较大，应只保存摘要和关键字段，而不是无上限原文。

## 兼容性规则

- 新增可选字段应保持向后兼容。
- 删除字段、改变字段含义或改变状态机语义必须新增 ADR 或更新相关设计文档。
- Agent 和 Server 的兼容策略必须在发布说明中记录。
- v0.1 的实现如果临时省略某个可选字段，应在文档和测试中明确记录，而不是静默偏离契约。
