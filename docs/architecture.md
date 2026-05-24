# 架构

状态：草案
最后更新：2026-05-24
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
| Server | `apps/server/` | 服务入口、HTTP/2 控制通道、Web 管理入口、注册接收、心跳接收、资产接收、任务下发、持久化编排 |
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
- 日志库属于 `libs/core` 的实现细节；`apps/*`、`libs/protocol` 和 `libs/platform` 只依赖项目自有日志接口。

## 日志边界

- 运行日志统一通过 `libs/core` 暴露的日志门面写出，不直接在业务代码中 `#include <spdlog/...>` 或使用第三方日志类型。
- Agent 和 Server 负责在进程入口完成日志初始化，包括日志级别、控制台输出和文件输出路径装配。
- 日志上下文应优先使用项目自有标识字段，例如 `agent_id`、`request_id`、`task_id`、`audit_id`，避免在各处自由拼接不一致前缀。
- 运行日志用于启动、排障和开发观测；审计事件用于安全留痕和业务事实记录。二者可以共享上下文字段，但不能视为同一通道。
- 如果后续需要切换日志库、补充脱敏、统一字段或增加测试 sink，应只修改 `libs/core` 内部实现，不改变业务侧调用方式。

## v0.1 依赖基线

| 能力 | 库 | 模块 | 原因 |
| --- | --- | --- | --- |
| 异步网络 | `boost-asio` | Agent, Server | 跨平台异步 I/O 基础，适合长期运行进程 |
| HTTP/2 | `nghttp2` | Agent, Server | 轻量 HTTP/2 长连接控制通道，见 ADR 0007 |
| Protobuf | `protobuf-lite` | Agent, Server, Protocol | 控制消息 schema 与二进制编码 |
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
| 包压缩 | `zstd`, `libarchive` | v0.3 | 自更新包、发布包和回滚流程设计确认后引入 |
| Web 前端构建链 | TBD | v0.3+ | Server 内置 Web 管理入口已由 ADR 0010 确认；前端构建链在实现阶段按静态产物托管需求选择 |
| 插件运行时 | external process / WASM / dynamic library | long term | 威胁模型和权限边界完成后再决策 |

## 网络模型

Agent 主动出站连接 Server。主控制通道采用 [ADR 0007：Agent 控制通道采用 HTTP/2 长连接与 protobuf-lite](adr/0007-agent-control-channel-http2-protobuf-lite.md)。

控制接口：

```text
POST /v1/control/events
GET  /v1/control/commands
```

控制流内使用 length-prefixed protobuf frame。Agent 上传 `AgentEvent`，Server 下发 `ServerCommand`。新增控制面能力必须优先设计为 HTTP/2 control message；协议迁移边界由 ADR 0007 统一维护。

## Web 管理面

Web 管理入口由 `zfleet_server` 内置托管，见 [ADR 0010：Server 内置 Web 管理入口](adr/0010-web-management-entry.md)。Web 是管理面，不是 Agent 控制面：

- Agent 控制通道继续使用 HTTP/2 长连接与 protobuf-lite；
- 浏览器访问静态前端和 `/api/v1/...` JSON 管理 API；
- 管理 API 读取 Server 结构化状态、资产快照、安装包仓库、发布 channel 和注册 token；
- Web 不直接读写 protobuf 控制流，不把任务下发、心跳或资产上报退回 REST polling；
- 默认只读展示 Agent 状态和资产，后台写操作仅限安装包上传、校验、发布、退役和注册 token 管理；
- 后台写操作必须写审计事件，且不得引入 shell、插件或高风险写入任务入口。

最小页面入口：

- `/install`：Agent 安装页，展示平台、架构、channel、发布版本、下载链接、摘要和注册 token 安装命令；
- `/agents`：Agent 状态列表，支持按状态、平台、版本筛选；
- `/agents/{agent_id}`：Agent 资产详情，展示最新和历史资产快照；
- `/admin/packages`：Agent 安装包管理，支持上传、校验、发布到 channel 和退役。

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

Server 使用 SQLite 持久化 Agent 当前状态、资产快照、任务和审计事件。数据库采用“结构化查询列 + protobuf blob + 审计 JSON 摘要”的模型：Web/API、运维查询和状态流转依赖结构化列；控制协议原文或任务类型专属 payload 使用 protobuf blob；`audit_events.payload_json` 仅作为人读排障摘要，不是控制协议源数据。心跳不落库，运行期在线判断依赖控制连接注册表；数据库只记录上线、离线等状态变化时间。

