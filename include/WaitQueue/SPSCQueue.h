//
// Created by lacas on 2025/12/10.
//

#ifndef IO_URING_SERVER_WAIT_QUEUE_H
#define IO_URING_SERVER_WAIT_QUEUE_H

#include "WaitQueue/SPSCBase.h"
#include <liburing.h>

template<typename T>
class SPSCQueue : public SPSCBase<T>{
private:
    static const int SIZE = 128;
    //这一部分是启动信号处理
    struct io_uring ring;
    struct io_uring_cqe* cqe;
    int ring_fd, peek_times = 1;
    std::atomic<bool> suspend{true};

    T result{};
public:
    SPSCQueue();
    ~SPSCQueue();
    //唤醒
    void on_data_ready_uring() noexcept;
    //等待
    void wait_for_data_uring() noexcept;
    //阻塞唤醒
    void notify_stop_uring() noexcept;
    //非阻塞查看
    void peek_uring() noexcept;
    //阻塞等待
    void wait_uring() noexcept;
    bool dequeue(T& task);
};

template<typename T>
void SPSCQueue<T>::wait_uring() noexcept {
    io_uring_wait_cqe(&ring,&cqe);
    if(cqe->user_data)
        return;
    io_uring_cqe_seen(&ring,cqe);
}

template<typename T>
bool SPSCQueue<T>::dequeue(T &task) {
    bool res = SPSCBase<T>::dequeue(task);
    if(!res)
        peek_times++;
    else peek_times = 1;
    return res;
}

template<typename T>
void SPSCQueue<T>::peek_uring() noexcept {
    cqe = nullptr;
    io_uring_peek_cqe(&ring,&cqe);
    if(cqe)
        io_uring_cqe_seen(&ring,cqe);
}

template<typename T>
void SPSCQueue<T>::notify_stop_uring() noexcept {
    io_uring_sqe* stop = io_uring_get_sqe(&ring);
    stop->user_data = 1;
    io_uring_prep_nop(stop);
    io_uring_submit(&ring);
}

template<typename T>
void SPSCQueue<T>::wait_for_data_uring() noexcept {
    suspend.store(true,std::memory_order_release);
    if(!(peek_times & ((1 << 18) - 1 ))){
        peek_times = 1;
        wait_uring();
    }
    else peek_uring();
    suspend.store(false,std::memory_order_release);
}

template<typename T>
void SPSCQueue<T>::on_data_ready_uring() noexcept {
    if(suspend.load(std::memory_order_acquire)) {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        sqe->user_data = 0;
        io_uring_submit(&ring);
    }
}

template<typename T>
SPSCQueue<T>::~SPSCQueue() {
    io_uring_queue_exit(&ring);
    cqe = nullptr;
}

template<typename T>
SPSCQueue<T>::SPSCQueue() {
    ring_fd = io_uring_queue_init(16, &ring, 0);
    if(ring_fd < 0){
        std::cerr << "create fd faild.\n";
        return;
    }
}

#endif //IO_URING_SERVER_WAIT_QUEUE_H