//
// Created by lacas on 2026/3/18.
//

#include "Server/Server.h"
#include "Server/Router.h"
#include "Http/HttpParser.h"
#include "Http/HttpResponse.h"
#include "ThreadPool.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netdb.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <atomic>
#include <optional>
#include <memory>
#include "Logger.h"
#include "liburing.h"

using namespace std;

// ── ConnCtx ──────────────────────────────────────────────────
// 连接上下文：单连接的 IO 状态（解析器、待处理读结果、写缓冲、协程句柄）
//
// 读方向没有连接自带的缓冲区：数据由内核从共享的 buffer ring 里挑一块填进来
// （见 setupBufRing），handleCqe 把这块的内容拷进 parser 之后立刻按 buffer id
// 还给池子——所以 buffer 的生命周期完全在 handleCqe 内部闭环，连接上不留凭证。
// 写方向仍然自带 writeBuffer：响应内容是应用生成的，大小和时机只有应用知道，
// 不是"内核挑一块"能表达的东西。
//
// 解析被拆成了两半，parser 因此必须挂在连接上而不是协程栈上：
//   feed（把字节 append 进累积缓冲）在 handleCqe 里做，好让那块 buffer 能马上归还；
//   try_parse（从累积缓冲里切出请求）留到协程里做，因为切出来之后要路由、要执行
//   handler、要写响应——那些都要能挂起，而 handleCqe 是普通函数挂不了。
//
// 所有权：连接表（mConnections）里的 shared_ptr 是唯一强引用，谁都不该再持一份
// 强引用（连接持有自己的协程帧，协程帧再持连接就成了环，永远释放不掉），
// 所以程序内部一律用 weak_ptr 引用它，用到的当下 lock() 出来。
//
// id 是连接在表里的身份证：它单调递增、永不复用，被编进 io_uring 的 user_data。
// 事件循环拿它查表，查不到就直接丢弃这条 CQE——这一步不读连接对象的任何字段，
// 所以"对象已经析构、CQE 还在路上"不再构成 use-after-free。
struct Server::ConnCtx {
    OpType   status;                        // 协程侧下一步想做什么（提交 IO 时看它）
    uint64_t id;                            // 全局唯一、单调递增，永不复用
    int      fd;

    // 解析状态随连接走：handleCqe 往里喂字节，协程从里面切请求（见文件头说明）
    HttpParser          parser;
    // 每次读完成的结果队列：>0 = 读到的字节数（数据已经 feed 进 parser），
    // 0 = 对端关闭，<0 = 出错或被取消；kReadTooLarge 表示累积已超上限
    std::deque<int32_t> pendingReads;
    // 协程是否正挂在"等数据"上。只有它为 true，handleCqe 收到读事件才 resume；
    // 协程挂在别处（比如正等 send 完成）时只入队，让它自己回到循环来取——
    // 这就是"不在错误的状态下 resume"。只在事件循环线程读写
    std::atomic<bool>   awaitingRead{false};

    std::string writeBuffer;                // 写缓冲（响应内容）
    size_t      write_offset = 0;           // 短写续传偏移

    std::atomic<bool> resumePending{false};
    std::optional<coro::Task<void>> task;
    std::coroutine_handle<>         handle;

    // 空闲超时用：当前是否正等客户端（在读/在写），以及从什么时候开始等
    std::atomic<bool>    waiting{false};
    std::atomic<int64_t> waitingSince{0};
    // 是否正挂在"等池里腾出 buffer"上。这种连接身上没有在途请求，
    // 空闲回收不能走 ASYNC_CANCEL，得直接把协程叫醒（见 scanIdle）。
    // 只在事件循环线程读写
    bool waitingBuffer = false;
    // 这条 fd 上是否有一条 multishot recv 在途。它是唯一握着 socket 引用的东西，
    // 所以 close 之前必须先让它结束：否则 close 只摘掉 fd、socket 还活着、
    // FIN 发不出去，客户端会一直等（见 submitClose）。只在事件循环线程读写
    bool recvArmed = false;
    // 已经发过取消、等那条 recv 结束就提交真正的 close（关闭流程的第二步）
    bool closePending = false;

    ConnCtx(OpType status, int fd)
            : status(status), id(0), fd(fd), task(std::nullopt) {}
};

// 累积缓冲超限时用它入队（复用标准 errno：消息过长）。recv 本身不会返回这个码，
// 所以协程看到它就知道是自己这一侧判断出来的"请求太大"，该回 413
static constexpr int32_t kReadTooLarge = -EMSGSIZE;

// 连接空闲超时：服务器在等客户端（等读或等写）超过这个时长就把连接回收。
// 没有这道闸，一个连上却不发数据的客户端会永久占住 fd 和缓冲池里的一块 buffer
static constexpr int64_t kIdleTimeoutMs = 5000;

static int64_t nowMs() {
    return chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now().time_since_epoch()).count();
}

// 从提交队列取一个 SQE。主路径上不在这里提交：事件循环每轮末尾会用
// io_uring_submit_and_wait_timeout 把本轮攒下的 SQE 一次性交给内核，
// 于是"提交 IO"和"等待完成"合并成同一次 io_uring_enter。
// 只有极端情况（单轮塞进上万条 SQE 把提交队列占满）才会就地冲一次腾空间
static struct io_uring_sqe* acquireSqe(struct io_uring& ring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        io_uring_submit(&ring);
        sqe = io_uring_get_sqe(&ring);
    }
    return sqe;
}

// ── IouringAwaiter ───────────────────────────────────────────
// co_await 挂起时向 io_uring 提交对应的 IO 请求，完成事件到达后由事件循环恢复协程。
// 只管"要提交一条 IO 才有的等"：写响应（send，含短写续传）和收尾（close）。
// 读方向已经改用 multishot recv（一次武装、持续产 CQE），挂起等数据走 WaitDataAwaiter，
// 从这儿提交读会变成"同一 fd 上两条在途 recv"。
// 同样只弱引用连接
struct Server::IouringAwaiter {
    Server*                server;
    std::weak_ptr<ConnCtx> connWeak;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto conn = connWeak.lock();
        if (!conn) return;                     // 连接已回收，没什么可提交的
        conn->handle = h;
        // 记下"从此刻起在等客户端"，空闲扫描据此判定超时
        const int64_t since = nowMs();
        conn->waitingSince.store(since, std::memory_order_relaxed);
        conn->waiting.store(true, std::memory_order_release);
        // 顺手把这个起点压进扫描水位。await_suspend 只在事件循环线程被触发
        // （所有 resume 都来自 handleCqe / eventfd 分支），水位用非原子普通成员即可
        server->noteWaitStart(since);
        // 每个 IO 事件一条，属于追踪级：默认级别下不产生格式化与入队开销
        LOGGER_TRACE("await_suspend: status={} fd={} handle={}",
                     (int)conn->status, conn->fd, (void*)h.address());

