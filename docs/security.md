# 安全

状态：草案
最后更新：2026-05-24
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录 z-fleet 的安全边界、威胁模型、安全默认值、身份、授权、审计和任务风险分级。

## 安全默认值

- z-fleet 只用于用户拥有或获得授权管理的设备。
- Agent 主动连接 Server，不暴露客户端端口。
- 默认只读，写入任务必须显式开启。
- v0.1 不支持 shell 或插件；包升级和回滚是唯一已实现的高风险写入路径，默认关闭。
- 所有注册、心跳、资产上报、任务结果和错误路径必须产生审计事件。
- Web 管理面默认只绑定本机或显式配置的管理地址；后台写操作在没有认证边界时不得对非本机开放。
- 注册 token 只用于 Agent 首次注册或 bootstrap，不得写入安装包，不得在日志、审计和 UI 历史记录中保存明文。

## 威胁模型主题

后续需要补充：

- Agent 身份生成和本地保存；
- 首次注册和重注册流程；
- 注册 token 的签发、作用域、过期、吊销、重放防护和审计；
- Server 身份校验；
- TLS 和证书策略；
- Web 管理面认证、CSRF 防护、会话过期、上传限制和反向代理边界；
- 任务授权和能力分级；
- 自更新签名和回滚保护；
- 密钥轮换；
- 插件系统边界。

## Web 管理面安全边界

Server 内置 Web 管理入口见 [ADR 0010](adr/0010-web-management-entry.md)。Web 管理面必须与 Agent 控制面分离：

- 浏览器只访问静态页面和 `/api/v1/...` 管理 API；
- 静态页面由 Server release 中的独立 `share/web/` 资源提供，启动时校验必需文件，静态路由拒绝目录穿越和符号链接逃逸；
- 管理 HTTP 连接基于 Boost.Beast 异步处理并对读取设置超时及请求大小上限，避免预连接、慢连接或超大上传阻塞管理入口；安装包 body 必须流式写入 staging 后再校验入库；
- Agent 控制流继续使用 HTTP/2 + protobuf-lite，不接受浏览器直接读写；
- Agent 状态和资产详情默认只读；
- 安装包上传、校验、发布、退役、注册 token 生成和吊销属于后台写操作，必须有认证边界并写审计事件；
- 上传安装包必须限制大小，写入 staging 后校验 manifest、路径、大小和 SHA-256，校验通过后才能进入包仓库；
- 上传文件名必须与 manifest 的组件、版本、平台、架构及 `build_type` 一致；`debug` 包禁止发布到 `stable`；
- 首次安装脚本必须在执行 `apply` 前校验 Server 返回的 installer 与 Agent package SHA-256；脚本不承担服务注册或开机自启；
- 安装包 channel 是 Server 侧发布指针，不代表对目标设备执行写任务；
- Agent 升级和回滚必须以 `high_risk_write` 任务执行，只有 `allow_high_risk_write = true` 时管理 API 才能创建任务；设置 desired、串联 installer、任务状态和回滚请求必须写审计。
- 升级执行前 Agent 必须校验下载 ZIP 与 manifest SHA-256；回滚只调用本地 installer 的 active/previous 机制，不执行任意 shell 载荷。

注册 token 最低要求：

- 只存储 token 哈希，不存储明文；
- 必须包含用途、过期时间、状态和使用次数；
- 可选绑定 platform、arch、channel 和最大使用次数；
- 生成、使用、过期、吊销和验证失败都应产生审计事件；
- 日志和审计中只记录 token id 或脱敏摘要。

## 任务能力等级

| 等级 | 含义 | 默认值 |
| --- | --- | --- |
| `readonly` | 只读取系统状态或资产信息，不改变目标设备 | enabled |
| `low_risk_write` | 低风险写入，例如更新 Agent 本地配置 | disabled |
| `high_risk_write` | 修改系统配置、安装软件、删除文件等 | disabled |
| `shell` | 任意命令或脚本执行 | disabled |

任何非 `readonly` 能力进入实现前，都必须先补齐威胁模型、策略开关、审计字段和测试用例。
