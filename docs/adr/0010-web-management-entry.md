# ADR 0010：Server 内置 Web 管理入口

状态：已接受
日期：2026-05-24

## 背景

z-fleet 需要提供轻量 Web 入口，覆盖 Agent 首次安装、在线状态查看、资产详情查看和 Agent 安装包发布管理。该能力服务于个人开发者、小团队、Homelab、边缘设备和测试环境，应保持低运维成本，并继续遵守既有安全模型：

- Agent 只主动连接 Server，不暴露客户端端口；
- Agent / Server 主控制通道继续使用 HTTP/2 长连接与 protobuf-lite；
- Web 只作为管理面，不替代 Agent 控制协议；
- 默认只读，写入能力和高风险操作不得借 Web UI 绕过策略；
- 安装包、注册 token、发布和吊销都需要可审计。

## 决策

1. **Web 由 `zfleet_server` 内置托管**

   `zfleet_server` 提供 Web 静态资源托管和 JSON 管理 API。初期不引入独立 Web 服务进程，避免增加部署组件、反向代理强依赖和多进程配置漂移。HTML、CSS 和 JavaScript 保持为独立资源文件，不以 C++ 字符串编译入二进制；前端资源作为 Server 发布包的 `payload/share/web/` 内容安装到对应 release。

   Server 默认从当前真实二进制所在 release 的 `share/web/` 加载资源，因此升级和回滚会同步切换页面与 API 对应版本。运维可通过 `web_static_dir` 或 `--web-static-dir` 显式覆盖资源根目录。管理 listener 启动前必须确认必需页面和资源文件存在，静态路由只允许固定 HTML 页面和受限 `/assets/` 文件类型，不允许目录穿越或符号链接逃逸。

2. **管理 API 与 Agent 控制通道分离**

   Agent 控制面继续使用 ADR 0007 定义的 HTTP/2 长连接 endpoint。浏览器和管理 API 不直接读写 protobuf 控制流，不把控制协议退回 REST polling。

   Web 管理 API 使用独立路径前缀：

   ```text
   /api/v1/...
   ```

   控制通道继续保留：

   ```text
   POST /v1/control/events
   GET  /v1/control/commands
   ```

3. **Web 入口范围**

   v0.x Web 管理入口至少包含：

   - Agent 安装页面：提供最少输入的安装引导、注册 token 驱动的最终安装命令和可复制执行入口；页面可见字段、信息密度和交互约束由 ADR 0013 定义；
   - Agent 状态列表：以紧凑表格展示 Agent ID、hostname、OS、arch、agent_version、online/offline/stale 状态和时间字段，并提供组合筛选；
   - Agent 资产详情：点击列表行打开侧边抽屉，展示最新资产、应用、服务和历史快照摘要；
   - 后台 Agent 安装包管理：上传、校验、列表、发布、退役 Agent 安装包，平台和架构由上传包 manifest 自动识别。

4. **安装包发布支持 channel**

   安装包上传后不可变，以内容摘要和 manifest 元数据定位。发布操作只修改 channel 指针，不覆盖安装包本体。

   初始 channel：

   - `stable`：默认推荐安装版本；
   - `candidate`：灰度验证版本；
   - `dev`：开发和测试版本。

   同一 `component + platform + arch + channel` 同一时间只能有一个默认发布包。灰度策略先以 channel 选择为边界，不在 v0.x 引入复杂百分比分流、设备分组或自动回滚策略。

5. **允许生成注册 token**

   Web 安装页允许生成一次性或短期有效注册 token，用于首次安装命令。token 的展示方式和页面交互由 ADR 0013 约束，但 token 本身必须具备：

   - 明确用途，例如 `agent_register`；
   - 过期时间；
   - 可吊销状态；
   - 可选的 platform、arch、channel、最大使用次数约束；
   - 生成、使用、过期和吊销审计事件。

   token 不写入安装包，不进入日志明文。安装命令可以引用 token，但日志和 UI 后续详情页应只显示脱敏值。

6. **安装包上传和校验边界**

   Server 接收安装包上传时必须流式写入 staging 文件，不能一次性加载大文件到内存。校验通过后再移动到包仓库。校验至少包含：

   - ZIP 或目录 package 内必须存在 `META/manifest.json`；
   - `component` 必须为 `agent`；
   - version、platform、arch、min_installer_version 等元数据满足 schema；
   - 文件路径不得为绝对路径，不得包含 `..`，不得跨组件写入；
   - 文件大小和 SHA-256 与 manifest 一致；
   - 后续签名能力落地后必须校验 `META/manifest.sig` 或等价签名文件。

