# 运维

状态：草案
最后更新：2026-05-21
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录当前可执行的本地构建、测试、运行、打包、安装、状态、回滚和排障流程。安装模型以 [ADR 0006：清单驱动的 zfleet_installer 与 active-version 启动模型](adr/0006-manifest-driven-installer.md) 为决策源。

## 构建与测试

项目使用 CMake + vcpkg manifest mode。日常和 CI 应优先使用仓库脚本，避免本地命令与 CI 漂移。

```bash
./scripts/build.sh
./scripts/test.sh
```

未指定 preset 时，Linux 默认使用 `linux-debug`，Windows Git Bash 默认使用 `windows-debug`。也可以显式指定：

```bash
./scripts/build.sh linux-release
./scripts/test.sh linux-release
```

需要避免构建占满 CPU 时，可限制 CMake 和 vcpkg 构建并发：

```bash
./scripts/build.sh linux-debug --jobs 4
ZF_BUILD_JOBS=4 ./scripts/build.sh linux-debug
```

`--jobs` 优先于 `ZF_BUILD_JOBS`。该限制控制的是并发任务数，不是严格 CPU 百分比。

标准预设：

- `linux-debug`
- `linux-release`
- `windows-debug`
- `windows-release`

测试通过 CTest 运行：

```bash
ctest --preset linux-debug
```

## vcpkg

项目使用 `scripts/vcpkg.sh` 作为 vcpkg 入口，统一工具版本、安装目录和 binary cache：

```bash
./scripts/vcpkg.sh bootstrap
./scripts/vcpkg.sh list
./scripts/vcpkg.sh install --jobs 4
```

目录约定：

- `.tools/vcpkg`：由 `.vcpkg-version` 固定的 vcpkg tool。
- `build/vcpkg_installed`：项目依赖安装目录。
- `.cache/vcpkg/archives`：项目 binary cache。

需要执行原生命令时使用 `exec`：

```bash
./scripts/vcpkg.sh exec search nghttp2
./scripts/vcpkg.sh exec help install
```

单独安装 manifest 依赖时也可限制并发：

```bash
ZF_BUILD_JOBS=4 ./scripts/vcpkg.sh install
./scripts/vcpkg.sh install --jobs 2
```

手动运行 `cmake --preset` 前，需要把项目 vcpkg 环境导入当前 shell：

```bash
./scripts/vcpkg.sh bootstrap
eval "$(./scripts/vcpkg.sh env)"
cmake --preset linux-debug
```

## 本地运行

Server 当前最小本地运行方式：

```bash
./build/linux-debug/apps/server/zfleet_server --database-path /tmp/zfleet-server/zfleet.db
```

Server 支持 `--config` 指定配置文件，并可通过 `--database-path`、`--listen`、`--log-level` 覆盖配置项。启动时会自动初始化 SQLite 数据库和当前最小 schema。

Agent 当前最小本地运行方式：

```bash
./build/linux-debug/apps/agent/zfleet_agent --data-dir /tmp/zfleet-agent
```

Agent 支持 `--config` 指定配置文件，并可通过 `--data-dir`、`--log-level` 覆盖配置项。首次启动会在 `data_dir/state.toml` 生成本地状态文件；重复使用同一 `data_dir` 时应复用同一个 `agent_id`。

## 打包

日常和 CI 优先使用 `scripts/make-package.sh`。脚本负责构建组件、准备 staging payload、读取组件 CMake 版本文件并调用 C++ packager；manifest 生成、SHA-256 计算和 ZIP 归档由 `zfleet_packager` 完成。

```bash
./scripts/make-package.sh <agent|server|installer> [--preset <preset>] [--out <dir>] [--no-build] [--force] [--zip]
```

默认行为：

- `--preset` 未指定时使用仓库默认 preset。
- `--out` 未指定时输出到 `build/packages`。
- 默认会先构建对应 preset；`--no-build` 用于复用已有构建产物。
- 默认生成目录 package；`--zip` 生成标准 `.zip` 安装包。
- 版本来自 `build/<preset>/apps/<component>/zfleet_<component>_version.txt`，不通过脚本手工输入。
- 当前脚本只收集组件主程序二进制到 `payload/bin/`，动态库或其他依赖自动收集尚未实现。

示例：

```bash
./scripts/make-package.sh agent --zip
./scripts/make-package.sh server --preset linux-release --out /tmp/zfleet-packages --force
```

需要对已准备好的 payload 目录打包时，可直接调用 packager：

```bash
./build/linux-debug/apps/packager/zfleet_packager pack \
  --component agent \
  --version 0.1.0 \
  --payload-dir /tmp/payload \
  --entry bin/zfleet_agent \
  --output-dir /tmp/packages \
  --min-installer-version 0.1.0 \
  --zip
```

