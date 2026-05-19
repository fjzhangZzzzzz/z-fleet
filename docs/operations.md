# 运维

状态：草案
最后更新：2026-05-19
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录本地运行、构建、配置、部署、发布、升级、回滚和排障流程。

## 构建

项目使用 CMake + vcpkg manifest mode。

```bash
./scripts/build.sh
./scripts/test.sh
```

未指定 preset 时，Linux 默认使用 `linux-debug`，Windows Git Bash 默认使用 `windows-debug`。也可以显式指定：

```bash
./scripts/build.sh linux-release
./scripts/test.sh linux-release
```

标准预设：

- `linux-debug`
- `linux-release`
- `windows-debug`
- `windows-release`

## 测试

测试通过 CTest 运行，单元测试与小型组件测试统一使用 Catch2。

```bash
./scripts/test.sh
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

当前已完成 P4 范围内的 installer 本地打包与部署脚本接入；后续部署、发布、升级和回滚流程继续以 [ADR 0006：清单驱动的 zfleet_installer 与 active-version 启动模型](adr/0006-manifest-driven-installer.md) 作为决策依据。

当前实施分期如下；已实现项在对应小节记录可执行用法，未实现能力只记录边界：

- `P1`：文档收口与承接，确认 ADR 0006 为决策源，并在运维文档集中记录部署、发布、升级、回滚主题入口。
- `P2`：落地 manifest/package 基本结构，以及 installer 最小 `apply` / `status` 能力。
- `P3`：补齐 active-version launcher 与 `rollback` 流程，形成相邻版本切换能力。
- `P4`：完成本地 `package` / `deploy-local` 脚本接入、bootstrap stub 复制和运维说明完善。
- `P5.1`：提取 `scripts/lib/common.sh` 公共脚本库，为后续 C/C++ 打包工具和归档部署准备。
- `P5.2`：由 C/C++ packager 接管目录 package 生成，`scripts/package.sh` 只保留 CLI 编排、构建前置和路径校验。
- `P5.3`：增加归档输入支持，保持目录包作为默认发布形态。
- `P5.4`：将安装包归档收敛为标准 `.zip` 容器，支持按需列出条目、单独读取 `META/manifest.json`，并以流式方式处理 `Create` / `Extract` 的 payload，避免大文件 OOM；后续签名只需在 `META/` 下新增 manifest 签名文件，不影响归档格式。

### P2：installer 最小命令

当前支持**目录形式 package** 和 **`.zip` 压缩归档包**：

- `<package_dir>/META/manifest.json`
- `<package_dir>/payload/...`

归档包使用标准 ZIP 容器和 deflate 压缩。归档库支持不完整解压即可列出条目、单独读取 `META/manifest.json`，便于后续先验证安装包元信息；当前 `zfleet_installer apply` 仍先解压到临时目录，再复用目录 package 的完整性校验。Create / Extract 都采用流式处理，避免一次性把大 payload 载入内存。

最小命令：

```bash
./build/linux-debug/apps/installer/zfleet_installer apply \
  --root /tmp/zfleet-root \
  --package /tmp/packages/agent-0.1.0

./build/linux-debug/apps/installer/zfleet_installer status \
  --root /tmp/zfleet-root \
  --component agent