7. **默认安全姿态**

   Web 管理面默认只监听本地地址或显式配置的管理地址。公网暴露必须由部署者显式开启，并配置认证边界。v0.x 可以先支持本地 admin token 或反向代理认证，但后台管理 API 在没有认证方案时不得默认对非本机开放。

## 建议 API

安装页：

```text
GET  /api/v1/install/options
POST /api/v1/install/tokens
GET  /api/v1/packages/agent/{package_id}/download
```

Agent 状态与资产：

```text
GET /api/v1/agents
GET /api/v1/agents/{agent_id}
GET /api/v1/agents/{agent_id}/assets/latest
GET /api/v1/agents/{agent_id}/assets
```

后台安装包管理：

```text
GET  /api/v1/admin/packages
POST /api/v1/admin/packages
GET  /api/v1/admin/packages/{package_id}
POST /api/v1/admin/packages/{package_id}/validate
POST /api/v1/admin/packages/{package_id}/publish
POST /api/v1/admin/packages/{package_id}/retire
```

API 是管理面契约草案，具体字段以后续 `docs/contracts.md` 修订为准。

页面层的信息架构、文案、交互密度和操作入口位置不在本 ADR 中重复定义，统一由 ADR 0013 约束。

## 数据模型

建议新增安装包表：

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
```

建议新增发布指针表：

```sql
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
```

建议新增注册 token 表：

```sql
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

token 只存储哈希，不存储明文。

## 备选方案

- **独立 Web 管理服务**：部署边界清晰，也便于使用主流 Web 框架，但会引入额外进程、配置、认证和版本兼容问题。当前目标是轻量部署，已拒绝作为 v0.x 默认方案。
- **浏览器直接接入 Agent 控制通道**：可以减少管理 API，但会让 Web 参与 protobuf 控制流，破坏控制面边界，也不适合浏览器安全模型，已拒绝。
- **只提供 CLI，不提供 Web**：实现成本低，但安装包发布、灰度 channel、资产列表和安装指引的可用性不足，不符合当前管理入口目标。
- **不设计 channel，仅保留单一 stable**：实现更简单，但无法支撑灰度发布和候选版本验证，已拒绝。
- **注册 token 后置**：可降低初期安全实现成本，但安装页无法形成完整 bootstrap 闭环，且后续补齐会影响安装命令和审计模型，已拒绝。

## 影响

- `zfleet_server` 需要增加管理 HTTP 路由、静态资源托管、安装包仓库、注册 token 和后台管理 API。
- `docs/contracts.md` 需要补充 Web 管理 API 字段、错误码、审计事件和权限边界。
- `docs/operations.md` 需要补充 Web 监听配置、安装包上传发布、channel 选择、注册 token 生成和排障流程。
- Server 发布包需要包含 Web 静态资源，并保持 `zfleet_installer` 的 manifest 安全契约。
- 静态页面可以独立迭代和审阅，但缺失或不安全的资源目录会使管理 listener 启动失败，避免带着残缺管理面运行。
- 安装包上传、发布、退役、token 生成、token 使用、token 吊销和失败路径都必须写审计事件。
- Web 不引入 shell、插件或高风险写入任务入口；远程任务能力仍按任务能力等级和后续 ADR 演进。

## 实施阶段

1. **管理 API 骨架**
   - 增加 Server 内置管理 HTTP listener 或在现有 listener 上增加明确路由边界；
   - 提供健康检查、静态资源托管和 `/api/v1` JSON 响应基础设施；
   - 默认仅本机监听，配置项显式控制管理入口。

2. **Agent 只读视图**
   - 实现 Agent 列表、详情和资产快照 API；
   - 状态计算结合连接注册表、last_seen_at、last_online_at 和 last_offline_at；
   - 前端实现 Agent 列表和资产详情。

3. **安装包仓库与 channel**
   - 实现安装包上传、staging、manifest 校验、package 入库；
   - 实现 `stable`、`candidate`、`dev` channel 发布指针；
   - 前端实现后台安装包列表、校验、发布和退役。

4. **安装页与注册 token**
   - 实现安装选项 API、下载 API 和注册 token 生成；
   - 后端生成最终安装命令，前端仅负责收集必要输入并展示结果；
   - token 使用、过期、吊销和失败路径写审计。

5. **验证与加固**
   - 补齐 API 单元测试、集成测试和上传包安全测试；
   - 补齐认证边界、CSRF/下载权限、上传大小限制和审计查询；
   - 后续根据部署规模评估是否拆分独立管理服务或引入更完整的前端构建链。
