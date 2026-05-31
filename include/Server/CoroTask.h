//
// Created by lacas on 2026/3/19.
//

#ifndef WEBPROJECT_COROTASK_H
#define WEBPROJECT_COROTASK_H

#include <coroutine>
#include <utility>
#include <exception>
#include <variant>

namespace coro {
    template <typename T = void>
    class Task {
    public:
        struct promise_type {
            std::variant<std::monostate, T, std::exception_ptr> result;
            // 也可以支持 co_yield 其他类型
            template<typename Awaitable>
            auto yield_value(Awaitable&& awaitable) {
                return std::forward<Awaitable>(awaitable);
            }

            inline auto get_return_object();
            inline std::suspend_always initial_suspend() noexcept;
            inline std::suspend_always final_suspend() noexcept;

            template <typename U>
            inline void return_value(U&& val);
            inline void unhandled_exception();
        };

        using Handle = std::coroutine_handle<promise_type>;

        inline Task(Task&& o) noexcept;
        inline Task& operator=(Task&& o) noexcept;
        inline ~Task();

        // 控制接口
        inline bool resume();
        inline bool done() const noexcept;
        inline T get();

    private:
        explicit inline Task(Handle h);
        Handle handle_ = nullptr;
    };

    template <typename T>
    inline auto Task<T>::promise_type::get_return_object() {
        return Task{Handle::from_promise(*this)};
    }

    template <typename T>
    inline std::suspend_always Task<T>::promise_type::initial_suspend() noexcept {
        return {};
    }

    template <typename T>
    inline std::suspend_always Task<T>::promise_type::final_suspend() noexcept {
        return {};
    }

    template <typename T>
    template <typename U>
    inline void Task<T>::promise_type::return_value(U&& val) {
        result.template emplace<T>(std::forward<U>(val));
    }

    template <typename T>
    inline void Task<T>::promise_type::unhandled_exception() {
        result.template emplace<std::exception_ptr>(std::current_exception());
    }

    template <typename T>
    inline Task<T>::Task(Handle h) : handle_(h) {}

    template <typename T>
    inline Task<T>::Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}

    template <typename T>
    inline Task<T>& Task<T>::operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    template <typename T>
    inline Task<T>::~Task() {
        if (handle_) handle_.destroy();
    }

    template <typename T>
    inline bool Task<T>::resume() {
        if (!handle_ || handle_.done()) return false;
        handle_.resume();
        return !handle_.done();
    }

    template <typename T>
    inline bool Task<T>::done() const noexcept {
        return !handle_ || handle_.done();
    }

    template <typename T>
    inline T Task<T>::get() {
        while (!done()) resume();
        if (std::holds_alternative<std::exception_ptr>(handle_.promise().result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(handle_.promise().result));
        }
        return std::get<T>(handle_.promise().result);
    }

// void 特化
    template <>
    class Task<void> {
    public:
        struct promise_type {
            std::exception_ptr exception;

            template<typename Awaitable>
            inline auto yield_value(Awaitable&& awaitable) {return std::forward<Awaitable>(awaitable);}
            inline auto get_return_object();
            inline std::suspend_always initial_suspend() noexcept;
            inline std::suspend_always final_suspend() noexcept;
            inline void return_void();
            inline void unhandled_exception();
        };

        using Handle = std::coroutine_handle<promise_type>;

        inline Task(Task&& o) noexcept;
        inline Task& operator=(Task&& o) noexcept;
        inline ~Task();

        inline bool resume();
        inline bool done() const noexcept;
        inline void get();

        std::coroutine_handle<> raw_handle();

    private:
        explicit inline Task(Handle h);
        Handle handle_ = nullptr;
    };

    inline auto Task<void>::promise_type::get_return_object() {
        return Task{Handle::from_promise(*this)};
    }

    inline std::suspend_always Task<void>::promise_type::initial_suspend() noexcept { return {}; }
    inline std::suspend_always Task<void>::promise_type::final_suspend() noexcept { return {}; }
    inline void Task<void>::promise_type::return_void() {}
    inline void Task<void>::promise_type::unhandled_exception() { exception = std::current_exception(); }

    inline Task<void>::Task(Handle h) : handle_(h) {}
    inline Task<void>::Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}

    inline Task<void>& Task<void>::operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    inline Task<void>::~Task() { if (handle_) handle_.destroy(); }

    inline bool Task<void>::resume() {
        if (!handle_ || handle_.done()) return false;
        handle_.resume();
        return !handle_.done();
    }

    inline bool Task<void>::done() const noexcept { return !handle_ || handle_.done(); }

    inline void Task<void>::get() {
        while (!done()) resume();
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

    inline std::coroutine_handle<> Task<void>::raw_handle() {
        return handle_;
    }

} // namespace coro

#endif //WEBPROJECT_COROTASK_H
