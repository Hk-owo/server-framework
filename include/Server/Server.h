//
// Created by lacas on 2026/3/18.
//

#ifndef WEBPROJECT_SERVER_H
#define WEBPROJECT_SERVER_H

#include "Router.h"
#include "CoroTask.h"
#include "WaitQueue/MPSCQueue.h"
#include "TimeWheel.h"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "liburing.h"

class Server{
private:
    enum class OpType{ACCEPT,READ,WRITE,CLOSE,EVENT};
    struct ConnCtx;
    struct IouringAwaiter;
    struct QueryAwaiter;
    std::shared_ptr<ConnCtx> mConnCtx,mEventCtx;
private:
    io_uring ring;
    int mSockFd,mEventFd;
    uint64_t mEventVal = 0;
    // 待恢复的协程：只弱引用连接。不换的话工作线程会拿着可能已被回收的连接去恢复协程
    MPSCQueue<std::pair<std::coroutine_handle<>, std::weak_ptr<ConnCtx>>> mPendingResumes;
    std::unordered_map<int,std::shared_ptr<ConnCtx>> mConnections;
    Router mRouter;
    // 四级级联时间轮：用来驱动连接的空闲超时（见 scanIdle）
    TimeWheelTop mTimeWheel;
    // 时间轮线程跨线程只置这个标志 + 写 eventfd，真正的扫描在事件循环线程做
    std::atomic<bool> mScanRequested{false};

private:
    // 扫描并回收空闲超时的连接（只在事件循环线程调用）
    void scanIdle();
    // 向时间轮注册下一次空闲扫描（每秒一次，自我重注册）
    void scheduleScan();

public:
    int listen(const std::string& bind,const std::string& port);
    void run(unsigned entries = 4096, unsigned flags = 0);
    void Get(std::string pattern, Router::Handler handler);
    void Post(std::string pattern, Router::Handler handler);
    // 异步 handler：投递全局线程池执行（慢任务用）；默认 Get/Post 为同步快速路径
    void GetAsync(std::string pattern, Router::Handler handler);
    void PostAsync(std::string pattern, Router::Handler handler);
    Server();
    ~Server();
    int submitMultishotAccept();
    coro::Task<void> clientConnect(std::weak_ptr<ConnCtx> connWeak);
    void submitRecv(int fd, std::string &buffer,ConnCtx& connCtx);
    void submitWrite(int fd, std::string &msg, ConnCtx &connCtx);
    void submitClose(int fd, ConnCtx &connCtx);
    int submitAccept();

    void submitEventFdRead();

    // connCtx 由 io_uring 的 user_data 带回（交给内核的指针保持原样）
    void handleCqe(ConnCtx *connCtx, int res, unsigned int cflags);
};

// ConnCtx / IouringAwaiter / QueryAwaiter 的完整定义在 Server.cpp：
// 它们直接操作 Server 私有成员，属于实现细节，不暴露在头文件中。
#endif //WEBPROJECT_SERVER_H
