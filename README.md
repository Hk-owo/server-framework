# ServerFramework

**基于 Linux io_uring 与 C++23 协程的 HTTP 服务器框架 —— 单线程事件循环 + 无锁 MPMC 任务队列。**

每个 TCP 连接对应一个独立协程：`co_await` 时向 io_uring 提交 IO 并挂起，CQE 到达后由事件循环恢复，代码写法接近同步、没有状态机。Accept / Recv / Send / Close 以及 eventfd 唤醒全部作为 SQE 提交，连接建立用 multishot accept 一次提交持续接收，完成事件按批消费。业务 handler 默认在事件循环线程**同步执行**（零调度开销），耗时任务通过 `GetAsync`/`PostAsync` 显式投递到线程池；线程池的任务队列是一条**槽位序号环形 MPMC 队列**，worker 空闲时直接挂在队列自带的 io_uring ring 上阻塞，空载时全部停在 `io_cqring_wait`，不消耗空转 CPU。

---

## 亮点

- **全链路 io_uring** —— accept / recv / send / close / eventfd 读取全部作为 SQE 提交，不为 IO 建立任何线程；multishot accept 一次提交即可持续接收新连接；事件循环用 `io_uring_for_each_cqe` 批量遍历完成队列、`io_uring_cq_advance` 一次性标记消费；**提交侧同样攒批**——`submitXxx` 只把 SQE 放进提交队列、不各自 `io_uring_submit`，由事件循环在每轮末尾用 `io_uring_submit_and_wait_timeout` 把"交出本轮 SQE"与"等待下一个完成事件"合并成**一次 `io_uring_enter`**。

- **C++23 协程连接模型** —— 每个连接一个协程，读写通过 `co_await IouringAwaiter{...}` 挂起，`await_suspend` 提交对应 IO，CQE 回到事件循环后恢复。协程帧挂在连接对象里，连接回收即协程结束，不需要状态机或回调金字塔。

- **同步快速路径 + 显式异步** —— `server.Get(...)` 注册的 handler **直接在事件循环线程执行**：没有任务入队、没有线程唤醒、没有跨线程协程恢复。实测 `/json` 4 线程 100 连接约 **20 万 req/s**（12 核单实例；各并发级别的完整数据见「性能实测」）。耗时 handler 用 `GetAsync`/`PostAsync` 注册，`QueryAwaiter` 把任务投进全局线程池，后台线程完成后经 `eventfd` 唤醒事件循环恢复协程。等待唤醒用的是 io_uring 的 **read**（由 read 把 eventfd 计数取走并清零）——若改用 `poll_add` 只监听可读而不消费计数，eventfd 从第一次写入起就永久可读，事件循环会空转一整个核。`resumePending` 的 CAS 保证同一协程不会被重复入队。

- **无锁 MPMC 任务队列** —— 所有 worker 共享一条任务队列、谁空闲谁取，因此任何线程被慢任务拖住都不会让排队任务无人处理。队列用**槽位序号**判定归属：入队者等到 `sequence == pos` 才写入，出队者等到 `sequence == pos + 1` 才取走，取走后把序号推到下一圈。多生产者多消费者都不需要锁，内存一次分配、槽位循环复用——**没有节点回收，也就没有 use-after-free**。

- **阻塞等待内建在队列里** —— worker 队列取空后挂在**队列自己的 io_uring ring** 上等待，而不是忙等轮询。多消费者等待需要两道保障：等待者用**计数器**登记（单布尔会被最后一个退出者清掉，导致其余等待者再也等不到唤醒）、CQE 用 **CAS 原子认领**（liburing 的 `io_uring_cqe_seen` 实现是 `*khead = *khead + 1`，并发消费会丢更新并把 CQ 的 head 推错）。

- **连接生命周期用弱引用 + 单调递增连接 id** —— 连接表独占持有连接对象，**所有跨线程或跨生命周期边界的引用都是 `weak_ptr`**：投进线程池的异步任务、待恢复协程队列都在取用时 `lock()` 确认存活，拿不到就丢弃。协程内部则用裸指针——连接持有协程帧，所以"协程在跑"就蕴含"连接活着"；这个不变式不能改用 `shared_ptr` 表达，否则连接与协程帧会互相持有成环。回到事件循环这一路则**既不存指针、也不解引用对象**：`user_data` 里放的是 `(操作类型, 连接 id)` 打包成的 64 位标签，`handleCqe` 先按 id 回表，查不到就是陈旧事件——id 单调递增、永不复用，所以"上一代连接"的 CQE 不可能撞上"新一代连接"，也就不需要额外的一代代次（裸指针会被 allocator 复用，连接 id 不会）。

