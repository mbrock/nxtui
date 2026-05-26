#pragma once

#include "nxt/rt/task.hpp"
#include <nxt/unique-fd.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) \
    || defined(__OpenBSD__) || defined(__DragonFly__)
#define NXT_RT_HAS_KQUEUE 1
#else
#define NXT_RT_HAS_KQUEUE 0
#endif

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <poll.h>
#include <ranges>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if NXT_RT_HAS_KQUEUE
#include <sys/event.h>
#include <sys/time.h>

#ifndef NOTE_NSECONDS
#define NOTE_NSECONDS 0
#endif
#endif

namespace nxt::rt {

inline constexpr bool has_kqueue_wand = NXT_RT_HAS_KQUEUE != 0;

#if NXT_RT_HAS_KQUEUE

using kqueue_event = struct kevent;

class kqueue_wand final : public wand
{
public:
    kqueue_wand()
        : kq_(::kqueue())
    {
        if (kq_.get() < 0)
            throw runtime_error{
                "kqueue failed: " + std::to_string(errno)};
    }

    kqueue_wand(const kqueue_wand &) = delete;
    kqueue_wand & operator=(const kqueue_wand &) = delete;
    kqueue_wand(kqueue_wand &&) = delete;
    kqueue_wand & operator=(kqueue_wand &&) = delete;

    void suspend(wait_token token, parked_task task) override
    {
        trace("kqueue park token=" + std::to_string(token));
        waiters_.emplace(token, task);
    }

    void cancel(wait_token token) override
    {
        auto found = completions_.find(token);
        if (found == completions_.end())
            return;

        found->second->request_cancel();
        pending_cancellations_.push_back(token);
        trace("kqueue request cancel token=" + std::to_string(token));
    }

    void wave(deck & d) override
    {
        stage_submissions(d);
        stage_cancellations(d);
        apply_changes();
    }

    void poll(deck & d)
    {
        auto timeout = timespec{.tv_sec = 0, .tv_nsec = 0};
        poll_with_timeout(d, &timeout);
    }

    void wait(deck & d)
    {
        poll_with_timeout(d, nullptr);
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
                    throw runtime_error{"nxt::rt kqueue wand deadlock"};
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

        trace("kqueue fulfill token=" + std::to_string(token));
        auto task = found->second;
        waiters_.erase(found);
        task.resume(d);
    }

private:
    template<typename Wish>
    wait_token prepare_kqueue_wish(
        Wish wish,
        std::shared_ptr<void> erased_state);

protected:
    wait_token prepare_wish(
        deck &,
        detail::promise_base &,
        detail::prepared_wish packet) override;

private:
    using kqueue_wish = wish_variant;