        switch (conn->status) {
            case OpType::WRITE:
                server->submitWrite(conn->fd, conn->writeBuffer, *conn);
                break;
            case OpType::CLOSE:
                server->submitClose(conn->fd, *conn);
                break;
            case OpType::READ:
                // 走到这里说明有人拿 IouringAwaiter 当"等数据"用了。真提交的话
                // 会在同一条 fd 上再插一条 recv，而 multishot 那条还在——所以只报错
                LOGGER_ERROR("IouringAwaiter used for READ fd={}, expected WaitDataAwaiter",
                             conn->fd);
                break;
            default:
                break;
        }
    }
    void await_resume() const noexcept {}
};

// ── WaitDataAwaiter ──────────────────────────────────────────
// 挂起等数据。读方向是 multishot recv，一次武装之后就一直在跑，所以这里
// **什么都不提交**，只把 handle 记下来，等 handleCqe 把读结果入队后唤醒。
//
// 唤醒由 ConnCtx::awaitingRead 把门：await_suspend 返回后协程就彻底停下了，
// 期间到达的读事件只会入队；handleCqe 看到这个标志为 true 才 resume。
// 反过来，协程正忙时（比如挂在 send 上）到达的读事件既不丢也不误唤醒
struct Server::WaitDataAwaiter {
    Server*                server;
    std::weak_ptr<ConnCtx> connWeak;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto conn = connWeak.lock();
        if (!conn) return;                     // 连接已回收，没有协程要恢复
        conn->handle = h;
        // 和 IouringAwaiter 一样：记下"从此刻起在等客户端"，空闲扫描据此判定超时
        const int64_t since = nowMs();
        conn->waitingSince.store(since, std::memory_order_relaxed);
        conn->waiting.store(true, std::memory_order_release);
        server->noteWaitStart(since);
        LOGGER_TRACE("await_suspend(WaitData): fd={} handle={}",
                     conn->fd, (void*)h.address());
    }
    void await_resume() const noexcept {}
};

// ── QueryAwaiter ─────────────────────────────────────────────
// co_await 时将业务 handler 投递到全局线程池异步执行，
// 完成后经 eventfd 唤醒事件循环恢复协程；resumePending CAS 防止重复入队。
// 投出去的任务只弱引用连接：任务在工作线程上跑的时候，连接可能已经被事件循环
// 回收了，这时必须发现"没人可恢复"，而不是拿着野指针去写 resumePending
struct Server::QueryAwaiter{
    Server*                server;
    std::weak_ptr<ConnCtx> connWeak;
    std::function<void()>  callback;
    bool await_ready() const noexcept { return false; }
    void await_resume() const noexcept {}
    void await_suspend(std::coroutine_handle<> h) {
        std::weak_ptr<ConnCtx> weak = connWeak;
        std::function<void()> task =
                [h, s = server, cb = std::move(callback), weak] mutable {
                    try { cb(); } catch (...) {}
                    auto ctx = weak.lock();
                    if (!ctx) return;          // 连接已回收，没有协程要恢复
                    bool expected = false;
                    if (ctx->resumePending.compare_exchange_strong(expected, true,
                                                                   std::memory_order_acq_rel)) {
                        auto item = std::make_pair(h, weak);
                        s->mPendingResumes.enqueue(item);
                        uint64_t v = 1;
                        int ret = write(s->mEventFd, &v, sizeof(v));
                        LOGGER_TRACE("write eventfd ret={} errno={}", ret, errno);
                    }
                };
        // 业务任务直投全局线程池：不经时间轮、无固定延迟
        ThreadPool::global_instance().submit(std::move(task));
    }
};

int Server::listen(const std::string &bind, const std::string& port) {
    // addrinfo 由 C 接口分配，用 unique_ptr 绑定 freeaddrinfo，避免漏释放
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    struct addrinfo* raw = nullptr;
    if (getaddrinfo(bind.c_str(), port.c_str(), &hints, &raw) != 0) {
        LOGGER_ERROR("getaddrinfo failed for {}:{}", bind, port);
        return -1;
    }
    std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> res(raw, &freeaddrinfo);

    int  fd    = -1;
    bool bound = false;
    for (struct addrinfo* p = res.get(); p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) {
            LOGGER_ERROR("socket creation failed, family={}", p->ai_family);
            continue;
        }

        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            LOGGER_WARN("setsockopt SO_REUSEADDR failed on fd={}", fd);
            // 通常不致命，继续执行
        }

        // 多个事件循环实例各绑同一端口，由内核按连接 4 元组哈希分发到其中一个。
        // 同一 TCP 连接的 4 元组终生不变，所以它的请求恒定落到同一实例，
        // 连接状态（协程 / 缓冲 / 连接表）天然不需要跨实例同步
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) == -1) {
            LOGGER_WARN("setsockopt SO_REUSEPORT failed on fd={}, 多实例将无法共享端口", fd);
        }

        if (::bind(fd, p->ai_addr, p->ai_addrlen) == -1) {
            LOGGER_ERROR("bind failed on fd={}, addr family={}", fd, p->ai_family);
            close(fd);
            continue;
        }

        if (::listen(fd, 10) == -1) {
            LOGGER_ERROR("listen failed on fd={}, backlog=10", fd);
            close(fd);
            continue;
        }

        bound = true;
        LOGGER_INF("Successfully bound and listening on fd={}", fd);
        break;
    }

    if (!bound) {
        LOGGER_ERROR("Failed to bind any address for {}:{}", bind, port);
        return -1;
    }
    mSockFd = fd;
    LOGGER_INF("Server listening on {}:{}", bind, port);
    return 0;
}

