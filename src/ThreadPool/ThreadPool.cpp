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
    isFinish.resize(mWorkerCount);
    mSignalQueue.resize(mWorkerCount);   // 每个 worker 一个私有唤醒信号

    // 工作线程：从共享任务队列取任务，谁空闲谁取
    for (size_t i = 0; i < mWorkerCount; ++i) {
        mWorkThread[i] = thread([i, this] {
            LOGGER_TRACE("WorkThread {} is working", i);

            while (true) {
                std::function<void()> task;

                // 尽量排空共享任务队列；dequeue 互斥保护（节点回收安全），
                // 任务在锁外执行，避免长时间持锁阻塞其他消费者
                while (true) {
                    {
                        std::lock_guard<std::mutex> lk(mDequeueLock);
                        if (!mTaskQueue.dequeue(task))
                            break;   // 队列空，退出排空循环
                    }
                    task();
                }

                // 队列已空：若 stop 已置位则退出，否则挂起等待唤醒信号
                if (stop.load(memory_order_acquire))
                    break;

                mSignalQueue[i].wait_for_data_uring();
            }

            // 退出前排空残余任务（stop 后 submit 方可能仍塞入了任务）
            {
                std::function<void()> task;
                while (true) {
                    {
                        std::lock_guard<std::mutex> lk(mDequeueLock);
                        if (!mTaskQueue.dequeue(task))
                            break;
                    }
                    task();
                }
            }

            isFinish[i].value.store(true, memory_order_release);
            LOGGER_TRACE("WorkThread {} is finish", i);
        });
    }
}

// 等待全部线程退出：持续向每个 worker 的唤醒信号发停止通知直到其标记完成
void ThreadPool::joinAll() {
    for (size_t i = 0; i < mWorkerCount; ++i) {
        while (!isFinish[i].value.load(memory_order_acquire)) {
            mSignalQueue[i].notify_stop_uring();
            std::this_thread::yield(); // 让出 CPU，避免空转
        }
        if (mWorkThread[i].joinable())
            mWorkThread[i].join();
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
    // 广播唤醒所有 worker 的等待信号：
    // 空闲的 worker 全部醒来，从共享队列竞争取任务（谁空闲谁取）
    for (auto& sig : mSignalQueue)
        sig.on_data_ready_uring();
}
