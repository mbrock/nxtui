#pragma once

#include "nxt/rt/task.hpp"

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
#include <unordered_set>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#if NXT_RT_HAS_LIBURING
#include <liburing.h>
#endif

extern "C" char ** environ;

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
                    if (d.empty() && !root.done() && has_pending_work()
                        && has_submitted_completions())
                        wait(d);
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

        [[nodiscard]] std::string describe() const
        {
            return std::visit(
                [](auto const & wish) {
                    return detail::describe_wish(wish);
                },
                *request_);
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
                                failure_message(result)}));
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
                                failure_message(result)}));
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
                } else if constexpr (std::is_same_v<T, piped_child>) {
                    state_->set_value(std::move(
                        *std::get<op::spawn_piped>(*this->request_).child));
                } else if constexpr (std::is_same_v<T, pty_child>) {
                    state_->set_value(std::move(
                        *std::get<op::spawn_pty>(*this->request_).child));
                } else if constexpr (std::is_same_v<T, child_result>) {
                    state_->set_value(child_result_from(
                        std::get<op::wait_child>(*this->request_).info));
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

        [[nodiscard]] std::string failure_message(int result) const
        {
            auto code = -result;
            auto message = std::string{"io_uring "}
                + this->describe()
                + " failed: "
                + std::strerror(code)
                + " ("
                + std::to_string(code)
                + ")";
            return message;
        }

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
                } else if constexpr (std::is_same_v<op_type, op::poll_until>) {
                    return 2;
                } else {
                    return 1;
                }
            },
            request);
    }

    static void attach_token(io_uring_sqe * sqe, wait_token token) noexcept
    {
        io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(token));
    }

    void stage_submissions(deck & d)
    {
        auto tokens = std::vector<wait_token>{};
        tokens.swap(pending_submissions_);
        auto deferred = std::vector<wait_token>{};

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

            auto required = sqes_required(completion.request());
            if (required > sq_space_left()) {
                deferred.push_back(token);
                continue;
            }

            if (stage_submission(d, token, completion.request()))
                completion.mark_submitted();
        }

        pending_submissions_.insert(
            pending_submissions_.begin(),
            deferred.begin(),
            deferred.end());
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
        auto deferred = std::unordered_set<wait_token>{};
        for (auto token : pending_cancellations_) {
            if (sq_space_left() == 0) {
                deferred.insert(token);
                continue;
            }

            auto * sqe = get_sqe();
            io_uring_prep_cancel64(
                sqe,
                static_cast<std::uint64_t>(token),
                0);
            attach_token(sqe, cancel_token(token));
            trace("uring prepare cancel token=" + std::to_string(token));
        }
        pending_cancellations_ = std::move(deferred);
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
    auto * poll_sqe = submission.get_sqe();
    io_uring_prep_poll_add(poll_sqe, fd, events);
    poll_sqe->flags |= IOSQE_IO_LINK;
    submission.attach(poll_sqe);

    auto * timeout_sqe = submission.get_sqe();
    io_uring_prep_link_timeout(
        timeout_sqe,
        &timeout,
        0);
    submission.attach(timeout_sqe);
    return true;
}

#endif

} // namespace nxt::rt