void Server::run(unsigned entries, unsigned flags) {
    int ret = io_uring_queue_init(entries, &ring, flags);
    if (ret < 0 && flags != 0) {
        // 内核不支持这些 flag（DEFER_TASKRUN 需要 6.1+）时退回默认模式，
        // 不因为一个优化开关而起不来
        LOGGER_WARN("io_uring_queue_init(flags={}) failed: {}, retrying with defaults",
                    flags, ret);
        ret = io_uring_queue_init(entries, &ring, 0);
    }
    if (ret < 0) {
        LOGGER_ERROR("io_uring_queue_init failed: {}", ret);
        exit(1);
    }

    // 缓冲池必须在第一条 recv 之前就绪：此后所有读方向的缓冲都由内核从这个环里挑
    if (setupBufRing() != 0) {
        LOGGER_ERROR("buffer ring setup failed, cannot serve reads");
        exit(1);
    }

    // 这两条 SQE 先留在提交队列里，循环第一次进内核时会一起交出去
    submitMultishotAccept();
    submitEventFdRead();

    struct io_uring_cqe *cqe = nullptr;

    while (true) {
        unsigned head;
        unsigned processed = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            uint64_t tag    = io_uring_cqe_get_data64(cqe);
            int      res    = cqe->res;
            unsigned cflags = cqe->flags;
            processed++;

            // tag 里装的是 (操作类型, 连接 id)，不是指针；回表校验在 handleCqe 内部完成
            handleCqe(tag, res, cflags);
        }
        io_uring_cq_advance(&ring, processed); // 批量标记消费，替代逐个 cqe_seen

        // 一次 io_uring_enter 同时干两件事：把本轮产生的 SQE 交给内核 + 等下一个完成事件。
        // 合并这两步不是可有可无的微优化：如果每个 submitXxx 各自 io_uring_submit，
        // 那么"提交下一个 IO"和"等待它完成"就是两次进入内核，这里一次搞定。
        //
        // 超时给到 1s 也不会漏事件：能唤醒本循环的只有 CQE——multishot accept、
        // 连接上的 recv/send/close、eventfd。空闲超时扫描跑在时间轮自己的 ring 上，
        // 它只写 eventfd 叫醒本循环。所以不必每毫秒回来空转一次
        struct __kernel_timespec ts { .tv_sec = 1, .tv_nsec = 0 };
        ret = io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, nullptr);
        // ETIME = 超时正常返回，继续循环
        // EINTR = 信号打断，继续
        if (ret < 0 && ret != -ETIME && ret != -EINTR) {
            LOGGER_ERROR("io_uring_submit_and_wait_timeout: {}", ret);
        }
    }
}

