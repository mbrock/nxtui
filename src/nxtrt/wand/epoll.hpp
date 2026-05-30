#pragma once

#include "nxtrt/exec_lifecycle.hpp"
#include "nxtrt/task.hpp"
#include <nxt/unique-fd.hpp>

#include <boost/container/hub.hpp>

#if defined(__linux__)
#define NXT_RT_HAS_EPOLL 1
#else
#define NXT_RT_HAS_EPOLL 0
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <ranges>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#if NXT_RT_HAS_EPOLL
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#endif

namespace nxtrt {

inline constexpr bool has_epoll_wand = NXT_RT_HAS_EPOLL != 0;

#if NXT_RT_HAS_EPOLL

using epoll_event_t = struct epoll_event;

class epoll_wand final : public wand
{
private:
    using epoll_wish = wish_variant;

    using prepared = detail::wand_exec::prepared;
    using queued = detail::wand_exec::queued;
    using ready_to_retire = detail::wand_exec::ready_to_retire;
    using retired = detail::wand_exec::retired;

    struct submitted {};
    struct cancel_queued {};

    using parked_phase = std::variant<queued, submitted, cancel_queued>;
    using settled_phase = std::variant<ready_to_retire>;
    using exec_lifecycle =
        detail::wand_exec::lifecycle<parked_phase, settled_phase>;
    using parked = exec_lifecycle::parked;
    using settled = exec_lifecycle::settled;
    using exec_state = exec_lifecycle::state;

    enum class event_kind : std::uintptr_t
    {
        op = 0,
        timer = 1,
    };

    struct exec;

    struct event_key
    {
        exec * execution = nullptr;
        event_kind kind = event_kind::op;
    };

    struct registration
    {
        int fd = -1;
        event_kind kind = event_kind::op;
        bool owns_fd = false;
    };

public:
    epoll_wand()
        : epoll_(::epoll_create1(EPOLL_CLOEXEC))
    {
        if (epoll_.get() < 0)
            throw runtime_error{
                "epoll_create1 failed: " + std::to_string(errno)};
    }

    epoll_wand(const epoll_wand &) = delete;
    epoll_wand & operator=(const epoll_wand &) = delete;
    epoll_wand(epoll_wand &&) = delete;
    epoll_wand & operator=(epoll_wand &&) = delete;

    void suspend(wait_token token, need task) override
    {
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
            return;
        }

        if (std::holds_alternative<submitted>(state->phase)) {
            cleanup(*execution);
            execution->specification.completion->complete(
                execution->specification.request,
                -ECANCELED,
                true);
            auto continuation = state->continuation;
            execution->state = settled{.phase = ready_to_retire{}};
            if (current_deck_ != nullptr)
                continuation.resume(*current_deck_);
        }
    }

    void wave(deck & d) override
    {
        current_deck_ = &d;
        compact_execs();
        stage_submissions(d);
    }

    void poll(deck & d)
    {
        poll_with_timeout(d, 0);
    }

    void wait(deck & d)
    {
        poll_with_timeout(d, -1);
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
                    throw runtime_error{"nxtrt epoll wand deadlock"};
                wait(d);
            }
        }
    }

