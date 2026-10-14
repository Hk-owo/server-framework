# ServerFramework

**基于 Linux io_uring 与 C++23 协程的 HTTP 服务器框架 —— 单线程事件循环 + 无锁 MPMC 任务队列。**

每个 TCP 连接对应一个独立协程：`co_await` 时向 io_uring 提交 IO 并挂起，CQE 到达后由事件循环恢复，代码写法接近同步、没有状态机。Accept / Recv / Send / Close 以及 eventfd 唤醒全部作为 SQE 提交，连接建立用 multishot accept 一次提交持续接收，完成事件按批消费。业务 handler 默认在事件循环线程**同步执行**（零调度开销），耗时任务通过 `GetAsync`/`PostAsync` 显式投递到线程池；线程池的任务队列是一条**槽位序号环形 MPMC 队列**，worker 空闲时直接挂在队列自带的 io_uring ring 上阻塞，空载时全部停在 `io_cqring_wait`，不消耗空转 CPU。

---

## 亮点

- **全链路 io_uring** —— accept / recv / send / close / eventfd 读取全部作为 SQE 提交，不为 IO 建立任何线程；multishot accept 一次提交即可持续接收新连接；事件循环用 `io_uring_for_each_cqe` 批量遍历完成队列、`io_uring_cq_advance` 一次性标记消费；**提交侧同样攒批**——`submitXxx` 只把 SQE 放进提交队列、不各自 `io_uring_submit`，由事件循环在每轮末尾用 `io_uring_submit_and_wait_timeout` 把"交出本轮 SQE"与"等待下一个完成事件"合并成**一次 `io_uring_enter`**。

- **读缓冲由内核从环里挑，一条 recv 一次武装到底（multishot + provided buffer ring）** —— 读缓冲不再由用户按连接预分配：启动时把一批 8KB 缓冲注册成一个环（`IORING_REGISTER_PBUF_RING`，默认 8192 块、`SF_BUF_COUNT` 可调、上限 32768）；读用的是 **`IORING_OP_RECV_MULTISHOT`**，提交一次就一直产 CQE（带 `IORING_CQE_F_MORE`），不必每个请求重投一条 recv。缓冲区由内核从环里挑（`IOSQE_BUFFER_SELECT`），完成事件的 `CQE flags` 高 16 位带回它挑中的 `buffer id`；`handleCqe` 把这段数据直接 `feed` 进该连接的解析器，然后**立刻按 id 还给环**（`io_uring_buf_ring_add` + `advance`）。内存占用因此从"连接数 × 64KB"（10 万连接 ≈ 6.4GB，连一个字节都没发过的空闲连接也占着）变成"当前真在读的那几块"：池本身只 `mmap(MAP_NORESERVE)` 预留虚拟地址、没人写到的页不落物理内存，而 multishot 在提交时并不预先占块，**20 条空闲连接压着 8 块的小池也能全部武装成功**。

- **读事件排队 + 原子标志门控：绝不在错误的状态下 resume** —— multishot 意味着数据可能在任何时刻到达，包括协程正忙着写响应的时候。所以 `handleCqe` 不再直接恢复协程：它把这次读的结果（字节数 / EOF / 错误）压进连接的 `pendingReads` 队列，**只有 `ConnCtx::awaitingRead` 为真**——也就是协程确实挂在那条"等数据"的挂起点上——才 `resume`；否则只入队，让协程忙完自己回来取。协程侧则是"队列非空就继续、空了才停下"：一次 recv 可能带回多个完整请求（粘包 / pipelining），它会用 `while (try_parse())` 把累积缓冲里能切的请求**全部**切出来处理，而不是只处理一个、把剩下的压到下一次 recv。

- **C++23 协程连接模型** —— 每个连接一个协程：写响应和收尾通过 `co_await IouringAwaiter{...}` 挂起（`await_suspend` 提交对应的 send/close），等数据则走 `WaitDataAwaiter`（recv 已经由 multishot 武装好，这里不提交任何 IO）。CQE 回到事件循环后恢复，协程帧挂在连接对象里、连接回收即协程结束，不需要状态机或回调金字塔。

