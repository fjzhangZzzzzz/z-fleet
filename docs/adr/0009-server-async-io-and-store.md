# ADR 0009: Server 异步 I/O 与异步 Store 架构

状态：已接受
日期：2026-05-23

## 背景

ADR 0008 已将 SQLite 写入收敛到 db actor，并为 HTTP/2 Server 增加连接上限和有限 worker pool。该实现可以控制 v0.x 风险，但仍保留每连接阻塞线程、每连接任务通知线程，以及 worker/db 提交后同步等待的桥接模型。

长期 Server 架构需要面向可扩展长连接：I/O 线程不应阻塞等待业务或数据库结果，HTTP/2 session 状态应有明确串行化边界，任务通知应由数据库 task queue 变化订阅驱动，而不是为每个连接创建后台线程。

## 决策

- Server 目标架构采用 Boost.Asio async I/O、nghttp2、有限业务 worker pool 和 SQLite db actor。
- HTTP/2 连接抽象为 `Http2Session`，由 Asio executor 驱动 `async_accept`、`async_read` 和 async write queue。
- 每个 session 使用 strand 串行访问 socket、nghttp2 session、stream map 和 write queue。
- 协议解码、业务编排和非 I/O 工作提交到有限 worker pool；worker 完成后 post 回 session strand。
- Store 生产路径演进为 `AsyncServerStore`：提交后立即返回，完成结果通过调用方指定 executor post 回来。
- SQLite db actor 保持单写队列、WAL、`busy_timeout` 和 `SQLITE_BUSY` / `SQLITE_LOCKED` 有界重试。
- `ClaimNextTaskForAgent` 的查询并修改语义必须继续在单个事务内完成。
- Stop 流程必须停止 accept、关闭 session、停止 worker pool、停止 db actor，并 join 所有后台线程。

## 约束

- 不引入 libuv；避免两套事件循环并存。
- 继续使用 nghttp2 作为 HTTP/2 协议状态机。
- I/O 线程、session strand、worker 线程不能调用 `.get()` 或等待数据库条件变量来获取业务结果。
- worker 线程不能直接访问 nghttp2 session、socket 或 session stream map。
- db actor 不能直接访问 session 状态；完成结果只能 post 回指定 executor。
- 连接数、worker 队列和 db 队列需要有明确上限或 backpressure 策略。

## 实施阶段

1. 固定异步接口：新增 `AsyncServerStore`，让 `ServerDatabase` 提供 completion-post 异步方法，同时保留同步兼容层。
2. 抽出 `Http2Session`：先建立 session 边界，再替换阻塞连接循环。
3. worker pool 改为 callback/post 模型，去除 `Submit(...).get()`。
4. HTTP/2 I/O async 化：用 Asio async read/write 和 session strand 替代每连接阻塞线程。
5. 任务通知 session 化：移除每连接 notifier 线程，改为数据库 task queue 变化订阅驱动，并按 agent post 到对应 session strand。
6. 补齐过载、停止、取消、异常路径和并发领取测试。

## 后果

- Server 并发模型将从线程 per connection 过渡为主流 async session 架构。
- Store 层可以在不阻塞 I/O 的前提下保持 SQLite 单写一致性。
- 后续替换 PostgreSQL store 时，HTTP/2 session 和协议处理层不需要感知具体数据库实现。