void Server::handleCqe(uint64_t tag, int res, unsigned cflags) {
    const OpType   op     = tagOp(tag);
    const uint64_t connId = tagConnId(tag);

    if (op == OpType::ACCEPT) {
        if (res >= 0) {
            int newFd = res;
            auto ctx = std::make_shared<ConnCtx>(OpType::READ, newFd);
            // 协程只持弱引用，避免"连接持有协程帧、协程帧又持有连接"的环
            std::weak_ptr<ConnCtx> weak = ctx;
            ctx->task.emplace(clientConnect(weak));
            ctx->handle = ctx->task.value().raw_handle();
            // 先登记进表拿到连接 id，协程 resume 后提交 IO 才有合法的标签可用
            const uint64_t newId = registerConn(ctx);
            LOGGER_INF("New connection fd={} id={}", newFd, newId);
            // 在让协程跑起来之前先把 multishot recv 武装上：此后 CQE 会自己来，
            // 数据到得比协程挂起还早也没关系——handleCqe 会入队，协程自己会取
            submitRecv(newFd, *ctx);
            ctx->task.value().resume();
        } else {
            LOGGER_ERROR("Accept error: {}", res);
        }
        // multishot 未结束则不重投
        if (!(cflags & IORING_CQE_F_MORE)) {
            LOGGER_WARN("Multishot accept stopped, resubmitting");
            submitMultishotAccept();
        }
        return;
    }

    // eventfd 唤醒：没有连接身份，靠操作类型就能认出来
    if (op == OpType::EVENT) {
        // 队列里存的是对连接的弱引用：拿到实指针才能恢复，拿不到说明连接已回收
        std::pair<std::coroutine_handle<>, std::weak_ptr<ConnCtx>> item;
        while (mPendingResumes.dequeue(item)) {
            auto& [h, weak] = item;
            auto ctx = weak.lock();
            if (!ctx) {
                LOGGER_WARN("Stale resume: connection already recycled");
                continue;
            }
            ctx->resumePending.store(false, std::memory_order_release);
            if (h && !h.done()) h.resume();
        }
        // 时间轮线程请求的空闲扫描：扫描要遍历连接表，只能在事件循环线程做
        if (mScanRequested.exchange(false, std::memory_order_acq_rel))
            scanIdle();
        submitEventFdRead();  // 重新提交，等下一次唤醒
        return;
    }

    // IORING_OP_ASYNC_CANCEL 自身的完成事件：res=0 表示确实取消了目标，
    // -ENOENT 表示目标已经自然完成、没赶上。两种情况都不需要做任何事——
    // 被取消的那条请求会带着它自己的标签再回一条 CQE，由下面那些分支处理。
    // 放在查表之前是有意的：这条 CQE 可能排在被取消请求的关闭流程之后才到，
    // 那时连接已经从表里摘掉了；它本身没有语义，不该被当成"陈旧 CQE"报出来
    if (op == OpType::CANCEL) {
        LOGGER_TRACE("cancel done id={} res={}", connId, res);
        return;
    }

    // 普通连接：按连接 id 回表。id 单调递增、永不复用，所以查不到就一定是陈旧事件。
    // 这一步只碰 Server 自己的哈希表，不读连接对象的任何字段——所以哪怕连接早已
    // 回收、内存都还给了 allocator，也只是"查表不匹配"而已，不会踩进已释放的内存
    auto it = mConnections.find(connId);
    if (it == mConnections.end()) {
        LOGGER_WARN("Stale CQE: conn id={} op={} already recycled, dropped",
                    connId, static_cast<int>(op));
        return;
    }
    std::shared_ptr<ConnCtx> conn = it->second;   // 本函数返回前它不会被析构

    // IO 完成事件到了，本连接不再是"空闲等待"状态。
    // CANCEL 自身的完成事件也会走到这里：它顺手清掉 waiting 无害——被取消的
    // 那条请求紧接着会回一条属于它自己的 CQE，把连接重新推进到正常流程里
    conn->waiting.store(false, std::memory_order_release);

    // 按标签里的操作类型分发。这个类型是**提交时**写进 user_data 的，
    // 不受 status 被谁改过影响，所以一条在途 recv 的完成事件永远不会
    // 因为连接已被决定关闭而被误当成 close 完成来处理
    switch (op) {
        case OpType::READ: {
            // 池里一块空闲 buffer 都没有：multishot recv 就此结束（这条 CQE 不带
            // F_MORE），这次也没挑到 buffer、池里一块都没少。不能就这么重新武装——
            // 池还是空的，会立刻再失败，把事件循环空转成 100% CPU。挂进等待队列，
            // 等谁归还了 buffer 再由 resumeStarved 重新武装；协程继续原地等数据，
            // 不必惊动它
            if (res == -ENOBUFS) {
                conn->waitingBuffer = true;
                conn->recvArmed     = false;   // multishot 就此结束，等重新武装
                // 这一次完成事件没有带回数据，连接其实还在等（等池里的 buffer），
                // 把等待标志放回去，让空闲超时照旧管着它
                conn->waiting.store(true, std::memory_order_release);
                LOGGER_TRACE("recv -ENOBUFS fd={} id={}, queued for buffer", conn->fd, conn->id);
                mBufStarved.push_back(connId);
                break;
            }

            // 数据先搬进连接的解析器，再立刻把这块 buffer 还回池子。
            // 顺序不能反：还回去之后内核马上可能把它交给别的连接，而解析器要跨
            // 多次 recv 累积，不能依赖池里那块内存。buffer id 在 CQE flags 高 16 位
            if (cflags & IORING_CQE_F_BUFFER) {
                const uint16_t bid = static_cast<uint16_t>(cflags >> IORING_CQE_BUFFER_SHIFT);
                if (res > 0) {
                    conn->parser.feed(static_cast<const char*>(mBufPool)
                                      + static_cast<size_t>(bid) * kBufSize,
                                      static_cast<size_t>(res));
                }
                putBackReadBuffer(bid);
            }

            // 入队这次读的结果。累积超限的话，这条连接已经不可能被解析出完整请求了，
            // 换成一个"请求太大"的结束码，让协程去回 413
            conn->pendingReads.push_back(conn->parser.overflowed() ? kReadTooLarge
                                                                   : static_cast<int32_t>(res));

            // 只有协程确实挂在"等数据"上才唤醒它。否则它正忙（比如挂在 send 上等
            // 写完），这一条只入队，等它自己回到循环来取——不在错误的状态下 resume。
            // 单线程下 exchange 不是必需的，但用它可以顺带报告"这次有没有真的唤醒人"
            if (conn->awaitingRead.exchange(false, std::memory_order_acq_rel)) {
                if (!conn->handle || conn->handle.done()) {
                    // 没有协程来收尾了（正常路径不该出现：能挂在这里就说明协程活着）
                    LOGGER_WARN("No coroutine to wake fd={} id={}, closing", conn->fd, conn->id);
                    submitClose(conn->fd, *conn);
                } else {
                    conn->handle.resume();
                }
            }

            // 没有 F_MORE 说明 multishot 结束了。原因只有三种，上面都已经落定：
            // -ENOBUFS 挂进了等待队列等重新武装、EOF 和错误则已经把负值/0 入了队，
            // 协程取到会自己走关闭流程。这里只补一件事：把"有 recv 在途"的记号清掉，
            // 并看看是不是有人在等这个时机才肯 close
            if (!(cflags & IORING_CQE_F_MORE)) {
                LOGGER_TRACE("recv multishot ended fd={} id={} res={}", conn->fd, conn->id, res);
                conn->recvArmed = false;
                if (conn->closePending) {
                    // 关闭流程的第二步：这条 recv 已经结束、socket 引用放下了，
                    // 现在提交 close 才能真正关掉 socket 并把 FIN 发出去
                    conn->closePending = false;
                    LOGGER_TRACE("recv drained, submitting deferred close fd={} id={}",
                                 conn->fd, conn->id);
                    submitCloseNow(conn->fd, *conn);
                }
            }
            break;
        }
        case OpType::WRITE: {
            if (res < 0) {
                LOGGER_ERROR("Write failed fd={} err={}", conn->fd, res);
                submitClose(conn->fd, *conn);
                break;
            }
            conn->write_offset += static_cast<size_t>(res);
            if (conn->write_offset < conn->writeBuffer.size()) {
                LOGGER_INF("Partial write fd={} {}/{}",
                           conn->fd, conn->write_offset, conn->writeBuffer.size());
                submitWrite(conn->fd, conn->writeBuffer, *conn);
                break;
            }
            conn->write_offset = 0;
            if (!conn->handle || conn->handle.done()) {
                LOGGER_ERROR("Invalid handle in WRITE fd={}", conn->fd);
                submitClose(conn->fd, *conn);
                break;
            }
            conn->handle.resume();
            break;
        }
        case OpType::CLOSE: {
            LOGGER_INF("Connection closed fd={} id={} err={}", conn->fd, conn->id, res);
            if (res < 0) {
                // close 失败或被取消：fd 还没真正关掉，这里补一刀，别漏 fd
                LOGGER_WARN("close did not take effect fd={} err={}, falling back to ::close",
                            conn->fd, res);
                ::close(conn->fd);
            }
            // 刻意不 resume 协程：CLOSE 是这条连接的终点。协程要么自己正挂在这条
            // close 上等收尾（走到协程尾部时的 co_await），要么是被代发 close 打断
            // 了在途 IO（READ/WRITE 分支发现对端已断时）。两种情况都该结束，
            // 而不是被唤醒后拿着一个语义对不上的 res 继续跑、再提交第二次 close
            // （那条多余的 close 会打在可能已被内核复用给新连接的 fd 号上）。
            // 协程帧随 ConnCtx 析构时由 task 一并销毁，帧内对象的析构照常发生
            //
            // 这里不再需要兜底归还 buffer：读方向的 buffer 在 handleCqe 里就 feed 完
            // 并还掉了，生命周期不跨出那一段，连接上根本没有"占着没还"的凭证
            unregisterConn(connId);
            break;
        }
        case OpType::CANCEL:
            // CANCEL 的完成事件在上面就 return 了，这里到不了；
            // 留这个分支只是让 switch 覆盖完 OpType 的全部取值（否则 -Wswitch 报警）
            break;
        default:
            LOGGER_ERROR("Unknown OpType fd={} id={}", conn->fd, conn->id);
            break;
    }
}