- **同步快速路径 + 显式异步** —— `server.Get(...)` 注册的 handler **直接在事件循环线程执行**：没有任务入队、没有线程唤醒、没有跨线程协程恢复。实测 `/json` 4 线程 100 连接约 **20 万 req/s**（12 核单实例；各并发级别的完整数据见「性能实测」）。耗时 handler 用 `GetAsync`/`PostAsync` 注册，`QueryAwaiter` 把任务投进全局线程池，后台线程完成后经 `eventfd` 唤醒事件循环恢复协程。等待唤醒用的是 io_uring 的 **read**（由 read 把 eventfd 计数取走并清零）——若改用 `poll_add` 只监听可读而不消费计数，eventfd 从第一次写入起就永久可读，事件循环会空转一整个核。`resumePending` 的 CAS 保证同一协程不会被重复入队。

- **无锁 MPMC 任务队列** —— 所有 worker 共享一条任务队列、谁空闲谁取，因此任何线程被慢任务拖住都不会让排队任务无人处理。队列用**槽位序号**判定归属：入队者等到 `sequence == pos` 才写入，出队者等到 `sequence == pos + 1` 才取走，取走后把序号推到下一圈。多生产者多消费者都不需要锁，内存一次分配、槽位循环复用——**没有节点回收，也就没有 use-after-free**。

- **阻塞等待内建在队列里** —— worker 队列取空后挂在**队列自己的 io_uring ring** 上等待，而不是忙等轮询。多消费者等待需要两道保障：等待者用**计数器**登记（单布尔会被最后一个退出者清掉，导致其余等待者再也等不到唤醒）、CQE 用 **CAS 原子认领**（liburing 的 `io_uring_cqe_seen` 实现是 `*khead = *khead + 1`，并发消费会丢更新并把 CQ 的 head 推错）。

- **连接生命周期用弱引用 + 单调递增连接 id** —— 连接表独占持有连接对象，**所有跨线程或跨生命周期边界的引用都是 `weak_ptr`**：投进线程池的异步任务、待恢复协程队列都在取用时 `lock()` 确认存活，拿不到就丢弃。协程内部则用裸指针——连接持有协程帧，所以"协程在跑"就蕴含"连接活着"；这个不变式不能改用 `shared_ptr` 表达，否则连接与协程帧会互相持有成环。回到事件循环这一路则**既不存指针、也不解引用对象**：`user_data` 里放的是 `(操作类型, 连接 id)` 打包成的 64 位标签，`handleCqe` 先按 id 回表，查不到就是陈旧事件——id 单调递增、永不复用，所以"上一代连接"的 CQE 不可能撞上"新一代连接"，也就不需要额外的一代代次（裸指针会被 allocator 复用，连接 id 不会）。

- **时间轮驱动的空闲超时** —— 四级级联时间轮（`ms → sec → min → hour`，最小刻度 1ms，由独立的 io_uring 1ms 超时驱动并带追赶机制）。当前接入点是**连接空闲超时**：时间轮每秒唤醒一次事件循环，扫描连接表回收"服务器在等客户端却一直等不到"的连接（阈值 5s），避免一个连上却不发数据的客户端永久占住 fd 和缓冲环里的一块 buffer。扫描对超时连接发一条 **`IORING_OP_ASYNC_CANCEL`**，按 `(操作类型, 连接 id)` 精确取消它那条在途的 `recv`/`send`——被取消的请求以 `-ECANCELED` 正常完成，协程照旧从 READ/WRITE 分支走完自己的关闭流程（协程尾部提交 `close`）。比 `shutdown + close` 更干净：**全程只有一条在途请求**，同一个 fd 上不会出现两条 close（第二条会打在可能已被内核复用给新连接的 fd 号上）；被取消的请求不再持有 socket 引用，随后的 close 才能真正关掉 socket 并把 FIN 发出去；而且取消本身就是一条 SQE，不必在事件循环里夹一个同步的 `shutdown` 系统调用。扫描也**不是每秒硬扫一遍连接表**：`Server` 记着所有等待连接里最早的那个等待起点（水位），没逼近超时边缘时 `scanIdle` 读这一个值就 O(1) 返回。遍历代价随连接数线性增长（10 万连接约 0.4ms），而它跑在事件循环线程上，每一次都是一记真实的延迟尖刺。