- **时间轮驱动的空闲超时** —— 四级级联时间轮（`ms → sec → min → hour`，最小刻度 1ms，由独立的 io_uring 1ms 超时驱动并带追赶机制）。当前接入点是**连接空闲超时**：时间轮每秒唤醒一次事件循环，扫描连接表回收"服务器在等客户端却一直等不到"的连接（阈值 5s），避免一个连上却不发数据的客户端永久占住 fd 和 64KB 读缓冲。扫描对超时连接发一条 **`IORING_OP_ASYNC_CANCEL`**，按 `(操作类型, 连接 id)` 精确取消它那条在途的 `recv`/`send`——被取消的请求以 `-ECANCELED` 正常完成，协程照旧从 READ/WRITE 分支走完自己的关闭流程（协程尾部提交 `close`）。比 `shutdown + close` 更干净：**全程只有一条在途请求**，同一个 fd 上不会出现两条 close（第二条会打在可能已被内核复用给新连接的 fd 号上）；被取消的请求不再持有 socket 引用，随后的 close 才能真正关掉 socket 并把 FIN 发出去；而且取消本身就是一条 SQE，不必在事件循环里夹一个同步的 `shutdown` 系统调用。

- **HTTP/1.1 完整实现** —— 请求行与 Header 解析、URI Query 自动解码、`Content-Length` 与 `Chunked Transfer-Encoding`、Keep-Alive 长连接；发送侧支持**短写续传**（记录断点，上层业务无感知）。

- **异步滚动日志** —— spdlog 异步 logger，只挂滚动文件 sink（每个 10MB、保留 5 个），不向 stdout 输出，因此即便把进程 stdout 重定向到文件也不会产生无限增长的日志。日志按级别分工：连接建立/关闭、空闲超时、各类错误走 INFO，而**每个 IO 事件**的追踪日志走 TRACE——默认级别下不产生任何格式化与入队开销。

- **io_uring 提交侧优化** —— 两层：① 事件循环是这条 ring 唯一的提交者，因此启用 `DEFER_TASKRUN | SINGLE_ISSUER`，把 task_work 推迟到下一次进入内核、让内核省掉提交侧的原子操作（内核不支持时自动退回默认模式）；② **提交与等待合并** —— 所有 `submitXxx` 只 `io_uring_get_sqe` + prep，不再各自 `io_uring_submit`，事件循环每轮末尾一次 `io_uring_submit_and_wait_timeout` 同时完成"交出本轮 SQE"和"等下一个完成事件"。第 ② 步之前，每完成一个 IO 要进两次内核（提交下一个 IO + 等待）。实测同一台机器上成对 A/B（单实例 `GET /json` 4t/100c）：**11.7 万 → 17.9 万 req/s**（+52%），`io_uring_enter` 从每请求 2.4 次降到 0.08 次；机器空闲时该配置的绝对吞吐为 **20.2 万**（见「性能实测」，两处数字口径一致、只是测量时机不同）。配合上面的日志分级，单请求 CPU 在 **4.9~5.6us** 量级。刻意未启用 `SQPOLL`——它需要 `CAP_SYS_NICE`，且内核 poller 会常驻占用一个核。