```sql
create table if not exists agents (
  agent_id text primary key,
  first_seen_at text not null,
  last_seen_at text not null,
  last_online_at text not null,
  last_offline_at text,
  platform text not null,
  agent_version text not null,
  status text not null
);

create table if not exists asset_snapshots (
  snapshot_id integer primary key autoincrement,
  agent_id text not null,
  occurred_at text not null,
  hostname text not null,
  os text not null,
  os_version text,
  arch text not null,
  agent_version text not null,
  event_blob blob not null
);

create table if not exists tasks (
  task_id text primary key,
  protocol_version text not null,
  agent_id text not null,
  task_type text not null,
  capability_level text not null,
  created_at text not null,
  expires_at text not null,
  input_blob blob not null,
  state text not null,
  assigned_at text,
  completed_at text
);

create table if not exists task_results (
  task_id text primary key,
  protocol_version text not null,
  request_id text not null,
  agent_id text not null,
  task_type text not null,
  occurred_at text not null,
  status text not null,
  error_code text,
  error_retryable integer,
  result_blob blob,
  error_blob blob
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

审计事件应入库，文本日志只作为运行排障辅助。后续接入 Web 时，列表、筛选和聚合优先读取结构化列；详情页需要完整协议内容时由 Server 解码 protobuf blob 后输出 JSON API。

Web 管理入口需要补充以下 Server 侧持久化对象：

```sql
create table if not exists agent_packages (
  package_id text primary key,
  component text not null,
  version text not null,
  platform text not null,
  arch text not null,
  filename text not null,
  storage_path text not null,
  size_bytes integer not null,
  sha256 text not null,
  manifest_json text not null,
  status text not null,
  uploaded_at text not null,
  validated_at text,
  published_at text,
  retired_at text
);

create table if not exists package_publications (
  publication_id text primary key,
  package_id text not null,
  channel text not null,
  platform text not null,
  arch text not null,
  is_default integer not null,
  published_at text not null,
  published_by text
);

create table if not exists registration_tokens (
  token_id text primary key,
  token_hash text not null,
  purpose text not null,
  channel text,
  platform text,
  arch text,
  max_uses integer,
  use_count integer not null,
  status text not null,
  created_at text not null,
  expires_at text not null,
  revoked_at text
);
```

安装包本体存储在 Server 管理的包仓库目录，数据库只保存路径、摘要和发布状态。注册 token 只保存哈希，不保存明文。

## 配置模型

配置文件建议使用 TOML：

```toml
[server]
control_listen = "127.0.0.1:8081"
management_listen = "127.0.0.1:8080"
database_path = "data/zfleet.db"
package_repository = "data/packages"
web_static_dir = ""

[agent]
control_url = "https://127.0.0.1:8081"
data_dir = "data/agent"
state_path = "state.toml"

[log]
level = "info"
file = "logs/zfleet.log"
```

安装目录由 launcher stub 根据固定安装结构传递给真实进程，不从 TOML 读取，也不写回 TOML。未指定配置文件时，Server 默认读取并维护 `<install_dir>/etc/server.toml`，Agent 默认读取并维护 `<install_dir>/etc/agent.toml`；`-c, --config` 可指定自定义配置文件。一般路径的绝对路径保持不变，相对路径以安装目录为基准。Server 的 `web_static_dir` 留空时例外：运行时从 active release 的真实二进制定位 `<release>/share/web`，确保页面资源与后端版本及回滚一致；配置非空值或 `--web-static-dir` 为运维显式覆盖。命令行参数只覆盖少量启动级配置，覆盖后的有效配置会在路径解析前写回配置文件。

Web 静态资源源码位于 `apps/server/web/`，保持 HTML、CSS 和 JavaScript 文件独立于 C++ 翻译单元。Server package 将该目录写入 `payload/share/web/`，installer 因而把资源放入对应 `releases/<version>/share/web/`。管理 listener 启动时验证必需资源文件，运行时只通过固定页面路由和安全的 `/assets/` 路由按需读取文件。

管理 listener 使用 Boost.Beast 构建每连接异步 HTTP/1.1 会话，accept 路径不等待客户端补齐请求数据，避免浏览器预连接或慢连接阻塞其他管理请求。当前输入防护默认包含 `10s` 请求读取超时、`16 KiB` header 上限和 `128 MiB` body 上限；超时或超限请求在进入管理 API 前终止。Agent package 上传使用 Beast `buffer_body` 分块写入 staging 文件，校验通过后才进入包仓库，不将整个 ZIP 加载到请求内存中。

Agent 本地持久化信息应区分“配置”和“状态”：

- 配置文件用于运维显式输入，例如 `control_url`、`data_dir`、`state_path` 和后续心跳周期、日志级别。
- 状态文件用于程序生成并维护的本地稳定状态，例如 `agent_id`。
- `agent_id` 不应写回运维配置文件，避免因配置模板覆盖、同步或人工编辑导致身份漂移。
- `state_path` 与其他路径配置遵守同一基准：绝对路径不变，相对路径按安装目录解析。后续如果本地状态增多，可平滑迁移到 SQLite，而不改变“配置与状态分离”的边界。

## 相关 ADR

- [0001：Agent 与 Server 使用 C++](adr/0001-use-cpp.md)
- [0002：Agent 使用主动出站连接](adr/0002-agent-outbound-connection.md)
- [0003：任务默认只读](adr/0003-default-read-only-tasks.md)
- [0004：v0.1 第三方依赖基线](adr/0004-third-party-dependency-baseline.md)
- [0005：日志通过 Core 门面封装](adr/0005-log-abstraction-boundary.md)
- [0010：Server 内置 Web 管理入口](adr/0010-web-management-entry.md)