- **HTTP/1.1 完整实现** —— 请求行与 Header 解析、URI Query 自动解码、`Content-Length` 与 `Chunked Transfer-Encoding`、Keep-Alive 长连接；发送侧支持**短写续传**（记录断点，上层业务无感知）。

- **异步滚动日志** —— spdlog 异步 logger，只挂滚动文件 sink（每个 10MB、保留 5 个），不向 stdout 输出，因此即便把进程 stdout 重定向到文件也不会产生无限增长的日志。日志按级别分工：连接建立/关闭、空闲超时、各类错误走 INFO，而**每个 IO 事件**的追踪日志走 TRACE——默认级别下不产生任何格式化与入队开销。

- **io_uring 提交侧优化** —— 两层：① 事件循环是这条 ring 唯一的提交者，因此启用 `DEFER_TASKRUN | SINGLE_ISSUER`，把 task_work 推迟到下一次进入内核、让内核省掉提交侧的原子操作（内核不支持时自动退回默认模式）；② **提交与等待合并** —— 所有 `submitXxx` 只 `io_uring_get_sqe` + prep，不再各自 `io_uring_submit`，事件循环每轮末尾一次 `io_uring_submit_and_wait_timeout` 同时完成"交出本轮 SQE"和"等下一个完成事件"。第 ② 步之前，每完成一个 IO 要进两次内核（提交下一个 IO + 等待）。实测同一台机器上成对 A/B（单实例 `GET /json` 4t/100c）：**11.7 万 → 17.9 万 req/s**（+52%），`io_uring_enter` 从每请求 2.4 次降到 0.08 次；机器空闲时该配置的绝对吞吐为 **20.2 万**（见「性能实测」，两处数字口径一致、只是测量时机不同）。配合上面的日志分级，单请求 CPU 在 **4.9~5.6us** 量级。刻意未启用 `SQPOLL`——它需要 `CAP_SYS_NICE`，且内核 poller 会常驻占用一个核。

- **多事件循环 × `SO_REUSEPORT`** —— 可按核数启动多个 `Server` 实例（环境变量 `SF_LOOPS`，默认取 CPU 核数），每个实例持有**私有的** io_uring ring、连接表与时间轮；实例之间只通过 `SO_REUSEPORT` 共享监听端口，由内核按连接 4 元组哈希分发——**同一 TCP 连接的请求恒定落到同一实例**，所以连接状态不需要任何跨实例同步。异步 handler 仍然投进共享线程池，完成后再写回**本实例的 eventfd**，因此协程恢复不会串到别的 ring 上。实测单实例 `/json` 约 22.6 万 req/s，2 实例推到 **46.7 万**；再加实例吞吐不再增长——瓶颈已经转移到压测端与 loopback 网络栈（见「性能实测」）。

---

## 快速开始

### 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux 内核 ≥ 5.19（provided buffer ring；推荐 ≥ 6.0 以完整支持 multishot accept） |
| 编译器 | 支持 C++23 的 GCC 或 Clang |
| 依赖 | `liburing`（需 `io_uring_setup_buf_ring` 等缓冲环接口）、`spdlog`、`fmt`、`nlohmann-json` |
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
│         ConnCtx / IouringAwaiter / WaitDataAwaiter / QueryAwaiter  │
│         状态：mConnections(按连接 id) / mPendingResumes / mTimeWheel│
│              mEventFd / 缓冲环 mBufRing / mBufStarved              │
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
  └─ 建 ConnCtx → 武装 multishot recv → 启动连接协程
       └─ co_await WaitDataAwaiter{}        ← 纯挂起：recv 已武装，这里不提交任何 IO
            CQE 到达 → handleCqe：
              ├─ 数据 feed 进 ConnCtx::parser（搬运，不做解析）
              ├─ 按 buffer id 把这块 buffer 还给环（立刻）
              ├─ 这次读的结果入队 pendingReads
              └─ 仅当 awaitingRead 为真才 resume 协程
       └─ 队列非空 → 继续：
              ├─ 解析：ConnCtx::parser.try_parse()（切分，可一次切出多个）
              ├─ 路由：Router.match
              ├─ 执行：同步 handler 直接调用
              │        异步 handler → co_await QueryAwaiter
              │                        └─ 投线程池 → handler 执行
              │                          → CAS resumePending → 入队 + write(eventfd)
              │                          → 事件循环 read 到 → 恢复协程
              └─ co_yield IouringAwaiter{WRITE}   ← 提交 send（短写续传）
       └─ co_await IouringAwaiter{CLOSE}          ← 先取消在途 recv，再 close
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

