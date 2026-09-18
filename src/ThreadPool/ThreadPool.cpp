// ThreadPool.cpp
#include "ThreadPool.h"
#include "Logger.h"
#include <thread>

using namespace std;

// ──────────────────────────────────────────────
// 生命周期
// ──────────────────────────────────────────────

ThreadPool::ThreadPool(size_t workerCount)
        : mWorkerCount(workerCount) {
    start();
}

ThreadPool::~ThreadPool() {
    // 若用户已主动调用 close()，此处直接返回
    if (isClose.load(memory_order_acquire))
        return;
    stop.store(true, memory_order_release);
    joinAll();
    isClose.store(true, memory_order_release);
}

void ThreadPool::close() {
    // 幂等：重复 close() 无副作用
    if (isClose.load(memory_order_acquire)) {
        LOGGER_INF("ThreadPool already closed.");
        return;
    }
    LOGGER_TRACE("ThreadPool closing");
    stop.store(true, memory_order_release);
    joinAll();
    isClose.store(true, memory_order_release);
    LOGGER_TRACE("ThreadPool is close");
}

// 全局任务池：8 个工作线程，承载业务 handler 等通用任务
ThreadPool& ThreadPool::global_instance() {
    static ThreadPool instance(8);
    return instance;
}

// 时间轮专用池：2 个工作线程，只执行时间轮到期的轻量任务
ThreadPool& ThreadPool::timewheel_instance() {
    static ThreadPool instance(2);
    return instance;
}

// ──────────────────────────────────────────────
// 启动
// ──────────────────────────────────────────────

void ThreadPool::start() {
    stop.store(false,    memory_order_relaxed);
    isClose.store(false, memory_order_relaxed);

    mWorkThread.resize(mWorkerCount);

    // 工作线程：从共享任务队列取任务，谁空闲谁取
    for (size_t i = 0; i < mWorkerCount; ++i) {
        mWorkThread[i] = thread([i, this] {
            LOGGER_TRACE("WorkThread {} is working", i);

            while (true) {
                std::function<void()> task;

                // 尽量排空共享任务队列；MPMCBase 的槽位带序号，
                // 多个消费者并发取任务不会取到同一个槽位，也不需要互斥锁
                while (mTaskQueue.dequeue(task))
                    task();

                // 队列已空：若 stop 已置位则退出，
                // 否则挂在队列内建的 io_uring ring 上阻塞等待唤醒
                if (stop.load(memory_order_acquire))
                    break;

                mTaskQueue.wait_for_data_uring();
            }

            // 退出前排空残余任务（stop 后 submit 方可能仍塞入了任务）
            {
                std::function<void()> task;
                while (mTaskQueue.dequeue(task))
                    task();
            }

            LOGGER_TRACE("WorkThread {} is finish", i);
        });
    }
}

// 唤醒所有 worker 并回收线程
void ThreadPool::joinAll() {
    // 唤醒事件是被原子认领消费掉的，所以要唤醒几个等待者就提交几次；
    // 正在跑任务的 worker 会自己在下一轮检查 stop，不需要额外信号
    for (size_t i = 0; i < mWorkerCount; ++i)
        mTaskQueue.notify_stop_uring();
    for (auto& th : mWorkThread) {
        if (th.joinable())
            th.join();
    }
}

// ──────────────────────────────────────────────
// 提交
// ──────────────────────────────────────────────

void ThreadPool::submit(std::function<void()> f) {
    if (isClose.load(memory_order_acquire)) {
        LOGGER_INF("ThreadPool is close.");
        return;
    }
    mTaskQueue.enqueue(f);
    // 提交一次唤醒：有 worker 挂着就唤醒一个，被唤醒者自己去排空队列
    mTaskQueue.on_data_ready_uring();
}
