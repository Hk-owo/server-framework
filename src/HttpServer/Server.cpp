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
// 所有权：mConnections 里的 shared_ptr 是唯一强引用，谁都不该再持一份强引用
// （连接持有自己的协程帧，协程帧再持连接就成了环，永远释放不掉），
// 所以程序内部一律用 weak_ptr 引用它，用到的当下 lock() 出来。
// 交给 io_uring 的 user_data 只能存64位整数，那儿保持原来的指针，不动。
struct Server::ConnCtx {
    OpType  status;
    int     fd;
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
            : status(status), fd(fd), readBuffer(1 << 16, '\0'), task(std::nullopt) {}
};

// 连接空闲超时：服务器在等客户端（等读或等写）超过这个时长就把连接回收。
// 没有这道闸，一个连上却不发数据的客户端会永久占住 fd 和 64KB 读缓冲
static constexpr int64_t kIdleTimeoutMs = 5000;

static int64_t nowMs() {
    return chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now().time_since_epoch()).count();
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
        // 记下"从此刻起在等客户端"，时间轮的空闲扫描据此判定超时
        conn->waitingSince.store(nowMs(), std::memory_order_relaxed);
        conn->waiting.store(true, std::memory_order_release);
        LOGGER_INF("await_suspend: status={} fd={} handle={}",
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
                        LOGGER_INF("write eventfd ret={} errno={}", ret, errno);
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
    mConnCtx->fd = mSockFd;
    LOGGER_INF("Server listening on {}:{}", bind, port);
    return 0;
}

void Server::run(unsigned entries, unsigned flags) {
    int ret = io_uring_queue_init(entries, &ring, flags);
    if (ret < 0) {
        LOGGER_ERROR("io_uring_queue_init failed: {}", ret);
        exit(1);
    }

    submitMultishotAccept();
    submitEventFdRead();

    struct io_uring_cqe *cqe = nullptr;

    while (true) {
        unsigned head;
        unsigned processed = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            ConnCtx* connCtx = reinterpret_cast<ConnCtx*>(
                    io_uring_cqe_get_data64(cqe));
            int      res   = cqe->res;
            unsigned cflags = cqe->flags;
            processed++;

            if (!connCtx) {
                LOGGER_ERROR("CQE with null connCtx, skipping");
                continue;
            }

            handleCqe(connCtx, res, cflags);   // 把 switch 抽成独立函数，下面说
        }
        io_uring_cq_advance(&ring, processed); // 批量标记消费，替代逐个 cqe_seen

        if (processed == 0) {
            struct __kernel_timespec ts { .tv_sec = 0, .tv_nsec = 1'000'000 }; // 1ms
            ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            // ETIME = 超时正常返回，继续循环推进时间轮
            // EINTR = 信号打断，继续
            if (ret < 0 && ret != -ETIME && ret != -EINTR) {
                LOGGER_ERROR("io_uring_wait_cqe_timeout: {}", ret);
            }
        }
    }
}