```

`status` 输出为单行 JSON。当前状态值：

- `not_installed`
- `installed`
- `corrupt`

示例：

```json
{"component":"agent","state":"installed","version":"0.1.0"}
```

### P2：manifest 最小 schema

```json
{
  "schema_version": 1,
  "component": "agent",
  "version": "0.1.0",
  "min_installer_version": "0.1.0",
  "files": [
    {
      "source": "payload/bin/zfleet_agent",
      "target": "bin/zfleet_agent",
      "size": 123,
      "sha256": "64 lowercase hex chars",
      "executable": true
    }
  ],
  "signatures": []
}
```

P2 约束：

- `component` 仅支持 `agent`、`server`、`installer`。
- `min_installer_version` 当前只要求字段存在且非空，不做版本比较。
- `source`、`target` 必须是安全相对路径；`source` 必须位于 `payload/` 下，`target` 不得写入 `META/`。
- `apply` 仅做完整性校验：检查文件存在、普通文件属性、大小、SHA-256，且 source 路径不得经过符号链接。
- `signatures` 字段可缺省或为空；P2 **不做签名验证，也不提供签名来源认证**。

### P3：rollback 与 launcher

P3 在 P2 基础上补齐相邻版本切换能力，但**不包含**打包脚本、归档解包、签名、自更新下载、服务托管、运行中进程替换或启动后健康探针。

新增命令：

```bash
./build/linux-debug/apps/installer/zfleet_installer rollback \
  --root /tmp/zfleet-root \
  --component agent
```

`previous-version` 语义：

- 状态文件仍位于 `<root>/zfleet/<component>/var/`。
- `active-version` 记录当前 launcher 应启动的版本。
- `previous-version` 只记录**相邻上一健康版本**。
- 首次 `apply` 只写 `active-version`，不生成 `previous-version`。
- 当健康 active 从 `A` 切到 `B` 且 `A != B` 时，installer 先写 `previous-version=A`，再写 `active-version=B`。
- 如果当前 active 已损坏，允许通过 `apply` 新版本修复，但不会把损坏版本写入 `previous-version`。
- `rollback` 成功从 `B -> A` 后，installer 会写 `previous-version=B`，再写 `active-version=A`，因此下一次 `rollback` 可再切回 `B`。

launcher 目录与启动模型：

- 固定入口为 `<root>/zfleet/<component>/bin/zfleet_*`。
- launcher 按自身路径推导 `<component_root>`，读取 `<component_root>/var/active-version`。
- 真实目标位于 `<component_root>/releases/<version>/bin/<same executable name>`。
- launcher 仅做启动路径安全检查：`active-version` 必须是安全单段版本，目标必须存在且为普通可执行文件。
- 参数按原样透传，不经过 shell；POSIX 通过 `execv` 替换当前进程，Windows 分支等待子进程并返回其退出码。
- P3 **不负责** 服务级重启、运行中进程替换或旧进程清理；新版本生效点仍是下一次通过固定 stub 路径启动组件。

### P4：本地打包与部署脚本

P4 提供两个仓库脚本：

- `scripts/package.sh`：从 `build/<preset>/apps/<component>/zfleet_<component>[.exe]` 编排 package，默认生成目录包，`--archive` 时调用 `build/<preset>/apps/packager/zfleet_packager[.exe] pack-archive` 生成 ZIP 安装包。
- `scripts/deploy-local.sh`：编排本地 `apply` / `status` / `rollback`，并在安装成功后复制 launcher stub 到固定 bootstrap 路径。

P5.1 在 P4 脚本基础上提取 `scripts/lib/common.sh` 作为公共 shell 函数库，用于复用路径解析、平台判断和参数错误处理。该文件只提供 `zf_` 前缀函数与仓库路径推导，不直接执行构建、打包或部署动作。

P5.2 由 C/C++ packager 接管目录 package 生成，`scripts/package.sh` 只保留 CLI 编排、构建前置和路径校验。当前阶段目录 package 仍是默认输出，不做签名或动态库递归收集。

P5.3 在 P5.2 基础上补齐归档输入支持。归档内部仍然只包含 `META/manifest.json + payload/...`，不纳入签名、远程下载、service 管理或运行中替换。

P5.4 在 P5.3 基础上将归档格式固定为标准 `.zip`，并明确采用流式 Create / Extract，保证归档/解包过程不因大文件触发 OOM。签名后续只需在 `META/` 下增加 manifest 签名文件，不改变归档格式。

P4 仍然只支持组件：

- `agent`
- `server`
- `installer`

#### package 目录布局

`package.sh` 默认只编排目录 package 生成，不做签名或下载；加 `--archive` 时改为生成 `.zip` 归档。默认输出到 `<repo>/build/packages/<component>/<version>/`：

```text
build/packages/
  agent/
    0.1.0/
      META/
        manifest.json
      payload/
        bin/
          zfleet_agent
