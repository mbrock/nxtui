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
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

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
            throw runtime_error{
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

    waiter<std::size_t> prepare(
        deck &,
        detail::promise_base &,
        recv_some_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<std::size_t>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto const & op = std::get<recv_some_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_recv(
            sqe,
            op.fd,
            op.buffer.data(),
            op.buffer.size(),
            op.flags);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<std::size_t>>(request, state));
        trace("uring prepare recv token=" + std::to_string(token));
        return waiter<std::size_t>{*this, token, state};
    }

    waiter<std::size_t> prepare(
        deck &,
        detail::promise_base &,
        send_some_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<std::size_t>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto const & op = std::get<send_some_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_send(
            sqe,
            op.fd,
            op.buffer.data(),
            op.buffer.size(),
            op.flags);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<std::size_t>>(request, state));
        trace("uring prepare send token=" + std::to_string(token));
        return waiter<std::size_t>{*this, token, state};
    }

    waiter<void> prepare(
        deck &,
        detail::promise_base &,
        connect_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<void>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto const & op = std::get<connect_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_connect(
            sqe,
            op.fd,
            op.sockaddr_ptr(),
            op.address_size);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<void>>(request, state));
        trace("uring prepare connect token=" + std::to_string(token));
        return waiter<void>{*this, token, state};
    }

    waiter<int> prepare(
        deck &,
        detail::promise_base &,
        poll_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<int>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto const & op = std::get<poll_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_poll_add(sqe, op.fd, op.events);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<int>>(request, state));
        trace("uring prepare poll token=" + std::to_string(token));
        return waiter<int>{*this, token, state};
    }

    waiter<void> prepare(
        deck &,
        detail::promise_base &,
        timeout_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<void>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto & op = std::get<timeout_wish>(*request);
        auto * sqe = get_sqe();
        io_uring_prep_timeout(sqe, &op.duration, 0, IORING_TIMEOUT_ETIME_SUCCESS);
        attach_token(sqe, token);
        completions_.emplace(
            token,
            std::make_unique<completion<void>>(request, state));
        trace("uring prepare timeout token=" + std::to_string(token));
        return waiter<void>{*this, token, state};
    }

    waiter<poll_until_result> prepare(
        deck &,
        detail::promise_base &,
        poll_until_wish wish) override
    {
        auto token = next_token_++;
        auto state = std::make_shared<wait_state<poll_until_result>>();
        auto request = std::make_shared<uring_wish>(wish);
        auto & op = std::get<poll_until_wish>(*request);

        auto * poll_sqe = get_sqe();
        io_uring_prep_poll_add(poll_sqe, op.fd, op.events);
        poll_sqe->flags |= IOSQE_IO_LINK;
        attach_token(poll_sqe, token);

        auto * timeout_sqe = get_sqe();
        io_uring_prep_link_timeout(
            timeout_sqe,
            &op.timeout,
            IORING_TIMEOUT_ETIME_SUCCESS);
        attach_token(timeout_sqe, token);

        completions_.emplace(
            token,
            std::make_unique<completion<poll_until_result>>(request, state));
        trace("uring prepare poll-until token=" + std::to_string(token));
        return waiter<poll_until_result>{*this, token, state};
    }

    void suspend(wait_token token, parked_task task) override
    {
        trace("uring park token=" + std::to_string(token));
        waiters_.emplace(token, task);
    }

    void cancel(wait_token token) override
    {
        auto found = completions_.find(token);
        if (found == completions_.end())
            return;
        if (!pending_cancellations_.insert(token).second)
            return;

        found->second->request_cancel();
        trace("uring request cancel token=" + std::to_string(token));
    }

    void wave(deck &) override
    {
        stage_cancellations();
        trace("uring wave submit");
        auto rc = io_uring_submit(&ring_);
        if (rc < 0)
            throw runtime_error{
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
                throw runtime_error{
                    "io_uring_peek_cqe failed: " + std::to_string(-rc)};
            if (cqe == nullptr)
                return;

            auto token =
                static_cast<wait_token>(io_uring_cqe_get_data64(cqe));
            auto result = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (is_cancel_token(token)) {
                trace("uring cancel complete token="
                    + std::to_string(original_token(token))
                    + " result=" + std::to_string(result));
                continue;
            }

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
        read_some_wish,
        recv_some_wish,
        send_some_wish,
        connect_wish,
        poll_wish,
        timeout_wish,
        poll_until_wish>;

    class completion_base
    {
    public:
        explicit completion_base(std::shared_ptr<uring_wish> request)
            : request_(std::move(request))
        {}

        virtual ~completion_base() = default;
        virtual void complete(int result) = 0;

        void request_cancel() noexcept
        {
            cancel_requested_ = true;
        }

        [[nodiscard]] bool cancel_requested() const noexcept
        {
            return cancel_requested_;
        }

    protected:
        std::shared_ptr<uring_wish> request_;
        bool cancel_requested_ = false;
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
            if (this->cancel_requested_) {
                state_->set_exception(
                    std::make_exception_ptr(operation_cancelled{}));
                return;
            }

            if constexpr (std::is_same_v<T, poll_until_result>) {
                if (result > 0) {
                    state_->set_value(poll_until_result{
                        .events = result,
                        .timed_out = false,
                    });
                } else if (result == 0 || result == -ETIME || result == -ECANCELED) {
                    state_->set_value(poll_until_result{
                        .events = 0,
                        .timed_out = true,
                    });
                } else {
                    state_->set_exception(
                        std::make_exception_ptr(
                            runtime_error{
                                "io_uring operation failed: "
                                + std::to_string(-result)}));
                }
            } else {
                if constexpr (std::is_void_v<T>) {
                    if (std::holds_alternative<timeout_wish>(*this->request_)
                        && result == -ETIME) {
                        state_->set_value();
                        return;
                    }
                }

                if (result < 0) {
                    state_->set_exception(
                        std::make_exception_ptr(
                            runtime_error{
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
        }

    private:
        std::shared_ptr<wait_state<T>> state_;
    };

    io_uring_sqe * get_sqe()
    {
        auto * sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
            throw runtime_error{"io_uring submission queue is full"};
        return sqe;
    }

    static void attach_token(io_uring_sqe * sqe, wait_token token) noexcept
    {
        io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(token));
    }

    void stage_cancellations()
    {
        for (auto token : pending_cancellations_) {
            auto * sqe = get_sqe();
            io_uring_prep_cancel64(
                sqe,
                static_cast<std::uint64_t>(token),
                0);
            attach_token(sqe, cancel_token(token));
            trace("uring prepare cancel token=" + std::to_string(token));
        }
        pending_cancellations_.clear();
    }

    static constexpr wait_token cancel_token_bit =
        wait_token{1} << (sizeof(wait_token) * 8 - 1);

    static wait_token cancel_token(wait_token token) noexcept
    {
        return token | cancel_token_bit;
    }

    static bool is_cancel_token(wait_token token) noexcept
    {
        return (token & cancel_token_bit) != 0;
    }

    static wait_token original_token(wait_token token) noexcept
    {
        return token & ~cancel_token_bit;
    }

    io_uring ring_{};
    wait_token next_token_ = 1;
    std::unordered_map<wait_token, std::unique_ptr<completion_base>> completions_;
    std::unordered_map<wait_token, parked_task> waiters_;
    std::unordered_set<wait_token> pending_cancellations_;
};

#endif

} // namespace nxt::rt
