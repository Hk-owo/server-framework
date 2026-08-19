//
// Created by lacas on 2026/2/20.
// Optimized version
//

#ifndef TIME_WHEEL_TIMEWHEEL_H
#define TIME_WHEEL_TIMEWHEEL_H

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include "liburing.h"

class TimeWheel;

class TimeWheelBasics {
public:
    struct DelayTime {
        uint hour        = 0;
        uint minute      = 0;
        uint second      = 0;
        uint millisecond = 0;
    };

protected:
    enum class Mask : uint {
        MILLISECOND = 1023u,
        SECOND      = (63u  << 10),
        MINUTE      = (63u  << 16),
        HOUR        = (511u << 22)
    };
    enum class XMask : uint {
        MILLISECOND = 0u,
        SECOND      = 1023u,
        MINUTE      = (63u << 10) | 1023u,
        HOUR        = (63u << 16) | (63u << 10) | 1023u
    };

    struct TaskNode {
        std::function<void()> task;
        uint time = 0;

        TaskNode() : task(nullptr) {}
        TaskNode(std::function<void()> t, uint compressed_time)
                : task(std::move(t)), time(compressed_time) {}
    };

    // atomic 保证推进线程写、add_task 线程读的跨线程可见性
    std::atomic<uint> nowIndex{0};

public:
    TimeWheelBasics() = default;
    virtual ~TimeWheelBasics() = default;
};

// ─────────────────────────────────────────────────────────────
// TimeWheel
// ─────────────────────────────────────────────────────────────
class TimeWheel : public TimeWheelBasics {
    friend class TimeWheelTop;
private:
    // per-slot 细粒度锁：add_task 与 advance 只在同一槽时才竞争
    struct Slot {
        std::vector<TaskNode> tasks;
        std::atomic_flag      lock = ATOMIC_FLAG_INIT;

        void acquire() noexcept {
            while (lock.test_and_set(std::memory_order_acquire));
        }
        void release() noexcept {
            lock.clear(std::memory_order_release);
        }
    };

    std::vector<std::unique_ptr<Slot>> mSlots;
    uint   mMask   = 0;
    uint   mXmask  = 0;
    uint   mOffset = 0;
    size_t mSize   = 0;

public:
    TimeWheel(uint mask, uint offset);
    ~TimeWheel() = default;

    // 禁止拷贝（Slot 含 atomic_flag，不可拷贝）
    TimeWheel(const TimeWheel&)            = delete;
    TimeWheel& operator=(const TimeWheel&) = delete;
    // 允许移动（vector<TimeWheel> emplace_back 需要）
    TimeWheel(TimeWheel&&)            = default;
    TimeWheel& operator=(TimeWheel&&) = default;

    // 高级轮推进：到期任务级联到 lower 轮
    void advance(TimeWheel* lower);
    // ms 轮推进：到期任务直接投线程池
    void advance();
    // 线程安全插入
    void add_task(std::function<void()> task, uint run_time);
};

// ─────────────────────────────────────────────────────────────
// TimeWheelTop
// ─────────────────────────────────────────────────────────────
class TimeWheelTop : public TimeWheelBasics {
private:
    struct io_uring  ring{};
    // [0]=ms  [1]=sec  [2]=min  [3]=hour
    std::vector<std::unique_ptr<TimeWheel>> mSubWheel;

    std::thread       mThread;
    std::atomic<bool> stop{false};

public:
    TimeWheelTop();
    ~TimeWheelTop() override;

    // 线程安全，可从任意线程调用
    void add_task(std::function<void()> task, DelayTime delayTime = {});
};

#endif // TIME_WHEEL_TIMEWHEEL_H