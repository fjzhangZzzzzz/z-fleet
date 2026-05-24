# ADR 0006：清单驱动的 zfleet_installer 与 active-version 启动模型

状态：已接受
日期：2026-05-17

## 背景

z-fleet 需要在本地调试、CI 发布与后续 Agent 自更新、Server 运维升级之间保持 **同一套安装语义**。若按平台维护 `install.sh` / `install.ps1`，长期易出现脚本漂移、回滚困难、与 manifest 不一致等问题。

项目已约定：

- 发布包由 `META/manifest.json` 描述内容；
- 配置、状态、数据与版本化二进制分离；
- 邻近两版本可回滚，不要求无限历史兼容；
- v0.1 不引入 systemd；暂不发布 agent+server bundle。

## 决策

1. **通用安装器**  
   实现 C++ 可执行文件 `zfleet_installer`，业务无关，仅根据 manifest 执行 `apply` / `rollback` / `status`。

2. **组件按子目录隔离**  
   多组件可共用同一 `<root>/`，但每个组件拥有独立子目录：

   - `<root>/installer/`
   - `<root>/agent/`
   - `<root>/server/`

   每个组件目录内独立维护 `bin/`、`releases/`、`var/active-version` 和安装状态；`apply` / `rollback` / `status` 均以组件目录为边界，不共享 active version。

3. **版本目录 + active-version + launcher stub**  
   - 真实二进制与库位于 `releases/<version>/`；  
   - `var/active-version` 记录当前版本，`var/previous-version` 记录相邻上一健康版本，供 `rollback` 在两个已验证版本间切换；
   - `bin/zfleet_*` 为固定路径的 launcher stub，读取 `active-version` 后启动对应 `releases` 内二进制；  
   - **不采用** 符号链接 `current -> releases/<version>`；Linux 与 Windows 统一该模型。

4. **发布包**  
   - agent/server 全量包只更新对应组件目录，发布形态支持目录 package 与标准 `.zip` 归档；两者均包含 `META/manifest.json` 与 `payload/`；
   - manifest 必须声明目标 `platform` 与 `arch`，并可声明最低 `min_installer_version` 要求；当前实现保留并校验最低版本字段存在，版本比较作为后续能力；
   - 另发布 **installer-only** 小包供首次 bootstrap 与 installer 单独更新，其 manifest 组件为 `installer`；  
   - 执行路径统一为 `zfleet_installer apply`；  
   - installer 更新同样写入 `installer/releases/<version>/` 并切换 `installer/var/active-version`，但不得在同一次执行中覆盖或清理当前正在运行的 installer release；新 installer 版本从下一次调用 `installer/bin/zfleet_installer` 时生效；launcher stub 视为稳定 bootstrap 边界，不纳入常规自更新。

5. **脚本职责**  
   `scripts/make-package.sh` / `install-local.sh` 仅负责编排构建、准备 payload、调用 C++ packager / installer，以及复制 launcher stub；manifest 生成、摘要计算、归档读写与安装/更新核心逻辑均由 C++ 程序承担。当前脚本只收集组件主程序二进制，动态库或其他依赖自动收集作为后续能力。

6. **可执行文件命名**  
   保持当前下划线形式：`zfleet_agent`、`zfleet_server`、`zfleet_installer`。

7. **manifest 最低安全契约**  
   manifest 至少声明组件、版本、平台、架构、目标相对路径、文件大小、SHA-256 摘要与可执行权限。installer 必须拒绝绝对路径、`..` 路径穿越、跨组件写入和未通过摘要校验的文件。签名与回滚保护在 v0.3 设计中补齐；签名不写入 manifest，安装包的 `META/` 目录需预留独立 manifest 签名文件扩展点。

8. **事务与回滚语义**  
   `apply` 先写入 staging 目录并完成摘要校验，再原子切换为 `releases/<version>/`，最后通过临时文件 + rename 更新 `var/active-version`。旧 release 默认保留；启动后健康确认、旧版本清理和回滚保护作为后续能力。`rollback` 只切换到已存在且校验通过的相邻版本。

9. **与 Server 发布 channel 的关系**
   Web 管理面中的安装包 channel 由 [ADR 0010](0010-web-management-entry.md) 定义，是 Server 侧发布指针，不改变本 ADR 的本地安装语义。package manifest、`releases/<version>`、`active-version` 和 `previous-version` 仍以组件和版本为边界；channel 不写入安装目录，不参与 `rollback` 判定，也不允许让同一安装 root 下同一组件按 channel 维护多套 active version。

## 备选方案

- **每平台 install 脚本**：实现快，长期维护与测试成本高，已拒绝。  
- **`current` 符号链接**：实现简单，Windows 与部分环境不稳定，已拒绝。  
- **安装时复制二进制到固定 `bin/`（无 releases）**：回滚需二次复制，与 manifest 差分模型冲突，已拒绝。  
- **agent+server bundle 单包**：降低首次步骤，但增加发布组合与 manifest 复杂度；现阶段不做。  
- **Go/Rust 独立安装器**：bootstrap 方便，但与主语言不一致；在要求 C++ 一致的前提下选用独立 C++ target。
- **连字符可执行文件命名**：更贴近部分发行包习惯，但会与当前 CMake 目标、运维文档和已有命令产生不必要迁移成本，已拒绝。

## 影响

- 正向：安装、升级、回滚、本地调试与 CI 共用一套可测试逻辑；组件状态互不覆盖；跨平台行为一致；为标准归档、v0.3 签名与 Agent 子进程升级预留扩展点。
- 负向：需实现 stub、installer 与打包管线；首次安装与 installer 更新依赖 installer-only 小包。  
- 后续工作：`docs/operations.md` 维护当前可执行的构建、打包、部署、状态和回滚流程；签名验证、远程下载、自更新编排、service 管理、启动健康确认和旧版本清理在后续业务里程碑中实现。ADR 作为决策源，操作步骤由运维文档承接；若后续部署文档明显膨胀或读者群分离，再拆出独立 deployment 文档。