Server::Server() : mSockFd(-1) {
    mEventFd = eventfd(0,EFD_CLOEXEC);
    LOGGER_INF("mEventFd={}", mEventFd);
    scheduleScan();   // 启动周期性的空闲连接扫描
}

// 登记一条新连接：给它发一个全局唯一、单调递增的 id，然后放进连接表。
// id 永不复用，所以"上一代连接"的陈旧 CQE 回来后根本查不到条目，
// 会被当成无关事件丢掉——这就是连接身份的全部依据
uint64_t Server::registerConn(const std::shared_ptr<ConnCtx>& conn) {
    const uint64_t id = mNextConnId++;
    conn->id = id;
    mConnections.emplace(id, conn);
    return id;
}

// 从连接表摘除。连接对象此刻可能还没析构（调用方手上那份 shared_ptr 就是
// 最后一个强引用），那没关系：它的 id 已经不在表里，新的 CQE 不会再来找它
void Server::unregisterConn(uint64_t id) {
    mConnections.erase(id);
}

// 向时间轮注册下一次空闲扫描，任务执行时再注册下一次，形成每秒一次的心跳。
// 时间轮任务跑在它自己的线程上，所以这里只置标志 + 写 eventfd，
// 真正的连接表遍历交给事件循环线程（见 handleCqe 的 EVENT 分支）
void Server::scheduleScan() {
    mTimeWheel.add_task([this] {
        mScanRequested.store(true, std::memory_order_release);
        uint64_t v = 1;
        ssize_t n = write(mEventFd, &v, sizeof(v));
        (void)n;   // 唤醒失败无所谓：下一轮扫描照旧
        scheduleScan();
    }, {0, 0, 1, 0});   // 1 秒后
}

// 回收空闲超时的连接（只在事件循环线程执行，可安全遍历连接表）
void Server::scanIdle() {
    if (mConnections.empty()) return;

    const int64_t now = nowMs();
    // 快路径：水位说"没有任何连接在等待"，或者"最老的那个都还没逼近超时边缘"，
    // 这一轮一个连接都不用碰。这才是空闲扫描的常态——全表遍历只在真有连接
    // 需要回收时才发生
    if (mOldestWaitSince == INT64_MAX) return;
    if (now - mOldestWaitSince < kIdleTimeoutMs) return;

    int64_t oldest = INT64_MAX;        // 本轮重算出的水位
    std::vector<std::shared_ptr<ConnCtx>> expired;

    for (auto& [connId, conn] : mConnections) {
        (void)connId;                  // 连接 id 只做 key，取用时用 conn->id
        if (!conn->waiting.load(std::memory_order_acquire)) continue;
        const int64_t since = conn->waitingSince.load(std::memory_order_relaxed);
        // 水位要把**所有**还在等待的连接都算上（包括下面被跳过的那些），
        // 否则回写的水位会偏大，把该扫的轮次误判成"还早"而跳过
        if (since < oldest) oldest = since;
        // 已经在正常关闭流程里（协程尾部正在等 close 完成）的连接不插手：
        // 取消掉那条 close 反而会让 fd 关不掉
        if (conn->status == OpType::CLOSE) continue;
        if (now - since < kIdleTimeoutMs) continue;
        // 先清标志，避免下一轮又把它挑出来
        conn->waiting.store(false, std::memory_order_release);
        expired.push_back(conn);
    }
    // 回写真实水位。刚被挑出来的那些也计了进去（这一轮它们的起点确实最老），
    // 所以下一轮可能多扫一次——多扫是保守的，不会漏掉该回收的连接
    mOldestWaitSince = oldest;

    for (auto& conn : expired) {
        // 挂"等池里腾出 buffer"上的连接身上没有在途请求：它上一条 recv 以
        // -ENOBUFS 结束，池里一块都没少，ASYNC_CANCEL 也就没有东西可取消。
        // 直接塞一个取消码把它从读写循环里叫醒，让它走自己的关闭流程——
        // 否则它会一直躺在等待队列里，永远等不到那条本该唤醒它的 CQE
        if (conn->waitingBuffer) {
            conn->waitingBuffer = false;
            LOGGER_INF("Connection idle timeout while waiting for buffer, waking fd={} id={}",
                       conn->fd, conn->id);
            // 统一走队列：把取消码送进去再唤醒，协程从那儿按正常路径收尾。
            // 这类连接身上没有在途请求，ASYNC_CANCEL 没有东西可取消（它上一条 recv
            // 以 -ENOBUFS 结束，池里一块都没少），所以只能这么叫醒它
            conn->pendingReads.push_back(-ECANCELED);
            if (conn->handle && !conn->handle.done()) {
                conn->handle.resume();
            } else {
                // 协程已经没了（不该发生：它一直挂在 recv 上）：没人来走关闭流程，
                // 那就代发一条 close，别让连接因为"标志已清"而永远留在表里
                submitClose(conn->fd, *conn);
            }
            continue;
        }
        LOGGER_INF("Connection idle timeout, cancelling in-flight IO fd={} id={}",
                   conn->fd, conn->id);
        // 用 IORING_OP_ASYNC_CANCEL 精确取消这条连接上那条在途的 recv/send，
        // 而不是 shutdown(fd) 之后再插一条 close。三个理由：
        //
        //  1. 被取消的请求会以 -ECANCELED 正常完成，于是协程照旧从 READ/WRITE
        //     分支走完它自己的关闭流程（协程尾部提交 close），全程只有一条在途
        //     请求，也就不会出现"同一 fd 上两条 close"——第二条会打在可能已被
        //     内核复用给新连接的 fd 号上，把别人的连接关掉；
        //  2. 取消之后那条 recv 不再持有 socket 引用，随后的 close 才能真正
        //     关掉 socket 并把 FIN 发出去（否则 fd 被摘掉而 socket 还活着，
        //     客户端永远等不到断开，这正是 shutdown 版本要绕开的问题）；
        //  3. 取消本身也是一条 SQE，走同一套提交/完成路径，不需要在事件循环里
        //     夹一个同步的 shutdown 系统调用。
        //
        // 全程不改 status：status 只表达"协程下一步想提交什么 IO"，
        // 谁是"正在关闭"由 CLOSE 这条 CQE 自己说清楚
        if (!cancelInFlight(*conn, conn->status)) {
            // 提交队列实在挤不出位置：把等待标志放回去，下一轮扫描再试
            conn->waiting.store(true, std::memory_order_release);
        }
    }
}

