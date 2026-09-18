# ServerFramework

**基于 Linux io_uring 与 C++23 协程的 HTTP 服务器框架 —— 单线程事件循环 + 无锁 MPMC 任务队列。**

每个 TCP 连接对应一个独立协程：`co_await` 时向 io_uring 提交 IO 并挂起，CQE 到达后由事件循环恢复，代码写法接近同步、没有状态机。Accept / Recv / Send / Close / Poll 全链路作为 SQE 提交，连接建立用 multishot accept 一次提交持续接收，完成事件按批消费。业务 handler 默认在事件循环线程**同步执行**（零调度开销），耗时任务通过 `GetAsync`/`PostAsync` 显式投递到线程池；线程池的任务队列是一条**槽位序号环形 MPMC 队列**，worker 空闲时直接挂在队列自带的 io_uring ring 上阻塞，空载时全部停在 `io_cqring_wait`，不消耗空转 CPU。

---

## 亮点

- **全链路 io_uring** —— accept / recv / send / close / poll 全部作为 SQE 提交，不为 IO 建立任何线程；multishot accept 一次提交即可持续接收新连接；事件循环用 `io_uring_for_each_cqe` 批量遍历完成队列、`io_uring_cq_advance` 一次性标记消费，减少内核态往返。

- **C++23 协程连接模型** —— 每个连接一个协程，读写通过 `co_await IouringAwaiter{...}` 挂起，`await_suspend` 提交对应 IO，CQE 回到事件循环后恢复。协程帧挂在连接对象里，连接回收即协程结束，不需要状态机或回调金字塔。

- **同步快速路径 + 显式异步** —— `server.Get(...)` 注册的 handler **直接在事件循环线程执行**：没有任务入队、没有线程唤醒、没有跨线程协程恢复。实测单核约 7.6 万 req/s（`/json`，8 线程 500 连接时约 9.1 万）。耗时 handler 用 `GetAsync`/`PostAsync` 注册，`QueryAwaiter` 把任务投进全局线程池，后台线程完成后经 `eventfd` 唤醒事件循环恢复协程。等待唤醒用的是 io_uring 的 **read**（由 read 把 eventfd 计数取走并清零）——若改用 `poll_add` 只监听可读而不消费计数，eventfd 从第一次写入起就永久可读，事件循环会空转一整个核。`resumePending` 的 CAS 保证同一协程不会被重复入队。

- **无锁 MPMC 任务队列** —— 所有 worker 共享一条任务队列、谁空闲谁取，因此任何线程被慢任务拖住都不会让排队任务无人处理。队列用**槽位序号**判定归属：入队者等到 `sequence == pos` 才写入，出队者等到 `sequence == pos + 1` 才取走，取走后把序号推到下一圈。多生产者多消费者都不需要锁，内存一次分配、槽位循环复用——**没有节点回收，也就没有 use-after-free**。

- **阻塞等待内建在队列里** —— worker 队列取空后挂在**队列自己的 io_uring ring** 上等待，而不是忙等轮询。多消费者等待需要两道保障：等待者用**计数器**登记（单布尔会被最后一个退出者清掉，导致其余等待者再也等不到唤醒）、CQE 用 **CAS 原子认领**（liburing 的 `io_uring_cqe_seen` 实现是 `*khead = *khead + 1`，并发消费会丢更新并把 CQ 的 head 推错）。

- **连接生命周期用弱引用** —— 连接表独占持有连接对象，**所有跨线程或跨生命周期边界的引用都是 `weak_ptr`**：投进线程池的异步任务、待恢复协程队列都在取用时 `lock()` 确认存活，拿不到就丢弃。协程内部则用裸指针——连接持有协程帧，所以"协程在跑"就蕴含"连接活着"；这个不变式不能改用 `shared_ptr` 表达，否则连接与协程帧会互相持有成环。

- **身份用对象而不是 fd** —— 每个 CQE 都先"回表"取一份 `shared_ptr` 保活，并**用指针值确认身份**（`it->second.get() == connCtx`），而不是只信 fd。因为 fd 是进程 fdtable 的**下标**，内核按"最小可用"分配（POSIX 要求），`close` 之后**立刻**可被下一次 `accept` 复用——同一批 CQE 里完全可能出现 `close(22)` 之后新连接又拿到 `22`，此时按 fd 删除就会删错对象。

