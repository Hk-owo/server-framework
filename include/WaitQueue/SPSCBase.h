//
// Created by lacas on 2025/12/8.
//

#ifndef IO_URING_SERVER_TQUEUE_H
#define IO_URING_SERVER_TQUEUE_H

/*
 * 标准的SPSC队列 单生产者单消费者
 * 生产者是主线程
 * 获取已完成的任务加入队列
 * 默认buffer是1 << 21
 */
template<typename T>
class SPSCBase{
private:
    //ring队列长度
    //必须是2的幂次，这里用了位运算来求模，不然会越界
    const size_t QSIZE = 1 << 21;

    alignas(std::hardware_constructive_interference_size) std::atomic<size_t> m_head{0};
    alignas(std::hardware_constructive_interference_size) std::atomic<size_t> m_tail{0};
    std::vector<T> m_queue;

public:
    SPSCBase();
    virtual ~SPSCBase() = default;
    virtual bool enqueue(T& task);
    virtual bool dequeue(T& task);
    size_t size() const;
};

template<typename T>
size_t SPSCBase<T>::size() const {
    size_t t = m_tail.load(std::memory_order_acquire);
    size_t h = m_head.load(std::memory_order_acquire);
    return (t - h) & (QSIZE - 1);
}

template<typename T>
SPSCBase<T>::SPSCBase() {
    m_queue.resize(QSIZE,{});
}

template<typename T>
bool SPSCBase<T>::enqueue(T& task) {
    while(1) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        //const size_t next = (tail + 1) % QSIZE;
        const size_t next = (tail + 1) & (QSIZE - 1);

        //检查是否已满
        if (next == m_head.load(std::memory_order_acquire)) {
            std::this_thread::yield();
            continue;
        }

        m_queue[tail] = move(task);
        m_tail.store(next, std::memory_order_release);
        return true;
    }
}

template<typename T>
bool SPSCBase<T>::dequeue(T &task) {
    const size_t head = m_head.load(std::memory_order_relaxed);

    //检查是否为空
    if(head == m_tail.load(std::memory_order_acquire))
        return false;

    task = move(m_queue[head]);
    const size_t next = (head + 1) % QSIZE;
    m_head.store(next, std::memory_order_release);
    return true;
}

#endif //IO_URING_SERVER_TQUEUE_H