// 把一个等待起点压进水位。水位只会被压得更小，而偏小是安全的：扫描最多多跑
// 几次；反过来，每次真正扫描时都会把它重算成当时的最小值，所以不会偏大漏扫
void Server::noteWaitStart(int64_t since) {
    if (since < mOldestWaitSince) mOldestWaitSince = since;
}

// 取消某条连接上那条在途请求，按 (操作类型, 连接 id) 精确匹配。
// cancel 自己的完成事件用 (CANCEL, id) 作标签，在 handleCqe 里被直接放过
bool Server::cancelInFlight(const ConnCtx& connCtx, OpType op) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("cancelInFlight: SQ full, fd={} id={}", connCtx.fd, connCtx.id);
        return false;
    }
    io_uring_prep_cancel64(sqe, packTag(op, connCtx.id), 0);
    io_uring_sqe_set_data64(sqe, packTag(OpType::CANCEL, connCtx.id));
    return true;
}
Server::~Server() {
    // 先注销缓冲环、再拆 ring：io_uring_queue_exit 之后就没法再跟内核谈注册了
    freeBufRing();
    io_uring_queue_exit(&ring);
}

// ── provided buffer ring：缓冲池 ──────────────────────────────
// 把一整池 buffer 注册成一个环交给内核，此后 recv 只说"从哪个池里挑"，由内核
// 自己选一块空闲的填进去。好处有两层：
//  1. 内存不再按连接数预分配。以前每条连接构造时就占一块 64KB 读缓冲，哪怕它
//     一个字节都没发过；现在池是共享的，池子里的块只在"真有 recv 在跑"的时候
//     才被内核挑走，解析完立刻还回来，所以占用跟着**实际在收数据的连接**走；
//  2. 只预留虚拟地址，没人写到的页不落物理内存（匿名页按需分配），于是
//     kBufCount × kBufSize 这个"名义容量"不等于真实占用。
// bid 直接取数组下标，于是归还时由 id 就能算出地址，不必再维护 id → 地址的表
int Server::setupBufRing() {
    mBufPoolBytes = static_cast<size_t>(kBufCount) * kBufSize;

    // MAP_NORESERVE：这是一整块预留，明确告诉内核不用为它留 swap/overcommit 承诺
    void* pool = mmap(nullptr, mBufPoolBytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pool == MAP_FAILED) {
        LOGGER_ERROR("bufpool: mmap {} bytes failed: {}", mBufPoolBytes, errno);
        mBufPool = nullptr;
        return -1;
    }
    mBufPool = pool;

    int err = 0;
    mBufRing = io_uring_setup_buf_ring(&ring, kBufCount, kBufGroup, 0, &err);
    if (!mBufRing) {
        LOGGER_ERROR("bufpool: io_uring_setup_buf_ring(entries={}, bgid={}) failed: {}",
                     kBufCount, kBufGroup, err);
        munmap(mBufPool, mBufPoolBytes);
        mBufPool = nullptr;
        return -1;
    }

    // 起步时把整池都挂进环里：环里有多少块，内核就能同时喂多少条 recv
    for (unsigned i = 0; i < kBufCount; ++i) {
        io_uring_buf_ring_add(mBufRing,
                              static_cast<char*>(mBufPool) + static_cast<size_t>(i) * kBufSize,
                              static_cast<unsigned>(kBufSize),
                              static_cast<uint16_t>(i),
                              kBufRingMask, static_cast<int>(i));
    }
    io_uring_buf_ring_advance(mBufRing, static_cast<int>(kBufCount));

    LOGGER_INF("bufpool: {} buffers x {}B registered (bgid={}), 虚拟 {} MB 只做预留",
               kBufCount, kBufSize, kBufGroup, mBufPoolBytes >> 20);
    return 0;
}

void Server::freeBufRing() {
    if (mBufRing) {
        io_uring_free_buf_ring(&ring, mBufRing, kBufCount, kBufGroup);
        mBufRing = nullptr;
    }
    if (mBufPool) {
        munmap(mBufPool, mBufPoolBytes);
        mBufPool = nullptr;
    }
}

// 把一块 provided buffer 还给池子。凭证是完成事件带回来的 buffer id，
// 调用点必须保证数据已经离开这块内存（feed 进解析器时拷了一份）——归还之后
// 内核立刻就可能把它交给别的连接，晚一步就是数据被盖掉
void Server::putBackReadBuffer(uint16_t bid) {
    if (bid == kNoBufId) return;                 // 这次没占着（比如 -ENOBUFS）

    io_uring_buf_ring_add(mBufRing,
                          static_cast<char*>(mBufPool) + static_cast<size_t>(bid) * kBufSize,
                          static_cast<unsigned>(kBufSize), bid, kBufRingMask, 0);
    io_uring_buf_ring_advance(mBufRing, 1);

    // 刚腾出一块，正好喂给一个在等的连接（一块 buffer 只够喂一条 recv）
    resumeStarved();
}

// 池里腾出了一块 buffer：从等待队列里挑一个连接，把它那条 recv 重新提交。
// 一对一——多出来的等待者等下一次归还。没有 buffer 就继续挂着，比直接关掉
// 连接温和：池子是暂时满，不是这条连接有什么错
void Server::resumeStarved() {
    while (!mBufStarved.empty()) {
        const uint64_t starvedId = mBufStarved.front();
        mBufStarved.pop_front();

        auto it = mConnections.find(starvedId);
        if (it == mConnections.end()) continue;   // 连接已经回收，等待资格作废
        ConnCtx& conn = *it->second;
        // 只重投"确实还挂在这次读上"的：等待标志还在、协程仍停在那条 recv 上。
        // 标志被 scanIdle 清掉（这连接已经超时被叫醒去关闭流程了）就不再重投
        if (!conn.waitingBuffer || conn.status != OpType::READ) continue;
        if (!conn.handle || conn.handle.done()) continue;

        conn.waitingBuffer = false;
        LOGGER_TRACE("bufpool: refeed recv fd={} id={}", conn.fd, conn.id);
        submitRecv(conn.fd, conn);
        return;
    }
}