```

`manifest.json` 由 packager 生成，最小结构如下：

```json
{
  "schema_version": 1,
  "component": "agent",
  "version": "0.1.0",
  "min_installer_version": "0.1.0",
  "files": [
    {
      "source": "payload/bin/zfleet_agent",
      "target": "bin/zfleet_agent",
      "size": 123,
      "sha256": "64 lowercase hex chars",
      "executable": true
    }
  ],
  "signatures": []
}
```

约束：

- `version` 和 `min_installer_version` 只允许安全单段字符：`[A-Za-z0-9._-]+`，且不得为 `.` 或 `..`。
- package 目录已存在时默认失败；只有 `--force` 才会删除**将要生成的那个 package 目录**。
- `component` 目录隔离，路径保持下划线命名风格，不引入额外 bundle 或多文件层级。
- 当前只收集主程序二进制，不递归收集动态库，不处理签名，不生成压缩包。

#### package.sh 用法

```bash
./scripts/package.sh --component <agent|server|installer> --version <version> \
  [--preset <preset>] \
  [--output-dir <dir>] \
  [--min-installer-version <version>] \
  [--build|--no-build] \
  [--force]
```

默认值：

- Linux 默认 `--preset linux-debug`
- `MINGW` / `MSYS` / `CYGWIN` 默认 `--preset windows-debug`
- `--output-dir` 默认 `<repo>/build/packages`
- `--min-installer-version` 默认 `0.1.0`
- 默认执行 `--build`

示例：

```bash
./scripts/package.sh --component agent --version 0.1.0

./scripts/package.sh --component server --version 0.1.1-rc1 \
  --preset linux-release \
  --no-build

./scripts/package.sh --component installer --version 0.1.0 \
  --output-dir /tmp/zfleet-packages \
  --force

./scripts/package.sh --component agent --version 0.1.0 \
  --archive

./scripts/package.sh --component agent --version 0.1.0 \
  --preset linux-release \
  --archive
```

说明：

- 日志写到 `stderr`。
- `stdout` 最后一行输出 package 绝对路径；目录模式输出 package 目录，`--archive` 模式输出 `.zip` 绝对路径。
- 目录模式下，hash、manifest 和目录复制逻辑都由 `zfleet_packager pack-dir` 负责；`--archive` 模式下对应逻辑由 `zfleet_packager pack-archive` 负责，`package.sh` 只做参数编排与输出转发。
- `--archive` 会调用 `zfleet_packager pack-archive`，其余参数含义与目录模式保持一致。

#### deploy-local.sh 用法

```bash
./scripts/deploy-local.sh apply --component <component> --version <version> \
  [--root <root>] \
  [--preset <preset>] \
  [--packages-dir <dir>] \
  [--package-dir <dir>] \
  [--force-package] \
  [--build|--no-build]

./scripts/deploy-local.sh status --component <component> \
  [--root <root>] \
  [--preset <preset>]

./scripts/deploy-local.sh rollback --component <component> \
  [--root <root>] \
  [--preset <preset>]
