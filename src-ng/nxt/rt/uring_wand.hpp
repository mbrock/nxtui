#pragma once

#include "nxt/rt/task.hpp"

#if __has_include(<liburing.h>)
#define NXT_RT_HAS_LIBURING 1
#else
#define NXT_RT_HAS_LIBURING 0
#endif

#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <exception>
#include <ranges>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <unordered_set>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#if NXT_RT_HAS_LIBURING
#include <liburing.h>
#endif

namespace nxt::rt {

inline constexpr bool has_liburing_wand = NXT_RT_HAS_LIBURING != 0;

#if NXT_RT_HAS_LIBURING

class uring_wand;

class uring_submission
{
public:
    uring_submission(
        uring_wand & wand,
        deck & d,
        wait_token token) noexcept
        : wand_(wand)
        , deck_(d)
        , token_(token)
    {}

    io_uring_sqe * get_sqe();
    void attach(io_uring_sqe * sqe) const noexcept;
    void complete_sync(int result);

    [[nodiscard]] wait_token token() const noexcept
    {
        return token_;
    }

private:
    uring_wand & wand_;
    deck & deck_;
    wait_token token_;
};

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
        found->second->request_cancel();
        if (found->second->submitted()
            && !pending_cancellations_.insert(token).second)
            return;
        trace("uring request cancel token=" + std::to_string(token));
    }

    void wave(deck & d) override
    {
        stage_submissions(d);
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

            handle_cqe(d, cqe);
        }
    }

    void wait(deck & d)
    {
        auto * cqe = static_cast<io_uring_cqe *>(nullptr);
        while (true) {
            auto rc = io_uring_wait_cqe(&ring_, &cqe);
            if (rc == -EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "io_uring_wait_cqe failed: " + std::to_string(-rc)};
            break;
        }

        if (cqe != nullptr)
            handle_cqe(d, cqe);
        poll(d);
    }

    template<typename T>
    void run_until_done(deck & d, task<T> & root)
    {
        while (!root.done()) {
            if (!d.empty())
                d.run_ready();
            poll(d);
            if (d.empty() && !root.done()) {
                if (has_pending_work()) {
                    wave(d);
                    poll(d);
                    continue;
                }
                if (!has_submitted_completions())
                    throw runtime_error{"nxt::rt uring wand deadlock"};
                wait(d);
            }
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

protected:
    wait_token prepare_wish(
        deck &,
        detail::promise_base &,
        detail::prepared_wish packet) override
    {
        return std::visit(
            [this, &packet](auto & wish) -> wait_token {
                return prepare_uring_wish(
                    std::move(wish),
                    std::move(packet.state));
            },
            packet.wish);
    }

private:
    friend class uring_submission;

    void handle_cqe(deck & d, io_uring_cqe * cqe)
    {
        auto token =
            static_cast<wait_token>(io_uring_cqe_get_data64(cqe));
        auto result = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (is_cancel_token(token)) {
            trace("uring cancel complete token="
                + std::to_string(original_token(token))
                + " result=" + std::to_string(result));
            return;
        }

        trace("uring complete token=" + std::to_string(token)
            + " result=" + std::to_string(result));

        complete(d, token, result);
    }

    using uring_wish = wish_variant;

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

        void mark_submitted() noexcept
        {
            submitted_ = true;
        }

        [[nodiscard]] bool submitted() const noexcept
        {
            return submitted_;
        }

        [[nodiscard]] bool cancel_requested() const noexcept
        {
            return cancel_requested_;
        }

        [[nodiscard]] uring_wish & request() noexcept
        {
            return *request_;
        }

    protected:
        std::shared_ptr<uring_wish> request_;
        bool cancel_requested_ = false;
        bool submitted_ = false;
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
                    if (std::holds_alternative<op::timeout>(*this->request_)
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
                } else if constexpr (std::is_same_v<T, statx_result>) {
                    state_->set_value(std::get<op::statx>(*this->request_).result);
                } else {
                    static_assert(std::is_void_v<T>, "unsupported uring result");
                }
            }
        }

    private:
        std::shared_ptr<wait_state<T>> state_;
    };

    template<typename Wish>
    wait_token prepare_uring_wish(
        Wish wish,
        std::shared_ptr<void> erased_state)
    {
        auto token = wait_token{0};
        if constexpr (std::is_same_v<Wish, op::manual>)
            token = wish.token;
        if (token == 0)
            token = next_token_++;

        using result_type = typename Wish::result_type;
        auto state =
            std::static_pointer_cast<wait_state<result_type>>(erased_state);
        auto request = std::make_shared<uring_wish>(std::move(wish));
        completions_.emplace(
            token,
            std::make_unique<completion<result_type>>(request, state));
        pending_submissions_.push_back(token);
        trace("uring prepare " + std::string{Wish::name}
            + " token=" + std::to_string(token));
        return token;
    }

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

    void stage_submissions(deck & d)
    {
        auto tokens = std::vector<wait_token>{};
        tokens.swap(pending_submissions_);

        for (auto token : tokens) {
            auto found = completions_.find(token);
            if (found == completions_.end())
                continue;

            auto & completion = *found->second;
            if (completion.cancel_requested()) {
                completion.complete(-ECANCELED);
                completions_.erase(found);
                fulfill(d, token);
                continue;
            }

            if (stage_submission(d, token, completion.request()))
                completion.mark_submitted();
        }
    }

    bool stage_submission(deck & d, wait_token token, uring_wish & request)
    {
        auto submission = uring_submission{*this, d, token};
        return std::visit(
            [&submission](auto & op) {
                return op.stage_uring(submission);
            },
            request);
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

    [[nodiscard]] bool has_submitted_completions() const noexcept
    {
        return std::ranges::any_of(
            completions_,
            [](auto const & entry) {
                return entry.second->submitted();
            });
    }

    [[nodiscard]] bool has_pending_work() const noexcept
    {
        return !pending_submissions_.empty() || !pending_cancellations_.empty();
    }

    io_uring ring_{};
    wait_token next_token_ = 1;
    std::unordered_map<wait_token, std::unique_ptr<completion_base>> completions_;
    std::unordered_map<wait_token, parked_task> waiters_;
    std::vector<wait_token> pending_submissions_;
    std::unordered_set<wait_token> pending_cancellations_;
};