private:
    class completion_base
    {
    public:
        virtual ~completion_base() = default;

        virtual bool submit(
            epoll_wand & wand,
            deck & d,
            wait_token token,
            epoll_wish & request,
            exec & execution) = 0;
        virtual bool on_event(
            epoll_wand & wand,
            deck & d,
            wait_token token,
            epoll_wish & request,
            epoll_event_t const & event,
            event_kind kind) = 0;
        virtual void complete(
            epoll_wish & request,
            int result,
            bool cancelled) = 0;

        [[nodiscard]] bool finished() const noexcept
        {
            return finished_;
        }

    protected:
        bool finished_ = false;
    };

    template<typename T>
    class completion final : public completion_base
    {
    public:
        explicit completion(std::shared_ptr<urge_state<T>> state)
            : state_(std::move(state))
        {}

        bool submit(
            epoll_wand & wand,
            deck & d,
            wait_token token,
            epoll_wish & request,
            exec & execution) override
        {
            return std::visit(
                [&](auto & op) {
                    return submit_op(wand, d, token, execution, op);
                },
                request);
        }

        bool on_event(
            epoll_wand & wand,
            deck & d,
            wait_token token,
            epoll_wish & request,
            epoll_event_t const & event,
            event_kind kind) override
        {
            if ((event.events & EPOLLERR) != 0) {
                auto error = socket_error(event_fd(wand, token, kind));
                if (error != 0) {
                    finish_result(-error);
                    return true;
                }
            }

            return std::visit(
                [&](auto & op) {
                    return event_op(wand, d, token, event, kind, op);
                },
                request);
        }

        void complete(
            epoll_wish & request,
            int result,
            bool cancelled) override
        {
            if (cancelled) {
                finish_cancelled();
                return;
            }

            if constexpr (std::is_same_v<T, poll_until_result>) {
                finish_poll_until(poll_until_result{
                    .events = result,
                    .timed_out = result == 0,
                });
            } else {
                finish_result(result);
                if constexpr (std::is_same_v<T, statx_result>) {
                    if (result >= 0)
                        state_->set_value(std::get<op::statx>(request).result);
                } else if constexpr (std::is_same_v<T, piped_child>) {
                    if (result >= 0)
                        state_->set_value(
                            std::move(*std::get<op::spawn_piped>(request).child));
                } else if constexpr (std::is_same_v<T, pty_child>) {
                    if (result >= 0)
                        state_->set_value(
                            std::move(*std::get<op::spawn_pty>(request).child));
                } else if constexpr (std::is_same_v<T, child_result>) {
                    if (result >= 0)
                        state_->set_value(child_result_from(
                            std::get<op::wait_child>(request).info));
                }
            }
        }

    private:
        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::manual &)
        {
            finish_result(0);
            return false;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::openat & op)
        {
            auto fd = ::openat(op.dirfd, op.path.c_str(), op.flags, op.mode);
            finish_result(fd < 0 ? -errno : fd);
            return false;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::statx & op)
        {
            auto rc = ::syscall(
                SYS_statx,
                op.dirfd,
                op.path.c_str(),
                op.flags,
                op.mask,
                &op.result);
            if (rc < 0) {
                finish_error(errno);
            } else {
                this->finished_ = true;
                if constexpr (std::is_same_v<T, statx_result>)
                    state_->set_value(op.result);
            }
            return false;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::getdents64 & op)
        {
            auto result = ::syscall(
                SYS_getdents64,
                op.fd,
                op.buffer.data(),
                op.buffer.size());
            finish_result(result < 0 ? -errno : static_cast<int>(result));
            return false;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::spawn_piped & op)
        {
            auto result = spawn_piped_child(op);
            if (result < 0) {
                finish_error(-result);
            } else {
                this->finished_ = true;
                if constexpr (std::is_same_v<T, piped_child>)
                    state_->set_value(std::move(*op.child));
            }
            return false;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::spawn_pty & op)
        {
            auto result = spawn_pty_child(op);
            if (result < 0) {
                finish_error(-result);
            } else {
                this->finished_ = true;
                if constexpr (std::is_same_v<T, pty_child>)
                    state_->set_value(std::move(*op.child));
            }
            return false;
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::wait_child & op)
        {
            if (op.pidfd < 0) {
                finish_result(-EBADF);
                return false;
            }
            wand.add(execution, op.pidfd, EPOLLIN | EPOLLONESHOT, event_kind::op);
            return true;
        }

        bool submit_op(
            epoll_wand &,
            deck &,
            wait_token,
            exec &,
            op::signal_child & op)
        {
            if (op.pidfd < 0) {
                finish_result(-EBADF);
                return false;
            }
            auto rc = send_pidfd_signal(op.pidfd, op.signal);
            finish_result(rc < 0 ? -errno : 0);
            return false;
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::read_some & op)
        {
            set_nonblocking(op.fd);
            auto result = op.offset < 0
                ? ::read(op.fd, op.buffer.data(), op.buffer.size())
                : ::pread(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_wait(wand, execution, result, op.fd, EPOLLIN);
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::write_some & op)
        {
            set_nonblocking(op.fd);
            auto result = op.offset < 0
                ? ::write(op.fd, op.buffer.data(), op.buffer.size())
                : ::pwrite(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_wait(wand, execution, result, op.fd, EPOLLOUT);
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::recv_some & op)
        {
            set_nonblocking(op.fd);
            auto result =
                ::recv(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_wait(wand, execution, result, op.fd, EPOLLIN);
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::send_some & op)
        {
            set_nonblocking(op.fd);
            auto result =
                ::send(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_wait(wand, execution, result, op.fd, EPOLLOUT);
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
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
            wand.add(execution, op.fd, EPOLLOUT | EPOLLONESHOT, event_kind::op);
            return true;
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::accept & op)
        {
            set_nonblocking(op.fd);
            auto result = accept_once(op);
            return finish_or_wait(wand, execution, result, op.fd, EPOLLIN);
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::poll & op)
        {
            wand.add(
                execution,
                op.fd,
                poll_to_epoll(op.events) | EPOLLONESHOT,
                event_kind::op);
            return true;
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::timeout & op)
        {
            auto timer = make_timerfd(op.duration);
            wand.add_owned(
                execution,
                timer.release(),
                EPOLLIN | EPOLLONESHOT,
                event_kind::timer);
            return true;
        }

        bool submit_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            exec & execution,
            op::poll_until & op)
        {
            wand.add(
                execution,
                op.fd,
                poll_to_epoll(op.events) | EPOLLONESHOT,
                event_kind::op);
            auto timer = make_timerfd(op.timeout);
            wand.add_owned(
                execution,
                timer.release(),
                EPOLLIN | EPOLLONESHOT,
                event_kind::timer);
            return true;
        }

        bool event_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::read_some & op)
        {
            auto result = op.offset < 0
                ? ::read(op.fd, op.buffer.data(), op.buffer.size())
                : ::pread(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_rearm(wand, result, op.fd, EPOLLIN);
        }

        bool event_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::write_some & op)
        {
            auto result = op.offset < 0
                ? ::write(op.fd, op.buffer.data(), op.buffer.size())
                : ::pwrite(op.fd, op.buffer.data(), op.buffer.size(), op.offset);
            return finish_or_rearm(wand, result, op.fd, EPOLLOUT);
        }

        bool event_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::recv_some & op)
        {
            auto result =
                ::recv(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_rearm(wand, result, op.fd, EPOLLIN);
        }

        bool event_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::send_some & op)
        {
            auto result =
                ::send(op.fd, op.buffer.data(), op.buffer.size(), op.flags);
            return finish_or_rearm(wand, result, op.fd, EPOLLOUT);
        }

        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::connect & op)
        {
            auto error = socket_error(op.fd);
            finish_result(error == 0 ? 0 : -error);
            return true;
        }

        bool event_op(
            epoll_wand & wand,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::accept & op)
        {
            auto result = accept_once(op);
            return finish_or_rearm(wand, result, op.fd, EPOLLIN);
        }

        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const & event,
            event_kind,
            op::poll &)
        {
            finish_result(epoll_to_poll(event.events));
            return true;
        }

        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::timeout &)
        {
            finish_result(0);
            return true;
        }

        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const & event,
            event_kind kind,
            op::poll_until &)
        {
            finish_poll_until(poll_until_result{
                .events = kind == event_kind::timer
                    ? 0
                    : epoll_to_poll(event.events),
                .timed_out = kind == event_kind::timer,
            });
            return true;
        }

        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            op::wait_child & op)
        {
            auto rc = ::waitid(P_PIDFD, op.pidfd, &op.info, WEXITED);
            if (rc < 0) {
                finish_error(errno);
            } else {
                this->finished_ = true;
                if constexpr (std::is_same_v<T, child_result>)
                    state_->set_value(child_result_from(op.info));
            }
            return true;
        }

        template<typename Op>
        bool event_op(
            epoll_wand &,
            deck &,
            wait_token,
            epoll_event_t const &,
            event_kind,
            Op &)
        {
            finish_result(-ENOTSUP);
            return true;
        }

        bool finish_or_wait(
            epoll_wand & wand,
            exec & execution,
            ssize_t result,
            int fd,
            std::uint32_t events)
        {
            if (result >= 0) {
                finish_result(static_cast<int>(result));
                return false;
            }
            if (!would_block(errno)) {
                finish_result(-errno);
                return false;
            }
            wand.add(execution, fd, events | EPOLLONESHOT, event_kind::op);
            return true;
        }

        bool finish_or_rearm(
            epoll_wand &,
            ssize_t result,
            int,
            std::uint32_t)
        {
            if (result >= 0) {
                finish_result(static_cast<int>(result));
                return true;
            }
            finish_result(-errno);
            return true;
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
            } else if constexpr (
                std::is_same_v<T, statx_result>
                || std::is_same_v<T, piped_child>
                || std::is_same_v<T, pty_child>
                || std::is_same_v<T, child_result>) {
            } else {
                state_->set_exception(
                    std::make_exception_ptr(
                        runtime_error{"epoll delivered unsupported result"}));
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
                            "epoll delivered poll_until result to wrong urge"}));
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
            if (err == EINTR) {
                state_->set_exception(
                    std::make_exception_ptr(interrupted_system_call{}));
                return;
            }
            state_->set_exception(
                std::make_exception_ptr(
                    runtime_error{
                        std::string{"epoll operation failed: "}
                        + std::strerror(err)
                        + " (" + std::to_string(err) + ")"}));
        }

        static child_result child_result_from(siginfo_t const & info)
        {
            return child_result{
                .pid = info.si_pid,
                .code = info.si_code,
                .exited = info.si_code == CLD_EXITED,
                .exit_code = info.si_code == CLD_EXITED ? info.si_status : 0,
                .signaled =
                    info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED,
                .signal =
                    info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED
                        ? info.si_status
                        : 0,
            };
        }

        static int event_fd(epoll_wand & wand, wait_token token, event_kind kind)
        {
            auto * execution = wand.exec_from_token(token);
            if (execution == nullptr)
                return -1;
            for (auto const & reg : execution->registrations) {
                if (reg.kind == kind)
                    return reg.fd;
            }
            return -1;
        }

        static int socket_error(int fd)
        {
            auto error = int{};
            auto size = socklen_t{sizeof(error)};
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
                return errno;
            return error;
        }

        std::shared_ptr<urge_state<T>> state_;
    };

    struct spec
    {
        spec(epoll_wish request, std::unique_ptr<completion_base> completion)
            : request(std::move(request))
            , completion(std::move(completion))
        {}

        epoll_wish request;
        std::unique_ptr<completion_base> completion;
    };

    struct exec
    {
        exec(spec specification, exec_state state = prepared{})
            : specification(std::move(specification))
            , state(std::move(state))
        {}

        spec specification;
        exec_state state = prepared{};
        std::vector<registration> registrations;
    };

    template<typename Wish>
    wait_token prepare_epoll_wish(
        Wish wish,
        std::shared_ptr<void> erased_state)
    {
        using result_type = typename Wish::result_type;
        auto state =
            std::static_pointer_cast<urge_state<result_type>>(erased_state);
        auto iterator = execs_.emplace(
            spec{
                epoll_wish{std::move(wish)},
                std::make_unique<completion<result_type>>(state)},
            prepared{});
        auto & execution = *iterator;
        pending_submissions_.push_back(&execution);
        return token_for(execution);
    }

    wait_token prep(
        deck &,
        detail::promise_base &,
        detail::prepared_wish packet) override
    {
        return std::visit(
            [this, &packet](auto & wish) -> wait_token {
                return prepare_epoll_wish(
                    std::move(wish),
                    std::move(packet.state));
            },
            packet.wish);
    }

    static wait_token token_for(exec & execution) noexcept
    {
        static_assert(sizeof(std::uintptr_t) <= sizeof(wait_token));
        static_assert(alignof(exec) >= 2);
        return static_cast<wait_token>(
            reinterpret_cast<std::uintptr_t>(&execution));
    }

    static exec * exec_from_token(wait_token token) noexcept
    {
        return reinterpret_cast<exec *>(
            static_cast<std::uintptr_t>(token));
    }

    static std::uint64_t encode_user_data(exec & execution, event_kind kind)
    {
        auto bits = reinterpret_cast<std::uintptr_t>(&execution);
        return static_cast<std::uint64_t>(
            bits | static_cast<std::uintptr_t>(kind));
    }

    static event_key decode_user_data(std::uint64_t data)
    {
        auto bits = static_cast<std::uintptr_t>(data);
        return event_key{
            .execution = reinterpret_cast<exec *>(bits & ~std::uintptr_t{1}),
            .kind = (bits & std::uintptr_t{1}) == 0
                ? event_kind::op
                : event_kind::timer,
        };
    }

    void add(
        exec & execution,
        int fd,
        std::uint32_t events,
        event_kind kind)
    {
        auto watch_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (watch_fd < 0)
            throw runtime_error{
                "fcntl(F_DUPFD_CLOEXEC) failed: " + std::to_string(errno)};
        add_fd(execution, watch_fd, events, kind, true);
    }

    void add_owned(
        exec & execution,
        int fd,
        std::uint32_t events,
        event_kind kind)
    {
        add_fd(execution, fd, events, kind, true);
    }

    void add_fd(
        exec & execution,
        int fd,
        std::uint32_t events,
        event_kind kind,
        bool owns_fd)
    {
        auto event = epoll_event_t{
            .events = events | EPOLLERR | EPOLLHUP,
            .data = {.u64 = encode_user_data(execution, kind)},
        };
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
            if (owns_fd)
                ::close(fd);
            throw runtime_error{
                "epoll_ctl add failed: " + std::to_string(errno)};
        }
        execution.registrations.push_back(registration{
            .fd = fd,
            .kind = kind,
            .owns_fd = owns_fd,
        });
    }

    void cleanup(exec & execution)
    {
        for (auto & reg : execution.registrations) {
            if (reg.fd >= 0)
                (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, reg.fd, nullptr);
            if (reg.owns_fd && reg.fd >= 0)
                ::close(reg.fd);
            reg.fd = -1;
        }
        execution.registrations.clear();
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
                settle(d, *execution);
                continue;
            }

            if (!std::holds_alternative<queued>(state->phase))
                continue;

            auto did_submit = execution->specification.completion->submit(
                *this,
                d,
                token_for(*execution),
                execution->specification.request,
                *execution);
            if (execution->specification.completion->finished()) {
                settle(d, *execution);
            } else if (did_submit) {
                state->phase = submitted{};
            }
        }
    }

    void poll_with_timeout(deck & d, int timeout_ms)
    {
        current_deck_ = &d;
        auto events = std::array<epoll_event_t, 64>{};
        while (true) {
            auto rc = ::epoll_wait(
                epoll_.get(),
                events.data(),
                static_cast<int>(events.size()),
                timeout_ms);
            if (rc == 0) {
                compact_execs();
                return;
            }
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw runtime_error{
                    "epoll_wait failed: " + std::to_string(errno)};

            for (auto i = 0; i != rc; ++i)
                handle_event(d, events[static_cast<std::size_t>(i)]);
            compact_execs();
            return;
        }
    }

    void handle_event(deck & d, epoll_event_t const & event)
    {
        auto key = decode_user_data(event.data.u64);
        if (key.execution == nullptr)
            return;

        auto * state = std::get_if<parked>(&key.execution->state);
        if (state == nullptr)
            return;

        if (!key.execution->specification.completion->on_event(
                *this,
                d,
                token_for(*key.execution),
                key.execution->specification.request,
                event,
                key.kind))
            return;

        cleanup(*key.execution);
        settle(d, *key.execution);
    }

    [[nodiscard]] bool has_submitted_completions() const noexcept
    {
        return std::ranges::any_of(
            execs_,
            [](exec const & execution) {
                auto const * state = std::get_if<parked>(&execution.state);
                return state != nullptr
                    && std::holds_alternative<submitted>(state->phase);
            });
    }

    [[nodiscard]] bool has_pending_work() const noexcept
    {
        return !pending_submissions_.empty();
    }

    void settle(deck & d, exec & execution)
    {
        auto * state = std::get_if<parked>(&execution.state);
        if (state == nullptr)
            return;

        auto continuation = state->continuation;
        execution.state = settled{.phase = ready_to_retire{}};
        continuation.resume(d);
    }

    void compact_execs()
    {
        std::erase_if(pending_submissions_, [](exec * execution) {
            return execution == nullptr
                || !std::holds_alternative<parked>(execution->state);
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

    static bool would_block(int err) noexcept
    {
        return err == EAGAIN || err == EWOULDBLOCK;
    }

    static void set_nonblocking(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            throw runtime_error{"fcntl(F_GETFL) failed: " + std::to_string(errno)};
        if ((flags & O_NONBLOCK) != 0)
            return;
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            throw runtime_error{"fcntl(F_SETFL) failed: " + std::to_string(errno)};
    }

    static std::uint32_t poll_to_epoll(short events) noexcept
    {
        auto out = std::uint32_t{};
        if ((events & POLLIN) != 0)
            out |= EPOLLIN;
        if ((events & POLLOUT) != 0)
            out |= EPOLLOUT;
        if ((events & POLLPRI) != 0)
            out |= EPOLLPRI;
        return out;
    }

    static int epoll_to_poll(std::uint32_t events) noexcept
    {
        auto out = 0;
        if ((events & EPOLLIN) != 0)
            out |= POLLIN;
        if ((events & EPOLLOUT) != 0)
            out |= POLLOUT;
        if ((events & EPOLLPRI) != 0)
            out |= POLLPRI;
        if ((events & EPOLLERR) != 0)
            out |= POLLERR;
        if ((events & EPOLLHUP) != 0)
            out |= POLLHUP;
        return out;
    }

    static nxt::unique_fd make_timerfd(kernel_timespec duration)
    {
        auto fd = nxt::unique_fd{
            ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)};
        if (fd.get() < 0)
            throw runtime_error{
                "timerfd_create failed: " + std::to_string(errno)};

        auto spec = itimerspec{
            .it_interval = {},
            .it_value = timespec{
                .tv_sec = duration.tv_sec,
                .tv_nsec = duration.tv_nsec,
            },
        };
        if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0)
            spec.it_value.tv_nsec = 1;
        if (::timerfd_settime(fd.get(), 0, &spec, nullptr) != 0)
            throw runtime_error{
                "timerfd_settime failed: " + std::to_string(errno)};
        return fd;
    }

    static int accept_once(op::accept const & op)
    {
        auto flags = op.flags;
#ifdef SOCK_CLOEXEC
        flags |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
        flags |= SOCK_NONBLOCK;
#endif
        auto fd = ::accept4(op.fd, nullptr, nullptr, flags);
        return fd < 0 ? -errno : fd;
    }

    static bool move_fd_above_stdio(int & fd)
    {
        if (fd > STDERR_FILENO)
            return true;
        auto replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0)
            return false;
        ::close(fd);
        fd = replacement;
        return true;
    }

    static bool make_cloexec_pipe(int pipefd[2])
    {
        if (::pipe2(pipefd, O_CLOEXEC) != 0)
            return false;
        if (!move_fd_above_stdio(pipefd[0])
            || !move_fd_above_stdio(pipefd[1])) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            pipefd[0] = -1;
            pipefd[1] = -1;
            return false;
        }
        return true;
    }

    static std::vector<char *> argv_ptrs(std::vector<std::string> & argv)
    {
        auto ptrs = std::vector<char *>{};
        ptrs.reserve(argv.size() + 1);
        for (auto & arg : argv)
            ptrs.push_back(arg.data());
        ptrs.push_back(nullptr);
        return ptrs;
    }

    class spawn_file_actions
    {
    public:
        spawn_file_actions()
        {
            if (::posix_spawn_file_actions_init(&actions_) != 0)
                throw runtime_error{"posix_spawn_file_actions_init failed"};
        }
        spawn_file_actions(const spawn_file_actions &) = delete;
        spawn_file_actions & operator=(const spawn_file_actions &) = delete;
        ~spawn_file_actions()
        {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
        [[nodiscard]] posix_spawn_file_actions_t * get() noexcept
        {
            return &actions_;
        }
    private:
        posix_spawn_file_actions_t actions_{};
    };

    static int check_spawn_file_action(int rc)
    {
        return rc == 0 ? 0 : -rc;
    }

    static int open_pidfd(pid_t pid)
    {
#ifdef SYS_pidfd_open
        return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
        errno = ENOSYS;
        return -1;
#endif
    }

    static int send_pidfd_signal(int pidfd, int signal)
    {
#ifdef SYS_pidfd_send_signal
        return static_cast<int>(
            ::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0));
#else
        errno = ENOSYS;
        return -1;
#endif
    }

    static int spawn_piped_child(op::spawn_piped & wish)
    {
        if (wish.argv.empty())
            return -EINVAL;

        auto pipefd = std::array<int, 2>{-1, -1};
        if (!make_cloexec_pipe(pipefd.data()))
            return -errno;

        auto read_fd = nxt::unique_fd{pipefd[0]};
        auto write_fd = nxt::unique_fd{pipefd[1]};

        auto actions = spawn_file_actions{};
        auto rc = check_spawn_file_action(
            ::posix_spawn_file_actions_addopen(
                actions.get(),
                STDIN_FILENO,
                "/dev/null",
                O_RDONLY,
                0));
        if (rc == 0)
            rc = check_spawn_file_action(
                ::posix_spawn_file_actions_adddup2(
                    actions.get(), write_fd.get(), STDOUT_FILENO));
        if (rc == 0)
            rc = check_spawn_file_action(
                ::posix_spawn_file_actions_adddup2(
                    actions.get(), write_fd.get(), STDERR_FILENO));
        if (rc == 0)
            rc = check_spawn_file_action(
                ::posix_spawn_file_actions_addclose(
                    actions.get(), read_fd.get()));
        if (rc == 0)
            rc = check_spawn_file_action(
                ::posix_spawn_file_actions_addclose(
                    actions.get(), write_fd.get()));
        if (rc < 0)
            return rc;

        auto ptrs = argv_ptrs(wish.argv);
        auto pid = pid_t{-1};
        rc = ::posix_spawnp(
            &pid,
            wish.argv.front().c_str(),
            actions.get(),
            nullptr,
            ptrs.data(),
            environ);
        if (rc != 0)
            return -rc;

        auto pidfd = open_pidfd(pid);
        if (pidfd < 0) {
            auto saved_errno = errno;
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, nullptr, 0);
            return -saved_errno;
        }

        write_fd.reset();
        *wish.child = piped_child{
            .pid = pid,
            .pidfd = nxt::unique_fd{pidfd},
            .output = std::move(read_fd),
        };
        return 0;
    }

    static void set_cloexec(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFD);
        if (flags < 0)
            throw runtime_error{"fcntl(F_GETFD) failed: " + std::to_string(errno)};
        if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
            throw runtime_error{"fcntl(F_SETFD) failed: " + std::to_string(errno)};
    }

    static winsize winsize_from(std::size_t columns, std::size_t rows)
    {
        return winsize{
            .ws_row = static_cast<unsigned short>(std::max<std::size_t>(1, rows)),
            .ws_col =
                static_cast<unsigned short>(std::max<std::size_t>(1, columns)),
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
    }

    static nxt::unique_fd open_pty_master()
    {
        auto master = nxt::unique_fd{::posix_openpt(
            O_RDWR | O_NOCTTY | O_CLOEXEC)};
        if (master.get() < 0)
            throw runtime_error{"posix_openpt failed: " + std::to_string(errno)};
        if (::grantpt(master.get()) < 0)
            throw runtime_error{"grantpt failed: " + std::to_string(errno)};
        if (::unlockpt(master.get()) < 0)
            throw runtime_error{"unlockpt failed: " + std::to_string(errno)};
        return master;
    }

    static std::string pty_slave_name(int master_fd)
    {
        auto name = std::array<char, 256>{};
        if (::ptsname_r(master_fd, name.data(), name.size()) != 0)
            throw runtime_error{"ptsname_r failed: " + std::to_string(errno)};
        return name.data();
    }

    static int spawn_pty_child(op::spawn_pty & wish)
    {
        if (wish.argv.empty())
            return -EINVAL;

        auto master = nxt::unique_fd{};
        auto slave = nxt::unique_fd{};
        try {
            master = open_pty_master();
            auto slave_name = pty_slave_name(master.get());
            slave = nxt::unique_fd{
                ::open(slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
            if (slave.get() < 0)
                return -errno;
            auto ws = winsize_from(wish.columns, wish.rows);
            if (::ioctl(slave.get(), TIOCSWINSZ, &ws) < 0)
                return -errno;
        } catch (const runtime_error &) {
            return -errno;
        }

        auto ptrs = argv_ptrs(wish.argv);
        auto pid = ::fork();
        if (pid < 0)
            return -errno;

        if (pid == 0) {
            ::close(master.get());
            if (::setsid() < 0)
                _exit(126);
            if (::ioctl(slave.get(), TIOCSCTTY, 0) < 0)
                _exit(126);
            ::dup2(slave.get(), STDIN_FILENO);
            ::dup2(slave.get(), STDOUT_FILENO);
            ::dup2(slave.get(), STDERR_FILENO);
            if (slave.get() > STDERR_FILENO)
                ::close(slave.get());
            ::execvp(ptrs[0], ptrs.data());
            _exit(errno == ENOENT ? 127 : 126);
        }

        slave.reset();

        auto pidfd = open_pidfd(pid);
        if (pidfd < 0) {
            auto saved_errno = errno;
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, nullptr, 0);
            return -saved_errno;
        }

        try {
            set_cloexec(master.get());
        } catch (const runtime_error &) {
            auto saved_errno = errno;
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, nullptr, 0);
            return -saved_errno;
        }

        *wish.child = pty_child{
            .pid = pid,
            .pidfd = nxt::unique_fd{pidfd},
            .master = std::move(master),
        };
        return 0;
    }

    nxt::unique_fd epoll_;
    boost::container::hub<exec> execs_;
    std::vector<exec *> pending_submissions_;
    deck * current_deck_ = nullptr;
};

template<typename T>
[[nodiscard]] inline T run_with_epoll(task<T> root)
{
    auto wand = epoll_wand{};
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
run_with_epoll(Fn && fn)
{
    return run_with_epoll(std::invoke(std::forward<Fn>(fn)));
}

#endif

} // namespace nxtrt
