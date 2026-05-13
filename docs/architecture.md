# 架构

状态：草案
最后更新：2026-05-13
关联里程碑：v0.1, v0.2, v0.3

## 目标

z-fleet 是面向个人开发者、小团队、Homelab、边缘设备和测试环境的轻量端点探针与管理框架。架构优先服务于：

- Agent 低占用、长期运行、空闲时 CPU 接近 0；
- Agent 主动连接 Server，不暴露客户端端口；
- 注册、心跳、资产上报、任务、审计、配置和错误码结构化；
- 默认只读，写入能力显式授权；
- 跨 Linux / Windows 的可验证最小闭环；
- 依赖可审计、可替换、可通过 vcpkg 固定版本来源。

## 组件

| 组件 | 路径 | 职责 |
| --- | --- | --- |
| Agent | `apps/agent/` | 生命周期、身份、本地配置、主动连接、注册、心跳、资产采集、任务执行调度 |
| Server | `apps/server/` | 服务入口、注册接收、心跳接收、资产接收、任务下发、持久化编排 |
| Core | `libs/core/` | 版本、日志、时间、ID、配置解析、通用错误处理 |
| Protocol | `libs/protocol/` | Agent / Server 共享消息结构、任务模型、审计事件、错误码、协议版本 |
| Platform | `libs/platform/` | Linux / Windows 系统信息、资产采集、路径、权限和服务管理差异封装 |
| 集成测试 | `tests/integration/` | 注册、心跳、资产上报和任务状态流转的跨组件测试 |

## 依赖原则

- v0.1 只引入支撑最小闭环的依赖：网络、结构化协议、持久化、日志、配置、CLI 和测试。
- 高风险或后期能力的依赖延后引入，例如自更新签名、包归档、插件运行时和 UI。
- 三方库通过 `vcpkg.json` manifest mode 管理，registry baseline 由 `vcpkg-configuration.json` 固定。
- 三方库应封装在模块边界内，避免业务代码直接扩散到全项目。
- 协议和持久化模型必须以项目自有结构体为核心，JSON、SQLite 等库只是序列化和存储实现。

## v0.1 依赖基线

| 能力 | 库 | 模块 | 原因 |
| --- | --- | --- | --- |
| 异步网络 | `boost-asio` | Agent, Server | 跨平台异步 I/O 基础，适合长期运行进程 |
| HTTP / WebSocket | `boost-beast` | Agent, Server | v0.1 使用 HTTP，后续可平滑扩展 WebSocket |
| TLS | `openssl` | Agent, Server | HTTPS、证书校验和后续安全能力基础 |
| ID 生成 | `boost-uuid` | Core, Protocol | 用于 `request_id`、`agent_id`、`task_id`、`audit_id` |
| JSON 协议 | `nlohmann-json` | Protocol | 初期可读、易调试，适合文档驱动协议演进 |
| 嵌入式数据库 | `sqlite3` | Server, Agent local state | 单文件、低运维成本，适合小团队和 Homelab |
| SQLite C++ 封装 | `sqlitecpp` | Server, Agent local state | RAII 风格封装，减少 C API 样板代码 |
| 日志 | `spdlog` | Core, apps | 低开销、跨平台、支持多 sink 和文件轮转 |
| 命令行解析 | `cli11` | apps | 简洁处理 `--config`、`--log-level`、`--data-dir` |
| 配置解析 | `tomlplusplus` | Core, apps | TOML 适合人工维护配置，比 JSON 更适合运维场景 |
| 测试 | `catch2` | libs, apps, tests | 轻量，适合当前小型单元测试和状态机测试 |

当前清单已落地在 [`vcpkg.json`](../vcpkg.json)。

## 延后引入的依赖

| 能力 | 候选方案 | 最早里程碑 | 引入条件 |
| --- | --- | --- | --- |
| 签名 / 更新校验 | `libsodium` | v0.2 / v0.3 | 身份挑战、任务签名或自更新签名设计确认后引入 |
| 二进制协议 / RPC | `protobuf`, `grpc` | v0.2+ | 协议稳定、跨语言或高吞吐需求明确后再评估 |
| 包压缩 | `zstd`, `libarchive` | v0.3 | 自更新包、发布包和回滚流程设计确认后引入 |
| Web UI | TBD | long term | 明确轻量管理界面范围后再选型 |
| 插件运行时 | external process / WASM / dynamic library | long term | 威胁模型和权限边界完成后再决策 |

## 网络模型

v0.1 推荐使用 HTTPS + JSON REST，Agent 主动出站连接 Server。

初始接口：

```text
POST /v1/agents/register
POST /v1/agents/{agent_id}/heartbeat
POST /v1/agents/{agent_id}/assets
```

v0.2 任务下发可先使用 Agent 轮询，避免 Server 需要主动连回客户端：

```text
GET  /v1/agents/{agent_id}/tasks/poll
POST /v1/tasks/{task_id}/result
```

如果后续需要更低延迟或双向流式消息，再基于相同协议模型引入 WebSocket。

## 数据模型边界

协议层应暴露项目自有类型，而不是让应用层直接依赖 JSON 字段：

```cpp
struct Heartbeat {
  std::string agent_id;
  std::string protocol_version;
  std::string observed_at;
};
```

JSON 序列化应集中在 `libs/protocol`：

```cpp
inline void to_json(nlohmann::json& j, const Heartbeat& h) {
  j = {
    {"agent_id", h.agent_id},
    {"protocol_version", h.protocol_version},
    {"observed_at", h.observed_at}
  };
}
```

## 持久化模型

v0.1 Server 使用 SQLite 持久化设备、心跳、资产快照和审计事件。建议初始表保持简单，并为后续迁移保留 `schema version`。

```sql
create table if not exists agents (
  agent_id text primary key,
  first_seen_at text not null,
  last_seen_at text not null,
  platform text not null,
  status text not null
);

create table if not exists audit_events (
  audit_id text primary key,
  occurred_at text not null,
  agent_id text,
  event_type text not null,
  request_id text,
  payload_json text not null
);
```

审计事件应入库，文本日志只作为运行排障辅助。

## 配置模型

配置文件建议使用 TOML：

```toml
[server]
listen = "127.0.0.1:8080"
database_path = "data/zfleet.db"

[agent]
server_url = "https://127.0.0.1:8080"
data_dir = "data/agent"

[log]
level = "info"
file = "logs/zfleet.log"
```

命令行参数只覆盖少量启动级配置，例如 `--config`、`--log-level` 和 `--data-dir`。

## 相关 ADR

- [0001：Agent 与 Server 使用 C++](adr/0001-use-cpp.md)
- [0002：Agent 使用主动出站连接](adr/0002-agent-outbound-connection.md)
- [0003：任务默认只读](adr/0003-default-read-only-tasks.md)
- [0004：v0.1 第三方依赖基线](adr/0004-third-party-dependency-baseline.md)
