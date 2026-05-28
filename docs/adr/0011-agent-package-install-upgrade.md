# ADR 0011：Agent 安装包发布、安装与升级模型

状态：已接受
日期：2026-05-24

## 背景

z-fleet 已具备清单驱动的 `zfleet_installer apply` 安装模型、Server 内置 Web 管理入口、Agent 安装包上传校验、channel 发布指针和注册 token 基础能力。后续需要把 Agent 发布、首次安装、远程升级和 installer 自身更新串成闭环，同时保持以下目标：

- 简化设计，避免引入系统包管理器、复杂服务管理或独立更新框架；
- 安装和升级流程易于理解，管理员可在 Web 页面完成版本维护；
- 新安装命令尽量短，但实际下载、校验、安装步骤必须可验证；
- Agent 升级属于高风险写操作，必须任务化、可审计、可追踪和可回滚；
- `zfleet_installer` 独立演进，不与 Agent 版本强绑定。

## 决策

1. **继续使用自有 ZIP 安装包和 `zfleet_installer apply`**

   Agent 与 installer 都以 z-fleet 自有 ZIP package 分发。ZIP 内部继续使用 `META/manifest.json` 和 `payload/` 结构，由 `zfleet_installer apply` 根据 manifest 安装到组件 release 目录。

   不引入 `.deb`、`.rpm`、`.msi` 或系统包管理器作为 v0.x 默认分发格式。文件名中的构建类型使用完整 `debug` 或 `release`，不再用容易和 Debian 包混淆的 `deb` 缩写。

2. **包名携带组件、版本、平台、架构和构建类型**

   标准文件名格式：

   ```text
   zfleet_<component>-v<version>-<platform>-<arch>-<build_type>.zip
   ```

   示例：

   ```text
   zfleet_agent-v1.0.0-linux-x86_64-debug.zip
   zfleet_agent-v1.0.0-linux-x86_64-release.zip
   zfleet_installer-v0.3.0-windows-x86_64-release.zip
   ```

   文件名用于人类识别、上传展示和排障。Server 不只依赖文件名判断关键元数据。

3. **`build_type` 写入 manifest**

   manifest schema 增加 `build_type` 字段，合法值初始为：

   - `debug`
   - `release`

   `build_type` 与 `component`、`version`、`platform`、`arch` 一样属于上传校验和发布选择的一部分。Server 上传时必须校验文件名解析出的构建类型与 manifest 中的 `build_type` 一致；冲突时拒绝入库。

   manifest 示例：

   ```json
   {
     "schema_version": 1,
     "component": "agent",
     "version": "1.0.0",
     "platform": "linux",
     "arch": "x86_64",
     "build_type": "release",
     "min_installer_version": "0.1.0",
     "files": []
   }
   ```

   `build_type` 不参与运行时协议版本兼容判断，但参与包选择、发布限制和升级策略。

4. **Agent 与 installer 独立发布**

   Agent package 和 installer package 都进入统一包仓库，但作为不同 `component` 管理。Agent 包只声明 `min_installer_version`，不绑定固定 installer 版本。

   Server 负责保证默认安装入口返回的 installer 满足目标 Agent 包的 `min_installer_version`。若某台 Agent 本地 installer 太旧，升级时由 Server 自动串联任务：先升级 installer，再升级 agent。

5. **channel 默认发布按组件、平台、架构、构建类型隔离**

   初始 channel 保持：

   - `stable`
   - `candidate`
   - `dev`

   同一时间只能有一个默认包：

   ```text
   component + channel + platform + arch + build_type
   ```

   发布限制：

   - `release` 包可发布到 `stable`、`candidate`、`dev`；
   - `debug` 包只能发布到 `candidate` 或 `dev`；
   - `debug` 包禁止发布到 `stable`。

6. **首次安装使用平台薄脚本**

   Web 安装页根据平台生成命令，下载平台脚本并传入参数执行。脚本只负责 bootstrap，不内嵌包内容。

   脚本职责：

   - 检测平台和架构；
   - 请求 Server 安装选项；
   - 下载满足目标 Agent 包要求的 installer package；
   - 校验 installer package SHA-256；
   - 下载 Agent package；
   - 校验 Agent package SHA-256；
   - 调用 `zfleet_installer apply --root <root> --package <agent.zip>`；
   - 写入 Agent 配置，包括 `control_url` 和注册 token；
   - 安装完成后尝试启动 Agent 进程。

   v0.x 暂不引入 systemd、Windows Service 或其他系统服务管理。安装脚本只尝试启动进程，不承诺开机自启。

7. **安装选项同时返回 Agent 与 installer**

   安装页 API 按 `platform`、`arch`、`channel`、`build_type` 返回可安装的 Agent 包和可用 installer 包。

   ```text
   GET /api/v1/install/options?platform=linux&arch=x86_64&channel=stable&build_type=release
   ```

   响应必须包含 Agent 和 installer 的 `package_id`、`version`、`sha256`、`download_url`。若没有满足 `min_installer_version` 的 installer，Server 返回明确错误，不生成不可执行命令。

