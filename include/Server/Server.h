//
// Created by lacas on 2026/3/18.
//

#ifndef WEBPROJECT_SERVER_H
#define WEBPROJECT_SERVER_H

#include "HttpParser.h"
#include "HttpResponse.h"
#include "ThreadPool.h"
#include "TimeWheel.h"
#include "CoroTask.h"
#include "WaitQueue/MPSCQueue.h"

class Server{
private:
    enum class OpType{ACCEPT,READ,WRITE,CLOSE,EVENT};
    struct ConnCtx;
    struct IouringAwaiter;
    struct QueryAwaiter;
    std::shared_ptr<ConnCtx> mConnCtx,mEventCtx;
private:
    TimeWheelTop timeWheelTop;
    io_uring ring;
    int mSockFd,mEventFd;
    uint64_t mEventVal = 0;
    MPSCQueue<std::pair<std::coroutine_handle<>, ConnCtx*>> mPendingResumes;
    std::vector<int> mPendingClose;
    std::unordered_map<int,std::shared_ptr<ConnCtx>> mConnections;
    std::unordered_map<std::string, std::function<void(const HttpRequest& req, HttpResponse& res)>> mGetHandler;
    std::unordered_map<std::string, std::function<void(const HttpRequest& req, HttpResponse& res)>> mPostHandler;
public:
    int listen(const std::string& bind,const std::string& port);
    void run(unsigned entries = 4096, unsigned flags = 0);
    void Get(std::string pattern,std::function<void(const HttpRequest& req, HttpResponse& res)>);
    void Post(std::string pattern,std::function<void(const HttpRequest& req, HttpResponse& res)>);
    Server();
    ~Server();
    int submitMultishotAccept();
    coro::Task<void> clientConnect(int clifd,ConnCtx* connCtx);
    void submitRecv(int fd, std::string &buffer,ConnCtx& connCtx);
    void submitWrite(int fd, std::string &msg, ConnCtx &connCtx);
    void submitClose(int fd, ConnCtx &connCtx);
    int submitAccept();

    void submitEventFdRead();

    void handleCqe(ConnCtx *connCtx, int res, unsigned int cflags);
};

// ── ConnCtx ──────────────────────────────────────────────────
// 修复：构造函数第一个参数 status 之前被硬编码为 READ，现在正确使用传入值
struct Server::ConnCtx {
    OpType  status;
    int     fd;
    std::string* buffer       = nullptr;
    ssize_t      bytes_read   = 0;
    size_t       write_offset = 0;       // 短写续传偏移

    std::atomic<bool> resumePending{false};
    std::optional<coro::Task<void>> task;
    std::coroutine_handle<>         handle;

    ConnCtx(OpType status, int fd, std::string* buffer)
            : status(status), fd(fd), buffer(buffer), task(std::nullopt) {}
};

// ── IouringAwaiter ───────────────────────────────────────────
struct Server::IouringAwaiter {
    Server*                  server;
    std::coroutine_handle<>* handle_ptr;
    Server::ConnCtx*         connState;
    std::string*             buffer = nullptr;   // CLOSE 时为 nullptr，显式默认
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        *handle_ptr = h;
        LOGGER_INF("await_suspend: status={} fd={} handle={}",
                   (int)connState->status, connState->fd, (void*)h.address());

        switch (connState->status) {
            case OpType::READ:
                assert(buffer != nullptr);
                server->submitRecv(connState->fd, *buffer, *connState);
                break;
            case OpType::WRITE:
                assert(buffer != nullptr);
                server->submitWrite(connState->fd, *buffer, *connState);
                break;
            case OpType::CLOSE:
                server->submitClose(connState->fd, *connState);
                break;
            default:
                break;
        }
    }
    void await_resume() const noexcept {}
};

struct Server::QueryAwaiter{
    Server* server;
    ConnCtx* connState;
    std::function<void()> callback;
    bool await_ready() const noexcept { return false; }
    void await_resume() const noexcept {}
    void await_suspend(std::coroutine_handle<> h) {
        auto* ctx = connState; // ConnCtx*
        std::function<void()> task =
                [h, s = server, cb = std::move(callback), ctx] mutable {
                    try { cb(); } catch (...) {}
                    bool expected = false;
                    if (ctx->resumePending.compare_exchange_strong(expected, true,
                                                                   std::memory_order_acq_rel)) {
                        auto item = std::make_pair(h, ctx);
                        s->mPendingResumes.enqueue(item);
                        uint64_t v = 1;
                        int ret = write(s->mEventFd, &v, sizeof(v));
                        LOGGER_INF("write eventfd ret={} errno={}", ret, errno);
                    }
                };
        server->timeWheelTop.add_task(std::move(task), {0, 0, 0, 3});
    }
};
#endif //WEBPROJECT_SERVER_H
