# 运维

状态：草案
最后更新：2026-05-13
关联里程碑：v0.1, v0.2, v0.3

## 范围

本文档记录本地运行、构建、配置、部署、升级、回滚和排障流程。

## 构建

项目使用 CMake + vcpkg manifest mode。

```bash
./scripts/build.sh linux-debug
./scripts/test.sh linux-debug
```

标准预设：

- `linux-debug`
- `linux-release`
- `windows-debug`
- `windows-release`

## 本地运行

待补充：

- Server 本地启动命令；
- Agent 本地启动命令；
- 本地配置文件示例；
- 数据目录和日志目录；
- SQLite 数据库位置。

## 排障

待补充：

- 构建失败；
- vcpkg 依赖安装失败；
- Server 监听失败；
- Agent 注册失败；
- 心跳或资产上报失败；
- 数据库迁移失败。