### 读缓冲：内核从环里挑

缓冲不再由用户按连接预分配，而是启动时一次性注册成一个环交给内核：

```cpp
// setupBufRing()：默认 8192 块 8KB，只预留虚拟地址，物理页按需落
mBufPool = mmap(nullptr, kBufCount * kBufSize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
mBufRing = io_uring_setup_buf_ring(&ring, kBufCount, kBufGroup, 0, &err);
for (unsigned i = 0; i < kBufCount; ++i)         // 起步时把整池挂进环
    io_uring_buf_ring_add(mBufRing, pool + i * kBufSize, kBufSize, i, kBufRingMask, i);
io_uring_buf_ring_advance(mBufRing, kBufCount);
```

读方向用 **multishot recv**：提交一次就一直在跑，之后每次有数据都自己产一条 CQE
（带 `IORING_CQE_F_MORE`），不需要逐请求重投；缓冲区由内核从环里挑：

```cpp
io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
sqe->flags     |= IOSQE_BUFFER_SELECT;
sqe->buf_group  = kBufGroup;                     // 内核自己挑一块空闲的填进去
```

完成事件把挑中的 `buffer id` 带在 `CQE flags` 高 16 位（`IORING_CQE_F_BUFFER`）。
`handleCqe` 把这段字节 `feed` 进该连接的解析器（`ConnCtx::parser`），然后**立刻按 id
归还**那块的——归还之后内核马上可能把它交给别的连接，所以既不跨挂起点、连接上也不
留凭证。所以在 `handleCqe` 返回时，pool 里那块已经回到环上了。

| | 现在 |
|---|---|
| 提交次数 | 一条连接一辈子只提交一次 recv（除池空/出错要重新武装） |
| 连接侧 | 不持有读缓冲，数据 feed 进解析器后那块 id 立刻还给环 |
| 内存占用 | 跟随"真正在收数据的那几块"，而不是连接表大小 |
| 预留方式 | `MAP_NORESERVE` 只占虚拟地址，没写到的页不落物理内存 |
| 空闲连接 | **一块都不占**：multishot 提交时内核并不预先取块，数据到了才取 |

`-ENOBUFS` 只在"数据到达却没有可用块"时出现（这一次**不**带 buffer id，池里一块没少）。
不能就地重新武装——池还是空的，会立刻再失败、把事件循环空转成 100% CPU——所以把连接
挂进 `mBufStarved`，等任一归还点把 buffer 还给环时一对一重新武装（`resumeStarved`），
协程继续挂在原地等、不必惊动它。这类连接身上没有在途请求，空闲超时对它不能用
`ASYNC_CANCEL`：`scanIdle` 往 `pendingReads` 里塞一个 `-ECANCELED` 再唤醒协程，
让它走和其他连接完全一样的关闭流程。

### 读事件排队与唤醒

数据可能在任何时刻到达，**包括协程正忙着写响应的时候**。所以 `handleCqe` 不直接恢复
协程，而是把这次读的结果（字节数 / EOF / 错误）压进 `ConnCtx::pendingReads`，由
`ConnCtx::awaitingRead` 把门。这两个"在等"的标志语义不同，不能合并：

| 标志 | 含义 | 谁看它 |
|---|---|---|
| `waiting` / `waitingSince` | 服务器在等客户端（等读或等写） | `scanIdle`（空闲超时） |
| `awaitingRead` | 协程正挂在**能被读事件唤醒**的那个挂起点上 | `handleCqe`（决定要不要 resume） |