```

默认值：

- `--root` 默认 `/tmp/zfleet-root`
- `--preset` 默认规则与 `package.sh` 相同
- `--packages-dir` 默认 `<repo>/build/packages`

行为：

- `apply` 未指定 `--package-dir` 时，会先调用 `scripts/package.sh` 生成 package。
- `apply` 指定 `--package-dir` 时，package manifest 中的 `component` / `version` 必须与命令行参数一致。
- `apply` 指定 `--force-package` 时，会把 `--force` 透传给 `package.sh`。
- `apply` 使用 `build/<preset>/apps/installer/zfleet_installer[.exe]` 执行安装。
- `apply` 成功后，会把 `build/<preset>/apps/launcher/zfleet_<component>[.exe]` 复制到 `<root>/zfleet/<component>/bin/`，作为固定 bootstrap stub。
- `status` / `rollback` 优先调用已部署的 `<root>/zfleet/installer/bin/zfleet_installer[.exe]`；不存在时回退到 `build/<preset>/apps/installer/zfleet_installer[.exe]`。

#### 首次部署顺序

首次在一个新的 root 上部署时，建议顺序如下：

1. 先部署 `installer`，建立 `<root>/zfleet/installer/` 的 release 与 bootstrap stub。
2. 再部署 `server`。
3. 最后部署 `agent`。

示例：

```bash
./scripts/deploy-local.sh apply --component installer --version 0.1.0
./scripts/deploy-local.sh apply --component server --version 0.1.0
./scripts/deploy-local.sh apply --component agent --version 0.1.0
```

如果 package 已经提前生成，也可以直接传目录：

```bash
./scripts/deploy-local.sh apply \
  --component agent \
  --version 0.1.0 \
  --package-dir /tmp/zfleet-packages/agent/0.1.0 \
  --no-build
```

#### 本地部署后的目录布局

安装完成后，本地 root 的最小布局如下：

```text
/tmp/zfleet-root/
  zfleet/
    agent/
      bin/
        zfleet_agent
      releases/
        0.1.0/
          META/
            manifest.json
          bin/
            zfleet_agent
      var/
        active-version
        previous-version
```

说明：

- `bin/zfleet_*` 是 launcher stub，不是 release 内真实业务二进制。
- `releases/<version>/bin/zfleet_*` 才是 manifest 管理的真实目标。
- 首次安装通常只有 `active-version`；发生版本切换后才会出现 `previous-version`。

#### status 与 rollback 示例

查看状态：

```bash
./scripts/deploy-local.sh status --component agent
```

可能输出：

```json
{"component":"agent","state":"installed","version":"0.1.0"}
```

执行回滚：

```bash
./scripts/deploy-local.sh rollback --component agent
```

常见流程：

1. 部署 `0.1.0`
2. 再部署 `0.1.1`
3. 如需回退，执行 `rollback`
4. launcher 后续从固定 `bin/zfleet_agent` 启动时，将重新指向 `0.1.0`

#### 错误与边界

- 参数错误统一返回 `2`。
- 执行失败统一返回 `1`。
- `package.sh` 找不到构建产物、哈希工具缺失、package 已存在且未加 `--force` 时会失败。
- `package.sh --archive` 找不到构建产物、哈希工具缺失、归档目标已存在且未加 `--force` 时会失败。
- `deploy-local.sh apply` 不负责服务停机、进程替换或运行中热更新；只负责生成 package、调用 installer 和复制 stub。
- `deploy-local.sh status` 如果已部署 installer stub 存在但其 `active-version` 或目标 release 已损坏，会按 installer 的错误或 `corrupt` 状态返回。
- 当前不会自动清理旧版本 package 目录，也不会清理 `<root>/zfleet/<component>/releases/` 中的历史版本。

#### 跨平台边界

- Windows Git Bash 下，如检测到 `cygpath`，脚本会把传给原生 `.exe` 的 `--root` / `--package` 参数转换为 Windows 本地路径。
- package 内部的 `manifest.json` 路径分隔符始终保持 `/`，不随宿主平台变化。
- 当前脚本只处理主二进制与 launcher stub，不负责 `.dll`、`.so`、`.dylib` 的递归打包和安装。
- `deploy-local.sh` 仍主要面向目录包工作流；如果要用 `.zip` 归档包，建议直接调用 `zfleet_installer apply --package <file.zip>`。

#### P4 明确不纳入范围

以下能力不属于 P4：

- 签名与签名验证
- 压缩、归档、解包
- 远程下载与自更新拉取
- service 管理
- 动态库递归收集
- 旧版本自动清理
- 运行中进程替换

## 排障

- 构建失败：
  先执行 `./scripts/build.sh`，如果失败，优先检查 `VCPKG_ROOT`、CMake preset 和本地编译器是否可用。
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
