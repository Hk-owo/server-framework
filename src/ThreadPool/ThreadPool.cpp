// ThreadPool.cpp
#include "ThreadPool.h"
#include "Logger.h"
#include <thread>

using namespace std;

// ──────────────────────────────────────────────
// 内部辅助
// ──────────────────────────────────────────────

// 选出 mQueueLoad 最小的队列下标
// 每 MINLDX_SIZE 个任务才重新采样一次，平摊开销
size_t ThreadPool::pickLeastLoaded() const noexcept {
    size_t minIdx  = 0;
    size_t minLoad = mQueueLoad[0].value.load(memory_order_relaxed);
    for (size_t i = 1; i < TSIZE; ++i) {
        size_t load = mQueueLoad[i].value.load(memory_order_relaxed);
        if (load < minLoad) {
            minLoad = load;
            minIdx  = i;
        }
    }
    return minIdx;
}

// 等待全部线程退出，替换原先的忙等自旋
void ThreadPool::joinAll() {
    for (size_t i = 0; i < TSIZE; ++i) {
        // 持续唤醒，直到该工作线程自己标记完成
        while (!isFinish[i].value.load(memory_order_acquire)) {
            mWorkQueue[i].notify_stop_uring();
            std::this_thread::yield(); // 让出 CPU，避免空转
        }
        if (mWorkThread[i].joinable())
            mWorkThread[i].join();
    }
    while (!isFinish[TSIZE].value.load(memory_order_acquire)) {
        mDistributeQueue.notify_stop_uring();
        std::this_thread::yield();
    }
    if (mDistributeThread.joinable())
        mDistributeThread.join();
}

// ──────────────────────────────────────────────
// 生命周期
// ──────────────────────────────────────────────

ThreadPool::ThreadPool() {
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

ThreadPool& ThreadPool::get_instance() {
    static ThreadPool threadPool;
    return threadPool;
}

// ──────────────────────────────────────────────
// 启动
// ──────────────────────────────────────────────

void ThreadPool::start() {
    stop.store(false,    memory_order_relaxed);
    isClose.store(false, memory_order_relaxed);
    mWorkThread.resize(TSIZE);

    // 工作线程
    for (size_t i = 0; i < TSIZE; ++i) {
        mWorkThread[i] = thread([i, this] {
            LOGGER_TRACE("WorkThread {} is working", i);

            while (true) {
                std::function<void()> task;

                // 尽量排空队列，再进入等待
                while (mWorkQueue[i].dequeue(task)) {
                    if (task) {
                        task();
                        // 任务执行完毕，更新负载计数
                        mQueueLoad[i].value.fetch_sub(1, memory_order_relaxed);
                    }
                }

                // 队列已空：若 stop 已置位则退出，否则挂起等待
                if (stop.load(memory_order_acquire))
                    break;

                mWorkQueue[i].wait_for_data_uring();
            }

            // 退出前排空残余任务（stop 后分发线程可能仍塞入了任务）
            {
                std::function<void()> task;
                while (mWorkQueue[i].dequeue(task))
                    if (task) task();
            }

            isFinish[i].value.store(true, memory_order_release);
            LOGGER_TRACE("WorkThread {} is finish", i);
        });
    }

    // 分发线程
    mDistributeThread = thread([this] {
        LOGGER_TRACE("DistributeThread is working");

        size_t batch  = 0; // 当前批次已分发数量
        size_t target = 0; // 当前目标队列（负载均衡结果）

        while (true) {
            std::function<void()> task;

            // 批量取任务并分发
            while (mDistributeQueue.dequeue(task)) {
                if (!task) continue;

                // 每隔 MINLDX_SIZE 个任务重新选一次最轻负载队列
                if (!(batch & (MINLDX_SIZE - 1)))
                    target = pickLeastLoaded();

                mWorkQueue[target].enqueue(task);
                mQueueLoad[target].value.fetch_add(1, memory_order_relaxed);
                ++batch;

                // 每积累 BATCH_SIZE 个任务，批量唤醒所有工作线程
                if (!(batch & (BATCH_SIZE - 1))) {
                    for (size_t i = 0; i < TSIZE; ++i)
                        mWorkQueue[i].on_data_ready_uring();
                }
            }

            // 队列暂时为空：唤醒工作线程处理已分发的任务
            if (batch > 0) {
                for (size_t i = 0; i < TSIZE; ++i)
                    mWorkQueue[i].on_data_ready_uring();
                batch = 0;
            }

            if (stop.load(memory_order_acquire))
                break;

            mDistributeQueue.wait_for_data_uring();
        }

        // stop 后排空分发队列
        {
            std::function<void()> task;
            size_t target = 0;
            size_t i = 0;
            while (mDistributeQueue.dequeue(task)) {
                if (!task) continue;
                mWorkQueue[target % TSIZE].enqueue(task);
                ++target;
            }
            // 唤醒所有工作线程处理残余任务
            for (size_t i = 0; i < TSIZE; ++i)
                mWorkQueue[i].on_data_ready_uring();
        }

        isFinish[TSIZE].value.store(true, memory_order_release);
        LOGGER_TRACE("DistributeThread is finish");
    });
}

// ──────────────────────────────────────────────
// 提交
// ──────────────────────────────────────────────

void ThreadPool::submit(std::function<void()> f) {
    if (isClose.load(memory_order_acquire)) {
        LOGGER_INF("ThreadPool is close.");
        return;
    }
    // f 是具名参数，本身就是左值，直接传
    // 函数参数按值传入时已经发生了一次 move/copy，无需再 move
    mDistributeQueue.enqueue(f);
    mDistributeQueue.on_data_ready_uring();
}