- **多事件循环 × `SO_REUSEPORT`** —— 可按核数启动多个 `Server` 实例（环境变量 `SF_LOOPS`，默认取 CPU 核数），每个实例持有**私有的** io_uring ring、连接表与时间轮；实例之间只通过 `SO_REUSEPORT` 共享监听端口，由内核按连接 4 元组哈希分发——**同一 TCP 连接的请求恒定落到同一实例**，所以连接状态不需要任何跨实例同步。异步 handler 仍然投进共享线程池，完成后再写回**本实例的 eventfd**，因此协程恢复不会串到别的 ring 上。实测单实例 `/json` 约 22.6 万 req/s，2 实例推到 **46.7 万**；再加实例吞吐不再增长——瓶颈已经转移到压测端与 loopback 网络栈（见「性能实测」）。

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
./tests/scripts/benchmark.sh --duration 10s            # 仓库自带：单端点分级压测
SF_LOOPS=1 ./tests/scripts/benchmark_matrix.sh --rounds 3   # 端点 × 并发矩阵（4 端点 × 5 级）
./tests/scripts/benchmark_low_pressure.sh              # 低压梯度 + 撤压后空载 CPU
bash tests/scripts/benchmark_multiloop.sh --loops 4    # 多实例扩展 + 瓶颈诊断
python3 tests/scripts/benchmark_report.py build/Release/benchmark/matrix   # 汇总为表格
```

> 压测结果受机器负载影响很大（同一配置在不同负载下可能差数倍），建议在空闲机器上跑，或只比较同一时刻的相邻测量。
>
> `benchmark_matrix.sh` 不设 `SF_LOOPS`，走 `Server` 的默认值（= CPU 核数）。「性能实测」里的矩阵是**单实例**口径，复现时记得带 `SF_LOOPS=1`；跑多实例请用 `benchmark_multiloop.sh`（它会绑核，口径不同，两张表别混着比）。

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
│         状态：mConnections(按连接 id) / mPendingResumes / mTimeWheel / mEventFd│
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
              │                          → 事件循环 read 到 → 恢复协程
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
| `mConnections`（连接表） | `shared_ptr<ConnCtx>`，按单调递增的连接 id 索引 | 唯一强引用；id 永不复用，查不到即陈旧事件 |
| 协程内部 | 裸指针 | 连接持有协程帧 ⇒ 协程在跑则连接必活 |
| `IouringAwaiter` / `QueryAwaiter` | `weak_ptr<ConnCtx>` | 挂起期间、异步执行期间连接可能已被回收 |
| `mPendingResumes`（待恢复队列） | `weak_ptr<ConnCtx>` | 跨线程传递，条目可能已失效 |
| io_uring `user_data` | `(操作类型, 连接 id)` 打包的 64 位标签 | 只有 64 位可用；带操作类型让 CQE 的处理路径在提交时就定死，不依赖会被多方改写的 `ConnCtx::status` |

原则：**所有跨边界的引用都是弱的；回到事件循环先按标签（id + 操作类型）定位与分流，再取强引用保活。**

### 空闲超时

```
TimeWheelTop（独立线程，1ms 推进 ms 轮）
   └─ 每秒 add_task 一次 → 置标志 + write(eventfd) → 自我重注册
        └─ 事件循环 EVENT 分支 → scanIdle()
             └─ 遍历连接表，对"等了超过 5s 还没等到客户端"的连接提交一条 IORING_OP_ASYNC_CANCEL
                  └─ 精确取消该连接上那条在途的 recv/send（按 (操作类型, 连接 id) 匹配）
                       └─ 被取消的请求以 -ECANCELED 完成 → 协程从 READ/WRITE 分支走完正常关闭流程
                            └─ 协程尾部提交 close（此时那条 recv 已经不持有 socket 引用）
