#pragma once

#include "nxtrt/exec_lifecycle.hpp"
#include "nxtrt/task.hpp"
#include <nxt/unique-fd.hpp>

#include <boost/container/hub.hpp>

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

namespace nxtrt {

inline constexpr bool has_kqueue_wand = NXT_RT_HAS_KQUEUE != 0;

#if NXT_RT_HAS_KQUEUE

using kqueue_event = struct kevent;

/// kqueue-backed wand for BSD runtime wishes.
///
/// Each awaited wish becomes one hub-stored execution record. Kevent `udata`
/// points at that record while variant phases make prepared, parked, settled,
/// delayed-delete, and retired states explicit.
class kqueue_wand final : public wand
{
private:
    using kqueue_wish = wish_variant;

    using prepared = detail::wand_exec::prepared;
    using queued = detail::wand_exec::queued;
    using ready_to_retire = detail::wand_exec::ready_to_retire;
    using retired = detail::wand_exec::retired;

    /// The operation has live kqueue registrations.
    struct submitted
    {};

    /// Cancellation arrived before submission.
    struct cancel_queued
    {};

    /// Cancellation must delete live kqueue registrations.
    struct delete_queued
    {};

    /// Phases that still own a parked coroutine continuation.
    using parked_phase = std::variant<
        queued,
        submitted,
        cancel_queued,
        delete_queued>;

    /// EV_DELETE changes are staged but have not been applied to the kqueue.
    struct delete_pending
    {};

    /// EV_DELETE changes were applied; wait once before hub reuse.
    struct delete_applied
    {};

    /// Phases after the waiting task has been fulfilled.
    using settled_phase = std::variant<
        ready_to_retire,
        delete_pending,
        delete_applied>;

    using exec_lifecycle =
        detail::wand_exec::lifecycle<parked_phase, settled_phase>;
    using parked = exec_lifecycle::parked;
    using settled = exec_lifecycle::settled;
    using exec_state = exec_lifecycle::state;

    struct exec;

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

    void suspend(wait_token token, need task) override
    {
        trace("kqueue park token={}", token);
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;
        if (!std::holds_alternative<prepared>(execution->state))
            return;
        execution->state = parked{
            .continuation = task,
            .phase = queued{},
        };
    }

    void cancel(wait_token token) override
    {
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;

        auto * state = std::get_if<parked>(&execution->state);
        if (state == nullptr)
            return;

        if (std::holds_alternative<queued>(state->phase)) {
            state->phase = cancel_queued{};
        } else if (std::holds_alternative<submitted>(state->phase)) {
            state->phase = delete_queued{};
            pending_cancellations_.push_back(execution);
        } else {
            return;
        }

        trace("kqueue request cancel token={}", token);
    }

    void wave(deck & d) override
    {
        compact_execs();
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
                    throw runtime_error{"nxtrt kqueue wand deadlock"};
                wait(d);
            }
        }
    }

    void complete(deck & d, wait_token token, int result)
    {
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;

        auto * state = std::get_if<parked>(&execution->state);
        if (state == nullptr)
            return;

        execution->specification.completion->complete(
            execution->specification.request,
            result,
            false);
        settle(d, *execution, ready_to_retire{});
    }

    void fulfill(deck & d, wait_token token)
    {
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;

        fulfill(d, *execution);
    }

private:
    template<typename Wish>
    wait_token prepare_kqueue_wish(
        Wish wish,
        std::shared_ptr<void> erased_state);

protected:
    wait_token prep(
        deck &,
        detail::promise_base &,
        detail::prepared_wish packet) override;

