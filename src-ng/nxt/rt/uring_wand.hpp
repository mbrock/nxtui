#pragma once

#include "nxt/rt/task.hpp"

#if __has_include(<liburing.h>)
#define NXT_RT_HAS_LIBURING 1
#else
#define NXT_RT_HAS_LIBURING 0
#endif

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if NXT_RT_HAS_LIBURING
#include <liburing.h>
#endif

namespace nxt::rt {

inline constexpr bool has_liburing_wand = NXT_RT_HAS_LIBURING != 0;

#if NXT_RT_HAS_LIBURING

/// Minimal io_uring-backed wand.
///
/// This first version only knows how to turn `manual_wish` into staged NOP
/// SQEs. That is enough to exercise the intended rhythm: prepare while the
/// coroutine runs, park the coroutine at suspension, wave the wand to submit,
/// then poll completions and requeue fulfilled tasks onto the deck.
class uring_wand final : public wand
{
public:
    explicit uring_wand(unsigned queue_depth = 64)
    {
        auto rc = io_uring_queue_init(queue_depth, &ring_, 0);
        if (rc < 0)
            throw std::runtime_error{
                "io_uring_queue_init failed: " + std::to_string(-rc)};
    }

    uring_wand(const uring_wand &) = delete;
    uring_wand & operator=(const uring_wand &) = delete;
    uring_wand(uring_wand &&) = delete;
    uring_wand & operator=(uring_wand &&) = delete;

    ~uring_wand() override
    {
        io_uring_queue_exit(&ring_);
    }

    waiter<void> prepare(
        deck &,
        detail::promise_base &,
        manual_wish wish) override
    {
        auto token = wish.token;
        if (token == 0)
            token = next_token_++;

        staged_manual_.push_back(token);
        return waiter<void>{*this, token};
    }

    void suspend(wait_token token, parked_task task) override
    {
        waiters_.emplace(token, task);
    }

    void wave(deck &) override
    {
        for (auto token : staged_manual_) {
            auto * sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                throw std::runtime_error{"io_uring submission queue is full"};

            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(token));
        }

        staged_manual_.clear();

        auto rc = io_uring_submit(&ring_);
        if (rc < 0)
            throw std::runtime_error{
                "io_uring_submit failed: " + std::to_string(-rc)};
    }

    /// Poll available completions and requeue fulfilled tasks.
    void poll(deck & d)
    {
        while (true) {
            io_uring_cqe * cqe = nullptr;
            auto rc = io_uring_peek_cqe(&ring_, &cqe);
            if (rc == -EAGAIN)
                return;
            if (rc < 0)
                throw std::runtime_error{
                    "io_uring_peek_cqe failed: " + std::to_string(-rc)};
            if (cqe == nullptr)
                return;

            auto token =
                static_cast<wait_token>(io_uring_cqe_get_data64(cqe));
            auto result = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (result < 0)
                throw std::runtime_error{
                    "io_uring operation failed: " + std::to_string(-result)};

            fulfill(d, token);
        }
    }

    void fulfill(deck & d, wait_token token)
    {
        auto found = waiters_.find(token);
        if (found == waiters_.end())
            return;

        auto task = found->second;
        waiters_.erase(found);
        task.resume(d);
    }

private:
    io_uring ring_{};
    wait_token next_token_ = 1;
    std::vector<wait_token> staged_manual_;
    std::unordered_map<wait_token, parked_task> waiters_;
};

#endif

} // namespace nxt::rt
