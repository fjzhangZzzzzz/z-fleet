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

## 本地运行

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
```

说明：

- `--config` 用于指定配置文件路径。
- `--data-dir` 可覆盖配置中的 `data_dir`。
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
