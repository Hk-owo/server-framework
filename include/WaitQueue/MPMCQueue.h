//
// Created by lacas on 2026/9/18.
//

#ifndef IO_URING_SERVER_MPMCQUEUE_H
#define IO_URING_SERVER_MPMCQUEUE_H

#include "WaitQueue/MPMCBase.h"
#include <iostream>
#include <liburing.h>

/*
 * MPMCBase + io_uring 阻塞等待
 * 有uring后缀的是用io_uring来实现的阻塞等待队列
 *
 * 和SPSCQueue/MPSCQueue一样，队列自己带一条ring做等待/唤醒；
 * 区别是等待者是多个（所有worker都挂在这一条队列上），所以：
 *   1. 等待者个数用计数器而不是bool——只有一个bool的话，最后一个退出的
 *      worker会把它清掉，此时其他worker还堵在wait里，之后入队就没人唤醒了
 *   2. CQE必须原子认领——liburing的io_uring_cqe_seen就是"*khead = *khead + 1"，
 *      多个消费者同时消费会丢更新、把CQ的head推错；而且io_uring_wait_cqe
 *      也不保证一个CQE只给一个等待者（两个线程可能拿到同一个），
 *      所以这里用CAS推进head，谁CAS成功谁拿走这个CQE
 *   3. 等待不带超时——io_uring_wait_cqe_timeout内部那个timeout sqe在真实CQE
 *      到达时会被取消，留下的-ECANCELED会堵在CQ里让下一次wait立刻返回，
 *      worker就变成空转；改用"登记等待者之后复查队列"来兜住竞态，直接阻塞
 */
template<typename T>
class MPMCQueue : public MPMCBase<T>{
private:
    static const int SIZE = 128;
    //这一部分是启动信号处理
    struct io_uring ring;
    int ring_fd;
    //正挂着等待的消费者个数
    std::atomic<int> mWaiting{0};
    //liburing的get_sqe/submit不是线程安全的，多生产者提交要串行
    std::atomic_flag mSubmitLock = ATOMIC_FLAG_INIT;

    T result{};

private:
    //原子认领一个CQE
    bool claim_cqe(struct io_uring_cqe** out) noexcept;
    //提交一个唤醒事件
    void submit_wake() noexcept;

public:
    MPMCQueue();
    ~MPMCQueue();

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
};

template<typename T>
bool MPMCQueue<T>::claim_cqe(struct io_uring_cqe** out) noexcept {
    //heads是内核共享的，只能用原子方式推进
    unsigned head = __atomic_load_n(ring.cq.khead,__ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(ring.cq.ktail,__ATOMIC_ACQUIRE);
    while(head != tail) {
        if(__atomic_compare_exchange_n(ring.cq.khead,&head,head + 1,
                                       false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            *out = &ring.cq.cqes[head & *ring.cq.kring_mask];
            return true;
        }
        //head被别的消费者抢走了，重新读尾
        tail = __atomic_load_n(ring.cq.ktail,__ATOMIC_ACQUIRE);
    }
    return false;
}

template<typename T>
void MPMCQueue<T>::submit_wake() noexcept {
    while(mSubmitLock.test_and_set(std::memory_order_acquire)) {}
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    //没有prep的话这个sqe带着残留opcode提交，唤醒事件不成立
    if(sqe) {
        io_uring_prep_nop(sqe);
        io_uring_submit(&ring);
    }
    mSubmitLock.clear(std::memory_order_release);
}

template<typename T>
void MPMCQueue<T>::wait_uring() noexcept {
    struct io_uring_cqe* cqe = nullptr;
    //先无锁认领一个，没有就进内核等一个事件；
    //多个等待者被同时唤醒时也只有一个CAS成功，其余回去继续等
    while(!claim_cqe(&cqe)) {
        io_uring_enter(ring.ring_fd,0,1,IORING_ENTER_GETEVENTS,nullptr);
    }
}

template<typename T>
void MPMCQueue<T>::peek_uring() noexcept {
    //非阻塞查看：认领到一个就消费掉，没有就直接返回
    struct io_uring_cqe* cqe = nullptr;
    claim_cqe(&cqe);
}

template<typename T>
void MPMCQueue<T>::wait_for_data_uring() noexcept {
    mWaiting.fetch_add(1,std::memory_order_acq_rel);
    //登记成等待者之后再复查一次：入队可能恰好发生在"上次取空"和"登记"之间，
    //那时生产者看不到等待者、不会提交唤醒。复查确认非空就直接返回，
    //上层会重新去dequeue，这样纯阻塞等待也不会漏事件
    if(!MPMCBase<T>::empty()) {
        mWaiting.fetch_sub(1,std::memory_order_acq_rel);
        return;
    }
    wait_uring();
    mWaiting.fetch_sub(1,std::memory_order_acq_rel);
}

template<typename T>
void MPMCQueue<T>::on_data_ready_uring() noexcept {
    //只要还有人挂着就提交一次唤醒；一次提交只唤醒一个，
    //被唤醒的worker自己去排空队列（谁空闲谁取）
    if(mWaiting.load(std::memory_order_acquire) > 0)
        submit_wake();
}

template<typename T>
void MPMCQueue<T>::notify_stop_uring() noexcept {
    //停止也是一次唤醒事件：CQE是被原子认领消费掉的，
    //所以要唤醒几个等待者就提交几次
    submit_wake();
}

template<typename T>
MPMCQueue<T>::~MPMCQueue() {
    io_uring_queue_exit(&ring);
}

template<typename T>
MPMCQueue<T>::MPMCQueue() {
    ring_fd = io_uring_queue_init(16, &ring, 0);
    if(ring_fd < 0){
        std::cerr << "create fd faild.\n";
        return;
    }
}

#endif //IO_URING_SERVER_MPMCQUEUE_H