void Server::Get(std::string pattern, Router::Handler handler) {
    LOGGER_INF("Register GET handler for: {}", pattern);
    mRouter.Get(std::move(pattern), std::move(handler));
}
void Server::Post(std::string pattern, Router::Handler handler) {
    LOGGER_INF("Register POST handler for: {}", pattern);
    mRouter.Post(std::move(pattern), std::move(handler));
}
void Server::GetAsync(std::string pattern, Router::Handler handler) {
    LOGGER_INF("Register ASYNC GET handler for: {}", pattern);
    mRouter.GetAsync(std::move(pattern), std::move(handler));
}
void Server::PostAsync(std::string pattern, Router::Handler handler) {
    LOGGER_INF("Register ASYNC POST handler for: {}", pattern);
    mRouter.PostAsync(std::move(pattern), std::move(handler));
}

int Server::submitMultishotAccept() {
    struct io_uring_sqe *sqe = acquireSqe(ring);
    if (!sqe) {
        return -1;
    }
    // 准备 multishot accept - 一次提交，持续接受新连接
    // 每次有新连接时自动产生 CQE，并自动重新武装
    io_uring_prep_multishot_accept(sqe, mSockFd, NULL, NULL, 0);
    // accept 不属于任何连接，标签里只填操作类型
    io_uring_sqe_set_data64(sqe, packTag(OpType::ACCEPT, 0));
    return 0;
}

int Server::submitAccept() {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitAccept: SQ full");
        return -1;
    }
    io_uring_prep_accept(sqe, mSockFd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, packTag(OpType::ACCEPT, 0));
    return 0;
}

void Server::submitRecv(int fd, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitRecv: SQ full, closing fd={}", fd);
        submitClose(fd, connCtx);
        return;
    }
    // multishot recv：提交一次就一直在跑，之后每次有数据都自己产一条 CQE
    // （带 IORING_CQE_F_MORE），不需要逐次重投。它必须配 provided buffer 用——
    // buf=NULL + len=0 + IOSQE_BUFFER_SELECT 等于告诉内核"从 bgid 这个池里挑一块
    // 空闲的填进去"，挑中的那块以 buffer id 的形式随 CQE 回来（flags 高 16 位），
    // 那是归还它、把内存还给池子的唯一凭证
    io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = static_cast<uint16_t>(kBufGroup);
    // 记下"这条 fd 上有一条 recv 在途"：关闭前必须先让它结束（见 submitClose）
    connCtx.recvArmed = true;
    // 标签带操作类型：这条 recv 的完成事件以后只会走 READ 分支，
    // 跟这条连接此后是不是被决定关闭没有关系
    io_uring_sqe_set_data64(sqe, packTag(OpType::READ, connCtx.id));
}

void Server::submitWrite(int fd, std::string& msg, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitWrite: SQ full, closing fd={}", fd);
        submitClose(fd, connCtx);
        return;
    }
    // 从上次写到的位置继续，支持短写续传
    const char* ptr  = msg.c_str() + connCtx.write_offset;
    size_t      left = msg.size()  - connCtx.write_offset;

    io_uring_prep_send(sqe, fd, ptr, left, 0);
    io_uring_sqe_set_data64(sqe, packTag(OpType::WRITE, connCtx.id));
}

void Server::submitClose(int fd, ConnCtx& connCtx) {
    // 读方向还武装着一条 multishot recv，它是唯一握着 socket 引用的东西：
    // 此刻直接 close 只是把 fd 从表里摘掉，socket 还活着，FIN 根本发不出去，
    // 客户端会一直等（这正是 README 里 shutdown+close 那个坑的 multishot 版本，
    // 而且 multishot recv 会一直武装，所以每次关闭都会踩到）。
    //
    // 所以先把它取消掉，真正的 close 留到它自己的完成事件里做——那一刻请求已经
    // 结束、引用才真正释放（见 handleCqe 的 READ 分支里对 closePending 的处理）。
    // 顺序不能换成"cancel 完后紧接着 close"：cancel 的完成只说明目标被取消了，
    // 不等它自己的 CQE 回来，引用可能还没放下
    if (connCtx.recvArmed) {
        connCtx.recvArmed    = false;
        connCtx.closePending = true;
        if (!cancelInFlight(connCtx, OpType::READ)) {
            // 连取消都提交不出去（SQ 满）：只能退回同步 close 兜底，别把 fd 漏了
            LOGGER_WARN("submitClose: cannot cancel in-flight recv, force close fd={}", fd);
            connCtx.closePending = false;
            ::close(fd);
        }
        return;
    }
    submitCloseNow(fd, connCtx);
}

void Server::submitCloseNow(int fd, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitClose: SQ full, force close fd={}", fd);
        ::close(fd);
        return;
    }
    io_uring_prep_close(sqe, fd);
    // 不在这里改 connCtx.status：status 是"协程下一步想提交什么 IO"，
    // 被外部改写成 CLOSE 就会让协程恢复后误以为自己该走关闭分支。
    // "这条连接正在关闭"这件事由这条 close 的完成事件自己负责（带 CLOSE 标签）
    io_uring_sqe_set_data64(sqe, packTag(OpType::CLOSE, connCtx.id));
}

void Server::submitEventFdRead() {
    struct io_uring_sqe *sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitEventFdRead: SQ full");
        return;
    }
    // 用 io_uring 的 read 等待并"取走" eventfd 的计数。
    // eventfd 一旦被写过，计数就会一直保持非零（除非被 read 清掉），
    // 所以若只用 poll_add 监听可读、从不去读，poll 会永久就绪，
    // 事件循环就会空转一整个核；read 读到累计值并清零后，下一次才真正阻塞
    io_uring_prep_read(sqe, mEventFd, &mEventVal, sizeof(mEventVal), 0);
    io_uring_sqe_set_data64(sqe, packTag(OpType::EVENT, 0));
}

