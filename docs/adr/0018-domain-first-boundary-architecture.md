# ADR 0018：Domain-first 边界架构与外部表示隔离

状态：已接受

日期：2026-06-02

## 背景

项目已经同时使用多种外部表示和存储表示：

- Agent/Server 控制通道使用 HTTP/2 length-prefixed protobuf frame；
- 管理 API、审计 payload 和 package manifest 使用 JSON；
- Agent/Server 配置和状态文件使用 TOML；
- Server 持久化使用 SQLite。

这些格式都适合作为边界表示，但不适合作为业务语义本身。如果业务代码直接围绕 protobuf generated type、`nlohmann::json`、TOML node 或 SQLite row 编写逻辑，会带来以下问题：

- 第三方库和 wire/storage 细节扩散到应用层，模块边界变弱；
- 同一个业务对象被多套 representation type 平行表达，字段和默认值容易漂移；
- 新增字段或 enum 时缺少统一顺序，容易只修改 `.proto`、JSON 或数据库列而遗漏 domain 规则；
- AI Agent 或后续维护者为了局部便利直接 include 底层表示头文件，逐步破坏可读性和低耦合目标；
- 将来替换某个表示方式或补充兼容策略时，修改面会扩散到业务流程代码。

本决策继承 [ADR 0005：日志通过 Core 门面封装](0005-log-abstraction-boundary.md) 的依赖隔离原则，以及 [ADR 0007：Agent 控制通道采用 HTTP/2 长连接与 protobuf-lite](0007-agent-control-channel-http2-protobuf-lite.md) 的 wire contract 边界。

## 决策

项目采用 **Domain-first 边界架构**：

- Domain type 是项目内部唯一业务语义模型。
- JSON、Protobuf、TOML、SQLite row、CLI args 和 HTTP DTO 都是 representation type。
- Representation type 只能出现在 codec、adapter、store、config 等边界模块中。
- 应用层业务流程只依赖 domain API、codec API、store API 和 config API，不直接依赖外部表示库或 generated wire type。
- 新增或扩展类型必须先定义或扩展 domain model，再设计外部表示和映射。

推荐职责划分：

```text
domain module
  enum、struct、业务状态机、默认值、合法状态流转、ToString/FromString

*_codec / *_adapter module
  Domain <-> JSON/protobuf/TOML/HTTP DTO
  编解码、字段校验、默认值兼容、错误归一化

*_store module
  Domain <-> SQLite row
  SQL schema、row 解析、持久化映射和迁移边界

apps/*
  业务流程编排、连接生命周期、任务执行
  不直接操作 representation type
```

新增或扩展类型时的顺序固定为：

1. 定义或扩展 domain type。
2. 明确 domain invariant，例如必填字段、合法 enum、默认值和状态流转。
3. 设计外部表示字段，例如 `.proto` field、JSON key、TOML key 或 SQL column。
4. 在对应 codec、adapter、store 或 config 模块中实现映射。
5. 添加测试覆盖 domain rule 和 representation mapping。

## 强制边界

生产代码必须遵守以下规则：

- Domain public header 不得 include 或暴露 protobuf generated header、`nlohmann::json`、TOML node、SQLite handle 或 row 类型。
- `apps/*` 业务代码不得直接 include protobuf generated header；控制通道 protobuf 细节必须集中到 `libs/protocol` 的 wire codec/adapter 中。
- JSON 库只允许出现在 JSON codec 或 manifest/config 等明确边界模块中。
- TOML 库只允许出现在 config/state 加载与保存边界中。
- SQLite 类型只允许出现在 store/database 边界中，不进入 domain public API。
- 新增 representation library 时，必须同步扩展本 ADR、`AGENTS.md` 和边界检查脚本。

## 备选方案

- **允许业务层直接使用 representation type**：短期改动少，但依赖扩散、字段漂移和格式切换成本高，已拒绝。
- **只用 protobuf 或 JSON 作为全项目统一模型**：可以减少类型数量，但会把 wire/storage 约束误当业务语义，并削弱 manifest、配置、管理 API 等可读性边界，已拒绝。
- **为每种表示建立完全独立的业务模型**：隔离强，但会造成大量重复和映射漂移，已拒绝。
- **仅通过文档约束，不做脚本检查**：解释充分但执行力弱，尤其不利于 AI Agent 长期遵守，已拒绝。

## 影响

- 正向影响：
  - 业务代码更接近业务语义，可读性和可维护性提升；
  - 外部表示库的依赖面收敛，替换或升级成本降低；
  - 新增字段、enum 和状态时有固定流程，减少遗漏；
  - AI Agent 和人工 review 可以通过脚本和清单判断是否破坏边界。

- 代价与约束：
  - 需要维护 domain type 与 representation type 的映射；
  - 短期内需要逐步偿还已有 protobuf 泄漏债务；
  - 新增外部格式时必须先补齐边界设计和检查规则。

## 当前实现

- `AGENTS.md` 声明 Domain-first 边界规则，作为本地协作和 AI Agent 执行依据。
- `scripts/check-boundaries.sh` 检查生产代码中的外部表示泄漏。
- `scripts/check.sh boundaries` 作为统一入口运行边界检查。
- `libs/protocol/src/control_codec.cpp` 是控制通道 protobuf generated type 的唯一生产代码边界。

后续新增控制通道能力应先扩展 domain type，再在 `libs/protocol` 的 codec/adapter 中映射到 protobuf wire contract。