- **时间轮驱动的空闲超时** —— 四级级联时间轮（`ms → sec → min → hour`，最小刻度 1ms，由独立的 io_uring 1ms 超时驱动并带追赶机制）。当前接入点是**连接空闲超时**：时间轮每秒唤醒一次事件循环，扫描连接表回收"服务器在等客户端却一直等不到"的连接（阈值 5s），避免一个连上却不发数据的客户端永久占住 fd 和 64KB 读缓冲。关闭前先 `shutdown(fd, SHUT_RDWR)`——连接上那条未完成的 `recv` 持有 socket 引用，只提交 `close` 的话 fd 会被摘掉而 socket 并不真正闭合，FIN 发不出去。

- **HTTP/1.1 完整实现** —— 请求行与 Header 解析、URI Query 自动解码、`Content-Length` 与 `Chunked Transfer-Encoding`、Keep-Alive 长连接；发送侧支持**短写续传**（记录断点，上层业务无感知）。

- **异步滚动日志** —— spdlog 异步 logger，只挂滚动文件 sink（每个 10MB、保留 5 个），不向 stdout 输出，因此即便把进程 stdout 重定向到文件也不会产生无限增长的日志。

---

## 快速开始

### 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux 内核 ≥ 5.10（推荐 ≥ 6.0 以完整支持 multishot accept） |
| 编译器 | 支持 C++23 的 GCC 或 Clang |
| 依赖 | `liburing`、`spdlog`、`fmt`、`nlohmann-json` |
| 构建 | CMake ≥ 3.20 |

### 构建与运行

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release -j$(nproc)

cd build/Release/bin && ./WebProject      # 监听 0.0.0.0:8080
```

### 最小示例

```cpp
#include "Server/Server.h"

int main() {
    Server srv;

    // 同步快速路径：直接在事件循环线程执行，零调度开销
    srv.Get("/hello", [](const HttpRequest& req, HttpResponse& res) {
        auto name = req.get_param_value("name");
        res.set_status(200).set_body("Hello, " + (name.empty() ? "World" : name));
    });

    // 异步路径：投递全局线程池，不阻塞事件循环
    srv.GetAsync("/slow", [](const HttpRequest& req, HttpResponse& res) {
        // 这里可以放数据库查询、外部调用等耗时操作
        res.set_status(200).set_json(R"({"ok":true})");
    });

    if (srv.listen("0.0.0.0", "8080") != 0) return 1;
    srv.run();   // 进入阻塞事件循环
}
```

### 压测

```bash
./tests/scripts/benchmark.sh --duration 10s     # 分级压测，自动起停服务器并汇总
```

---

## 架构

```
┌────────────────────────────────────────────────────────────────────┐
│ 业务层  HttpServer        src/HttpServer/Httpserver.cpp            │
│         注册路由 → 构造 Server → listen → run（阻塞不返回）         │
├────────────────────────────────────────────────────────────────────┤
│ 路由层  Router            include/Server/Router.h                  │
│         精确匹配 + 目录前缀匹配；只负责"匹配"不负责"执行"            │
│         产物 Route{handler, async} 决定走同步还是异步路径           │
├────────────────────────────────────────────────────────────────────┤
│ 服务层  Server            src/HttpServer/Server.cpp                │
│         单线程 io_uring 事件循环 + 每连接一个协程                   │
│         ConnCtx / IouringAwaiter / QueryAwaiter                    │
│         状态：mConnections / mPendingResumes / mTimeWheel / mEventFd│
├────────────────────────────────────────────────────────────────────┤
│ 协议层  HttpParser / HttpResponse                                 │
├────────────────────────────────────────────────────────────────────┤
│ 基础设施 ThreadPool(global 8 / timewheel 2) · TimeWheelTop ·      │
│          Logger(spdlog 异步 + 滚动文件)                             │
└────────────────────────────────────────────────────────────────────┘
```

### 一条请求的完整链路

```
accept (multishot CQE)
  └─ 建 ConnCtx → 启动连接协程
       └─ co_await IouringAwaiter{READ}            ← 挂起，提交 recv
            CQE 到达 → handleCqe → 恢复协程
              ├─ 解析：HttpParser.feed + try_parse
              ├─ 路由：Router.match
              ├─ 执行：同步 handler 直接调用
              │        异步 handler → co_await QueryAwaiter
              │                        └─ 投线程池 → handler 执行
              │                          → CAS resumePending → 入队 + write(eventfd)
              │                          → 事件循环 poll 到 → 恢复协程
              └─ co_yield IouringAwaiter{WRITE}   ← 提交 send（短写续传）
       └─ co_await IouringAwaiter{CLOSE}          ← 提交 close
