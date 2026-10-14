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
#include <cstddef>
#include <cstdint>
#include <deque>
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
    struct WaitDataAwaiter;
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
    // 空闲扫描的"水位"：所有正在等待的连接里，最早的那个等待起点。
    // 没逼近超时边缘时，scanIdle 只读这一个值就能 O(1) 返回，不必碰连接表——
    // 遍历代价随连接数线性增长（10 万连接约 0.4ms），而它跑在事件循环线程上，
    // 每一次都是一记真实的延迟尖刺。
    // INT64_MAX 表示当前没有任何连接在等待，也就没有东西需要回收
    int64_t mOldestWaitSince = INT64_MAX;

    // ── provided buffer ring：缓冲由内核从环里挑，不再每连接预分配 ──────
    // 以前每条连接自带一块 64KB 读缓冲（构造时就分配，空闲连接也占着；
    // 10 万连接约 6.4GB）。现在把 kBufCount 块 buffer 注册成一个环交给内核：
    // recv 提交时只声明"请从 bgid 这个池里挑一块空的"，内核自己选一块填进去，
    // 完成事件里把它的 buffer id 带回来。池是共享、循环复用的，于是内存占用
    // 跟着**实际在收数据的连接**走，而不是跟着连接表大小走
    //
    // 默认 8192 条 × 8KB = 每实例 128KB。选它不是因为内核上限（那是 32768），
    // 而是因为注册给内核的 ring 内存会被 pin 住、计入 RLIMIT_MEMLOCK：本机
    // memlock 只有 8MB（硬限制同样是 8MB，普通用户调不上去），而默认
    // SF_LOOPS = CPU 核数，每个实例的 pbuf ring 与 io_uring 自己的 SQ/CQ 都要吃这笔额度。
    // 按 32768 条（每实例 512KB）实测只能起 9 个实例，12 核的默认配置直接起不来；
    // 8192 条时 12 个实例合计约 5.4MB，留有余量。
    // 想要更大的池：先把 memlock 调大（要 root），再用 SF_BUF_COUNT 指定条目数
    static constexpr unsigned kBufCount    = 8192;       // 默认条目数
    static constexpr unsigned kMaxBufCount = 32768;      // 内核允许的上限（IORING_REGISTER_PBUF_RING）
    static constexpr unsigned kMinBufCount = 256;        // 注册失败时逐级减半的下限
    static constexpr size_t   kBufSize     = 8192;       // 单块 buffer 大小
    static constexpr int      kBufGroup    = 0;          // buffer group id（bgid）
    // 内核的硬性要求，写错了注册会在运行期 -EINVAL，不如编译期拦住
    static_assert((kBufCount & (kBufCount - 1)) == 0 && kBufCount <= kMaxBufCount,
                  "buffer ring 默认条目数必须是 2 的幂且不超过 32768");
    static_assert((kMaxBufCount & (kMaxBufCount - 1)) == 0, "上限也必须是 2 的幂");
    static_assert((kMinBufCount & (kMinBufCount - 1)) == 0 && kMinBufCount <= kBufCount,
                  "降级下限也得是 2 的幂，且不超过默认值");
    // "这次 recv 没占着 buffer"。0xFFFF 是无效 bid，池子永远发不到这个编号
    static constexpr uint16_t kNoBufId   = 0xFFFF;

    struct io_uring_buf_ring* mBufRing = nullptr;   // 挂在 ring 上、交给内核挑的缓冲环
    void*  mBufPool      = nullptr;                 // 池基址（只预留虚拟地址，物理页按需落）
    size_t mBufPoolBytes = 0;
    // 实际注册成功的条目数与对应的掩码。正常情况下等于 kBufCount，
    // 只有在 memlock 不够（多实例）时才会降级变小
    unsigned mBufCount    = 0;
    int      mBufRingMask = 0;
    // 池暂时空了（recv 拿到 -ENOBUFS）时挂在这儿的连接 id，等有 buffer 归还再重投
    std::deque<uint64_t> mBufStarved;

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
    // 取消某条连接上那条在途请求，按 (操作类型, 连接 id) 精确匹配
    bool cancelInFlight(const ConnCtx& connCtx, OpType op);
    // 真的把 close 提交出去（submitClose 会在读方向还在途时先取消、把这一步推后）
    void submitCloseNow(int fd, ConnCtx& connCtx);
    // 把一个等待起点压进扫描水位（只在事件循环线程调用）
    void noteWaitStart(int64_t since);
    // 扫描并回收空闲超时的连接（只在事件循环线程调用）
    void scanIdle();
    // 向时间轮注册下一次空闲扫描（每秒一次，自我重注册）
    void scheduleScan();

    // 建缓冲池并注册给内核（run() 里、提交第一条 SQE 之前调用）
    int  setupBufRing();
    // 注销缓冲环并释放池（析构时调用，必须早于 io_uring_queue_exit）
    void freeBufRing();
    // 归还一块 provided buffer（handleCqe 里 feed 完就还，凭证是 CQE 带回的 buffer id），
    // 顺手把腾出来的那块喂给在等的连接
    void putBackReadBuffer(uint16_t bid);
    // 池里刚空出一块 buffer：挑一个挂在等待队列里的连接重投它那条 recv
    void resumeStarved();

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
    // multishot recv：一次武装后持续产 CQE。缓冲区仍由内核从 buffer ring 里挑
    // （IOSQE_BUFFER_SELECT），所以它必须配 provided buffer 用
    void submitRecv(int fd, ConnCtx& connCtx);
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
