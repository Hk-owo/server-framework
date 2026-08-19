// ThreadPool.h
#ifndef IO_URING_SERVER_THREADPOOL_H
#define IO_URING_SERVER_THREADPOOL_H

#include "WaitQueue/MPSCQueue.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

/*
 * 可配置工作线程数的线程池：N 个工作线程共享一个任务队列。
 *
 * 模型：任务不再绑定线程 —— 所有 worker 从同一个共享任务队列取任务，
 * 谁空闲谁取，任何线程被慢任务拖住都不会导致排队任务无人处理。
 *
 * 结构：
 *   - mTaskQueue  共享任务队列（MPSCBase 纯数据结构，head CAS 多消费者安全）
 *   - mSignalQueue per-worker 唤醒信号（每个 worker 一个私有 MPSCQueue，
 *                  仅使用其 io_uring ring 做等待/唤醒，peek_times 每实例独享无竞争）
 *   - submit() 入队后广播唤醒所有 worker 的等待信号
 *
 * 框架内置两个独立实例：
 *   - global_instance()  全局任务池（8 个工作线程）：承载业务 handler 等通用任务
 *   - timewheel_instance() 时间轮专用池（2 个工作线程）：只执行时间轮到期的轻量任务
 */
class ThreadPool {
private:
    // 用 padded 结构避免 isFinish 各元素间的伪共享
    struct alignas(64) PaddedAtomicBool {
        std::atomic<bool> value{false};
    };

    std::atomic<bool> stop{false};
    std::atomic<bool> isClose{false};

    size_t mWorkerCount = 0;   // 工作线程数（运行时配置）
    std::vector<std::thread>   mWorkThread;
    // 含 atomic 成员不可拷贝移动，用 deque 就地构造
    std::deque<PaddedAtomicBool> isFinish; // [0..n-1]=工作线程

    // 共享任务队列：多生产者（submit 方）无锁入队；dequeue 多消费者
    // 由 mDequeueLock 互斥保护 —— MPSCBase 的节点回收是单消费者安全的，
    // 并发 dequeue 会因"先 move 后 delete"的交叉而 use-after-free
    MPSCBase<std::function<void()>> mTaskQueue;
    std::mutex mDequeueLock;
    // per-worker 唤醒信号：仅使用 io_uring ring 的等待/唤醒能力
    std::deque<MPSCQueue<std::function<void()>>> mSignalQueue;

private:
    explicit ThreadPool(size_t workerCount);
    ~ThreadPool();

    void start();
    // 等待所有线程退出（消除 close/~dtor 中的重复逻辑）
    void joinAll();

public:
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 全局任务池：8 个工作线程，承载业务 handler 等通用任务
    static ThreadPool& global_instance();
    // 时间轮专用池：2 个工作线程，只执行时间轮到期的轻量任务
    static ThreadPool& timewheel_instance();
    void submit(std::function<void()> f);
    void close();
};

#endif // IO_URING_SERVER_THREADPOOL_H