写响应期间 `waiting` 是 true、`awaitingRead` 却是 false——此时到达的读事件只入队，
等协程写完自己回来取。协程侧就是"队列非空就继续、空了才停下"：

```cpp
if (pendingReads.empty()) {              // 空 → 停下
    status = READ;                       // 供 cancelInFlight 按 (READ, id) 匹配
    awaitingRead = true;
    co_await WaitDataAwaiter{};          // 纯挂起：recv 已武装，不提交任何 IO
    awaitingRead = false;
    continue;
}
while (!pendingReads.empty()) {          // 非空 → 继续
    n = pop();
    while (auto req = parser.try_parse())   // 一次可能切出多个（粘包 / pipelining）
        ... 路由 → handler → co_yield 写响应 ...
}
```

`WaitDataAwaiter::await_suspend` 里什么都不提交——recv 是 multishot 的，一直武装着。

### 关闭前必须先让 recv 结束

multishot recv 是**唯一握着 socket 引用的东西**。不先让它结束就 `close`，只是把 fd 从
进程表里摘掉、socket 还活着、**FIN 发不出去**，客户端会一直等到自己超时（就是前面讲的
`shutdown+close` 那个坑，multishot 让它每次关闭都会踩到）。所以关闭分两步：

```cpp
if (connCtx.recvArmed) {                     // 还有一条 recv 在途
    connCtx.recvArmed    = false;
    connCtx.closePending = true;
    cancelInFlight(connCtx, OpType::READ);   // 第一步：取消它
    return;                                  // 真正的 close 推后
}
submitCloseNow(fd, connCtx);                 // 没有在途 recv：直接关
```

第二步在 `handleCqe` 的 READ 分支里做——收到那条**被取消的 recv 自己的 CQE**（不带
`F_MORE`）时才提交 close，那一刻请求真的结束了、引用才真正放下。不能图省事在 cancel
的完成事件里就 close：`ASYNC_CANCEL` 的完成只说明"目标被取消了"，引用可能还没放下。

> `IORING_REGISTER_PBUF_RING` 只接受 ≤ 32768 且为 2 的幂的条目数（再大直接 `-EINVAL`）。
> 但默认只用 **8192** 条：注册给内核的 ring 内存会被 pin 住、计入 `RLIMIT_MEMLOCK`，
> 而它的硬限制在本机是 8MB（普通用户调不上去），默认 `SF_LOOPS` = CPU 核数时每个实例
> 都要吃这笔额度——按 32768 条（每实例 512KB）实测只起得来 9 个实例，12 核的默认配置
> 直接起不来；8192 条则 12 个实例合计约 5.4MB。注册失败（`-ENOMEM`）时还会按 2 的幂
> 逐级减半重试，宁可某个实例池小一点，也不要整个进程退出。要更大的池就把 memlock
> 调大（需要 root）再用 `SF_BUF_COUNT` 指定。`Server.h` 用 `static_assert` 把这些约束
> 钉在编译期。

### 空闲超时

```
TimeWheelTop（独立线程，1ms 推进 ms 轮）
   └─ 每秒 add_task 一次 → 置标志 + write(eventfd) → 自我重注册
        └─ 事件循环 EVENT 分支 → scanIdle()
             └─ 遍历连接表，对"等了超过 5s 还没等到客户端"的连接提交一条 IORING_OP_ASYNC_CANCEL
                  └─ 精确取消该连接上那条在途的 recv/send（按 (操作类型, 连接 id) 匹配）
                       └─ 被取消的请求以 -ECANCELED 完成 → 协程从 READ/WRITE 分支走完正常关闭流程
                            └─ 协程尾部提交 close（此时那条 recv 已经不持有 socket 引用）
             （挂在"等缓冲环腾出 buffer"上的连接身上没有在途请求可取消：
                往 pendingReads 里塞一个 -ECANCELED 再唤醒协程，走同一条关闭流程）
```

扫描放在**事件循环线程**（它要遍历连接表、还要提交 io_uring 操作），时间轮线程只置标志并唤醒。

