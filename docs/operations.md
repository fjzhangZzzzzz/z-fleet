# 运维

状态：草案
最后更新：2026-05-25
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录当前可执行的本地构建、测试、运行、打包、安装、状态、回滚和排障流程。安装模型以 [ADR 0006：清单驱动的 zfleet_installer 与 active-version 启动模型](adr/0006-manifest-driven-installer.md) 为决策源。

Server 内置 Web 管理入口、安装包 channel 和注册 token 的运维模型以 [ADR 0010：Server 内置 Web 管理入口](adr/0010-web-management-entry.md) 为决策源。

源码构建与终端首次安装的跨平台脚本边界评估见 [ADR 0012：跨平台构建、打包与安装脚本边界](adr/0012-cross-platform-script-boundary.md)。当前已实现 Linux Bash 与 Windows PowerShell 薄 bootstrap 端点。

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
./build/linux-debug/apps/server/zfleet_server \
  --database-path /tmp/zfleet-server/zfleet.db \
  --web-static-dir "$PWD/apps/server/web"
```

Server 支持 `-c, --config` 指定配置文件，并可通过 `--database-path`、`--control-listen`、`--management-listen`、`--package-repository`、`--web-static-dir`、`--log-level` 覆盖配置项。未指定配置文件时默认使用安装目录下的 `etc/server.toml`；通过 launcher stub 启动时，安装目录由固定 `bin/zfleet_server` 路径自动传递给真实进程。直接运行 build 产物不处于 installer release 结构内，因此本地调试 Web 时需要如上显式指定源码静态资源目录。启动时会自动初始化 SQLite 数据库和当前最小 schema。

Web 管理面由 `zfleet_server` 内置托管，配置与 Agent 控制监听分离：

```toml
[server]
control_listen = "127.0.0.1:8081"
management_listen = "127.0.0.1:8080"
management_public_url = "http://127.0.0.1:8080"
database_path = "data/zfleet.db"
package_repository = "data/packages"
web_static_dir = ""
allow_high_risk_write = false
```

安全默认值：

- `management_listen` 默认应绑定本机地址；
- 对非本机开放前必须配置认证边界，例如本地 admin token 或反向代理认证；
- 控制通道和管理 API 使用不同路径，不通过 Web 调用 Agent HTTP/2 protobuf 控制流；
- 安装包上传、发布、退役和注册 token 操作必须写审计事件。
- `management_public_url` 用于生成 Agent 下载升级包的 URL，必须配置为 Agent 可达的管理地址；
- `allow_high_risk_write` 默认为 `false`；只有显式启用后，详情页才能创建升级或回滚任务；
- `web_static_dir` 留空时从当前 active release 的 `share/web/` 加载静态资源；配置非空值时按安装目录解析为显式覆盖；
- 管理 listener 启动前校验必需 HTML、CSS 和 JavaScript 文件；`/assets/` 不接受目录穿越、符号链接文件或非允许类型资源。
- 管理 listener 基于 Boost.Beast 异步处理连接，默认对请求读取应用 `10s` 超时、`16 KiB` header 上限和 `128 MiB` body 上限；超限上传会返回 `413`，慢请求会返回 `408`。Agent ZIP 上传按块写入 staging 文件，校验通过前不会进入包仓库。

Agent 当前最小本地运行方式：

```bash
./build/linux-debug/apps/agent/zfleet_agent --data-dir /tmp/zfleet-agent
```

Agent 支持 `-c, --config` 指定配置文件，并可通过 `--data-dir`、`--state-path`、`--log-level` 覆盖配置项。未指定配置文件时默认使用安装目录下的 `etc/agent.toml`；通过 launcher stub 启动时，安装目录由固定 `bin/zfleet_agent` 路径自动传递给真实进程。绝对路径保持不变；相对路径以安装目录为基准。首次启动会在状态路径生成本地状态文件；重复使用同一状态路径时应复用同一个 `agent_id`。

## 打包

日常和 CI 优先使用 `scripts/make-package.sh`。脚本负责构建组件、准备 staging payload、读取组件 CMake 版本文件并调用 C++ packager；manifest 生成、SHA-256 计算和 ZIP 归档由 `zfleet_packager` 完成。

```bash
./scripts/make-package.sh <agent|server|installer>... [--preset <preset>] [--out <dir>] [--no-build] [--force] [--zip]
```

默认行为：

- `--preset` 未指定时使用仓库默认 preset。
- `--out` 未指定时输出到 `build/packages`。
- 默认会先构建对应 preset；`--no-build` 用于复用已有构建产物。
- 默认生成目录 package；`--zip` 生成标准 `.zip` 安装包。
- 版本来自 `build/<preset>/apps/<component>/zfleet_<component>_version.txt`，不通过脚本手工输入。
- 平台与架构由构建 preset 写入 manifest，上传 Web 管理面后不可人工改写。
- 当前脚本收集组件主程序二进制到 `payload/bin/`；对 Server 还会收集 `apps/server/web/` 到 `payload/share/web/`。动态库或其他运行时依赖自动收集尚未实现。

示例：

```bash
./scripts/make-package.sh agent server --zip
./scripts/make-package.sh server installer --preset linux-release --out /tmp/zfleet-packages --force
```

需要对已准备好的 payload 目录打包时，可直接调用 packager：

```bash
./build/linux-debug/apps/packager/zfleet_packager pack \
  --component agent \
  --version 0.1.0 \
  --platform linux \
  --arch x86_64 \
  --build-type debug \
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
  "platform": "linux",
  "arch": "x86_64",
  "build_type": "debug",
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