8. **升级走任务模型，并在 `agents` 表保存 desired state**

   Agent 当前版本和期望版本统一记录在 `agents` 表。建议新增字段：

   ```text
   current_package_id
   desired_version
   desired_package_id
   desired_set_at
   desired_set_by
   upgrade_state
   last_upgrade_task_id
   last_upgrade_error
   last_upgrade_at
   ```

   `agent_version` 继续保留为当前 Agent 上报版本；`current_package_id` 在 Agent 能上报 package id 后补齐。升级触发时 Server 写入 desired 字段并创建任务，不允许只靠字段差异隐式执行写操作。

9. **Agent 详情页维护入口下发升级任务**

   管理员在 Agent 状态详情页进入维护入口，选择“升级”并选择目标 Agent package。Server 必须校验：

   - package 存在且未退役；
   - package component 为 `agent`；
   - package platform/arch 与目标 Agent 匹配；
   - build type 符合策略；
   - 默认禁止降级，除非后续显式加入 `allow_downgrade`；
   - 当前策略允许 high risk write。

   校验通过后写 desired state、创建升级任务并写审计事件。

10. **installer 过旧时 Server 自动串联任务**

    当目标 Agent package 的 `min_installer_version` 高于目标 Agent 当前可用 installer 版本时，Server 不直接下发 Agent 升级任务，而是创建任务链：

    ```text
    upgrade_installer -> upgrade_agent
    ```

    第一阶段升级 installer。Agent 重新确认 installer 可用后，Server 再下发 Agent 升级任务。任一阶段失败，任务链停止并记录失败原因。

11. **升级后 Agent 尝试自行启动新 Agent 进程**

    Agent 收到升级任务后：

    - 下载并校验目标 package；
    - 调用本地 installer apply；
    - apply 成功后尝试启动新 Agent 进程；
    - 当前进程退出或停止旧控制流；
    - 新 Agent 上线后上报版本，Server 比对 `desired_package_id` 和当前状态。

    Server 在新 Agent 重新上线并上报匹配版本前，状态保持 `waiting_reconnect`，不提前判定最终升级成功。

12. **回滚仍基于 installer active/previous 模型**

    Agent 维护入口后续提供回滚任务，调用：

    ```text
    zfleet_installer rollback --component agent
    ```

    回滚成功后，Server 应将 desired state 同步到回滚后的当前版本，避免 Agent 上线后再次被自动升级到刚回滚前的版本。

13. **安全、审计和错误可观测性是升级入口的前置条件**

    安装包上传、发布、退役、设置 desired state、创建任务、任务分配、任务运行、任务成功、任务失败、升级确认都必须写审计。

    升级错误至少应覆盖：

    - `package_not_found`
    - `package_retired`
    - `platform_arch_mismatch`
    - `build_type_not_allowed`
    - `installer_too_old`
    - `download_failed`
    - `checksum_mismatch`
    - `apply_failed`
    - `start_new_agent_failed`
    - `waiting_reconnect_timeout`
    - `agent_reported_unexpected_version`

## 备选方案

- **使用系统原生包格式**：可以复用系统包管理器和服务管理能力，但会显著增加平台差异、签名链、权限和卸载语义复杂度。当前目标是轻量、统一和可控，暂不采用。
- **只从包名读取构建类型**：实现简单，但文件名不稳定且易被重命名破坏语义。已决定将 `build_type` 写入 manifest，并把文件名仅作为辅助校验。
- **Agent 与 installer 版本强绑定**：能减少选择逻辑，但会导致 installer 小改动迫使 Agent 重新发版，也不利于长期兼容。已决定只使用 `min_installer_version`。
- **升级后等待用户手动重启**：实现最简单，但升级体验差且状态长时间不确定。已决定 apply 成功后由 Agent 尝试自行启动新 Agent 进程，Server 仍以重新上线确认为最终成功。
- **desired state 独立建表**：范式更清晰，但当前 Agent 状态读写集中在 `agents` 表，v0.x 为降低迁移和查询成本，先放入 `agents` 表。
- **installer 过旧时直接失败**：实现更简单，但用户体验差。已决定 Server 自动串联 installer 升级与 Agent 升级任务。

## 影响

- `libs/package` manifest schema 需要增加 `build_type` 字段和校验。
- `scripts/make-package.sh` 和 `zfleet_packager` 需要输出包含 `build_type` 的包名和 manifest。
- Server 包仓库需要支持 `agent` 与 `installer` 两类 component，不再只面向 Agent package。
- `agent_packages` 或后续统一 `packages` 表需要增加 `build_type`，发布指针唯一约束也要包含 `component` 和 `build_type`。
- `/api/v1/install/options` 需要同时返回 Agent 与 installer 包。
- Web 安装页需要提供平台脚本命令和高级参数。
- Agent 协议和任务模型需要支持 package update、installer update、升级状态和错误结果。
- `agents` 表需要增加 desired state 字段。
- Agent runtime 需要能执行 package update、调用 installer、启动新 Agent 进程并上报结果。
- 安全文档、契约文档和运维文档需要同步补充升级能力、审计事件、错误码和策略开关。