目录 package 与 ZIP package 具有相同内部结构：

```text
META/
  manifest.json
payload/
  bin/
    zfleet_agent
```

manifest 最小 schema：

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
  ]
}
```

当前 `min_installer_version` 只要求存在且非空，不做版本比较；当前不要求 `META/manifest.sig`，尚不做 manifest 签名验证。

## 安装部署

本地部署优先使用 `scripts/install-local.sh`。脚本面向本地 root 安装，会按需调用 `make-package.sh` 生成 package，再调用 installer `apply`，安装成功后复制对应 launcher stub 到固定 bootstrap 路径。

```bash
./scripts/install-local.sh apply <agent|server|installer> [--root <root>] [--preset <preset>] [--zip] [--force] [--no-build]
./scripts/install-local.sh status <agent|server|installer> [--root <root>] [--preset <preset>]
./scripts/install-local.sh rollback <agent|server|installer> [--root <root>] [--preset <preset>]
```

默认行为：

- `--root` 未指定时使用 `/tmp/zfleet-root`。
- `--preset` 未指定时使用仓库默认 preset。
- `apply --zip` 生成并安装 ZIP package；未指定 `--zip` 时生成并安装目录 package。
- `status` 和 `rollback` 优先使用已部署的 `<root>/zfleet/installer/bin/zfleet_installer`；不存在时使用当前构建输出中的 installer。
- `status` 和 `rollback` 不接受 `--zip`、`--force`、`--no-build`。

建议首次本地安装顺序：

```bash
./scripts/install-local.sh apply installer --zip
./scripts/install-local.sh apply server --zip
./scripts/install-local.sh apply agent --zip
```

已有 package 可直接用 installer 安装：

```bash
./build/linux-debug/apps/installer/zfleet_installer apply \
  --root /tmp/zfleet-root \
  --package /tmp/packages/agent/0.1.0.zip
```

查询和回滚：

```bash
./scripts/install-local.sh status agent --root /tmp/zfleet-root
./scripts/install-local.sh rollback agent --root /tmp/zfleet-root
```

`status` 输出单行 JSON：

```json
{"component":"agent","state":"installed","version":"0.1.0"}
```

当前状态值：

- `not_installed`
- `installed`
- `corrupt`

## 安装目录

组件安装在同一 root 下的独立子目录中，互不共享 active version：

```text
<root>/zfleet/
  installer/
  agent/
  server/
```

单个组件目录结构：

```text
<root>/zfleet/<component>/
  bin/
    zfleet_<component>        # launcher stub
  releases/
    <version>/
      META/
        manifest.json
      bin/
        zfleet_<component>    # 实际二进制
  var/
    active-version
    previous-version
```

`apply` 先写入 `.staging/<version>` 并校验 manifest 中声明的文件大小、SHA-256、路径和可执行权限，再切换到 `releases/<version>` 并更新 `var/active-version`。当从健康版本 `A` 切换到 `B` 时，会记录 `previous-version=A`；`rollback` 成功后会交换 active 与 previous。

launcher stub 位于固定 `bin/zfleet_*` 路径，启动时读取 `var/active-version` 并执行 `releases/<version>/bin/zfleet_*`。installer 自身也按同一模型更新，新版本从下一次通过 `installer/bin/zfleet_installer` 调用时生效。

## 当前边界

- ZIP 使用标准容器与 deflate 压缩，支持列出条目、读取指定文件和流式创建/解压。
- 安装包签名尚未验证；后续可在 `META/` 下增加 manifest 签名文件。
- 尚未实现远程下载、自更新调度、service 管理、运行中进程替换或启动后健康确认。
- 脚本尚未自动收集动态库或其他运行时依赖。
- 旧 release 默认保留，尚未实现清理策略。
- 当前本地脚本只支持 `agent`、`server`、`installer` 三个组件。

## 排障

- `component is invalid`：组件名不是 `agent`、`server`、`installer`。
- `package path is invalid`：`--package` 指向的目录或 ZIP 不存在，或 package 内缺少 `META/manifest.json`。
- `manifest is invalid`：manifest schema、组件、版本、路径、摘要或文件属性不满足安全契约。
- `release is corrupt`：已安装 release 与 manifest 不一致，通常需要重新 `apply` 健康版本。
- `active version is missing`：组件尚未安装或 `var/active-version` 丢失。
- `previous version is missing`：当前组件没有可回滚的相邻上一版本。
- Windows Git Bash 下路径异常时，优先使用脚本入口；公共脚本会处理仓库路径和 Windows 路径转换。