但每秒一次的扫描并不会真的每秒遍历一遍连接表——那在连接多起来之后很贵（10 万连接约 0.4ms），
而它是在事件循环线程上跑的，等于每秒往请求延迟里插一记尖刺。所以 `Server` 维护一个水位
`mOldestWaitSince`：**所有正在等待的连接里最早的那个等待起点**。`scanIdle` 进来先看这一个值：

```cpp
if (mOldestWaitSince == INT64_MAX) return;              // 压根没有连接在等待
if (now - mOldestWaitSince < kIdleTimeoutMs) return;    // 最老的那个都还没逼近超时
```

只有这两条都不成立，才真的遍历连接表；遍历时顺便把水位**重算**成当时的最小值写回去。
水位的更新见 `IouringAwaiter::await_suspend`（每次开始等待时把它往下压）与 `Server::noteWaitStart`。
水位只会偏小、不会偏大——连接被回收后它可能残留一个过期的旧值，那最多让扫描多跑一轮；
而每次真正扫描都会重算，所以**该回收的连接一个都不会漏**。

超时回收不再用 `shutdown(fd)` 再补一条 `close`，而是发一条 `IORING_OP_ASYNC_CANCEL` 取消在途那一条请求：它随即以 `-ECANCELED` 完成，协程照旧走完自己的关闭流程。这样做有三个好处——其一，全程只有一条在途请求，同一个 fd 上不会出现第二条 `close`（那条会打在可能已被内核复用给新连接的 fd 号上，把别人的连接关掉）；其二，被取消的请求不再持有 socket 引用，随后的 `close` 才能真正关掉 socket 并把 FIN 发出去（否则 fd 被摘掉而 socket 还活着，客户端永远等不到断开）；其三，取消本身也是一条 SQE，不必在事件循环里夹一个同步的 `shutdown` 系统调用。

同时 `ConnCtx::status` 只表达"协程下一步想提交什么 IO"，`submitClose` 不再把它改写成"正在关闭"——一条 CQE 该走哪条处理路径，由它自己标签里的**操作类型**说了算，因此不会出现"`recv` 还在途时又插一条 `close`，于是 `recv` 的完成事件被当成 close 完成来处理"这种状态错配。

---

## 性能实测

> 环境：12 核 / 7.2 GiB / 内核 6.17 / GCC C++23 Release；wrk 4.1.0。
> 绝对值受机器负载影响（同机运行 IDE 等进程时整机约慢 10%），请以量级与相对关系为准。

> **口径**：矩阵是**单实例、不绑核**（4 端点 × 5 级别 × 3 轮 = 60 轮，每轮 10s，每个端点前
> 重启服务器并预热），扩展性是**服务端绑前 N 核**——两张表不要跨着比单实例绝对值。
> 本轮矩阵是在**机器有其他负载**时测的（同批 3 轮之间很稳，见下）；扩展性那张见本节末尾说明。

### 端点 × 并发矩阵（Keep-Alive，10s/级，3 轮取中位数）

| 端点 | Low 2/10 | Medium 4/50 | High 4/100 | VeryHigh 8/200 | Extreme 8/500 |
|------|----------|-------------|------------|----------------|---------------|
| `GET /json` | 167.1k | 193.0k | **203.9k** | 197.8k | 200.9k |
| `GET /api/user/42` | 146.2k | 188.0k | **195.1k** | 193.1k | 190.8k |
| `POST /echo` | 152.4k | 177.8k | **178.7k** | 142.6k | 135.3k |
| `GET /async/status` | 5.7k | 6.2k | **6.2k** | 6.2k | 6.2k |

（单位 req/s，加粗为该行最好成绩；60 轮零崩溃，`connect`/`read`/`write` 错误与 `Non-2xx` 全部为 0；Extreme 级别有 27~32 次 wrk 侧 socket timeout，属压测端现象。3 轮之间的离散度很小：`/json` 各档极差 ≤ 3%。）

跟上一版（**每个请求都要重投一条 `recv`**）相比，**低并发档提升最明显**：`/json` Low 111.4k → **167.1k**（+50%）、Medium 154.3k → **193.0k**（+25%）。省掉的正是"每请求一次 `io_uring_get_sqe` + 一次提交"，这笔开销在低并发下的相对占比最大。高并发档两组数据互有高低（1%~11%），本轮又是在有负载的机器上测的，不足以断定优劣。