`build_type` 必须为 `debug` 或 `release`。安装选项解析 Agent 默认包时会选择满足 `min_installer_version` 的 installer 默认包；当前不要求 `META/manifest.sig`，尚不做 manifest 签名验证。

## Web 安装包发布

Web 后台安装包管理面向 Agent 与 installer package，不改变 `zfleet_installer` 的安装语义。channel 是 Server 侧发布指针，不写入 package manifest，也不改变目标设备上的 `releases/<version>`、`active-version` 和 `previous-version`。

发布流程：

1. 管理员在 `/admin/packages` 上传 Agent 或 installer ZIP package；文件名使用 `zfleet_<component>-v<version>-<platform>-<arch>-<build_type>.zip`。
2. Server 将上传内容流式写入 staging 文件。
3. Server 解析 `META/manifest.json`，校验 component、version、platform、arch、build_type、文件名、路径、文件大小和 SHA-256；目标元数据仅从 manifest 读取。
4. 校验通过后写入包仓库和 `agent_packages` 记录。
5. 管理员将 package 发布到 `stable`、`candidate` 或 `dev` channel。
6. 安装页 `/install` 根据用户选择的平台与 channel 生成最终安装命令；下载链接、默认包解析、架构检测和版本选择由后端与平台脚本完成。

channel 约束：

- 初始 channel 为 `stable`、`candidate`、`dev`。
- 同一 `component + channel + platform + arch + build_type` 同一时间只能有一个默认发布包。
- `debug` 包不可发布到 `stable`。
- 发布新包只移动 channel 指针，不覆盖旧包本体。
- 退役 package 不物理删除，保留审计和回滚参考。

注册 token 与安装流程：

1. 管理员在安装页或后台生成注册 token。
2. token 可绑定用途、过期时间、channel、platform、arch 和最大使用次数。
3. Server 只保存 token 哈希，明文只在创建响应中返回一次。
4. 安装页生成 Linux bootstrap 命令；Windows 可取 `/api/v1/install/windows.ps1`。薄脚本请求安装选项，下载并校验 installer 与 Agent ZIP，先部署 installer、再执行 Agent `apply` 并尝试启动 Agent。
5. Agent 将 token 写入本地配置并在注册事件中发送；Server 对携带 token 的首次注册校验使用次数、过期时间及可选的平台/架构约束，重连不重复消费已用 token。
6. 安装页命令文本由 `/api/v1/install/commands` 下发，前端不再拼接命令。

当前安装页交互约束：

- 只向最终用户暴露“平台”和“发布通道”；
- 不暴露 `control_url`、`arch`、`build_type`；
- 不在页面上直接展示推荐版本和包摘要；
- 由 Server 结合自身配置生成最终安装命令；
- 由平台脚本在目标机器上检测架构并请求对应默认安装包。

