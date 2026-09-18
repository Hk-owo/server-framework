// ThreadPool.h
#ifndef IO_URING_SERVER_THREADPOOL_H
#define IO_URING_SERVER_THREADPOOL_H

#include "WaitQueue/MPMCQueue.h"
#include <atomic>
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
 *   - mTaskQueue 共享任务队列（MPMCQueue = MPMCBase 无锁队列 + 内建 io_uring 阻塞等待）。
 *                多消费者并发出队不会取到同一个槽位；队列取空后 worker 直接挂在
 *                队列自己的 ring 上阻塞，由 submit 方提交唤醒 ——
 *                不需要 per-worker 的唤醒队列，worker 也不会空转自旋
 *
 * 框架内置两个独立实例：
 *   - global_instance()  全局任务池（8 个工作线程）：承载业务 handler 等通用任务
 *   - timewheel_instance() 时间轮专用池（2 个工作线程）：只执行时间轮到期的轻量任务
 */
class ThreadPool {
private:
    std::atomic<bool> stop{false};
    std::atomic<bool> isClose{false};

    size_t mWorkerCount = 0;   // 工作线程数（运行时配置）
    std::vector<std::thread>   mWorkThread;

    // 共享任务队列：多生产者（submit 方）无锁入队；多消费者（worker）无锁出队；
    // 队列空时 worker 用队列内建的 io_uring ring 阻塞等待，submit 时提交唤醒
    MPMCQueue<std::function<void()>> mTaskQueue;

private:
    explicit ThreadPool(size_t workerCount);
    ~ThreadPool();

    void start();
    // 唤醒所有 worker 并回收线程
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
