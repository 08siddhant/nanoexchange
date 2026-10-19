#pragma once
// Bounded multi-producer / single-consumer queue.
// Network threads push commands, the symbol's engine thread pops them.
// Bounded so a flood of orders turns into "busy" rejections (backpressure)
// instead of unbounded memory growth.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace nx {

template <class T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {}

    // Returns false if the queue is full.
    bool tryPush(T item) {
        {
            std::lock_guard lk(m_);
            if (q_.size() >= capacity_) return false;
            q_.push_back(std::move(item));
        }
        cv_.notify_one();
        return true;
    }

    // Always enqueues (used for control messages like Stop).
    void push(T item) {
        {
            std::lock_guard lk(m_);
            q_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    template <class Rep, class Period>
    std::optional<T> popFor(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lk(m_);
        if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty(); })) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        return item;
    }

    std::size_t size() const {
        std::lock_guard lk(m_);
        return q_.size();
    }

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::deque<T>           q_;
    std::size_t             capacity_;
};

}  // namespace nx
