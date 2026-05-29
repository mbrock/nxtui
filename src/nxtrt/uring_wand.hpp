#pragma once

#include "nxtrt/exec_lifecycle.hpp"
#include "nxtrt/task.hpp"

#include <boost/container/hub.hpp>

#if __has_include(<liburing.h>)
#define NXT_RT_HAS_LIBURING 1
#else
#define NXT_RT_HAS_LIBURING 0
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <ranges>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/pidfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#if NXT_RT_HAS_LIBURING
#include <liburing.h>
#endif

extern "C" char ** environ;

namespace nxtrt {

inline constexpr bool has_liburing_wand = NXT_RT_HAS_LIBURING != 0;

#if NXT_RT_HAS_LIBURING

class uring_wand;

/// Per-wish staging context handed to `op::*::stage_uring`.
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

/// io_uring-backed wand for Linux runtime wishes.
///
/// Each awaited wish becomes one hub-stored execution record. SQE/CQE
/// `user_data` points at that record while variant phases make the legal
/// prepared, parked, settled, and retired states explicit.
class uring_wand final : public wand
{
private:
    using uring_wish = wish_variant;

    using prepared = detail::wand_exec::prepared;
    using queued = detail::wand_exec::queued;
    using ready_to_retire = detail::wand_exec::ready_to_retire;
    using retired = detail::wand_exec::retired;

    /// Main operation SQE has been submitted; an op CQE may arrive.
    struct submitted
    {};

    /// Cancellation has been requested but not submitted to io_uring.
    struct cancel_queued
    {};

    /// Cancel SQE has been submitted; op and cancel CQEs may both arrive.
    struct cancel_submitted
    {};

    /// Cancel CQE arrived before the op CQE.
    struct cancel_drained
    {};

    /// Phases that still own a parked coroutine continuation.
    using parked_phase = std::variant<
        queued,
        submitted,
        cancel_queued,
        cancel_submitted,
        cancel_drained>;

    /// The op CQE arrived first; keep the slot until cancel CQE drains.
    struct waiting_cancel_cqe
    {};

    /// Phases after the waiting task has been fulfilled.
    using settled_phase = std::variant<
        ready_to_retire,
        waiting_cancel_cqe>;

    using exec_lifecycle =
        detail::wand_exec::lifecycle<parked_phase, settled_phase>;
    using parked = exec_lifecycle::parked;
    using settled = exec_lifecycle::settled;
    using exec_state = exec_lifecycle::state;

    struct exec;

public:
    explicit uring_wand(unsigned queue_depth = 1024)
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
            state->phase = cancel_queued{};
            pending_cancellations_.push_back(execution);
        } else {
            return;
        }

