# 运维

状态：草案
最后更新：2026-05-13
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录本地运行、构建、配置、部署、升级、回滚和排障流程。

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

## 排障

待补充：

- 构建失败；
- vcpkg 依赖安装失败；
- Server 监听失败；
- Agent 注册失败；
- 心跳或资产上报失败；
- 数据库迁移失败。