template<typename T>
[[nodiscard]] inline T run(task<T> root)
{
    auto wand = uring_wand{};
    auto d = deck{&wand};
    d.start(root);
    wand.run_until_done(d, root);

    if constexpr (std::is_void_v<T>) {
        std::move(root).result();
    } else {
        return std::move(root).result();
    }
}

template<task_factory Fn>
[[nodiscard]] inline task_result_t<std::invoke_result_t<Fn>> run(Fn && fn)
{
    return run(std::invoke(std::forward<Fn>(fn)));
}

inline io_uring_sqe * uring_submission::get_sqe()
{
    return wand_.get_sqe();
}

inline void uring_submission::attach(io_uring_sqe * sqe) const noexcept
{
    uring_wand::attach_token(sqe, token_);
}

inline void uring_submission::complete_sync(int result)
{
    wand_.complete(deck_, token_, result);
}

inline bool op::manual::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_nop(sqe);
    submission.attach(sqe);
    return true;
}

inline bool op::openat::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_openat(
        sqe,
        dirfd,
        path.c_str(),
        flags,
        mode);
    submission.attach(sqe);
    return true;
}

inline bool op::statx::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_statx(
        sqe,
        dirfd,
        path.c_str(),
        flags,
        mask,
        &result);
    submission.attach(sqe);
    return true;
}

inline bool op::getdents64::stage_uring(uring_submission & submission)
{
    auto result = ::syscall(
        SYS_getdents64,
        fd,
        buffer.data(),
        buffer.size());
    if (result < 0)
        result = -errno;

    trace("uring complete sync getdents64 token="
        + std::to_string(submission.token())
        + " result=" + std::to_string(result));
    submission.complete_sync(static_cast<int>(result));
    return false;
}

inline bool op::read_some::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_read(
        sqe,
        fd,
        buffer.data(),
        buffer.size(),
        offset);
    submission.attach(sqe);
    return true;
}

inline bool op::write_some::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_write(
        sqe,
        fd,
        buffer.data(),
        buffer.size(),
        offset);
    submission.attach(sqe);
    return true;
}

inline bool op::recv_some::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_recv(
        sqe,
        fd,
        buffer.data(),
        buffer.size(),
        flags);
    submission.attach(sqe);
    return true;
}

inline bool op::send_some::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_send(
        sqe,
        fd,
        buffer.data(),
        buffer.size(),
        flags);
    submission.attach(sqe);
    return true;
}

inline bool op::connect::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_connect(
        sqe,
        fd,
        sockaddr_ptr(),
        address_size);
    submission.attach(sqe);
    return true;
}

inline bool op::poll::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_poll_add(sqe, fd, events);
    submission.attach(sqe);
    return true;
}

inline bool op::timeout::stage_uring(uring_submission & submission)
{
    auto * sqe = submission.get_sqe();
    io_uring_prep_timeout(
        sqe,
        &duration,
        0,
        IORING_TIMEOUT_ETIME_SUCCESS);
    submission.attach(sqe);
    return true;
}

inline bool op::poll_until::stage_uring(uring_submission & submission)
{
    auto * poll_sqe = submission.get_sqe();
    io_uring_prep_poll_add(poll_sqe, fd, events);
    poll_sqe->flags |= IOSQE_IO_LINK;
    submission.attach(poll_sqe);

    auto * timeout_sqe = submission.get_sqe();
    io_uring_prep_link_timeout(
        timeout_sqe,
        &timeout,
        IORING_TIMEOUT_ETIME_SUCCESS);
    submission.attach(timeout_sqe);
    return true;
}

#endif

} // namespace nxt::rt
