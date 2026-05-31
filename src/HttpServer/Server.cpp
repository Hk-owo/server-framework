//
// Created by lacas on 2026/3/18.
//

#include "HtmlContent.h"
#include "Server/Server.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "Logger.h"
#include "liburing.h"

using namespace std;

int Server::listen(const std::string &bind, const std::string& port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(bind.c_str(), port.c_str(), &hints, &res) != 0) {
        LOGGER_ERROR("getaddrinfo failed for {}:{}", bind, port);
        return -1;
    }
    struct addrinfo *p = res;
    int fd = -1;
    bool bound = false;
    do {
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

    } while ((p = p->ai_next) != nullptr);
    freeaddrinfo(res);
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

        for (auto fd : mPendingClose)
            mConnections.erase(fd);   // shared_ptr 引用计数归零，ConnCtx 析构
        mPendingClose.clear();

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
            auto ctx = std::make_shared<ConnCtx>(OpType::READ, newFd, nullptr);
            ctx->task.emplace(clientConnect(newFd, ctx.get()));
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

    switch (connCtx->status) {
        case OpType::READ: {
            connCtx->bytes_read = res;
            if (!connCtx->handle || connCtx->handle.done()) {
                auto it = mConnections.find(connCtx->fd);
                if (it != mConnections.end())
                    submitClose(connCtx->fd, *it->second);
                break;
            }
            connCtx->handle.resume();
            break;
        }
        case OpType::WRITE: {
            if (res < 0) {
                LOGGER_ERROR("Write failed fd={} err={}", connCtx->fd, res);
                auto it = mConnections.find(connCtx->fd);
                if (it != mConnections.end())
                    submitClose(connCtx->fd, *it->second);
                break;
            }
            connCtx->write_offset += static_cast<size_t>(res);
            if (connCtx->buffer &&
                connCtx->write_offset < connCtx->buffer->size()) {
                LOGGER_INF("Partial write fd={} {}/{}",
                           connCtx->fd, connCtx->write_offset, connCtx->buffer->size());
                submitWrite(connCtx->fd, *connCtx->buffer, *connCtx);
                break;
            }
            connCtx->write_offset = 0;
            if (!connCtx->handle || connCtx->handle.done()) {
                LOGGER_ERROR("Invalid handle in WRITE fd={}", connCtx->fd);
                submitClose(connCtx->fd, *connCtx);
                break;
            }
            connCtx->handle.resume();
            break;
        }
        case OpType::CLOSE: {
            LOGGER_INF("Connection closed fd={}", connCtx->fd);
            if (connCtx->handle && !connCtx->handle.done())
                connCtx->handle.resume();
            mPendingClose.push_back(connCtx->fd);
            break;
        }
        case OpType::EVENT: {
            // 用 io_uring 提交的 read 来替代阻塞 ::read
            std::pair<std::coroutine_handle<>, ConnCtx*> item;
            while (mPendingResumes.dequeue(item)) {
                auto& [h, ctx] = item;
                // 验证连接仍然存活
                if (mConnections.find(ctx->fd) == mConnections.end()) {
                    LOGGER_WARN("Stale resume for fd={}, skipping", ctx->fd);
                    ctx->resumePending.store(false, std::memory_order_release);
                    continue;
                }
                ctx->resumePending.store(false, std::memory_order_release);
                if (h && !h.done()) h.resume();
            }
            submitEventFdRead();  // 重新提交，等下一次唤醒
            break;
        }
        default:
            LOGGER_ERROR("Unknown OpType fd={}", connCtx->fd);
            break;
    }
}

Server::Server() : mSockFd(-1) {
    mConnCtx = std::make_shared<ConnCtx>(OpType::ACCEPT, -1, nullptr);
    mEventFd = eventfd(0,EFD_CLOEXEC);
    mEventCtx = std::make_shared<ConnCtx>(OpType::EVENT, mEventFd, nullptr);
    LOGGER_INF("mEventFd={} mEventCtx->fd={}", mEventFd, mEventCtx->fd);
}
Server::~Server() {
    io_uring_queue_exit(&ring);
}
void Server::Get(std::string pattern, std::function<void(const HttpRequest& req, HttpResponse& res)> handler) {
    // 确保路径以/开头
    if (pattern.empty() || pattern[0] != '/') {
        pattern = "/" + pattern;
    }
    LOGGER_INF("Register GET handler for: {}", pattern);
    mGetHandler[std::move(pattern)] = std::move(handler);
}
void Server::Post(std::string pattern, std::function<void(const HttpRequest& req, HttpResponse& res)> handler) {
    // 确保路径以/开头
    if (pattern.empty() || pattern[0] != '/') {
        pattern = "/" + pattern;
    }

    LOGGER_INF("Register POST handler for: {}", pattern);
    mPostHandler[std::move(pattern)] = std::move(handler);
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
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        LOGGER_ERROR("submitEventFdRead: SQ full");
        return;
    }
    io_uring_prep_poll_add(sqe, mEventFd, POLLIN);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(mEventCtx.get()));
    io_uring_submit(&ring);
}

