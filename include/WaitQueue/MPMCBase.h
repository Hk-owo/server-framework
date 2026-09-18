//
// Created by lacas on 2026/9/18.
//

#ifndef IO_URING_SERVER_MPMCBASE_H
#define IO_URING_SERVER_MPMCBASE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <utility>

/*
 * 标准的MPMC队列 多生产者多消费者
 * 生产者是submit方（事件循环线程/任意业务线程）
 * 消费者是所有工作线程，谁空闲谁取，消费者之间不需要再加互斥锁
 *
 * 每个槽位带一个sequence序号，入队/出队都用序号判断该槽位是否轮到自己：
 *   入队者等待 sequence == pos      出队者等待 sequence == pos + 1
 * 所以两个消费者不会取到同一个槽位，也不会读到别人正在写的数据；
 * 内存一次分配、槽位循环复用，不存在节点回收，也就没有use-after-free
 * 默认buffer是1 << 16
 */
template<typename T>
class MPMCBase{
private:
    //ring队列长度
    //必须是2的幂次，这里用了位运算来求模，不然会越界
    const size_t QSIZE = 1 << 16;

    //槽位：sequence记录该槽位正在等待的序号
    struct Cell{
        std::atomic<size_t> sequence;
        T data;
        Cell():sequence(0),data(){}
    };

    alignas(std::hardware_constructive_interference_size) std::atomic<size_t> m_head{0};
    alignas(std::hardware_constructive_interference_size) std::atomic<size_t> m_tail{0};
    std::unique_ptr<Cell[]> m_queue;

public:
    MPMCBase();
    virtual ~MPMCBase() = default;
    virtual bool enqueue(T& task);
    virtual bool dequeue(T& task);
    bool empty() const;
    size_t size() const;
};

template<typename T>
bool MPMCBase<T>::empty() const {
    //只看当前出队位置这一格：队列是FIFO的，这一格的数据没就绪就是空。
    //这里必须看sequence而不是比较首尾指针，否则入队方刚推进tail、
    //数据还没发布的那一刻会被误判成非空，等待方就会空转
    const size_t pos = m_head.load(std::memory_order_relaxed);
    const Cell* cell = &m_queue[pos & (QSIZE - 1)];
    const size_t seq = cell->sequence.load(std::memory_order_acquire);
    return (intptr_t)seq - (intptr_t)(pos + 1) < 0;
}

template<typename T>
size_t MPMCBase<T>::size() const {
    size_t t = m_tail.load(std::memory_order_acquire);
    size_t h = m_head.load(std::memory_order_acquire);
    return t - h;
}

template<typename T>
MPMCBase<T>::MPMCBase() {
    m_queue = std::make_unique<Cell[]>(QSIZE);
    //每个槽位初始等待的序号就是它自己的下标
    for(size_t i = 0; i < QSIZE; ++i)
        m_queue[i].sequence.store(i, std::memory_order_relaxed);
}

template<typename T>
bool MPMCBase<T>::enqueue(T& task) {
    while(1) {
        size_t pos = m_tail.load(std::memory_order_relaxed);
        Cell* cell = &m_queue[pos & (QSIZE - 1)];
        const size_t seq = cell->sequence.load(std::memory_order_acquire);
        const intptr_t diff = (intptr_t)seq - (intptr_t)pos;

        //序号相等：槽位空闲且轮到本生产者，尝试抢占
        if(diff == 0) {
            if(m_tail.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                cell->data = std::move(task);
                //发布：消费者看到sequence走到pos+1才会取走这份数据
                cell->sequence.store(pos + 1, std::memory_order_release);
                return true;
            }
            //CAS失败：pos被其他生产者抢先推进了，重新读
        }
        //序号落后：槽位还没被消费者腾出来，队列已满
        else if(diff < 0) {
            std::this_thread::yield();
        }
        //序号超过：其他生产者已经推进过，重新读
    }
}

template<typename T>
bool MPMCBase<T>::dequeue(T& task) {
    while(1) {
        size_t pos = m_head.load(std::memory_order_relaxed);
        Cell* cell = &m_queue[pos & (QSIZE - 1)];
        const size_t seq = cell->sequence.load(std::memory_order_acquire);
        const intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

        //序号相等：槽位数据已就绪且轮到本消费者，尝试抢占
        if(diff == 0) {
            if(m_head.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                task = std::move(cell->data);
                //发布：生产者看到sequence走到pos+QSIZE才会复用该槽位
                cell->sequence.store(pos + QSIZE, std::memory_order_release);
                return true;
            }
            //CAS失败：pos被其他消费者抢先推进了，重新读
        }
        //序号落后：该槽位还没有数据，队列为空
        else if(diff < 0) {
            return false;
        }
        //序号超过：其他消费者已经推进过，重新读
    }
}

#endif //IO_URING_SERVER_MPMCBASE_H
