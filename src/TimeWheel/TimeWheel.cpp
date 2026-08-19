//
// Created by lacas on 2026/3/10.
// Optimized version
//

#include "TimeWheel.h"
#include "ThreadPool.h"
#include "Logger.h"

using namespace std;

// ─────────────────────────────────────────────────────────────
// TimeWheel
// ─────────────────────────────────────────────────────────────

TimeWheel::TimeWheel(uint mask, uint offset) : mMask(mask), mOffset(offset) {
    if      (mask == (uint)Mask::MILLISECOND) { mSize = 1000; mXmask = (uint)XMask::MILLISECOND; }
    else if (mask == (uint)Mask::SECOND)      { mSize = 60;   mXmask = (uint)XMask::SECOND;      }
    else if (mask == (uint)Mask::MINUTE)      { mSize = 60;   mXmask = (uint)XMask::MINUTE;      }
    else if (mask == (uint)Mask::HOUR)        { mSize = 512;  mXmask = (uint)XMask::HOUR;        }
    mSlots.resize(mSize);
    for(auto& elem : mSlots) elem = make_unique<Slot>();
    nowIndex.store(0, memory_order_relaxed);
}

void TimeWheel::add_task(std::function<void()> task, uint run_time) {
    uint slot      = (run_time & mMask) >> mOffset;
    uint remaining = run_time & mXmask;
    if (slot >= mSize) slot = static_cast<uint>(mSize - 1);

    auto& s = mSlots[slot];
    s->acquire();
    s->tasks.emplace_back(std::move(task), remaining);
    s->release();
}

// ms 轮：到期直接投线程池
void TimeWheel::advance() {
    uint idx = nowIndex.fetch_add(1, memory_order_relaxed);
    if (idx >= mSize) {
        nowIndex.store(0, memory_order_relaxed);
        idx = 0;
    }

    // swap 出任务后立即释锁，不持锁执行回调
    vector<TaskNode> fired;
    {
        auto& s = mSlots[idx];
        s->acquire();
        fired.swap(s->tasks);
        s->release();
    }
    for (auto& node : fired)
        if (node.task)
            ThreadPool::timewheel_instance().submit(std::move(node.task));
}

// 高级轮：到期任务级联到低一级轮
void TimeWheel::advance(TimeWheel* lower) {
    uint idx = nowIndex.fetch_add(1, memory_order_relaxed);
    if (idx >= mSize) {
        nowIndex.store(0, memory_order_relaxed);
        idx = 0;
    }

    vector<TaskNode> fired;
    {
        auto& s = mSlots[idx];
        s->acquire();
        fired.swap(s->tasks);
        s->release();
    }
    for (auto& node : fired) {
        if (!node.task) continue;
        if (node.time == 0)
            // 残余时间为 0：已到期，直接投
            ThreadPool::timewheel_instance().submit(std::move(node.task));
        else
            lower->add_task(std::move(node.task), node.time);
    }
}

// ─────────────────────────────────────────────────────────────
// TimeWheelTop
// ─────────────────────────────────────────────────────────────

TimeWheelTop::TimeWheelTop() {
    io_uring_queue_init(2, &ring, 0);

    mSubWheel.resize(4);
    mSubWheel[0] = make_unique<TimeWheel>((uint)Mask::MILLISECOND, 0);
    mSubWheel[1] = make_unique<TimeWheel>((uint)Mask::SECOND,      10);
    mSubWheel[2] = make_unique<TimeWheel>((uint)Mask::MINUTE,      16);
    mSubWheel[3] = make_unique<TimeWheel>((uint)Mask::HOUR,        22);

    mThread = std::thread([this] {
        io_uring_cqe* cqe = nullptr;
        struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 1'000'000}; // 1ms

        // 记录启动时刻，用于追赶机制
        const uint   maxMs = 60u * 60u * 1000u; // 1小时循环周期 = 3600000ms
        const auto   epoch = chrono::steady_clock::now();
        uint         expected = 0; // 本线程已经推进了多少 ms

        while (!stop.load(memory_order_acquire)) {
            io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            if (cqe) {
                io_uring_cqe_seen(&ring, cqe);
                cqe = nullptr;
            }

            // 计算从启动至今应推进的 ms 数（相对于循环周期取模）
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - epoch).count();
            uint target = static_cast<uint>(elapsed % maxMs);

            // 追赶：至少推进 1 次，落后时连续推进直到追上
            do {
                expected++;
                if (expected >= maxMs) expected = 0;

                mSubWheel[0]->advance();                                // 每 ms
                if (expected % 1000  == 0) mSubWheel[1]->advance(mSubWheel[0].get()); // 每秒
                if (expected % 60000 == 0) mSubWheel[2]->advance(mSubWheel[1].get()); // 每分
                if (expected % maxMs == 0) mSubWheel[3]->advance(mSubWheel[2].get()); // 每时
            } while (expected < target);
        }
    });
}

TimeWheelTop::~TimeWheelTop() {
    stop.store(true, memory_order_release);
    io_uring_queue_exit(&ring); // 唤醒阻塞中的 io_uring，让线程尽快退出
    if (mThread.joinable())
        mThread.join();
}

void TimeWheelTop::add_task(std::function<void()> task, DelayTime delayTime) {
    if (!task) return;

    // 读取各轮当前指针（relaxed：推进线程写，此处只需近似值）
    uint msNow  = mSubWheel[0]->nowIndex.load(memory_order_relaxed);
    uint secNow = mSubWheel[1]->nowIndex.load(memory_order_relaxed);
    uint minNow = mSubWheel[2]->nowIndex.load(memory_order_relaxed);
    uint hrNow  = mSubWheel[3]->nowIndex.load(memory_order_relaxed);

    // 加上当前指针偏移，计算绝对槽位
    delayTime.millisecond += msNow;
    delayTime.second      += secNow;
    delayTime.minute      += minNow;

    // 进位
    if (delayTime.millisecond >= 1000) {
        delayTime.second      += delayTime.millisecond / 1000;
        delayTime.millisecond %= 1000;
    }
    if (delayTime.second >= 60) {
        delayTime.minute += delayTime.second / 60;
        delayTime.second %= 60;
    }
    if (delayTime.minute >= 60) {
        delayTime.hour += delayTime.minute / 60;
        delayTime.minute %= 60;
    }
    if (delayTime.hour >= 512) {
        LOGGER_INF("TimeWheel limit: max delay is 511 hours");
        return;
    }

    // 加上 hour 轮偏移并环绕
    delayTime.hour = (delayTime.hour + hrNow) & 511u;

    // 压缩为 31 位整数
    uint run_time = (delayTime.millisecond & 1023u)
                    | ((delayTime.second & 63u)  << 10)
                    | ((delayTime.minute & 63u)  << 16)
                    | ((delayTime.hour   & 511u) << 22);

    // run_time < 3：当前槽和紧邻下一槽可能正在推进，
    // 用精度换线程安全，直接投线程池
    if (run_time < 3) {
        ThreadPool::timewheel_instance().submit(std::move(task));
        return;
    }

    // 根据最高有效时间字段选择插入哪一级轮
    if      (delayTime.hour   != hrNow  % 512u) mSubWheel[3]->add_task(std::move(task), run_time);
    else if (delayTime.minute != minNow % 60u)  mSubWheel[2]->add_task(std::move(task), run_time);
    else if (delayTime.second != secNow % 60u)  mSubWheel[1]->add_task(std::move(task), run_time);
    else                                         mSubWheel[0]->add_task(std::move(task), run_time);
}