void Server::handleCqe(ConnCtx* connCtx, int res, unsigned cflags) {
    if (connCtx->fd == mSockFd) {
        if (res >= 0) {
            int newFd = res;
            LOGGER_INF("New connection fd={}", newFd);
            auto ctx = std::make_shared<ConnCtx>(OpType::READ, newFd);
            // 协程只持弱引用，避免"连接持有协程帧、协程帧又持有连接"的环
            std::weak_ptr<ConnCtx> weak = ctx;
            ctx->task.emplace(clientConnect(weak));
            ctx->handle = ctx->task.value().raw_handle();
            mConnections[newFd] = ctx;
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

    // eventfd 唤醒：mEventCtx 由 Server 长期持有，不进连接表
    if (connCtx->status == OpType::EVENT) {
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

    // 普通连接：先回表取一份 shared_ptr 保活，本函数返回前它不会被析构。
    // 这里必须用指针值确认身份——fd 会被内核立刻复用，只按 fd 查可能查到
    // 刚接手同一个 fd 号的新连接（那正是"误删新连接"的根源）
    std::shared_ptr<ConnCtx> conn;
    auto it = mConnections.find(connCtx->fd);
    if (it != mConnections.end() && it->second.get() == connCtx)
        conn = it->second;
    if (!conn) {
        // 连接已回收，或这个 fd 号已经归了新连接：陈旧事件，丢弃
        LOGGER_WARN("Stale CQE for fd={}, dropped", connCtx->fd);
        return;
    }

    // IO 完成事件到了，本连接不再是"空闲等待"状态
    conn->waiting.store(false, std::memory_order_release);

    switch (conn->status) {
        case OpType::READ: {
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
            LOGGER_INF("Connection closed fd={}", conn->fd);
            if (conn->handle && !conn->handle.done())
                conn->handle.resume();
            // 连接由本函数持有的 shared_ptr 保活，直接从表里摘掉即可，
            // 不需要延迟到批末：之后再指向它的陈旧CQE会因回表校验失败被丢弃。
            // 按迭代器删而不是按 fd 删，避免误删刚复用同一 fd 号的新连接
            mConnections.erase(it);
            break;
        }
        default:
            LOGGER_ERROR("Unknown OpType fd={}", conn->fd);
            break;
    }
}

Server::Server() : mSockFd(-1) {
    mConnCtx = std::make_shared<ConnCtx>(OpType::ACCEPT, -1);
    mEventFd = eventfd(0,EFD_CLOEXEC);
    mEventCtx = std::make_shared<ConnCtx>(OpType::EVENT, mEventFd);
    LOGGER_INF("mEventFd={} mEventCtx->fd={}", mEventFd, mEventCtx->fd);
    scheduleScan();   // 启动周期性的空闲连接扫描
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
    std::vector<std::shared_ptr<ConnCtx>> expired;

    for (auto& [fd, conn] : mConnections) {
        if (!conn->waiting.load(std::memory_order_acquire)) continue;
        if (now - conn->waitingSince.load(std::memory_order_relaxed) < kIdleTimeoutMs) continue;
        // 先清标志，避免下一轮又把它挑出来
        conn->waiting.store(false, std::memory_order_release);
        expired.push_back(conn);
    }

    for (auto& conn : expired) {
        LOGGER_INF("Connection idle timeout, closing fd={}", conn->fd);
        // 必须先 shutdown：此刻连接上还挂着未完成的 recv，它持有 socket 引用，
        // 会让 io_uring 的 close 虽然成功返回、socket 却不真正闭合（FIN 发不出去，
        // 客户端永远等不到断开，而 recv 也永远等不到数据——死锁）。
        // shutdown 会立刻发出 FIN 并让那个 recv 以 EOF 返回，之后 close 才能生效
        ::shutdown(conn->fd, SHUT_RDWR);
        submitClose(conn->fd, *conn);
    }
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
    struct io_uring_sqe *sqe;
    // 获取 SQE
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        // 如果 SQ 满了，先提交再重试
        io_uring_submit(&ring);
        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            return -1;
        }
    }
    // 准备 multishot accept - 一次提交，持续接受新连接
    // 每次有新连接时自动产生 CQE，并自动重新武装
    io_uring_prep_multishot_accept(sqe, mSockFd, NULL, NULL, 0);
    // 设置用户数据，用于在 CQE 中识别这是 accept 操作
    io_uring_sqe_set_data64(sqe, (uint64_t)mConnCtx.get());
    // 提交到内核
    return io_uring_submit(&ring);
}

int Server::submitAccept() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitAccept: SQ full");
        return -1;
    }
    io_uring_prep_accept(sqe, mSockFd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)mConnCtx.get());
    return io_uring_submit(&ring);
}

void Server::submitRecv(int fd, std::string& buffer, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitRecv: SQ full, closing fd={}", fd);
        submitClose(fd, connCtx);
        return;
    }
    io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), 0);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&connCtx));
    io_uring_submit(&ring);
}

void Server::submitWrite(int fd, std::string& msg, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitWrite: SQ full, closing fd={}", fd);
        submitClose(fd, connCtx);
        return;
    }
    // 从上次写到的位置继续，支持短写续传
    const char* ptr  = msg.c_str() + connCtx.write_offset;
    size_t      left = msg.size()  - connCtx.write_offset;

    io_uring_prep_send(sqe, fd, ptr, left, 0);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&connCtx));
    io_uring_submit(&ring);
}

void Server::submitClose(int fd, ConnCtx& connCtx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitClose: SQ full, force close fd={}", fd);
        ::close(fd);
        return;
    }
    connCtx.status = OpType::CLOSE;
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&connCtx));
    int ret = io_uring_submit(&ring);
    if (ret < 0) {
        LOGGER_ERROR("submitClose submit failed: {}", ret);
        ::close(fd);
    }
}

void Server::submitEventFdRead() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitEventFdRead: SQ full");
        return;
    }
    // 用 io_uring 的 read 等待并"取走" eventfd 的计数。
    // eventfd 一旦被写过，计数就会一直保持非零（除非被 read 清掉），
    // 所以若只用 poll_add 监听可读、从不去读，poll 会永久就绪，
    // 事件循环就会空转一整个核；read 读到累计值并清零后，下一次才真正阻塞
    io_uring_prep_read(sqe, mEventFd, &mEventVal, sizeof(mEventVal), 0);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(mEventCtx.get()));
    io_uring_submit(&ring);
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
            if (n < 0) LOGGER_ERROR("Read error fd={} err={}", connState->fd, n);
            else       LOGGER_INF("Client closed fd={}", connState->fd);
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
