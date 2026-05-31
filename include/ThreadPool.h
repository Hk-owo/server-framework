// ThreadPool.h
#ifndef IO_URING_SERVER_THREADPOOL_H
#define IO_URING_SERVER_THREADPOOL_H

#include "WaitQueue/SPSCQueue.h"
#include "WaitQueue/MPSCQueue.h"
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

/*
 * 8核心CPU优化配置
 * 8个工作线程 + 1个分发线程
 */
class ThreadPool {
private:
    static const size_t TSIZE = 8;

    // BATCH_SIZE: 分发线程每积累多少个任务批量唤醒一次工作线程
    // MINLDX_SIZE: 负载均衡采样窗口大小（均须为2的幂次）
    const size_t BATCH_SIZE   = 1 << 10;
    const size_t MINLDX_SIZE  = 1 << 7;

    // 用 padded 结构避免 isFinish 各元素间的伪共享
    struct alignas(64) PaddedAtomicBool {
        std::atomic<bool> value{false};
    };

    std::atomic<bool> stop{false};
    std::atomic<bool> isClose{false};

    std::vector<std::thread>   mWorkThread;
    std::thread                mDistributeThread;
    PaddedAtomicBool           isFinish[TSIZE + 1]; // [0..TSIZE-1]=工作线程, [TSIZE]=分发线程

    SPSCQueue<std::function<void()>> mWorkQueue[TSIZE];
    MPSCQueue<std::function<void()>> mDistributeQueue;

    // 记录每条工作队列的近似积压长度，用于负载均衡
    // 同样 padding 避免伪共享
    struct alignas(64) PaddedSize {
        std::atomic<size_t> value{0};
    };
    PaddedSize mQueueLoad[TSIZE];

private:
    ThreadPool();
    ~ThreadPool();

    void start();
    // 等待所有线程退出（消除 close/~dtor 中的重复逻辑）
    void joinAll();
    // 选出负载最轻的工作队列下标
    size_t pickLeastLoaded() const noexcept;

public:
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    static ThreadPool& get_instance();
    void submit(std::function<void()> f);
    void close();
};

#endif // IO_URING_SERVER_THREADPOOL_H