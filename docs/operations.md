# 运维

状态：草案
最后更新：2026-05-17
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录本地运行、构建、配置、部署、发布、升级、回滚和排障流程。

## 构建

项目使用 CMake + vcpkg manifest mode。

```bash
./scripts/build.sh linux-debug
./scripts/test.sh linux-debug
```

标准预设：

- `linux-debug`
- `linux-release`
- `windows-debug`
- `windows-release`

## 测试

测试通过 CTest 运行，单元测试与小型组件测试统一使用 Catch2。

```bash
./scripts/test.sh linux-debug
ctest --preset linux-debug
```

约定：

- 新增测试默认使用 Catch2，而不是手写 `main()` 返回码式 smoke test。
- 测试目标应接入 Catch2 测试发现机制，保证单个 `TEST_CASE` 可被 CTest 直接发现和执行。
- 功能实现、协议调整、本地状态和错误路径变更应优先测试先行，或至少与测试在同一提交中同步落地。

## 本地运行

Server 当前最小本地运行方式：

```bash
./build/linux-debug/apps/server/zfleet_server --database-path /tmp/zfleet-server/zfleet.db
```

配置文件示例：

```toml
[server]
listen = "127.0.0.1:8080"
database_path = "data/zfleet.db"

[log]
level = "info"
file = "logs/zfleet-server.log"
enable_console = true
```

说明：

- `--config` 用于指定配置文件路径。
- `--database-path` 可覆盖配置中的 `database_path`。
- `--listen` 可覆盖配置中的 `listen`。
- `--log-level` 可覆盖配置中的 `log.level`。
- Server 启动时会自动初始化 SQLite 数据库和 v0.1 最小 schema。

Agent 当前最小本地运行方式：

```bash
./build/linux-debug/apps/agent/zfleet_agent --data-dir /tmp/zfleet-agent
```

配置文件示例：

```toml
[agent]
server_url = "http://127.0.0.1:8080"
data_dir = "data/agent"
state_file = "state.toml"

[log]
level = "info"
file = "logs/zfleet-agent.log"
enable_console = true
```

说明：

- `--config` 用于指定配置文件路径。
- `--data-dir` 可覆盖配置中的 `data_dir`。
- `--log-level` 可覆盖配置中的 `log.level`。
- Agent 首次启动会在 `data_dir/state.toml` 生成本地状态文件。
- 当前 `state.toml` 只保存 `agent_id`，属于程序持久化状态，不应与运维配置混写。
- 重复启动同一 `data_dir` 时，Agent 必须复用同一个 `agent_id`。

## 部署、发布与升级

当前 installer 尚未实现；后续部署、发布、升级和回滚流程以 [ADR 0006：清单驱动的 zfleet_installer 与 active-version 启动模型](adr/0006-manifest-driven-installer.md) 作为决策依据。

本文档当前仅承接实施分期，不展开未实现命令、完整 manifest schema 或平台脚本细节：

- `P1`：文档收口与承接，确认 ADR 0006 为决策源，并在运维文档集中记录部署、发布、升级、回滚主题入口。
- `P2`：落地 manifest/package 基本结构，以及 installer 最小 `apply` / `status` 能力。
- `P3`：补齐 active-version launcher 与 `rollback` 流程，形成相邻版本切换能力。
- `P4`：完成跨平台验证、脚本接入和运维说明完善。

## 排障

- 构建失败：
  先执行 `./scripts/build.sh linux-debug`，如果失败，优先检查 `VCPKG_ROOT`、CMake preset 和本地编译器是否可用。
- Server 监听失败：
  检查 `--listen` 或配置文件中的 `server.listen` 是否被占用，确认绑定地址和端口格式为 `host:port`。
- Agent 注册、心跳或资产上报失败：
  先检查 `server_url` 是否可达，再查看 Agent 与 Server 日志中带 `request_id`、`agent_id`、`route` 的错误记录。
- `state.toml` 未生成或 `agent_id` 异常变化：
  检查 `data_dir` 是否可写，并确认重复启动时仍使用同一 `data_dir`。
- 数据库文件或表未生成：
  检查 Server 启动参数中的 `database_path` 是否可写，并确认 `agents`、`heartbeats`、`asset_snapshots`、`audit_events` 表已初始化。
- 非法 JSON 或未归类请求：
  `v0.1` 不会将这类请求写入 `audit_events`；应通过 Server 运行日志排查，后续版本再纳入独立安全异常事件模型。