```

---

## 核心机制

### 任务队列：槽位序号环形队列

队列预分配一组**槽位**，每个槽位带一个 `sequence` 序号：

| 角色 | 条件 | 动作 |
|------|------|------|
| 入队者 | 槽位 `sequence == pos` | CAS 抢占 `tail`，写入数据，把 `sequence` 发布为 `pos + 1` |
| 出队者 | 槽位 `sequence == pos + 1` | CAS 抢占 `head`，取走数据，把 `sequence` 发布为 `pos + QSIZE` |

序号既是"数据是否就绪"的标志，也是"槽位轮到谁"的凭证，因此多个生产者与多个消费者可以并发操作同一条队列而不需要任何锁；槽位循环复用，全程没有节点分配与回收。

另外，`enqueue` 在队列满时自旋让出而非失败，保持"提交一定能进队列"的语义。

### worker 的阻塞与唤醒

队列自带一条 io_uring ring 作为唤醒通道：

- 提交侧：入队后若仍有等待者，提交一个 nop——**必须 `io_uring_prep_nop`**，否则这个 sqe 会带着残留 opcode 提交，唤醒事件不成立。
- 消费侧：`io_uring_enter(GETEVENTS)` 阻塞等待，被唤醒后回来排空队列（谁空闲谁取）。

多消费者场景下 `io_uring_wait_cqe` 不保证一个 CQE 只交给一个等待者，所以唤醒事件由消费者用 CAS 自行认领，而不是依赖 liburing 的 `cqe_seen`。

### 连接的所有权、引用与身份

| 角色 | 持有方式 | 理由 |
|------|---------|------|
| `mConnections`（连接表） | `shared_ptr<ConnCtx>` | 唯一强引用 |
| 协程内部 | 裸指针 | 连接持有协程帧 ⇒ 协程在跑则连接必活 |
| `IouringAwaiter` / `QueryAwaiter` | `weak_ptr<ConnCtx>` | 挂起期间、异步执行期间连接可能已被回收 |
| `mPendingResumes`（待恢复队列） | `weak_ptr<ConnCtx>` | 跨线程传递，条目可能已失效 |
| io_uring `user_data` | 裸指针（交给内核，只能存 64 位整数） | 回到 `handleCqe` 后回表取 `shared_ptr` 保活 + 用指针值校验身份 |

原则：**所有跨边界的引用都是弱的；回表时既取强引用保活，又用指针值验身份。**

### 空闲超时

```
TimeWheelTop（独立线程，1ms 推进 ms 轮）
   └─ 每秒 add_task 一次 → 置标志 + write(eventfd) → 自我重注册
        └─ 事件循环 EVENT 分支 → scanIdle()
             └─ 遍历连接表，回收处于等待状态超过 5s 的连接
