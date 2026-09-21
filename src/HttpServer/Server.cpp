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
// 连接上下文：单连接的 IO 状态（读写缓冲、短写续传偏移、协程句柄）
//
// 读写缓冲直接放在这里（不再是指向协程栈的裸指针）：
// 地址属于连接对象自己，io_uring 提交出去的缓冲区不会随协程栈变动而悬垂。
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
    std::string readBuffer;                 // 读缓冲
    std::string writeBuffer;                // 写缓冲（响应内容）
    ssize_t      bytes_read   = 0;
    size_t       write_offset = 0;          // 短写续传偏移

    std::atomic<bool> resumePending{false};
    std::optional<coro::Task<void>> task;
    std::coroutine_handle<>         handle;

    // 空闲超时用：当前是否正等客户端（在读/在写），以及从什么时候开始等
    std::atomic<bool>    waiting{false};
    std::atomic<int64_t> waitingSince{0};

    ConnCtx(OpType status, int fd)
            : status(status), id(0), fd(fd), readBuffer(1 << 16, '\0'), task(std::nullopt) {}
};

// 连接空闲超时：服务器在等客户端（等读或等写）超过这个时长就把连接回收。
// 没有这道闸，一个连上却不发数据的客户端会永久占住 fd 和 64KB 读缓冲
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
// 缓冲直接取自连接自己（按 status 选读/写缓冲），不需要外部再传指针进来；
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
            case OpType::READ:
                server->submitRecv(conn->fd, conn->readBuffer, *conn);
                break;
            case OpType::WRITE:
                server->submitWrite(conn->fd, conn->writeBuffer, *conn);
                break;
            case OpType::CLOSE:
                server->submitClose(conn->fd, *conn);
                break;
            default:
                break;
        }
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
            // res < 0：读失败，或被 IORING_OP_ASYNC_CANCEL 取消（-ECANCELED）
            conn->bytes_read = res;
            if (!conn->handle || conn->handle.done()) {
                submitClose(conn->fd, *conn);
                break;
            }
            conn->handle.resume();
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
        if (!cancelInFlight(*conn)) {
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

// 取消某条连接上那条在途请求（按 user_data 精确匹配，即按 (操作类型, 连接 id)）。
// cancel 自己的完成事件用 (CANCEL, id) 作标签，在 handleCqe 里被直接放过
bool Server::cancelInFlight(const ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("cancelInFlight: SQ full, fd={} id={}", connCtx.fd, connCtx.id);
        return false;
    }
    io_uring_prep_cancel64(sqe, packTag(connCtx.status, connCtx.id), 0);
    io_uring_sqe_set_data64(sqe, packTag(OpType::CANCEL, connCtx.id));
    return true;
}
Server::~Server() {
    io_uring_queue_exit(&ring);
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

void Server::submitRecv(int fd, std::string& buffer, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = acquireSqe(ring);
    if (!sqe) {
        LOGGER_ERROR("submitRecv: SQ full, closing fd={}", fd);
        submitClose(fd, connCtx);
        return;
    }
    io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), 0);
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
    HttpParser   httpParser;
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

    while (!shouldClose) {
        connState->status     = OpType::READ;
        connState->bytes_read = 0;
        connState->handle     = nullptr;
        co_await IouringAwaiter{this, connWeak};

        ssize_t n = connState->bytes_read;
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
            break;
        }

        httpParser.feed(connState->readBuffer.data(), n);

        if (auto optReq = httpParser.try_parse()) {
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
                }
        } // if 结束
    }

    connState->status = OpType::CLOSE;
    connState->handle = nullptr;
    co_await IouringAwaiter{this, connWeak};
    co_return;
}
