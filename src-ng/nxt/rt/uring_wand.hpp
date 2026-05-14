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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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
        auto op = operation{};
        op.what = operation_kind::manual;
        op.void_result = state;
        operations_.emplace(token, std::move(op));
        staged_.push_back(token);
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
        auto op = operation{};
        op.what = operation_kind::openat;
        op.open_result = state;
        op.dirfd = wish.dirfd;
        op.path = std::move(wish.path);
        op.flags = wish.flags;
        op.mode = wish.mode;
        operations_.emplace(token, std::move(op));
        staged_.push_back(token);
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
        auto op = operation{};
        op.what = operation_kind::read;
        op.size_result = state;
        op.fd = wish.fd;
        op.buffer = wish.buffer;
        op.offset = wish.offset;
        operations_.emplace(token, std::move(op));
        staged_.push_back(token);
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
        trace("uring wave staged=" + std::to_string(staged_.size()));
        for (auto token : staged_) {
            auto * sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                throw std::runtime_error{"io_uring submission queue is full"};

            auto & op = operations_.at(token);
            switch (op.what) {
            case operation_kind::manual:
                io_uring_prep_nop(sqe);
                break;
            case operation_kind::openat:
                io_uring_prep_openat(
                    sqe,
                    op.dirfd,
                    op.path.c_str(),
                    op.flags,
                    op.mode);
                break;
            case operation_kind::read:
                io_uring_prep_read(
                    sqe,
                    op.fd,
                    op.buffer.data(),
                    op.buffer.size(),
                    op.offset);
                break;
            }
            io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(token));
        }

        staged_.clear();

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
        auto found = operations_.find(token);
        if (found == operations_.end())
            return;

        auto & op = found->second;
        if (result < 0) {
            auto exception = std::make_exception_ptr(
                std::runtime_error{
                    "io_uring operation failed: "
                    + std::to_string(-result)});
            set_exception(op, exception);
        } else {
            set_value(op, result);
        }

        operations_.erase(found);
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
    enum class operation_kind
    {
        manual,
        openat,
        read,
    };

    struct operation
    {
        operation_kind what = operation_kind::manual;
        std::shared_ptr<wait_state<void>> void_result{};
        std::shared_ptr<wait_state<int>> open_result{};
        std::shared_ptr<wait_state<std::size_t>> size_result{};
        int dirfd = AT_FDCWD;
        std::string path{};
        int flags = O_RDONLY;
        mode_t mode = 0;
        int fd = -1;
        std::span<std::byte> buffer{};
        off_t offset = -1;
    };

    static void set_exception(
        operation & op,
        std::exception_ptr exception) noexcept
    {
        if (op.void_result)
            op.void_result->set_exception(exception);
        if (op.open_result)
            op.open_result->set_exception(exception);
        if (op.size_result)
            op.size_result->set_exception(exception);
    }

    static void set_value(operation & op, int result)
    {
        switch (op.what) {
        case operation_kind::manual:
            op.void_result->set_value();
            break;
        case operation_kind::openat:
            op.open_result->set_value(result);
            break;
        case operation_kind::read:
            op.size_result->set_value(static_cast<std::size_t>(result));
            break;
        }
    }

    io_uring ring_{};
    wait_token next_token_ = 1;
    std::vector<wait_token> staged_;
    std::unordered_map<wait_token, operation> operations_;
    std::unordered_map<wait_token, parked_task> waiters_;
};

#endif

} // namespace nxt::rt