```

扫描放在**事件循环线程**（它要遍历连接表、还要提交 io_uring 操作），时间轮线程只置标志并唤醒。

超时回收不再用 `shutdown(fd)` 再补一条 `close`，而是发一条 `IORING_OP_ASYNC_CANCEL` 取消在途那一条请求：它随即以 `-ECANCELED` 完成，协程照旧走完自己的关闭流程。这样做有三个好处——其一，全程只有一条在途请求，同一个 fd 上不会出现第二条 `close`（那条会打在可能已被内核复用给新连接的 fd 号上，把别人的连接关掉）；其二，被取消的请求不再持有 socket 引用，随后的 `close` 才能真正关掉 socket 并把 FIN 发出去（否则 fd 被摘掉而 socket 还活着，客户端永远等不到断开）；其三，取消本身也是一条 SQE，不必在事件循环里夹一个同步的 `shutdown` 系统调用。

同时 `ConnCtx::status` 只表达"协程下一步想提交什么 IO"，`submitClose` 不再把它改写成"正在关闭"——一条 CQE 该走哪条处理路径，由它自己标签里的**操作类型**说了算，因此不会出现"`recv` 还在途时又插一条 `close`，于是 `recv` 的完成事件被当成 close 完成来处理"这种状态错配。

---

## 性能实测

> 环境：12 核 / 15 GiB / 内核 6.17 / GCC C++23 Release；wrk 4.1.0。
> 绝对值受机器负载影响（同机运行 IDE 等进程时整机约慢 10%），请以量级与相对关系为准。

> **注**：本章两张表都是本轮改动后在空闲机器上重测的，但**口径不同**：矩阵是
> **单实例、不绑核**（4 端点 × 5 级别 × 3 轮），扩展性是**服务端绑前 N 核、压测端不绑核**。
> 所以不要跨表比较单实例绝对值。

### 端点 × 并发矩阵（Keep-Alive，10s/级，3 轮取中位数）

| 端点 | Low 2/10 | Medium 4/50 | High 4/100 | VeryHigh 8/200 | Extreme 8/500 |
|------|----------|-------------|------------|----------------|---------------|
| `GET /json` | 111.4k | 154.3k | 201.9k | **222.4k** | 199.6k |
| `GET /api/user/42` | 160.7k | 194.6k | 187.9k | 212.3k | **229.2k** |
| `POST /echo` | 145.5k | 185.2k | 176.1k | 192.5k | **220.0k** |
| `GET /async/status` | 5.9k | 6.2k | 6.2k | **6.3k** | 6.2k |

（单位 req/s，加粗为该行最好成绩；60 轮零崩溃，`connect`/`read`/`write` 错误与 `Non-2xx` 全部为 0；Extreme 级别有 22~44 次 wrk 侧 socket timeout，属压测端现象。）

延迟：同步端点 P50 0.06~2.1ms；服务器 CPU——同步端点 **0.90~1.00 核**（每请求约 4.9~5.6us），异步端点仅 0.22~0.29 核（`/async/status` 的 handler 只 sleep，几乎不耗 CPU）。

### 异步路径的天花板

`/async/status` 的 handler 内含 `sleep(1ms)`：

```
8 个 worker × (1ms sleep + 289us 链路开销)  ⇒  上限 ≈ 6,200 req/s
实测跨 10 → 500 连接恒定在 5.9k~6.3k（±3%）
```

吞吐对连接数完全不敏感是"固定服务率 + 无限排队"的特征：连接数只改变队列长度，不改变吞吐；延迟随并发线性增长（1.68ms → 7.47 → 15.74 → 30.88 → 79.07ms）。

### 测量可信度

把连接数 `L`、吞吐 `λ`、实测平均延迟 `W` 代进排队论关系 `L = λW`：**异步路径吻合得很好**（误差 < 6%），说明没有请求被静默丢弃、也没有超时截断统计。**同步路径**在低并发放吻合，并发越高偏离越大（`/json` 从 1.3 倍一路升到 5.4 倍），且延迟分布很不均匀（P50 只有 0.06~2.1ms，P99 却达 15~416ms），提示存在排队论之外的延迟来源（压测端或内核调度）。因此同步路径请**以 P50 看典型延迟、以吞吐看容量**，平均延迟与长尾数字需谨慎解读。

---

### 多事件循环扩展性（`SF_LOOPS`）

| 实例数 | 线程数 | `-t4 -c100` | `-t8 -c500` | 相对单实例 |
|--------|--------|-------------|-------------|-----------|
| 1 | 4 | 226,434 | 218,392 | 1.0× |
| 2 | 6 | **430,788** | **466,552** | **1.9×** |
| 4 | 10 | 394,962 | 439,571 | 1.7× |

（同为 10s/级；服务端绑前 N 核，压测端不绑核，所以与上面矩阵表的单实例数字不可直接对照。）

扩展只在**第一档接近线性**：2 实例到 1.9×，再往上不再涨、反而略降。原因看饱和率——2 实例时服务端已用 1.85~1.93 核（绑 2 核），饱和率 93~97%，是服务端在顶；到 4 实例只剩 47~49%（绑 4 核却只用了 1.9 核），**瓶颈已经转移到压测端与 loopback 网络栈**，不是服务端扩展不动。想测服务端上限得多机压测。

另一个值得注意的点：实例数上来后各事件循环线程的 CPU tick 极差变大（2 实例 2~3%，4 实例 15~26%）。`SO_REUSEPORT` 是按连接 4 元组哈希分发的，连接数（100~500）相对实例数不够多时，分到各实例的连接本就不均。

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

1. **负载分布依赖内核哈希**：`SO_REUSEPORT` 按连接 4 元组分发，客户端 IP 集中（NAT 之后）或长连接场景下可能倾斜，目前没有主动重平衡的手段。
2. **`listen(fd, 10)` backlog 偏小**：突发连接下 accept 会积压。
3. **路由前缀匹配是线性扫描**：当前路由表小（6 条）无影响，路由数量上升后需要换成前缀树。

---

## 配套文档

- `docs/benchmark-2026-09-18-matrix.md` —— 端点 × 并发矩阵、低压梯度与稳定性完整数据
- `docs/benchmark-2026-08-19-arch-update.md` 等 —— 各阶段性能记录

## 许可

学习/实验性质的高性能服务器框架，可自由修改与扩展。