private:
    static void set_nonblocking(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            return;
        if ((flags & O_NONBLOCK) == 0)
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static void set_close_on_exec(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFD, 0);
        if (flags < 0)
            return;
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    static int accept_once(op::accept const & op)
    {
        auto accepted = ::accept(op.fd, nullptr, nullptr);
        if (accepted < 0)
            return accepted;

#ifdef SOCK_CLOEXEC
        if ((op.flags & SOCK_CLOEXEC) != 0)
            set_close_on_exec(accepted);
#endif
#ifdef SOCK_NONBLOCK
        if ((op.flags & SOCK_NONBLOCK) != 0)
            set_nonblocking(accepted);
#endif
        return accepted;
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
        virtual ~completion_base() = default;

        virtual bool submit(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_wish & request,
            std::vector<kqueue_event> & changes) = 0;
        virtual bool on_event(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_wish & request,
            kqueue_event const & event) = 0;
        virtual void delete_events(
            wait_token token,
            kqueue_wish & request,
            std::vector<kqueue_event> & changes) = 0;
        virtual void complete(
            kqueue_wish & request,
            int result,
            bool cancelled) = 0;

        [[nodiscard]] bool finished() const noexcept
        {
            return finished_;
        }

        [[nodiscard]] bool consume_delete_pending() noexcept
        {
            return std::exchange(delete_pending_, false);
        }

    protected:
        void mark_delete_pending() noexcept
        {
            delete_pending_ = true;
        }

        bool finished_ = false;
        bool delete_pending_ = false;
    };

    template<typename T>
    class completion final : public completion_base
    {
    public:
        explicit completion(std::shared_ptr<urge_state<T>> state)
            : state_(std::move(state))
        {}

        bool submit(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_wish & request,
            std::vector<kqueue_event> & changes) override
        {
            return std::visit(
                [&](auto & op) {
                    return submit_op(wand, d, token, changes, op);
                },
                request);
        }

        bool on_event(
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_wish & request,
            kqueue_event const & event) override
        {
            if ((event.flags & EV_ERROR) != 0) {
                finish_error(static_cast<int>(event.data));
                return true;
            }

            return std::visit(
                [&](auto & op) {
                    return event_op(wand, d, token, event, op);
                },
                request);
        }

        void delete_events(
            wait_token token,
            kqueue_wish & request,
            std::vector<kqueue_event> & changes) override
        {
            std::visit(
                [&](auto & op) {
                    delete_op_events(token, changes, op);
                },
                request);
        }

        void complete(
            kqueue_wish &,
            int result,
            bool cancelled) override
        {
            if (cancelled) {
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
            kqueue_wand &,
            deck &,
            wait_token token,
            std::vector<kqueue_event> & changes,
            op::connect & op)
        {
            set_nonblocking(op.fd);
            if (::connect(op.fd, op.sockaddr_ptr(), op.address_size) == 0) {
                finish_result(0);
                return false;
            }

            if (errno != EINPROGRESS && !would_block(errno)) {
                finish_result(-errno);
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
            op::accept & op)
        {
            set_nonblocking(op.fd);
            auto result = accept_once(op);
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
            kqueue_wand & wand,
            deck & d,
            wait_token token,
            kqueue_event const &,
            op::accept & op)
        {
            auto result = accept_once(op);
            return finish_or_rearm(wand, d, token, result, op.fd, EVFILT_READ);
        }

        bool event_op(
            kqueue_wand & wand,
            deck &,
            wait_token token,
            kqueue_event const & event,
            op::poll & op)
        {
            finish_result(poll_events_from_filter(event.filter));
            if (wand.delete_poll_siblings(token, op, event.filter))
                this->mark_delete_pending();
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
            if (wand.delete_poll_until_siblings(token, op, event.filter))
                this->mark_delete_pending();
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
            op::accept & op)
        {
            set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
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
                            "urge"}));
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
                            "urge"}));
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

        std::shared_ptr<urge_state<T>> state_;
    };

    /// Immutable wish recipe plus the typed completion sink for an exec.
    struct spec
    {
        spec(
            kqueue_wish request,
            std::unique_ptr<completion_base> completion)
            : request(std::move(request))
            , completion(std::move(completion))
        {}

        /// Closed wish that knows how to register its kqueue events.
        kqueue_wish request;
        /// Type-erased bridge to the urge result state.
        std::unique_ptr<completion_base> completion;
    };

    /// Address-stable execution record used directly as wait token/udata.
    struct exec
    {
        exec(
            spec specification,
            exec_state state = prepared{})
            : specification(std::move(specification))
            , state(std::move(state))
        {}

        /// The operation being realized by this execution.
        spec specification;
        /// Current lifecycle state.
        exec_state state = prepared{};
    };

    /// Return the public wait token for a live hub execution.
    static wait_token token_for(exec & execution) noexcept
    {
        static_assert(sizeof(std::uintptr_t) <= sizeof(wait_token));
        return static_cast<wait_token>(
            reinterpret_cast<std::uintptr_t>(&execution));
    }

    /// Decode a wait token back to the live hub execution it names.
    static exec * exec_from_token(wait_token token) noexcept
    {
        return reinterpret_cast<exec *>(
            static_cast<std::uintptr_t>(token));
    }

    void stage_submissions(deck & d)
    {
        auto executions = std::vector<exec *>{};
        executions.swap(pending_submissions_);

        for (auto * execution : executions) {
            if (execution == nullptr)
                continue;

            auto * state = std::get_if<parked>(&execution->state);
            if (state == nullptr)
                continue;

            if (std::holds_alternative<cancel_queued>(state->phase)) {
                execution->specification.completion->complete(
                    execution->specification.request,
                    -ECANCELED,
                    true);
                settle(d, *execution, ready_to_retire{});
                continue;
            }

            if (!std::holds_alternative<queued>(state->phase))
                continue;

            auto did_submit = execution->specification.completion->submit(
                *this,
                d,
                token_for(*execution),
                execution->specification.request,
                pending_changes_);
            if (execution->specification.completion->finished()) {
                settle(d, *execution, ready_to_retire{});
            } else if (did_submit) {
                state->phase = submitted{};
            }
        }
    }

    void stage_cancellations(deck & d)
    {
        auto executions = std::vector<exec *>{};
        executions.swap(pending_cancellations_);

        for (auto * execution : executions) {
            if (execution == nullptr)
                continue;

            auto * state = std::get_if<parked>(&execution->state);
            if (state == nullptr
                || !std::holds_alternative<delete_queued>(state->phase))
                continue;

            auto const before = pending_changes_.size();
            execution->specification.completion->delete_events(
                token_for(*execution),
                execution->specification.request,
                pending_changes_);
            execution->specification.completion->complete(
                execution->specification.request,
                -ECANCELED,
                true);
            auto phase = pending_changes_.size() == before
                ? settled_phase{ready_to_retire{}}
                : settled_phase{delete_pending{}};
            settle(d, *execution, std::move(phase));
        }
    }

    void apply_changes()
    {
        if (pending_changes_.empty()) {
            mark_deletes_applied();
            return;
        }

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
            if (rc == 0) {
                mark_deletes_applied();
                return;
            }
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

    [[nodiscard]] bool delete_poll_siblings(
        wait_token token,
        op::poll const & op,
        short completed_filter)
    {
        auto changes = std::vector<kqueue_event>{};
        if (completed_filter != EVFILT_READ && (op.events & POLLIN) != 0)
            set_event(changes, op.fd, EVFILT_READ, EV_DELETE, 0, 0, token);
        if (completed_filter != EVFILT_WRITE && (op.events & POLLOUT) != 0)
            set_event(changes, op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, token);
        auto const any = !changes.empty();
        pending_changes_.insert(
            pending_changes_.end(),
            changes.begin(),
            changes.end());
        return any;
    }

    [[nodiscard]] bool delete_poll_until_siblings(
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
        auto const any = !changes.empty();
        pending_changes_.insert(
            pending_changes_.end(),
            changes.begin(),
            changes.end());
        return any;
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
            if (rc == 0) {
                drain_applied_deletes();
                compact_execs();
                return;
            }
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "kevent wait failed: " + std::to_string(errno)};

            for (auto i = 0; i != rc; ++i)
                handle_event(d, events[static_cast<std::size_t>(i)]);
            drain_applied_deletes();
            compact_execs();
            return;
        }
    }

    void handle_event(deck & d, kqueue_event const & event)
    {
        auto token = event_token(event);
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;

        auto * state = std::get_if<parked>(&execution->state);
        if (state == nullptr)
            return;

        trace("kqueue complete token={}", token);
        if (!execution->specification.completion->on_event(
                *this,
                d,
                token,
                execution->specification.request,
                event))
            return;

        auto phase = execution->specification.completion->consume_delete_pending()
            ? settled_phase{delete_pending{}}
            : settled_phase{ready_to_retire{}};
        settle(d, *execution, std::move(phase));
    }

    [[nodiscard]] bool has_submitted_completions() const noexcept
    {
        return std::ranges::any_of(
            execs_,
            [](exec const & execution) {
                auto const * state = std::get_if<parked>(&execution.state);
                return state != nullptr
                    && (std::holds_alternative<submitted>(state->phase)
                        || std::holds_alternative<delete_queued>(
                            state->phase));
            });
    }

    [[nodiscard]] bool has_pending_work() const noexcept
    {
        return !pending_submissions_.empty()
            || !pending_cancellations_.empty()
            || !pending_changes_.empty();
    }

    void fulfill(deck & d, exec & execution, need continuation)
    {
        trace("kqueue fulfill token={}", token_for(execution));
        continuation.resume(d);
    }

    void fulfill(deck & d, exec & execution)
    {
        if (auto * state = std::get_if<parked>(&execution.state))
            fulfill(d, execution, state->continuation);
    }

    template<typename Phase>
    void settle(deck & d, exec & execution, Phase phase)
    {
        auto * state = std::get_if<parked>(&execution.state);
        if (state == nullptr)
            return;

        auto continuation = state->continuation;
        execution.state = settled{.phase = std::move(phase)};
        fulfill(d, execution, continuation);
    }

    /// Mark EV_DELETE changes as applied after a successful changelist flush.
    void mark_deletes_applied()
    {
        for (auto & execution : execs_) {
            auto * state = std::get_if<settled>(&execution.state);
            if (state != nullptr
                && std::holds_alternative<delete_pending>(state->phase))
                state->phase = delete_applied{};
        }
    }

    /// Let one receive pass drain stale events before deleted exec reuse.
    void drain_applied_deletes()
    {
        for (auto & execution : execs_) {
            auto * state = std::get_if<settled>(&execution.state);
            if (state != nullptr
                && std::holds_alternative<delete_applied>(state->phase))
                state->phase = ready_to_retire{};
        }
    }

    /// Erase retired hub records only after registration users are gone.
    void compact_execs()
    {
        std::erase_if(pending_submissions_, [](exec * execution) {
            return execution == nullptr
                || !std::holds_alternative<parked>(execution->state);
        });
        std::erase_if(pending_cancellations_, [](exec * execution) {
            if (execution == nullptr)
                return true;
            auto const * state = std::get_if<parked>(&execution->state);
            return state == nullptr
                || !std::holds_alternative<delete_queued>(state->phase);
        });

        for (auto it = execs_.begin(); it != execs_.end();) {
            if (exec_lifecycle::is_retirable(it->state))
                it->state = retired{};
            if (std::holds_alternative<retired>(it->state)) {
                it = execs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    nxt::unique_fd kq_;
    boost::container::hub<exec> execs_;
    std::vector<exec *> pending_submissions_;
    std::vector<exec *> pending_cancellations_;
    std::vector<kqueue_event> pending_changes_;
};

template<typename Wish>
wait_token kqueue_wand::prepare_kqueue_wish(
    Wish wish,
    std::shared_ptr<void> erased_state)
{
    using result_type = typename Wish::result_type;
    auto state =
        std::static_pointer_cast<urge_state<result_type>>(erased_state);
    auto iterator = execs_.emplace(
        spec{
            kqueue_wish{std::move(wish)},
            std::make_unique<completion<result_type>>(state)},
        prepared{});
    auto & execution = *iterator;
    pending_submissions_.push_back(&execution);
    auto token = token_for(execution);
    trace("kqueue prepare {} token={}", Wish::name, token);
    return token;
}

inline wait_token kqueue_wand::prep(
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

} // namespace nxtrt