coro::Task<void> Server::clientConnect(std::weak_ptr<ConnCtx> connWeak) {
    HttpResponse httpResponse;

    // 本协程内部用裸指针：连接持有这个协程帧，所以只要本协程在跑，连接就一定活着
    // （回收连接只发生在协程挂起期间，而事件循环是单线程的，两者不会并行）。
    // 跨线程、跨生命周期的地方不能这么用，那些地方（线程池任务、待恢复队列）
    // 都各自用 weak_ptr 做了存活校验
    ConnCtx* connState = nullptr;
    {
        auto sp = connWeak.lock();
        if (!sp) co_return;              // 连接已回收
        connState = sp.get();
    }

    bool shouldClose = false;

    // 解析器挂在连接上（ConnCtx::parser）：字节搬运由 handleCqe 做（好让那块 buffer
    // 立刻还回池子），从里面切请求留在这里——因为切出来之后要路由、要执行 handler、
    // 要写响应，这些都得能挂起，而 handleCqe 是普通函数
    HttpParser& httpParser = connState->parser;

    while (!shouldClose) {
        // 队列空 → 停下：置"在等数据"标志后挂起。读方向是 multishot recv，
        // 一次武装后一直在跑，这里不需要提交任何 IO
        if (connState->pendingReads.empty()) {
            // status 摆回 READ：空闲超时要取消的就是这条 multishot recv，
            // cancelInFlight 按 (操作类型, 连接 id) 匹配，这里得对得上
            connState->status = OpType::READ;
            connState->awaitingRead.store(true, std::memory_order_release);
            // 置标志到真正挂起之间不会有 CQE 被处理（单线程），复查一次只是稳妥：
            // 万一队列里已经有东西了，就别挂起，直接接着消费
            if (connState->pendingReads.empty()) {
                co_await WaitDataAwaiter{this, connWeak};
            }
            connState->awaitingRead.store(false, std::memory_order_release);
            continue;
        }

        // 队列非空 → 继续：把已经落地的读结果全部消费掉
        while (!connState->pendingReads.empty() && !shouldClose) {
            const int32_t n = connState->pendingReads.front();
            connState->pendingReads.pop_front();

            if (n == kReadTooLarge) {
                // 累积超过 HttpParser::kMaxBufferSize：解析器已经不再收新数据，
                // 这条连接不可能再切出请求了，回一个 413 收尾
                LOGGER_WARN("Request too large fd={} limit={}", connState->fd,
                            HttpParser::kMaxBufferSize);
                httpResponse = HttpResponse();
                httpResponse.set_status(413)
                            .set_header("Connection", "close")
                            .set_body("Payload Too Large");
                connState->status      = OpType::WRITE;
                connState->writeBuffer = httpResponse.build();
                connState->handle      = nullptr;
                co_yield IouringAwaiter{this, connWeak};
                shouldClose = true;
                break;
            }

            if (n <= 0) {
                if (n == -ECANCELED) {
                    // 空闲超时扫描用 IORING_OP_ASYNC_CANCEL 取消了这条在途 recv：
                    // 属于正常回收路径，不是读错误
                    LOGGER_INF("Read cancelled by idle scan fd={}", connState->fd);
                } else if (n < 0) {
                    LOGGER_ERROR("Read error fd={} err={}", connState->fd, n);
                } else {
                    LOGGER_INF("Client closed fd={}", connState->fd);
                }
                shouldClose = true;
                break;
            }

            // 把解析器里已经完整的请求全部处理掉。一次 recv 可能带回多个请求
            // （粘包 / pipelining），只处理一个的话其余的会一直压到下一次 recv 才有机会
            bool malformed = false;
            try {
                while (auto optReq = httpParser.try_parse()) {
                    HttpRequest& req = *optReq;
                    httpResponse = HttpResponse();

                    // 路由匹配（精确 + 前缀）由 Router 负责，执行分两条路径：
                    //   同步快路径（默认）：handler 直接在事件循环线程执行，零调度开销
                    //   异步慢路径：Route::async 时经 QueryAwaiter 投全局线程池，防阻塞
                    bool hit = false;
                    Router::Route route;
                    if (mRouter.match(req, route)) {
                        if (route.async) {
                            HttpRequest  reqSnap = req;
                            auto resSnap = std::make_shared<HttpResponse>();
                            co_await QueryAwaiter{this, connWeak,
                                                  [handler = route.handler, reqSnap = std::move(reqSnap), resSnap]() mutable {
                                                      handler(reqSnap, *resSnap);
                                                  }
                            };
                            httpResponse = std::move(*resSnap);
                        } else {
                            route.handler(req, httpResponse);   // 同步快速路径
                        }
                        hit = true;
                    }

                    if (!hit) {
                        httpResponse.set_status(404).set_body("Not Found");
                    }

                    std::string connHdr = req.get_header_value("connection");
                    HttpUtils::to_lower(connHdr);
                    bool keepAlive = (connHdr != "close");
                    httpResponse.set_header("Connection", keepAlive ? "keep-alive" : "close");

                    connState->status      = OpType::WRITE;
                    connState->writeBuffer = httpResponse.build();
                    connState->handle      = nullptr;
                    co_yield IouringAwaiter{this, connWeak};

                    if (!keepAlive) {
                        shouldClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                // 畸形请求：HttpParser 对坏的请求行 / Content-Length / chunk size
                // 都会抛。以前这里没人接，异常直接结束协程——而"协程结束"不等于
                // "连接被关闭"：没人提交 close，fd 和连接表条目就这么漏下了。
                // 记下来，出去回一个 400 再走正常关闭流程
                // （co_yield 不能写在 catch 里，C++ 不允许 handler 中出现 await）
                LOGGER_WARN("Malformed request fd={}: {}", connState->fd, e.what());
                malformed = true;
            }

            if (malformed) {
                httpResponse = HttpResponse();
                httpResponse.set_status(400)
                            .set_header("Connection", "close")
                            .set_body("Bad Request");
                connState->status      = OpType::WRITE;
                connState->writeBuffer = httpResponse.build();
                connState->handle      = nullptr;
                co_yield IouringAwaiter{this, connWeak};
                shouldClose = true;
                break;
            }
        }
    }

    connState->status = OpType::CLOSE;
    connState->handle = nullptr;
    co_await IouringAwaiter{this, connWeak};
    co_return;
}
