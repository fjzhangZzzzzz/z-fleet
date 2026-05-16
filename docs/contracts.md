# 契约

状态：草案
最后更新：2026-05-13
关联里程碑：v0.1, v0.2

## 范围

本文档记录 Agent / Server 之间的共享契约，包括协议版本、消息结构、任务模型、审计事件、错误码和兼容性约束。

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
- 心跳是更新 `last_seen_at` 的唯一标准入口。

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
| `payload_json` | `string` | yes | 事件补充上下文的 JSON 字符串 |

v0.1 固定事件类型：

- `agent.register`
- `agent.heartbeat`
- `agent.asset_snapshot`

约束：

- 成功路径和已归类失败路径都必须落审计。
- 对于无法解析请求体、无法识别事件类型、无法提取稳定 `request_id` 或 `agent_id` 的请求，`v0.1` 不写入 `audit_events`。
- 上述未归类请求不属于 `v0.1` 业务审计范围，可通过运行日志观测；后续版本再纳入独立安全异常事件模型。
- `payload_json` 应包含足够排障的信息，例如错误码、HTTP 状态码、Agent 基础字段摘要，但不应存放敏感密钥材料。
- v0.1 不要求审计事件暴露独立外部接口；落库是最低要求。

## v0.2 任务模型

待定义：

- 任务类型；
- 输入和输出结构；
- 状态机；
- 错误码；
- 超时和取消语义；
- 审计字段；
- 所需权限和默认开关。

## 兼容性规则

- 新增可选字段应保持向后兼容。
- 删除字段、改变字段含义或改变状态机语义必须新增 ADR 或更新相关设计文档。
- Agent 和 Server 的兼容策略必须在发布说明中记录。
- v0.1 的实现如果临时省略某个可选字段，应在文档和测试中明确记录，而不是静默偏离契约。