Agent 维护流程：

1. 在详情页选择目标 Agent package 并创建升级任务；默认禁止同版本升级和降级。
2. 含 `min_installer_version` 的 Agent 包会先调度满足要求的已发布 installer 包；其成功前 Agent 包任务不可领取。
3. Agent 下载并校验 ZIP 与 manifest 摘要后调用本地 installer `apply`，尝试启动新 Agent；Server 在新版本重连前显示 `waiting_reconnect`。
4. 详情页回滚创建高风险任务，Agent 调用 `zfleet_installer rollback --component agent` 并启动恢复版本；Server 清除旧 desired 目标，在重连后确认成功。
5. `waiting_reconnect` 超过 10 分钟的任务会在下一次管理 API 请求时标记为 `waiting_reconnect_timeout`。

脚本维护：

- Linux 与 Windows 薄脚本模板已外置到 `apps/server/web/scripts/install/`。
- Linux 脚本下载优先 `curl`，其次 `wget`，最后回退 `python3 urllib`，降低工具缺失导致的安装失败概率。

## 安装部署

本地部署优先使用 `scripts/install-local.sh`。脚本面向本地 root 安装，会按需调用 `make-package.sh` 生成 package，再调用 installer `apply`，安装成功后复制对应 launcher stub 到固定 bootstrap 路径。

```bash
./scripts/install-local.sh apply <agent|server|installer>... [--root <root>] [--preset <preset>] [--zip] [--force] [--replace] [--no-build]
./scripts/install-local.sh status <agent|server|installer> [--root <root>] [--preset <preset>]
./scripts/install-local.sh rollback <agent|server|installer> [--root <root>] [--preset <preset>]
```

默认行为：

- `--root` 未指定时使用 `~/zfleet`。
- `--preset` 未指定时使用仓库默认 preset。
- `apply` 可一次指定多个组件；脚本会先构建一次 preset，再为每个组件生成并安装 package。
- 如果目标组件已在本地运行，`apply` 会先停止当前进程，安装完成后自动重新启动该组件。
- `apply --zip` 生成并安装 ZIP package；未指定 `--zip` 时生成并安装目录 package。
- `apply --force` 覆盖 `build/packages` 下已存在的同版本 package 输出。
- `apply --replace` 覆盖本地 root 下已安装的同版本目标组件 release，用于本地开发时重新部署相同版本号的二进制。
- `status` 和 `rollback` 优先使用已部署的 `<root>/installer/bin/zfleet_installer`；不存在时使用当前构建输出中的 installer。
- `status` 和 `rollback` 不接受 `--zip`、`--force`、`--replace`、`--no-build`。

建议首次本地安装顺序：

```bash
./scripts/install-local.sh apply installer --zip
./scripts/install-local.sh apply server agent --zip
```

已有 package 可直接用 installer 安装：

```bash
./build/linux-debug/apps/installer/zfleet_installer apply \
  --root ~/zfleet \
  --package /tmp/packages/agent/0.1.0.zip
```

查询和回滚：

```bash
./scripts/install-local.sh status agent --root ~/zfleet
./scripts/install-local.sh rollback agent --root ~/zfleet
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
<root>/
  installer/
  agent/
  server/
```

单个组件目录结构：

```text
<root>/<component>/
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

launcher stub 会把组件根目录通过内部环境变量传给真实进程。Agent 和 Server 不从配置文件读取 `install_dir`，也不会把 `install_dir` 写回配置文件；默认配置文件位于组件根目录下的 `etc/agent.toml` 或 `etc/server.toml`，可用 `-c, --config` 指向自定义配置文件。配置文件不存在时，启动流程会先按内置默认值生成 TOML；命令行覆盖项会在路径解析前写回配置文件。

Server 安装包额外包含 `share/web/` 静态资源，安装后位于 `releases/<version>/share/web/`。默认配置不固定该路径，真实 `zfleet_server` 依据自身 release 位置加载对应资源；`apply` 和 `rollback` 因此会随二进制一起切换 Web 页面版本。

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
