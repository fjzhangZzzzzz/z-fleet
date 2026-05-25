# ADR 0012：跨平台构建、打包与安装脚本边界

状态：草案
日期：2026-05-25

## 背景

z-fleet 已同时支持 Linux 与 Windows 本地构建，并正在形成安装包发布和首次安装闭环。这里需要区分两类用户：

- 贡献者和 CI 需要从源码完成依赖准备、构建、测试、打包和本地部署；
- 被管理设备的使用者只需要取得可信安装包并完成首次安装，不应被迫安装源码构建工具或特定开发 shell。

若把两类入口混为一套跨平台 shell 脚本，Windows 终端安装会携带 Git Bash 等非必要前置条件；若现在就把全部构建和本地部署脚本复制为 Bash 与 PowerShell 两套，则会重复维护相同的编排和参数契约。

## 现状证据

仓库内现有事实如下：

- `CMakePresets.json` 已为 Linux 与 Windows 定义同名风格的 configure、build 和 test preset，并用宿主平台条件限制误用。
- `scripts/build.sh`、`test.sh`、`vcpkg.sh`、`make-package.sh`、`install-local.sh` 与 `scripts/lib/common.sh` 合计 948 行；用户日常入口已经统一为 `./scripts/<action>.sh`。
- Windows 适配集中在 `scripts/lib/common.sh` 和 `scripts/vcpkg.sh`：宿主检测、`cygpath` 路径转换、`VsDevCmd.bat` 注入、`.exe` 文件名和 `bootstrap-vcpkg.bat` 选择。业务打包和安装流程并未按平台各写一套。
- 提交 `57f126c` 为 Windows 构建适配在三个脚本文件中增加 77 行、删除 3 行；提交 `f22721e` 随后修正过一处 Windows 打包版本文件名问题。现阶段已经出现兼容维护成本，但范围仍局限于平台适配层。
- [ADR 0006](0006-manifest-driven-installer.md) 已规定 manifest、摘要、归档安装、状态切换与回滚由 C++ `zfleet_packager` / `zfleet_installer` 负责，shell 只负责编排和准备 payload。
- [ADR 0011](0011-agent-package-install-upgrade.md) 已提出首次安装使用平台薄脚本；当前 [运维文档](../operations.md) 仍明确远程下载和 service 管理尚未实现。

工具链本身也支持这一边界：

- CMake Presets 用于共享常见 configure/build/test 选项，并支持以 `${hostSystemName}` 条件启用平台 preset。
- vcpkg 官方入口本来就区分 Windows `bootstrap-vcpkg.bat` 与 Unix `bootstrap-vcpkg.sh`，manifest mode 可通过 CMake toolchain 自动取得依赖。
- Microsoft 文档说明 MSVC 命令行工具需要 Developer Command Prompt 或 Developer PowerShell 设置的环境，且 `VsDevCmd.bat` 是正式入口。

## 方案比较

| 方案 | 收益 | 成本与风险 | 判断 |
| --- | --- | --- | --- |
| 所有场景继续只使用统一 Bash 脚本 | 入口最少；开发和 CI 行为一致；可复用现有实现 | Windows 终端首次安装被迫依赖 Git Bash；将来 service、权限和签名能力会让 Bash 适配层膨胀 | 不适合作为终端部署入口 |
| 立即将所有脚本拆成 Bash 与 PowerShell 两套 | Windows 使用原生 shell；平台操作容易直接表达 | 重复维护近千行编排、参数与错误语义；打包/安装核心已在 C++，收益不足以覆盖漂移风险 | 当前不采用 |
| 开发编排统一，终端 bootstrap 按平台拆分 | 源码工作流仍只有一套；Windows 用户安装无 Git Bash 前置；平台差异停留在薄入口；复用 C++ 安装语义 | 需要新增并测试两个小型 bootstrap；需保持 API 与参数契约一致 | 推荐 |

## 决策

1. **构建、测试、打包和本地开发部署继续使用统一 Bash 入口。**

   保留现有：

   ```bash
   ./scripts/build.sh
   ./scripts/test.sh
   ./scripts/make-package.sh agent --zip
   ./scripts/install-local.sh apply agent --zip
   ```

   支持环境明确为 Linux Bash 与 Windows Git Bash。preset、依赖缓存、package 输出和本地安装参数继续保持统一，平台适配集中于 `scripts/lib/common.sh` 与必要的工具 bootstrap 分支。

2. **不创建完整镜像版的 `build.ps1`、`make-package.ps1` 或 `install-local.ps1`。**

   在源码构建用户可以使用 Git Bash、且现有平台分支仍小于业务流程本身的阶段，维护两套同功能入口只会增加测试矩阵和文档负担。

3. **面向终端设备的首次安装按平台提供薄 bootstrap。**

   按 ADR 0011 后续实现：

   - Linux 提供 POSIX `sh` 入口；
   - Windows 提供原生 PowerShell 入口；
   - Web 安装页只展示目标平台的一条可复制命令；
   - 脚本只负责平台/架构检测、获取安装选项、下载、SHA-256 校验、调用 `zfleet_installer`、写初始配置和尝试启动 Agent；
   - package 校验、release 切换、回滚和升级语义不得重新实现在脚本内。

4. **CI 必须分别运行 Linux 和 Windows 的统一脚本入口。**

   Bash 源文件相同不代表两平台行为相同。Windows 路径转换、MSVC 环境初始化、`.exe` 查找和 package 产物必须由 Windows job 覆盖；Linux job 覆盖 Unix bootstrap 与产物路径。

5. **达到以下条件时重新评估源码脚本的拆分。**

   - Windows 贡献者明确需要不安装 Git Bash 的源码构建体验；
   - Windows 专属流程进入签名、MSI、Service 注册或管理员权限编排；
   - Linux 与 Windows 参数契约或产物结构必须分离；
   - 平台条件分支开始在多个入口重复实现，无法继续收敛在公共适配层或 C++ 工具中。

## 便捷入口目标

| 用户 | Linux | Windows |
| --- | --- | --- |
| 贡献者构建/打包/本地部署 | `./scripts/*.sh` | Git Bash 中运行同一 `./scripts/*.sh` |
| 终端首次安装 | Web 页面给出一条 `sh` 命令 | Web 页面给出一条 PowerShell 命令 |
| 后续升级/回滚 | Agent 任务调用 installer | Agent 任务调用 installer |

该入口模型让开发者只维护一套日常命令，让 Windows 安装用户无需了解 Git Bash，同时继续把可验证的安装状态变更约束在 C++ installer 中。

## 影响

- 正向：不复制已有脚本；保留统一 CI/本地复现路径；终端安装体验与平台习惯一致；符合 ADR 0006 的安全边界与 ADR 0011 的安装闭环方向。
- 负向：Windows 源码开发仍依赖 Git Bash；bootstrap 落地后必须增加 Linux 与 Windows 的端到端安装验证。
- 后续工作：在 ADR 0011 落地首次安装时新增两个薄 bootstrap 和对应集成测试；在建立 CI 配置时加入 Linux 与 Windows 脚本验证 job。

## 参考

- CMake, `cmake-presets(7)`: <https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html>
- Microsoft Learn, vcpkg FAQ: <https://learn.microsoft.com/vcpkg/about/faq>
- Microsoft Learn, vcpkg CMake integration: <https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration>
- Microsoft Learn, Visual Studio Developer Command Prompt and Developer PowerShell: <https://learn.microsoft.com/en-us/visualstudio/ide/reference/command-prompt-powershell?view=vs-2022>