        trace("uring request cancel token=" + std::to_string(token));
    }

    void wave(deck & d) override
    {
        stage_submissions(d);
        stage_cancellations();
        compact_execs();
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
                    if (d.empty() && !root.done() && has_pending_work()
                        && has_submitted_completions())
                        wait(d);
                    continue;
                }
                if (!has_submitted_completions())
                    throw runtime_error{"nxtrt uring wand deadlock"};
                wait(d);
            }
        }
    }

    void complete(deck & d, wait_token token, int result)
    {
        auto * execution = exec_from_token(token);
        if (execution == nullptr)
            return;

        handle_op_cqe(d, *execution, result);
        compact_execs();
    }

    void fulfill(deck & d, wait_token token)
    {
        if (auto * execution = exec_from_token(token))
            fulfill(d, *execution);
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

    /// Low-bit tag stored next to the exec pointer in CQE user data.
    enum class cqe_kind : std::uintptr_t
    {
        op = 0,
        cancel = 1,
    };

    /// Decoded SQE/CQE user data.
    struct cqe_key
    {
        exec * execution = nullptr;
        cqe_kind kind = cqe_kind::op;
    };

    void handle_cqe(deck & d, io_uring_cqe * cqe)
    {
        auto key = decode_user_data(io_uring_cqe_get_data64(cqe));
        auto result = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (key.execution == nullptr)
            return;

        if (key.kind == cqe_kind::cancel) {
            trace("uring cancel complete token="
                + std::to_string(token_for(*key.execution))
                + " result=" + std::to_string(result));
            handle_cancel_cqe(*key.execution);
            compact_execs();
            return;
        }

        trace("uring complete token=" + std::to_string(token_for(*key.execution))
            + " result=" + std::to_string(result));

        handle_op_cqe(d, *key.execution, result);
        compact_execs();
    }

    class completion_base
    {
    public:
        virtual ~completion_base() = default;

        /// Transfer an io_uring result into the typed waiter state.
        virtual void complete(
            uring_wish & request,
            int result,
            bool cancelled) = 0;

        /// Human-readable wish name for diagnostics.
        [[nodiscard]] static std::string describe(uring_wish const & request)
        {
            return std::visit(
                [](auto const & wish) {
                    return detail::describe_wish(wish);
                },
                request);
        }
    };

    template<typename T>
    class completion final : public completion_base
    {
    public:
        explicit completion(std::shared_ptr<wait_state<T>> state)
            : state_(std::move(state))
        {}

        void complete(
            uring_wish & request,
            int result,
            bool cancelled) override
        {
            if (cancelled) {
                state_->set_exception(
                    std::make_exception_ptr(operation_cancelled{}));
                return;
            }

            if constexpr (std::is_same_v<T, poll_until_result>) {
                state_->set_exception(
                    std::make_exception_ptr(
                        runtime_error{
                            "io_uring poll_until is implemented as a "
                            "task-level race"}));
            } else {
                if constexpr (std::is_void_v<T>) {
                    if (std::holds_alternative<op::timeout>(request)
                        && result == -ETIME) {
                        state_->set_value();
                        return;
                    }
                }

                if (result < 0) {
                    if (result == -EINTR) {
                        state_->set_exception(
                            std::make_exception_ptr(
                                interrupted_system_call{}));
                        return;
                    }
                    state_->set_exception(
                        std::make_exception_ptr(
                            runtime_error{
                                failure_message(request, result)}));
                    return;
                }

                if constexpr (std::is_void_v<T>) {
                    state_->set_value();
                } else if constexpr (std::is_same_v<T, int>) {
                    state_->set_value(result);
                } else if constexpr (std::is_same_v<T, std::size_t>) {
                    state_->set_value(static_cast<std::size_t>(result));
                } else if constexpr (std::is_same_v<T, statx_result>) {
                    state_->set_value(std::get<op::statx>(request).result);
                } else if constexpr (std::is_same_v<T, piped_child>) {
                    state_->set_value(std::move(
                        *std::get<op::spawn_piped>(request).child));
                } else if constexpr (std::is_same_v<T, pty_child>) {
                    state_->set_value(std::move(
                        *std::get<op::spawn_pty>(request).child));
                } else if constexpr (std::is_same_v<T, child_result>) {
                    state_->set_value(child_result_from(
                        std::get<op::wait_child>(request).info));
                } else {
                    static_assert(std::is_void_v<T>, "unsupported uring result");
                }
            }
        }

    private:
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

        [[nodiscard]] static std::string failure_message(
            uring_wish const & request,
            int result)
        {
            auto code = -result;
            auto message = std::string{"io_uring "}
                + describe(request)
                + " failed: "
                + std::strerror(code)
                + " ("
                + std::to_string(code)
                + ")";
            return message;
        }

        std::shared_ptr<wait_state<T>> state_;
    };

    /// Immutable wish recipe plus the typed completion sink for an exec.
    struct spec
    {
        spec(
            uring_wish request,
            std::unique_ptr<completion_base> completion)
            : request(std::move(request))
            , completion(std::move(completion))
        {}

        /// Closed wish that knows how to stage its SQE.
        uring_wish request;
        /// Type-erased bridge to the awaiter's result state.
        std::unique_ptr<completion_base> completion;
    };

    /// Address-stable execution record used directly as wait token/user data.
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

    template<typename Wish>
    wait_token prepare_uring_wish(
        Wish wish,
        std::shared_ptr<void> erased_state)
    {
        using result_type = typename Wish::result_type;
        auto state =
            std::static_pointer_cast<wait_state<result_type>>(erased_state);
        auto iterator = execs_.emplace(
            spec{
                uring_wish{std::move(wish)},
                std::make_unique<completion<result_type>>(state)},
            prepared{});
        auto & execution = *iterator;
        pending_submissions_.push_back(&execution);
        auto token = token_for(execution);
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

    [[nodiscard]] unsigned sq_space_left() const noexcept
    {
        return io_uring_sq_space_left(
            const_cast<io_uring *>(&ring_));
    }

    [[nodiscard]] static unsigned sqes_required(uring_wish const & request)
    {
        return std::visit(
            [](auto const & op) -> unsigned {
                using op_type = std::decay_t<decltype(op)>;
                if constexpr (
                    std::is_same_v<op_type, op::getdents64>
                    || std::is_same_v<op_type, op::spawn_piped>
                    || std::is_same_v<op_type, op::spawn_pty>
                    || std::is_same_v<op_type, op::signal_child>) {
                    return 0;
                } else {
                    return 1;
                }
            },
            request);
    }

    /// Return the public wait token for a live hub execution.
    static wait_token token_for(exec & execution) noexcept
    {
        static_assert(sizeof(std::uintptr_t) <= sizeof(wait_token));
        static_assert(alignof(exec) >= 2);
        return static_cast<wait_token>(
            reinterpret_cast<std::uintptr_t>(&execution));
    }

    /// Decode a wait token back to the live hub execution it names.
    static exec * exec_from_token(wait_token token) noexcept
    {
        return reinterpret_cast<exec *>(
            static_cast<std::uintptr_t>(token));
    }

    /// Pack an exec pointer and CQE kind into io_uring user data.
    static std::uint64_t encode_user_data(
        exec & execution,
        cqe_kind kind) noexcept
    {
        auto bits = reinterpret_cast<std::uintptr_t>(&execution);
        return static_cast<std::uint64_t>(
            bits | static_cast<std::uintptr_t>(kind));
    }

    /// Unpack io_uring user data into an exec pointer and CQE kind.
    static cqe_key decode_user_data(std::uint64_t data) noexcept
    {
        auto bits = static_cast<std::uintptr_t>(data);
        return cqe_key{
            .execution = reinterpret_cast<exec *>(bits & ~std::uintptr_t{1}),
            .kind = (bits & std::uintptr_t{1}) == 0
                ? cqe_kind::op
                : cqe_kind::cancel,
        };
    }

    static void attach_exec(
        io_uring_sqe * sqe,
        exec & execution,
        cqe_kind kind) noexcept
    {
        io_uring_sqe_set_data64(sqe, encode_user_data(execution, kind));
    }

    /// Stage queued executions, deferring those that need more SQE space.
    void stage_submissions(deck & d)
    {
        auto executions = std::vector<exec *>{};
        executions.swap(pending_submissions_);
        auto deferred = std::vector<exec *>{};

        for (auto * execution : executions) {
            if (execution == nullptr)
                continue;

            auto * state = std::get_if<parked>(&execution->state);
            if (state == nullptr)
                continue;

            if (std::holds_alternative<cancel_queued>(state->phase)) {
                settle(
                    d,
                    *execution,
                    -ECANCELED,
                    true,
                    ready_to_retire{});
                continue;
            }

            if (!std::holds_alternative<queued>(state->phase))
                continue;

            auto required = sqes_required(execution->specification.request);
            if (required > sq_space_left()) {
                deferred.push_back(execution);
                continue;
            }

            if (stage_submission(d, *execution)
                && std::holds_alternative<parked>(execution->state))
                std::get<parked>(execution->state).phase = submitted{};
        }

        pending_submissions_.insert(
            pending_submissions_.begin(),
            deferred.begin(),
            deferred.end());
    }

    bool stage_submission(deck & d, exec & execution)
    {
        auto submission = uring_submission{*this, d, token_for(execution)};
        return std::visit(
            [&submission](auto & op) {
                return op.stage_uring(submission);
            },
            execution.specification.request);
    }

    /// Stage cancel SQEs for submitted executions that were stopped.
    void stage_cancellations()
    {
        auto executions = std::vector<exec *>{};
        executions.swap(pending_cancellations_);
        auto deferred = std::vector<exec *>{};

        for (auto * execution : executions) {
            if (execution == nullptr)
                continue;

            auto * state = std::get_if<parked>(&execution->state);
            if (state == nullptr
                || !std::holds_alternative<cancel_queued>(state->phase))
                continue;

            if (sq_space_left() == 0) {
                deferred.push_back(execution);
                continue;
            }

            auto * sqe = get_sqe();
            io_uring_prep_cancel64(
                sqe,
                encode_user_data(*execution, cqe_kind::op),
                0);
            attach_exec(sqe, *execution, cqe_kind::cancel);
            state->phase = cancel_submitted{};
            trace("uring prepare cancel token="
                + std::to_string(token_for(*execution)));
        }
        pending_cancellations_ = std::move(deferred);
    }

    /// Complete the main operation CQE and fulfill the parked task.
    void handle_op_cqe(deck & d, exec & execution, int result)
    {
        auto * state = std::get_if<parked>(&execution.state);
        if (state == nullptr)
            return;

        auto cancelled =
            std::holds_alternative<cancel_queued>(state->phase)
            || std::holds_alternative<cancel_submitted>(state->phase)
            || std::holds_alternative<cancel_drained>(state->phase);
        auto waiting_for_cancel =
            std::holds_alternative<cancel_submitted>(state->phase);
        if (waiting_for_cancel) {
            settle(
                d,
                execution,
                result,
                cancelled,
                waiting_cancel_cqe{});
        } else {
            settle(
                d,
                execution,
                result,
                cancelled,
                ready_to_retire{});
        }
    }

    /// Mark cancel CQE drain progress without touching the fulfilled task.
    void handle_cancel_cqe(exec & execution)
    {
        if (auto * state = std::get_if<parked>(&execution.state);
            state != nullptr
            && std::holds_alternative<cancel_submitted>(state->phase)) {
            state->phase = cancel_drained{};
            return;
        }

        if (auto * state = std::get_if<settled>(&execution.state);
            state != nullptr
            && std::holds_alternative<waiting_cancel_cqe>(state->phase))
            state->phase = ready_to_retire{};
    }

    /// Store the typed result, move to a settled phase, and requeue the task.
    template<typename Phase>
    void settle(
        deck & d,
        exec & execution,
        int result,
        bool cancelled,
        Phase phase)
    {
        auto * state = std::get_if<parked>(&execution.state);
        if (state == nullptr)
            return;

        auto continuation = state->continuation;
        execution.specification.completion->complete(
            execution.specification.request,
            result,
            cancelled);
        execution.state = settled{.phase = phase};
        fulfill(d, execution, continuation);
    }

    void fulfill(deck & d, exec & execution, parked_task continuation)
    {
        trace("uring fulfill token=" + std::to_string(token_for(execution)));
        continuation.resume(d);
    }

    void fulfill(deck & d, exec & execution)
    {
        if (auto * state = std::get_if<parked>(&execution.state))
            fulfill(d, execution, state->continuation);
    }

    [[nodiscard]] static bool is_submitted(parked_phase const & phase) noexcept
    {
        return std::holds_alternative<submitted>(phase)
            || std::holds_alternative<cancel_queued>(phase)
            || std::holds_alternative<cancel_submitted>(phase)
            || std::holds_alternative<cancel_drained>(phase);
    }

    /// Erase retired hub records at sync points after pointer users are gone.
    void compact_execs()
    {
        std::erase_if(pending_submissions_, [](exec * execution) {
            return execution == nullptr
                || !std::holds_alternative<parked>(execution->state);
        });
        std::erase_if(pending_cancellations_, [](exec * execution) {
            if (execution == nullptr)
                return true;
            auto * state = std::get_if<parked>(&execution->state);
            return state == nullptr
                || !std::holds_alternative<cancel_queued>(state->phase);
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

    [[nodiscard]] bool has_submitted_completions() const noexcept
    {
        return std::ranges::any_of(
            execs_,
            [](exec const & execution) {
                auto * state = std::get_if<parked>(&execution.state);
                return state != nullptr && is_submitted(state->phase);
            });
    }

    [[nodiscard]] bool has_pending_work() const noexcept
    {
        return !pending_submissions_.empty() || !pending_cancellations_.empty();
    }

    io_uring ring_{};
    boost::container::hub<exec> execs_;
    std::vector<exec *> pending_submissions_;
    std::vector<exec *> pending_cancellations_;
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
    if (auto * execution = uring_wand::exec_from_token(token_))
        uring_wand::attach_exec(sqe, *execution, uring_wand::cqe_kind::op);
}

inline void uring_submission::complete_sync(int result)
{
    wand_.complete(deck_, token_, result);
}

inline bool set_fd_cloexec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

inline bool move_fd_above_stdio(int & fd)
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

inline bool make_cloexec_pipe(int pipefd[2])
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

inline std::vector<char *> argv_ptrs(std::vector<std::string> & argv)
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

inline int check_spawn_file_action(int rc)
{
    return rc == 0 ? 0 : -rc;
}

inline int open_pidfd(pid_t pid)
{
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

inline int send_pidfd_signal(int pidfd, int signal)
{
#ifdef SYS_pidfd_send_signal
    return static_cast<int>(
        ::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

inline void set_cloexec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        throw runtime_error{"fcntl(F_GETFD) failed: " + std::to_string(errno)};
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        throw runtime_error{"fcntl(F_SETFD) failed: " + std::to_string(errno)};
}

inline winsize winsize_from(std::size_t columns, std::size_t rows)
{
    return winsize{
        .ws_row = static_cast<unsigned short>(std::max<std::size_t>(1, rows)),
        .ws_col = static_cast<unsigned short>(std::max<std::size_t>(1, columns)),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
}

inline nxt::unique_fd open_pty_master()
{
    auto master = nxt::unique_fd{::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)};
    if (master.get() < 0)
        throw runtime_error{"posix_openpt failed: " + std::to_string(errno)};
    if (::grantpt(master.get()) < 0)
        throw runtime_error{"grantpt failed: " + std::to_string(errno)};
    if (::unlockpt(master.get()) < 0)
        throw runtime_error{"unlockpt failed: " + std::to_string(errno)};
    return master;
}

inline std::string pty_slave_name(int master_fd)
{
    auto name = std::array<char, 256>{};
    if (::ptsname_r(master_fd, name.data(), name.size()) != 0)
        throw runtime_error{"ptsname_r failed: " + std::to_string(errno)};
    return name.data();
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

inline bool op::spawn_piped::stage_uring(uring_submission & submission)
{
    if (argv.empty()) {
        submission.complete_sync(-EINVAL);
        return false;
    }

    auto pipefd = std::array<int, 2>{-1, -1};
    if (!make_cloexec_pipe(pipefd.data())) {
        submission.complete_sync(-errno);
        return false;
    }

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
            ::posix_spawn_file_actions_addclose(actions.get(), read_fd.get()));
    if (rc == 0)
        rc = check_spawn_file_action(
            ::posix_spawn_file_actions_addclose(actions.get(), write_fd.get()));
    if (rc < 0) {
        submission.complete_sync(rc);
        return false;
    }

    auto ptrs = argv_ptrs(argv);
    auto pid = pid_t{-1};
    rc = ::posix_spawnp(
        &pid,
        argv.front().c_str(),
        actions.get(),
        nullptr,
        ptrs.data(),
        environ);
    if (rc != 0) {
        submission.complete_sync(-rc);
        return false;
    }

    auto pidfd = open_pidfd(pid);
    if (pidfd < 0) {
        auto saved_errno = errno;
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        submission.complete_sync(-saved_errno);
        return false;
    }

    write_fd.reset();
    *child = piped_child{
        .pid = pid,
        .pidfd = nxt::unique_fd{pidfd},
        .output = std::move(read_fd),
    };
    submission.complete_sync(0);
    return false;
}

inline bool op::spawn_pty::stage_uring(uring_submission & submission)
{
    if (argv.empty()) {
        submission.complete_sync(-EINVAL);
        return false;
    }

    auto master = nxt::unique_fd{};
    auto slave = nxt::unique_fd{};
    try {
        master = open_pty_master();
        auto slave_name = pty_slave_name(master.get());
        slave = nxt::unique_fd{
            ::open(slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
        if (slave.get() < 0) {
            submission.complete_sync(-errno);
            return false;
        }
        auto ws = winsize_from(columns, rows);
        if (::ioctl(slave.get(), TIOCSWINSZ, &ws) < 0) {
            submission.complete_sync(-errno);
            return false;
        }
    } catch (const runtime_error &) {
        submission.complete_sync(-errno);
        return false;
    }

    auto ptrs = argv_ptrs(argv);
    auto pid = ::fork();
    if (pid < 0) {
        submission.complete_sync(-errno);
        return false;
    }

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
        submission.complete_sync(-saved_errno);
        return false;
    }

    try {
        set_cloexec(master.get());
    } catch (const runtime_error &) {
        auto saved_errno = errno;
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        submission.complete_sync(-saved_errno);
        return false;
    }

    *child = pty_child{
        .pid = pid,
        .pidfd = nxt::unique_fd{pidfd},
        .master = std::move(master),
    };
    submission.complete_sync(0);
    return false;
}

inline bool op::wait_child::stage_uring(uring_submission & submission)
{
    if (pidfd < 0) {
        submission.complete_sync(-EBADF);
        return false;
    }

    auto * sqe = submission.get_sqe();
    io_uring_prep_waitid(sqe, P_PIDFD, pidfd, &info, WEXITED, 0);
    submission.attach(sqe);
    return true;
}

inline bool op::signal_child::stage_uring(uring_submission & submission)
{
    if (pidfd < 0) {
        submission.complete_sync(-EBADF);
        return false;
    }

    auto rc = send_pidfd_signal(pidfd, signal);
    submission.complete_sync(rc < 0 ? -errno : 0);
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
    (void)fd;
    (void)events;
    (void)timeout;
    submission.complete_sync(-ENOTSUP);
    return false;
}

#endif

} // namespace nxtrt
