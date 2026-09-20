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

#include "liburing.h"

class Server{
private:
    enum class OpType{ACCEPT,READ,WRITE,CLOSE,EVENT,CANCEL};
    struct ConnCtx;
    struct IouringAwaiter;
    struct QueryAwaiter;
private:
    io_uring ring;
    int mSockFd,mEventFd;
    uint64_t mEventVal = 0;
    // 待恢复的协程：只弱引用连接。不换的话工作线程会拿着可能已被回收的连接去恢复协程
    MPSCQueue<std::pair<std::coroutine_handle<>, std::weak_ptr<ConnCtx>>> mPendingResumes;
    // 连接表按"连接 id"索引。id 单调递增、永不复用，所以一条 CQE 带回来的 id
    // 在表里查不到时它就一定是陈旧事件——查表这个动作本身就完成了身份校验，
    // 既不需要指针去比对，也不会去读可能已经析构的连接对象
    std::unordered_map<uint64_t, std::shared_ptr<ConnCtx>> mConnections;
    // 下一个连接 id（只在事件循环线程分配）
    uint64_t mNextConnId = 1;
    Router mRouter;
    // 四级级联时间轮：用来驱动连接的空闲超时（见 scanIdle）
    TimeWheelTop mTimeWheel;
    // 时间轮线程跨线程只置这个标志 + 写 eventfd，真正的扫描在事件循环线程做
    std::atomic<bool> mScanRequested{false};

private:
    // ── user_data 编码：操作类型 + 连接 id ────────────────────────
    // 64 位里高 4 位放操作类型（recv / send / close / accept / eventfd），
    // 低 60 位放连接 id。带上操作类型意味着一条 CQE 该走哪条处理路径在
    // **提交那一刻**就定死了，不再依赖 ConnCtx::status 这个会被协程、
    // submitClose、scanIdle 多方改写的共享字段——提交 close 不会再让一条
    // 在途 recv 的完成事件被误当成 close 完成来处理
    static constexpr uint64_t kConnIdMask = (1ULL << 60) - 1;
    static uint64_t packTag(OpType op, uint64_t connId) {
        return (static_cast<uint64_t>(op) << 60) | (connId & kConnIdMask);
    }
    static OpType tagOp(uint64_t tag) {
        return static_cast<OpType>(tag >> 60);
    }
    static uint64_t tagConnId(uint64_t tag) {
        return tag & kConnIdMask;
    }

    // 给连接分配一个全局唯一、单调递增的 id 并登记进连接表（只在事件循环线程调用）
    uint64_t registerConn(const std::shared_ptr<ConnCtx>& conn);
    // 从连接表摘除。连接对象由调用方手上那份 shared_ptr 保活到本次事件处理结束
    void unregisterConn(uint64_t id);
    // 取消某条连接上那条在途请求（空闲超时用），按 (操作类型, 连接 id) 精确匹配
    bool cancelInFlight(const ConnCtx& connCtx);
    // 扫描并回收空闲超时的连接（只在事件循环线程调用）
    void scanIdle();
    // 向时间轮注册下一次空闲扫描（每秒一次，自我重注册）
    void scheduleScan();

public:
    int listen(const std::string& bind,const std::string& port);
    // 默认开启 DEFER_TASKRUN | SINGLE_ISSUER：事件循环线程是这个 ring 唯一的提交者，
    // DEFER_TASKRUN 把 task_work 推迟到下一次进入内核，配合 SINGLE_ISSUER
    // 让内核省掉提交侧的原子操作（两者必须成对使用，否则 init 会失败）
    void run(unsigned entries = 4096,
             unsigned flags = IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER);
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

    // user_data 里放 (操作类型, 连接 id) 打包成的 64 位标签，不放裸指针：
    // 回到 handleCqe 后按 id 查表，查不到就是陈旧事件，全程不碰可能已析构的对象
    void handleCqe(uint64_t tag, int res, unsigned int cflags);
};

// ConnCtx / IouringAwaiter / QueryAwaiter 的完整定义在 Server.cpp：
// 它们直接操作 Server 私有成员，属于实现细节，不暴露在头文件中。
#endif //WEBPROJECT_SERVER_H
