#pragma once

#include "nxt/rt/task.hpp"

#if __has_include(<liburing.h>)
#define NXT_RT_HAS_LIBURING 1
#else
#define NXT_RT_HAS_LIBURING 0
#endif

#include <cerrno>
#include <exception>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#if NXT_RT_HAS_LIBURING
#include <liburing.h>
#endif

namespace nxt::rt {

inline constexpr bool has_liburing_wand = NXT_RT_HAS_LIBURING != 0;

#if NXT_RT_HAS_LIBURING

/// Minimal io_uring-backed wand.
///
/// This early version turns closed wish objects into staged SQEs. `wave()`
/// submits staged work, and `poll()` drains completions, stores typed results,
/// and requeues fulfilled tasks onto the deck.
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

        auto state = std::make_shared<wait_state<void>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto * sqe = get_sqe();
        io_uring_prep_nop(sqe);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<void>>(request, state));
        trace("uring prepare manual token=" + std::to_string(token));
        return waiter<void>{*this, token, state};
    }

    waiter<int> prepare(
        deck &,
        detail::promise_base &,
        openat_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<int>>();
        auto request = std::make_shared<uring_wish>(std::move(wish));
        auto const & op = std::get<openat_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_openat(
            sqe,
            op.dirfd,
            op.path.c_str(),
            op.flags,
            op.mode);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<int>>(request, state));
        trace("uring prepare openat token=" + std::to_string(token));
        return waiter<int>{*this, token, state};
    }

    waiter<std::size_t> prepare(
        deck &,
        detail::promise_base &,
        read_some_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<std::size_t>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto const & op = std::get<read_some_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_read(
            sqe,
            op.fd,
            op.buffer.data(),
            op.buffer.size(),
            op.offset);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<std::size_t>>(request, state));
        trace("uring prepare read token=" + std::to_string(token));
        return waiter<std::size_t>{*this, token, state};
    }

    void suspend(wait_token token, parked_task task) override
    {
        trace("uring park token=" + std::to_string(token));
        waiters_.emplace(token, task);
    }

    void wave(deck &) override
    {
        trace("uring wave submit");
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

            trace("uring complete token=" + std::to_string(token)
                + " result=" + std::to_string(result));

            complete(d, token, result);
        }
    }

    void complete(deck & d, wait_token token, int result)
    {
        auto found = completions_.find(token);
        if (found == completions_.end())
            return;

        found->second->complete(result);
        completions_.erase(found);
        fulfill(d, token);
    }

    void fulfill(deck & d, wait_token token)
    {
        auto found = waiters_.find(token);
        if (found == waiters_.end())
            return;

        trace("uring fulfill token=" + std::to_string(token));
        auto task = found->second;
        waiters_.erase(found);
        task.resume(d);
    }

private:
    using uring_wish = std::variant<
        manual_wish,
        openat_wish,
        read_some_wish>;

    class completion_base
    {
    public:
        explicit completion_base(std::shared_ptr<uring_wish> request)
            : request_(std::move(request))
        {}

        virtual ~completion_base() = default;
        virtual void complete(int result) = 0;

    protected:
        std::shared_ptr<uring_wish> request_;
    };

    template<typename T>
    class completion final : public completion_base
    {
    public:
        completion(
            std::shared_ptr<uring_wish> request,
            std::shared_ptr<wait_state<T>> state)
            : completion_base(std::move(request))
            , state_(std::move(state))
        {}

        void complete(int result) override
        {
            if (result < 0) {
                state_->set_exception(
                    std::make_exception_ptr(
                        std::runtime_error{
                            "io_uring operation failed: "
                            + std::to_string(-result)}));
                return;
            }

            if constexpr (std::is_void_v<T>) {
                state_->set_value();
            } else if constexpr (std::is_same_v<T, int>) {
                state_->set_value(result);
            } else if constexpr (std::is_same_v<T, std::size_t>) {
                state_->set_value(static_cast<std::size_t>(result));
            } else {
                static_assert(std::is_void_v<T>, "unsupported uring result");
            }
        }

    private:
        std::shared_ptr<wait_state<T>> state_;
    };

    io_uring_sqe * get_sqe()
    {
        auto * sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
            throw std::runtime_error{"io_uring submission queue is full"};
        return sqe;
    }

    static void attach_token(io_uring_sqe * sqe, wait_token token) noexcept
    {
        io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(token));
    }

    io_uring ring_{};
    wait_token next_token_ = 1;
    std::unordered_map<wait_token, std::unique_ptr<completion_base>> completions_;
    std::unordered_map<wait_token, parked_task> waiters_;
};

#endif

} // namespace nxt::rt
