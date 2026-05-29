# 测试分层

状态：草案
最后更新：2026-05-27

本文定义 z-fleet 的测试分层、目录边界和覆盖矩阵。目标不是增加测试数量本身，而是让测试更容易定位、扩展和维护。

## 分层原则

- 单元测试验证单个模块的纯逻辑、状态转换、序列化和边界校验。
- 组件测试验证单个应用或库在本地文件系统、数据库、进程和局部网络条件下的行为。
- 集成测试验证跨组件闭环，包括 agent、server、installer、installer 内部 launcher stub 和 HTTP/2 控制链路。
- 同一类行为只应在一个主层级里作为“主断言”，其他层级只做必要的联通验证。

## 目录约定

- `libs/*/tests/`：对应库的单元测试和少量局部组件测试。
- `apps/*/tests/`：对应应用的组件测试，优先覆盖本应用内部流程。
- `tests/integration/`：跨组件端到端场景和真实链路验证。
- `tests/support/`：测试公共辅助函数，不承载业务断言。

## 当前矩阵

| 模块 | 当前位置 | 角色 | 覆盖状态 | 备注 |
| --- | --- | --- | --- | --- |
| `libs/core` | `libs/core/tests/` | 单元 | 较完整 | 适合继续补纯逻辑边界 |
| `libs/protocol` | `libs/protocol/tests/` | 单元 | 较完整 | 适合补非法组合和状态机边界 |
| `libs/package` | `libs/package/tests/` | 单元 | 较完整 | 适合补损坏包、unsafe path、重复项 |
| `libs/platform` | `libs/platform/tests/` | 单元 | 基础覆盖 | 适合补平台差异分支 |
| `apps/installer` | `apps/installer/tests/` 与 `apps/installer/launcher/tests/` | 组件 | 较完整 | 适合继续补 launcher 刷新、失败恢复和平台差异 |
| `apps/packager` | `apps/packager/tests/` | 组件 | 较完整 | 适合补组合矩阵和错误输入 |
| `apps/agent` | `apps/agent/tests/` | 组件 | 较完整 | 适合补网络失败、重试和半包场景 |
| `apps/server` | `apps/server/tests/` | 组件 | 偏重集成 | 适合拆分纯逻辑与真实 IO |
| `tests/integration` | `tests/integration/` | 集成 | 已有闭环 | 适合拆分为更小主题文件 |

## 组合优先级

1. 成功路径：关键业务流程必须有端到端验证。
2. 失败路径：非法输入、资源缺失、网络失败、校验失败、权限失败必须有断言。
3. 幂等路径：重复执行、重复安装、重复发布、重复回滚必须有断言。
4. 并发路径：数据库写入、连接注册、任务领取、异步回调需要验证互斥和一致性。

## 维护规则

- 新增测试文件优先放到最接近所有权的目录。
- 如果测试需要真实进程、socket、HTTP/2 或数据库，优先视为组件或集成测试，不要放进纯单元文件。
- `tests/integration/` 中的每个文件应尽量只负责一个主题，避免单文件聚合过多场景。
- 当组件测试开始依赖多个模块的真实交互时，应评估是否迁移到 `tests/integration/`。