延迟：同步端点 P50 0.05~2.8ms；服务器 CPU——同步端点 **0.92~1.00 核**（`/json` High 档 0.99 核跑 203.9k ⇒ 每请求约 4.9us），异步端点仅 0.22~0.29 核（`/async/status` 的 handler 只 sleep，几乎不耗 CPU）。

### 异步路径的天花板

`/async/status` 的 handler 内含 `sleep(1ms)`：

```
8 个 worker × (1ms sleep + 289us 链路开销)  ⇒  上限 ≈ 6,200 req/s
实测跨 10 → 500 连接恒定在 5.7k~6.2k（±4%）
```

吞吐对连接数完全不敏感是"固定服务率 + 无限排队"的特征：连接数只改变队列长度，不改变吞吐；延迟随并发线性增长（1.74ms → 7.74 → 16.03 → 31.38 → 76.84ms）。

### 测量可信度

把连接数 `L`、吞吐 `λ`、实测平均延迟 `W` 代进排队论关系 `L = λW`：**异步路径吻合得很好**（误差 < 4%），说明没有请求被静默丢弃、也没有超时截断统计。**同步路径**低并发档也吻合（`/json` 的 Low/Medium/High 三档与 `L = λW` 差在 15% 以内），但并发越高实测平均延迟越超出预测，且延迟分布很不均匀（P50 只有 0.05~2.8ms，P99 却达 0.5~197ms）——提示存在排队论之外的延迟来源（压测端或内核调度）。因此同步路径请**以 P50 看典型延迟、以吞吐看容量**，平均延迟与长尾数字需谨慎解读。

---

### 多事件循环扩展性（`SF_LOOPS`）

| 实例数 | 线程数 | `-t4 -c100` | `-t8 -c500` | 相对单实例 |
|--------|--------|-------------|-------------|-----------|
| 1 | 4 | 226,434 | 218,392 | 1.0× |
| 2 | 6 | **430,788** | **466,552** | **1.9×** |
| 4 | 10 | 394,962 | 439,571 | 1.7× |

（这是**上一版代码、空闲机器**上的绑核数据，所以与上面矩阵表的单实例数字不可直接对照。
本轮矩阵改动后用 `benchmark_multiloop.sh` 复测时，机器带着其他负载：同一配置在几分钟内就抖出
160k ↔ 437k（脚本自己警告"不同负载下可能差数倍，务必在空闲机器上跑，或只比较同一时刻的相邻
测量"），4 实例档的服务端 CPU 只有 0.99/4 核——瓶颈落在压测端与 loopback，不是服务端，
因此不作为数据替换。）

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
4. **池容量受"内核条目数"与"memlock"两重限制**：`IORING_REGISTER_PBUF_RING` 最多 32768 条（`buffer id` 只有 16 位，拆多个 `bgid` 也就到 65536），超出的连接会在 `mBufStarved` 里排队等某个归还点——不失败，但带上排队延迟；而注册给内核的 ring 内存会被 pin 住、计入 `RLIMIT_MEMLOCK`，8MB 的默认硬限制下每实例最多约 512KB（注册失败会逐级减半降级）。所以默认池取 8192 条，可用 `SF_BUF_COUNT` 覆盖（配 memlock 一起调）。注意这跟"连接数"不是一回事：multishot recv 提交时**不预先占块**，空闲连接一块都不占，只有真在收数据的那一刻才消耗池里的块。
5. **单连接请求累积上限 1MB**：`HttpParser::kMaxBufferSize` 命中后回 413 并关闭连接。没有它，一个只发半截请求（或声明超大 `Content-Length`）的客户端就能把解析缓冲撑到任意大。要支持更大的 body，得调大这个常量、或者改成先看 `Content-Length` 再决定收不收。

---

## 配套文档

- `docs/benchmark-2026-09-18-matrix.md` —— 端点 × 并发矩阵、低压梯度与稳定性完整数据
- `docs/benchmark-2026-08-19-arch-update.md` 等 —— 各阶段性能记录

## 许可

学习/实验性质的高性能服务器框架，可自由修改与扩展。
