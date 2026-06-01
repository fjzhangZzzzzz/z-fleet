# ADR 0015：launcher stub 归属 installer 的安装模型补充

状态：已接受
日期：2026-05-29
补充： [ADR 0006](0006-manifest-driven-installer.md)

## 背景

[ADR 0006](0006-manifest-driven-installer.md) 已经确定 z-fleet 采用 `releases/<version>`、`active-version` 与固定 `bin/zfleet_*` launcher stub 的启动模型，但没有把以下问题定义完整：

- launcher stub 在正式发布与首次安装时由谁交付；
- launcher stub 的刷新由哪一层保证；
- launcher 与 installer 的版本关系如何表达；
- launcher 是否应作为独立组件、独立包或独立安装目录存在。

当前实现中，本地开发脚本会在 `apply` 后复制 launcher stub，而正式安装脚本只安装 installer 与 agent package，导致“模型要求存在固定入口 stub”与“正式安装链路未明确交付 stub”之间存在缺口。

## 决策

1. **launcher stub 归属 installer，不作为独立发布组件**

   launcher 不再视为与 `agent`、`server`、`installer` 同级的常规组件，不引入独立 channel、独立 package 或独立安装状态。

2. **launcher 与 installer 共用版本语义**

   不单独维护 launcher 版本，继续只维护 installer 版本，并复用 `min_installer_version` 作为兼容性约束。若某个 agent/server package 依赖新的 launcher 行为，应通过提升 `min_installer_version` 触发安装端先满足对应 installer 版本。

3. **installer release 必须携带 launcher 模板资产**

   installer package 在 `installer/releases/<version>/bin/` 内除真实 installer 二进制外，还必须包含单个 `zfleet_launcher` 模板二进制。`zfleet_installer apply` 与 `rollback` 使用该模板复制并重命名出固定入口 `zfleet_installer`、`zfleet_agent`、`zfleet_server`。launcher 资产属于 installer release 的一部分，而不是额外顶级目录或独立安装对象。

4. **`apply` 负责部署和刷新固定入口 stub**

   `zfleet_installer apply` 在完成 package 解包、校验、切换 active version 后，必须确保目标组件固定入口 stub 存在且与当前 active installer release 匹配。该行为属于 installer 的标准安装语义，而不是仅由本地脚本或平台 bootstrap 补充。

5. **不新增独立 `launcher/` 安装目录**

   安装根目录继续只保留组件级目录：

   - `<root>/installer/`
   - `<root>/agent/`
   - `<root>/server/`

   其中 `agent/bin/zfleet_agent`、`server/bin/zfleet_server` 与 `installer/bin/zfleet_installer` 由 installer 负责部署和必要刷新；launcher 生命周期不单独暴露为第四个顶级目录。

6. **源码语义上将 launcher 收敛到 installer**

   即使构建目标在过渡期内仍可独立存在，源码与发布语义上也应将 launcher 视为 installer 的内部能力，避免继续把它表达为独立业务组件。

## 影响

- 正向：正式安装、首次 bootstrap、本地脚本与后续升级链路使用同一套 launcher 交付语义；`min_installer_version` 的含义更完整；避免引入第四种常规发布组件。
- 负向：installer package 内容与 `apply` 逻辑需要扩展；CI 与运维文档需要从“检查 launcher 构建产物”调整为“检查 installer 是否携带并部署 launcher 资产”。
- 后续工作：补充 installer package 内容约定、`apply` 的 stub 部署行为、bootstrap 脚本调用路径，以及相应 CI 检查与测试覆盖。
