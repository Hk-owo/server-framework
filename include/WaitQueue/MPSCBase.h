//
// Created by lacas on 2026/3/10.
//

#ifndef WEBPROJECT_MPSCBASE_H
#define WEBPROJECT_MPSCBASE_H

#include <atomic>
#include <cstddef>
#include <utility>

/*
 * MPSC队列 多生产者单消费者
 */
template<typename T>
class MPSCBase{
private:
    // 队列节点
    struct node {
        T data;
        std::atomic<node*> next{nullptr};
        node():data(){}
        explicit node(T data):data(data){}
    };

    std::atomic<node*> head_;
    std::atomic<node*> tail_;
    std::atomic<size_t> size_;
public:
    MPSCBase();
    virtual ~MPSCBase();
    MPSCBase(const MPSCBase&) = delete;
    MPSCBase& operator=(const MPSCBase&) = delete;
    virtual bool enqueue(T& task);
    virtual bool dequeue(T& task);
    size_t size() const;
};

template<typename T>
MPSCBase<T>::~MPSCBase() {
    while (node* old_head = head_.load(std::memory_order_relaxed)) {
        head_.store(old_head->next.load(std::memory_order_relaxed), std::memory_order_relaxed);
        delete old_head;
    }
}

template<typename T>
size_t MPSCBase<T>::size() const {
    return size_.load(std::memory_order_acquire);
}

template<typename T>
MPSCBase<T>::MPSCBase() {
    // 创建哨兵节点
    node* dummy = new node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
    size_.store(0, std::memory_order_relaxed);
}

template<typename T>
bool MPSCBase<T>::enqueue(T& task) {
    node* new_node = new node();
    new_node->data = std::move(task);

    node* tail = tail_.load(std::memory_order_acquire);  //读取 tail

    while (true) {
        node* next = tail->next.load(std::memory_order_acquire);

        if (next == nullptr) {
            // 尝试将新节点链接到 tail
            if (tail->next.compare_exchange_weak(next, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                //更新 tail
                tail_.compare_exchange_strong(tail, new_node, std::memory_order_release, std::memory_order_relaxed);
                size_.fetch_add(1,std::memory_order_relaxed);
                return true;
            }
            // 失败：next 被其他线程更新了，重试
        } else {
            // 帮助推进 tail
            tail_.compare_exchange_weak(tail, next, std::memory_order_release, std::memory_order_relaxed);
            tail = tail_.load(std::memory_order_acquire);
        }
    }
}

template<typename T>
bool MPSCBase<T>::dequeue(T &task) {
    node* old_head = head_.load(std::memory_order_acquire);
    while (true) {
        node* next = old_head->next.load(std::memory_order_acquire);
        // 队列为空
        if (next == nullptr) {
            return false;
        }
        // 尝试移动 head
        if (head_.compare_exchange_weak(old_head, next, std::memory_order_release, std::memory_order_acquire)) {
            // 成功获取节点
            task = std::move(next->data);
            delete old_head;
            size_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        old_head = head_.load(std::memory_order_acquire);
    }
}

#endif //WEBPROJECT_MPSCBASE_H