coro::Task<void> Server::clientConnect(int fd, ConnCtx* connState) {
    std::string  readBuffer(1 << 16, '\0');
    std::string  writeBuffer;
    HttpParser   httpParser;
    HttpResponse httpResponse;

    connState->buffer = &readBuffer;
    bool shouldClose = false;

    while (!shouldClose) {
        connState->status     = OpType::READ;
        connState->bytes_read = 0;
        connState->handle     = nullptr;
        co_await IouringAwaiter{this, &connState->handle, connState, &readBuffer};

        ssize_t n = connState->bytes_read;
        if (n <= 0) {
            if (n < 0) LOGGER_ERROR("Read error fd={} err={}", fd, n);
            else       LOGGER_INF("Client closed fd={}", fd);
            break;
        }

        httpParser.feed(readBuffer.data(), n);

        if (auto optReq = httpParser.try_parse()) {
                HttpRequest& req = *optReq;
                httpResponse = HttpResponse();

                bool hit = false;
                const auto& path = req.parsed_uri().path;

                if (req.method == "GET") {
                    if (auto it = mGetHandler.find(path); it != mGetHandler.end()) {
                        auto handler = it->second;
                        HttpRequest  reqSnap = req;
                        auto resSnap = std::make_shared<HttpResponse>();
                        co_await QueryAwaiter{this, connState,
                                              [handler, reqSnap = std::move(reqSnap), resSnap]() mutable {
                                                  handler(reqSnap, *resSnap);
                                              }
                        };
                        httpResponse = std::move(*resSnap);
                        hit = true;
                    } else {
                        for (auto& [pattern, handler] : mGetHandler) {
                            if (pattern.back() == '/' && path.rfind(pattern, 0) == 0) {
                                auto handlerCopy = handler;
                                HttpRequest  reqSnap = req;
                                auto resSnap = std::make_shared<HttpResponse>();
                                co_await QueryAwaiter{this, connState,
                                                      [handlerCopy, reqSnap = std::move(reqSnap), resSnap]() mutable {
                                                          handlerCopy(reqSnap, *resSnap);
                                                      }
                                };
                                httpResponse = std::move(*resSnap);
                                hit = true;
                                break;
                            }
                        }
                    }
                } else if (req.method == "POST") {
                    if (auto it = mPostHandler.find(path); it != mPostHandler.end()) {
                        auto handler = it->second;
                        HttpRequest  reqSnap = req;
                        auto resSnap = std::make_shared<HttpResponse>();
                        co_await QueryAwaiter{this, connState,
                                              [handler, reqSnap = std::move(reqSnap), resSnap]() mutable {
                                                  handler(reqSnap, *resSnap);
                                              }
                        };
                        httpResponse = std::move(*resSnap);
                        hit = true;
                    }
                }

                if (!hit) {
                    httpResponse.set_status(404).set_body("Not Found");
                }

                std::string connHdr = req.get_header_value("connection");
                HttpUtils::to_lower(connHdr);
                bool keepAlive = (connHdr != "close");
                httpResponse.set_header("Connection", keepAlive ? "keep-alive" : "close");

                connState->status  = OpType::WRITE;
                writeBuffer        = httpResponse.build();
                connState->buffer  = &writeBuffer;
                connState->handle  = nullptr;
                co_yield IouringAwaiter{this, &connState->handle, connState, &writeBuffer};

                connState->buffer = &readBuffer;

                if (!keepAlive) {
                    shouldClose = true;
                }
        } // if 结束
    }

    connState->status = OpType::CLOSE;
    connState->handle = nullptr;
    co_await IouringAwaiter{this, &connState->handle, connState};
    co_return;
}