    static void set_nonblocking(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            return;
        if ((flags & O_NONBLOCK) == 0)
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static bool would_block(int err) noexcept
    {
        return err == EAGAIN || err == EWOULDBLOCK;
    }

    static void set_event(
        std::vector<kqueue_event> & changes,
        uintptr_t ident,
        short filter,
        unsigned short flags,
        unsigned int fflags,
        intptr_t data,
        wait_token token)
    {
        auto event = kqueue_event{};
        EV_SET(
            &event,
            ident,
            filter,
            flags,
            fflags,
            data,
            reinterpret_cast<void *>(static_cast<uintptr_t>(token)));
        changes.push_back(event);
    }

    static wait_token event_token(kqueue_event const & event) noexcept
    {
        return static_cast<wait_token>(
            reinterpret_cast<uintptr_t>(event.udata));
    }

    static std::int64_t nanoseconds(kernel_timespec duration) noexcept
    {
        auto value = duration.tv_sec * 1'000'000'000LL + duration.tv_nsec;
        return value <= 0 ? 1 : value;
    }

    static int poll_events_from_filter(short filter) noexcept
    {
        if (filter == EVFILT_READ)
            return POLLIN;
        if (filter == EVFILT_WRITE)
            return POLLOUT;
        return 0;
    }

    class completion_base
    {
    public:
        explicit completion_base(std::shared_ptr<kqueue_wish> request)
            : request_(std::move(request))
        {}

        virtual ~completion_base() = default;

        virtual bool submit(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            std::vector<kqueue_event> & changes) = 0;
        virtual bool on_event(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const & event) = 0;
        virtual void delete_events(
            wait_token token,
            std::vector<kqueue_event> & changes) = 0;
        virtual void complete(int result) = 0;

        [[nodiscard]] bool finished() const noexcept
        {
            return finished_;
        }

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

    protected:
        std::shared_ptr<kqueue_wish> request_;
        bool cancel_requested_ = false;
        bool submitted_ = false;
        bool finished_ = false;
    };

    template<typename T>
    class completion final : public completion_base
    {
    public:
        completion(
            std::shared_ptr<kqueue_wish> request,
            std::shared_ptr<wait_state<T>> state)
            : completion_base(std::move(request))
            , state_(std::move(state))
        {}

        bool submit(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            std::vector<kqueue_event> & changes) override
        {
            if (this->cancel_requested_) {
                finish_cancelled();
                return false;
            }

            return std::visit(
                [&](auto & op) {
                    return submit_op(wand, d, token, changes, op);
                },
                *this->request_);
        }

        bool on_event(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const & event) override
        {
            if ((event.flags & EV_ERROR) != 0) {
                finish_error(static_cast<int>(event.data));
                return true;
            }

            if (this->cancel_requested_) {
                finish_cancelled();
                return true;
            }

            return std::visit(
                [&](auto & op) {
                    return event_op(wand, d, token, event, op);
                },
                *this->request_);
        }

        void delete_events(
            wait_token token,
            std::vector<kqueue_event> & changes) override
        {
            std::visit(
                [&](auto & op) {
                    delete_op_events(token, changes, op);
                },
                *this->request_);
        }

        void complete(int result) override
        {
            if (this->cancel_requested_) {
                finish_cancelled();
                return;
            }
            finish_result(result);
        }

    private:
        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token,
            std::vector<kqueue_event> &,
            op::manual &)
        {
            finish_result(0);
            return false;
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token,
            std::vector<kqueue_event> &,
            op::openat & op)
        {
            auto fd = ::openat(op.dirfd, op.path.c_str(), op.flags, op.mode);
            finish_result(fd < 0 ? -errno : fd);
            return false;
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::read_some & op)
        {
            set_nonblocking(op.fd);
            auto result = op.offset < 0
                ? ::read(op.fd, op.buffer.data(), op.buffer.size())
                : ::pread(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_wait(
                token,
                changes,
                result,
                op.fd,
                EVFILT_READ);
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::write_some & op)
        {
            set_nonblocking(op.fd);
            auto result = op.offset < 0
                ? ::write(op.fd, op.buffer.data(), op.buffer.size())
                : ::pwrite(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_wait(
                token,
                changes,
                result,
                op.fd,
                EVFILT_WRITE);
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::recv_some & op)
        {
            set_nonblocking(op.fd);
            auto result =
                ::recv(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_wait(
                token,
                changes,
                result,
                op.fd,
                EVFILT_READ);
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::send_some & op)
        {
            set_nonblocking(op.fd);
            auto result =
                ::send(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_wait(
                token,
                changes,
                result,
                op.fd,
                EVFILT_WRITE);
        }

        bool submit_op(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::connect & op)
        {
            set_nonblocking(op.fd);
            if (::connect(op.fd, op.sockaddr_ptr(), op.address_size) == 0) {
                wand.complete(d, token, 0);
                return false;
            }

            if (errno != EINPROGRESS && !would_block(errno)) {
                wand.complete(d, token, -errno);
                return false;
            }

            set_event(
                changes,
                op.fd,
                EVFILT_WRITE,
                EV_ADD | EV_ONESHOT,
                0,
                0,
                token);
            return true;
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::poll & op)
        {
            if ((op.events & POLLIN) != 0) {
                set_event(
                    changes,
                    op.fd,
                    EVFILT_READ,
                    EV_ADD | EV_ONESHOT,
                    0,
                    0,
                    token);
            }
            if ((op.events & POLLOUT) != 0) {
                set_event(
                    changes,
                    op.fd,
                    EVFILT_WRITE,
                    EV_ADD | EV_ONESHOT,
                    0,
                    0,
                    token);
            }
            return true;
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::timeout & op)
        {
            set_event(
                changes,
                token,
                EVFILT_TIMER,
                EV_ADD | EV_ONESHOT,
                NOTE_NSECONDS,
                static_cast<intptr_t>(nanoseconds(op.duration)),
                token);
            return true;
        }

        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::poll_until & op)
        {
            if ((op.events & POLLIN) != 0) {
                set_event(
                    changes,
                    op.fd,
                    EVFILT_READ,
                    EV_ADD | EV_ONESHOT,
                    0,
                    0,
                    token);
            }
            if ((op.events & POLLOUT) != 0) {
                set_event(
                    changes,
                    op.fd,
                    EVFILT_WRITE,
                    EV_ADD | EV_ONESHOT,
                    0,
                    0,
                    token);
            }
            set_event(
                changes,
                token,
                EVFILT_TIMER,
                EV_ADD | EV_ONESHOT,
                NOTE_NSECONDS,
                static_cast<intptr_t>(nanoseconds(op.timeout)),
                token);
            return true;
        }

        template<typename Op>
        bool submit_op(
            kqueue_wand &,
            deck &,
            wait_token,
            std::vector<kqueue_event> &,
            Op &)
        {
            static_assert(
                !std::is_same_v<Op, Op>,
                "operation is not supported by kqueue_wand");
            return false;
        }

        bool event_op(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const &,
            op::read_some & op)
        {
            auto result = op.offset < 0
                ? ::read(op.fd, op.buffer.data(), op.buffer.size())
                : ::pread(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_rearm(wand, d, token, result, op.fd, EVFILT_READ);
        }

        bool event_op(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const &,
            op::write_some & op)
        {
            auto result = op.offset < 0
                ? ::write(op.fd, op.buffer.data(), op.buffer.size())
                : ::pwrite(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_rearm(wand, d, token, result, op.fd, EVFILT_WRITE);
        }

        bool event_op(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const &,
            op::recv_some & op)
        {
            auto result =
                ::recv(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_rearm(wand, d, token, result, op.fd, EVFILT_READ);
        }

        bool event_op(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const &,
            op::send_some & op)
        {
            auto result =
                ::send(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_rearm(wand, d, token, result, op.fd, EVFILT_WRITE);
        }

        bool event_op(
            kqueue_wand &,
            deck &,
            wait_token,
            kqueue_event const &,
            op::connect & op)
        {
            auto error = int{};
            auto size = socklen_t{sizeof(error)};
            if (::getsockopt(
                    op.fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &error,
                    &size) != 0) {
                finish_error(errno);
                return true;
            }
            finish_result(error == 0 ? 0 : -error);
            return true;
        }

        bool event_op(
            kqueue_wand &,
            deck &,
            wait_token,
            kqueue_event const & event,
            op::poll &)
        {
            finish_result(poll_events_from_filter(event.filter));
            return true;
        }

        bool event_op(
            kqueue_wand &,
            deck &,
            wait_token,
            kqueue_event const &,
            op::timeout &)
        {
            finish_result(0);
            return true;
        }

        bool event_op(
            kqueue_wand & wand,
            deck &,
            wait_token token,
            kqueue_event const & event,
            op::poll_until & op)
        {
            if (event.filter == EVFILT_TIMER) {
                finish_poll_until(poll_until_result{
                    .events = 0,
                    .timed_out = true,
                });
            } else {
                finish_poll_until(poll_until_result{
                    .events = poll_events_from_filter(event.filter),
                    .timed_out = false,
                });
            }
            wand.delete_poll_until_siblings(token, op, event.filter);
            return true;
        }

        template<typename Op>
        bool event_op(
            kqueue_wand &,
            deck &,
            wait_token,
            kqueue_event const &,
            Op &)
        {
            finish_error(ENOTSUP);
            return true;
        }

        void delete_op_events(
            wait_token,
            std::vector<kqueue_event> &,
            op::manual &)
        {}

        void delete_op_events(
            wait_token,
            std::vector<kqueue_event> &,
            op::openat &)
        {}

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::read_some & op)
        {
            set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::write_some & op)
        {
            set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::recv_some & op)
        {
            set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::send_some & op)
        {
            set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::connect & op)
        {
            set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::poll & op)
        {
            if ((op.events & POLLIN) != 0)
                set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
            if ((op.events & POLLOUT) != 0)
                set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::timeout &)
        {
            set_event(changes, token, EVFILT_TIMER, EV_DELETE, 0, 0, token);
        }

        void delete_op_events(
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::poll_until & op)
        {
            if ((op.events & POLLIN) != 0)
                set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
            if ((op.events & POLLOUT) != 0)
                set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
            set_event(changes, token, EVFILT_TIMER, EV_DELETE, 0, 0, token);
        }

        template<typename Op>
        void delete_op_events(
            wait_token,
            std::vector<kqueue_event> &,
            Op &)
        {}

        bool finish_or_wait(
            wait_token token,
            std::vector<kqueue_event> & changes,
            ssize_t result,
            int fd,
            short filter)
        {
            if (result >= 0) {
                finish_result(static_cast<int>(result));
                return false;
            }
            if (!would_block(errno)) {
                finish_result(-errno);
                return false;
            }
            set_event(changes, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, token);
            return true;
        }

        bool finish_or_rearm(
            kqueue_wand & wand,
            deck &,
            wait_token token,
            ssize_t result,
            int fd,
            short filter)
        {
            if (result >= 0) {
                finish_result(static_cast<int>(result));
                return true;
            }
            if (!would_block(errno)) {
                finish_result(-errno);
                return true;
            }
            wand.arm(token, fd, filter);
            return false;
        }

        void finish_result(int result)
        {
            this->finished_ = true;
            if (result < 0) {
                finish_error(-result);
                return;
            }

            if constexpr (std::is_void_v<T>) {
                state_->set_value();
            } else if constexpr (std::is_same_v<T, int>) {
                state_->set_value(result);
            } else if constexpr (std::is_same_v<T, std::size_t>) {
                state_->set_value(static_cast<std::size_t>(result));
            } else {
                state_->set_exception(
                    std::make_exception_ptr(
                        runtime_error{
                            "kqueue delivered scalar result to unsupported "
                            "waiter"}));
            }
        }

        void finish_poll_until(poll_until_result result)
        {
            this->finished_ = true;
            if constexpr (std::is_same_v<T, poll_until_result>) {
                state_->set_value(result);
            } else {
                state_->set_exception(
                    std::make_exception_ptr(
                        runtime_error{
                            "kqueue delivered poll_until result to wrong "
                            "waiter"}));
            }
        }

        void finish_cancelled()
        {
            this->finished_ = true;
            state_->set_exception(
                std::make_exception_ptr(operation_cancelled{}));
        }

        void finish_error(int err)
        {
            this->finished_ = true;
            state_->set_exception(
                std::make_exception_ptr(
                    runtime_error{
                        "kqueue operation failed: "
                        + std::to_string(err)}));
        }

        std::shared_ptr<wait_state<T>> state_;
    };

    void stage_submissions(deck & d)
    {
        auto tokens = std::vector<wait_token>{};
        tokens.swap(pending_submissions_);

        for (auto token : tokens) {
            auto found = completions_.find(token);
            if (found == completions_.end())
                continue;

            auto submitted =
                found->second->submit(*this, d, token, pending_changes_);
            if (found->second->finished()) {
                completions_.erase(found);
                fulfill(d, token);
            } else if (submitted) {
                found->second->mark_submitted();
            }
        }
    }

    void stage_cancellations(deck & d)
    {
        auto tokens = std::vector<wait_token>{};
        tokens.swap(pending_cancellations_);

        for (auto token : tokens) {
            auto found = completions_.find(token);
            if (found == completions_.end())
                continue;

            found->second->delete_events(token, pending_changes_);
            found->second->complete(-ECANCELED);
            completions_.erase(found);
            fulfill(d, token);
        }
    }

    void apply_changes()
    {
        if (pending_changes_.empty())
            return;

        auto changes = std::vector<kqueue_event>{};
        changes.swap(pending_changes_);
        while (true) {
            auto rc = ::kevent(
                kq_.get(),
                changes.data(),
                static_cast<int>(changes.size()),
                nullptr,
                0,
                nullptr);
            if (rc == 0)
                return;
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "kevent changelist failed: " + std::to_string(errno)};
        }
    }

    void arm(wait_token token, int fd, short filter)
    {
        auto change = kqueue_event{};
        EV_SET(
            &change,
            fd,
            filter,
            EV_ADD | EV_ONESHOT,
            0,
            0,
            reinterpret_cast<void *>(static_cast<uintptr_t>(token)));
        while (true) {
            auto rc = ::kevent(kq_.get(), &change, 1, nullptr, 0, nullptr);
            if (rc == 0)
                return;
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "kevent rearm failed: " + std::to_string(errno)};
        }
    }

    void delete_poll_until_siblings(
        wait_token token,
        op::poll_until const & op,
        short completed_filter)
    {
        auto changes = std::vector<kqueue_event>{};
        if (completed_filter != EVFILT_TIMER)
            set_event(changes, token, EVFILT_TIMER, EV_DELETE, 0, 0, token);
        if (completed_filter != EVFILT_READ && (op.events & POLLIN) != 0)
            set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
        if (completed_filter != EVFILT_WRITE && (op.events & POLLOUT) != 0)
            set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        pending_changes_.insert(
            pending_changes_.end(),
            changes.begin(),
            changes.end());
    }

    void poll_with_timeout(deck & d, timespec const * timeout)
    {
        apply_changes();

        auto events = std::array<kqueue_event, 64>{};
        while (true) {
            auto rc = ::kevent(
                kq_.get(),
                nullptr,
                0,
                events.data(),
                static_cast<int>(events.size()),
                timeout);
            if (rc == 0)
                return;
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "kevent wait failed: " + std::to_string(errno)};

            for (auto i = 0; i != rc; ++i)
                handle_event(d, events[static_cast<std::size_t>(i)]);
            return;
        }
    }

    void handle_event(deck & d, kqueue_event const & event)
    {
        auto token = event_token(event);
        auto found = completions_.find(token);
        if (found == completions_.end())
            return;

        trace("kqueue complete token=" + std::to_string(token));
        if (!found->second->on_event(*this, d, token, event))
            return;

        completions_.erase(found);
        fulfill(d, token);
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
        return !pending_submissions_.empty()
            || !pending_cancellations_.empty()
            || !pending_changes_.empty();
    }

    nxt::unique_fd kq_;
    wait_token next_token_ = 1;
    std::unordered_map<wait_token, std::unique_ptr<completion_base>> completions_;
    std::unordered_map<wait_token, parked_task> waiters_;
    std::vector<wait_token> pending_submissions_;
    std::vector<wait_token> pending_cancellations_;
    std::vector<kqueue_event> pending_changes_;
};

template<typename Wish>
wait_token kqueue_wand::prepare_kqueue_wish(
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
    auto request = std::make_shared<kqueue_wish>(std::move(wish));
    completions_.emplace(
        token,
        std::make_unique<completion<result_type>>(request, state));
    pending_submissions_.push_back(token);
    trace("kqueue prepare " + std::string{Wish::name}
        + " token=" + std::to_string(token));
    return token;
}

inline wait_token kqueue_wand::prepare_wish(
    deck &,
    detail::promise_base &,
    detail::prepared_wish packet)
{
    return std::visit(
        [this, &packet](auto & wish) -> wait_token {
            return prepare_kqueue_wish(
                std::move(wish),
                std::move(packet.state));
        },
        packet.wish);
}

template<typename T>
[[nodiscard]] inline T run_with_kqueue(task<T> root)
{
    auto wand = kqueue_wand{};
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
[[nodiscard]] inline task_result_t<std::invoke_result_t<Fn>>
run_with_kqueue(Fn && fn)
{
    return run_with_kqueue(std::invoke(std::forward<Fn>(fn)));
}

#endif

} // namespace nxt::rt