```

扫描放在**事件循环线程**（它要遍历连接表、还要提交 io_uring 操作），时间轮线程只置标志并唤醒。

关闭前先 `shutdown(fd, SHUT_RDWR)`：此刻连接上挂着一条未完成的 `recv`，它持有 socket 引用，只提交 `close` 会让 fd 被摘掉而 socket 不真正闭合——FIN 发不出去，客户端等不到断开，那条 `recv` 也等不到数据。

---

## 性能实测

> 环境：12 核 / 15 GiB / 内核 6.17 / GCC C++23 Release；wrk 4.1.0。
> 绝对值受机器负载影响（同机运行 IDE 等进程时整机约慢 10%），请以量级与相对关系为准。

### 端点 × 并发矩阵（Keep-Alive，10s/级，3 轮取中位数）

| 端点 | Low 2/10 | Medium 4/50 | High 4/100 | VeryHigh 8/200 | Extreme 8/500 |
|------|----------|-------------|------------|----------------|---------------|
| `GET /json` | 72.6k | 86.3k | 81.1k | 85.0k | **97.3k** |
| `GET /api/user/42` | 66.6k | 83.4k | 82.2k | 86.1k | **96.2k** |
| `POST /echo` | 66.0k | 75.2k | 78.5k | 80.4k | **90.3k** |
| `GET /async/status` | 5.8k | 6.2k | 6.1k | 6.2k | 6.2k |

（单位 req/s；20 个组合的 `connect/read/write/timeout` 与 `Non-2xx` 全部为 0。）

延迟：同步端点 P50 亚毫秒~5ms、P99 < 9ms；服务器 CPU 稳定在 1.10~1.27 核。

### 异步路径的天花板

`/async/status` 的 handler 内含 `sleep(1ms)`：

```
8 个 worker × (1ms sleep + 289us 链路开销)  ⇒  上限 ≈ 6,200 req/s
实测跨 10 → 500 连接恒定在 5.8k~6.2k（±3%）
```

吞吐对连接数完全不敏感是"固定服务率 + 无限排队"的特征：连接数只改变队列长度，不改变吞吐；延迟随并发线性增长（1.72ms → 7.74 → 16.40 → 32.18 → 78.93ms）。

### 测量可信度

把连接数 `L`、吞吐 `λ`、实测平均延迟 `W` 代进排队论关系 `L = λW`，20 个组合中除最小档（10 连接，统计意义弱）外全部吻合，误差 0.3%~5.3%——说明没有请求被静默丢弃、没有超时截断统计、连接数守恒。

---

## 项目结构

```
include/
├── Server/
│   ├── Server.h          # io_uring HTTP 服务器主类
│   ├── Router.h          # 路由表（精确 + 目录前缀匹配）
│   ├── HttpServer.h      # 业务层入口
│   └── CoroTask.h        # C++23 协程 Task<T>
├── Http/
│   ├── HttpParser.h      # HTTP/1.1 请求解析
│   └── HttpResponse.h    # HTTP 响应构造
├── WaitQueue/
│   ├── SPSCBase.h / SPSCQueue.h   # 无锁 SPSC 环形队列 + uring 唤醒
│   ├── MPSCBase.h / MPSCQueue.h   # 无锁 MPSC 链表队列 + uring 唤醒
│   └── MPMCBase.h / MPMCQueue.h   # 无锁 MPMC 环形队列 + 内建 uring 阻塞等待
├── ThreadPool.h          # 共享任务队列线程池（global 8 / timewheel 2）
├── TimeWheel.h           # 四级级联时间轮
└── Logger.h              # spdlog 异步日志封装

src/
├── Http/                 # 解析器与响应构造
├── HttpServer/           # Server.cpp（事件循环/协程/awaiter）、Router.cpp、Httpserver.cpp
├── ThreadPool/           # 线程池实现
├── TimeWheel/            # 时间轮实现
├── Logger/               # 日志实现
└── main.cpp              # 入口：构造 HttpServer

tests/scripts/            # benchmark.sh / test_stress.sh / test_low_pressure.sh / test_api.sh
```

---

## 已知限制

1. **`listen(fd, 10)` backlog 偏小**：突发连接下 accept 会积压。
2. **路由前缀匹配是线性扫描**：当前路由表小（6 条）无影响，路由数量上升后需要换成前缀树。

---

## 配套文档

- `docs/benchmark-2026-09-18-matrix.md` —— 端点 × 并发矩阵、低压梯度与稳定性完整数据
- `docs/benchmark-2026-08-19-arch-update.md` 等 —— 各阶段性能记录

## 许可

学习/实验性质的高性能服务器框架，可自由修改与扩展。
