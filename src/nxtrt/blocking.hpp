#pragma once

#include "nxtrt/idea.hpp"
#include "nxtrt/task.hpp"
#include "nxt/unique-fd.hpp"

#include <cerrno>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>

namespace nxtrt {

/// Ordinary owned synchronous work, not a task factory or borrowed result.
template<typename Fn>
concept blocking_call =
    std::move_constructible<Fn> && std::invocable<Fn &> && !idea<Fn>
    && !std::is_reference_v<std::invoke_result_t<Fn &>>
    && (std::is_void_v<std::invoke_result_t<Fn &>>
        || std::move_constructible<std::invoke_result_t<Fn &>>);

namespace detail {

// A level-readable pipe. Only fd writes cross threads; no deck, bell, wand,
// promise, or continuation is touched. Multiple writes may coalesce at
// EAGAIN.
class blocking_notification
{
public:
    blocking_notification()
    {
        int fds[2];
        if (::pipe(fds) != 0)
            throw runtime_error{"blocking notification pipe failed"};
        read_.reset(fds[0]);
        write_.reset(fds[1]);
        for (auto fd : fds) {
            auto flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
                || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                throw runtime_error{"blocking notification setup failed"};
        }
    }

    void notify() noexcept
    {
        char byte = 1;
        while (::write(write_.get(), &byte, 1) < 0 && errno == EINTR) {
        }
    }

    void drain() noexcept
    {
        char bytes[128];
        while (true) {
            auto n = ::read(read_.get(), bytes, sizeof(bytes));
            if (n > 0 || (n < 0 && errno == EINTR))
                continue;
            break;
        }
    }

    task<void> wait()
    {
        // A raw task handle must not destroy a parked wand continuation.
        // Fail fast instead of leaving an fd exec pointing into a freed
        // frame.
        bool resumed = false;

        struct guard
        {
            bool & resumed;

            ~guard()
            {
                if (!resumed) {
                    std::fputs(
                        "nxtrt: destroyed a pending blocking wait\n",
                        stderr);
                    std::abort();
                }
            }
        } residence{resumed};

        try {
            co_await op::poll{read_.get(), POLLIN};
        } catch (...) {
            resumed = true;
            throw;
        }
        resumed = true;
    }

private:
    nxt::unique_fd read_, write_;
};

struct blocking_job
{
    virtual ~blocking_job() = default;
    virtual void invoke() noexcept = 0;

    void cancel() noexcept
    {
        auto lock = std::lock_guard{mutex};
        cancelled = true;
        changed.notify();
    }

    void execute() noexcept
    {
        {
            auto lock = std::lock_guard{mutex};
            // This is the queued -> running linearization point.
            // Cancellation after it cannot prevent invocation, even before
            // user code starts.
            running = !cancelled;
        }
        if (running)
            invoke();
    }

    void publish() noexcept
    {
        auto lock = std::lock_guard{mutex};
        done = true;
        changed.notify();
        settled.notify_all();
    }

    // Safety for an exception in owner-side wait setup. Normal cancellation
    // uses asynchronous settlement; destroying parked tasks is rejected.
    void join() noexcept
    {
        auto lock = std::unique_lock{mutex};
        if (submitted)
            settled.wait(lock, [this] { return done; });
    }

    blocking_notification changed;
    std::mutex mutex;
    std::condition_variable settled;
    bool submitted = false;
    bool running = false;
    bool cancelled = false;
    bool done = false;
};

template<blocking_call Fn>
struct typed_blocking_job final : blocking_job
{
    using result_type = std::invoke_result_t<Fn &>;
    using stored_type = std::conditional_t<
        std::is_void_v<result_type>,
        std::monostate,
        result_type>;

    explicit typed_blocking_job(Fn fn)
        : fn(std::move(fn))
    {
    }

    void invoke() noexcept override
    {
        try {
            if constexpr (std::is_void_v<result_type>) {
                std::invoke(fn);
                result.emplace();
            } else {
                result.emplace(std::invoke(fn));
            }
        } catch (...) {
            failure = std::current_exception();
        }
    }

    Fn fn;
    std::optional<stored_type> result;
    std::exception_ptr failure;
};

} // namespace detail

/// One-deck bounded admission of synchronous work to fixed worker threads.
/// capacity includes queued, running and settled/not-yet-delivered calls.
/// Await close() before destruction when stopping early. Workers never use
/// NXT.
class blocking_pool
{
public:
    explicit blocking_pool(std::size_t workers, std::size_t capacity)
        : capacity_(capacity)
    {
        if (workers == 0 || capacity == 0)
            throw runtime_error{"blocking pool needs workers and capacity"};
        try {
            for (std::size_t i = 0; i != workers; ++i)
                workers_.emplace_back([this] { work(); });
        } catch (...) {
            join_workers();
            throw;
        }
    }

    blocking_pool(const blocking_pool &) = delete;
    blocking_pool & operator=(const blocking_pool &) = delete;

    ~blocking_pool()
    {
        if (!calls_.empty()) {
            std::fputs(
                "nxtrt: blocking pool destroyed before calls settled\n",
                stderr);
            std::abort();
        }
        join_workers();
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::size_t occupied() const noexcept
    {
        return occupied_;
    }

    template<blocking_call Fn>
    struct recipe
    {
        blocking_pool * owner;
        Fn fn;
        std::stop_token stop;

        task<std::invoke_result_t<Fn &>> operator()()
        {
            return owner->run(std::move(fn), stop);
        }
    };

    /// Lazy, move-only-capable idea. Invocation transfers its callable to a
    /// task; worker admission happens only when that task runs on the deck.
    template<blocking_call Fn>
    [[nodiscard]] recipe<Fn> call(Fn fn, std::stop_token stop = {})
    {
        return {this, std::move(fn), stop};
    }

    /// Cancellation wins if observed before result delivery. It skips
    /// queued work but drains running work, discarding either its result or
    /// exception.
    template<blocking_call Fn>
    [[nodiscard]] task<std::invoke_result_t<Fn &>>
    run(Fn fn, std::stop_token stop = {})
    {
        bind();
        auto job =
            std::make_shared<detail::typed_blocking_job<Fn>>(std::move(fn));
        calls_.push_back(job);
        auto residence = call_guard{*this, *job};
        auto cancel = [job] { job->cancel(); }; // ordinary, not coroutine
        auto on_stop =
            std::stop_callback{current_task_stop_token(), cancel};
        auto external_stop = std::stop_callback{stop, cancel};
        while (true) {
            job->changed.drain();
            {
                auto lock = std::lock_guard{job->mutex};
                if (closing_ || job->cancelled)
                    throw operation_cancelled{};
                if (occupied_ < capacity_) {
                    auto queue_lock = std::lock_guard{queue_mutex_};
                    queue_.push_back(job);
                    job->submitted = true;
                    ++occupied_;
                    break;
                }
            }
            co_await hope<void>{job->changed.wait(), false};
        }
        available_.notify_one();
        while (true) {
            job->changed.drain();
            {
                auto lock = std::lock_guard{job->mutex};
                if (job->done) {
                    if (job->cancelled)
                        throw operation_cancelled{};
                    if (job->failure)
                        std::rethrow_exception(job->failure);
                    break;
                }
            }
            // A fresh delegate does not follow this task's stop token: the
            // fd poll must remain alive until worker publication has
            // settled.
            co_await hope<void>{job->changed.wait(), false};
        }
        if constexpr (!std::is_void_v<std::invoke_result_t<Fn &>>)
            co_return std::move(*job->result);
    }

    /// Owner-deck only. Cancels admission/queued work; running code
    /// settles.
    void stop() noexcept
    {
        closing_ = true;
        for (auto & job : calls_)
            job->cancel();
    }

    /// Terminal and idempotent; itself stop-shielded. Also joins the
    /// threads.
    [[nodiscard]] task<void> close()
    {
        bind();
        stop();
        while (!calls_.empty()) {
            changed_.drain();
            co_await hope<void>{changed_.wait(), false};
        }
        join_workers();
    }

private:
    struct call_guard
    {
        blocking_pool & owner;
        detail::blocking_job & job;

        ~call_guard()
        {
            job.join();
            if (job.submitted)
                --owner.occupied_;
            std::erase_if(
                owner.calls_, [this](auto & p) { return p.get() == &job; });
            for (auto & p : owner.calls_)
                p->changed.notify();
            owner.changed_.notify();
        }
    };

    void bind()
    {
        auto * d = current_deck();
        if (d == nullptr || (deck_ != nullptr && deck_ != d))
            throw runtime_error{"blocking pool needs its original deck"};
        deck_ = d;
    }

    void work() noexcept
    {
        while (true) {
            std::shared_ptr<detail::blocking_job> job;
            {
                auto lock = std::unique_lock{queue_mutex_};
                available_.wait(lock, [this] {
                    return worker_stop_ || !queue_.empty();
                });
                if (queue_.empty())
                    return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job->execute();
            auto * finished = job.get();
            // The task's residence keeps this alive until publication. Drop
            // the worker's ownership first so callable/result destruction
            // can never happen late on a worker after the task's scope has
            // exited.
            job.reset();
            finished
                ->publish(); // no worker access after releasing this lock
        }
    }

    void join_workers() noexcept
    {
        {
            auto lock = std::lock_guard{queue_mutex_};
            worker_stop_ = true;
        }
        available_.notify_all();
        for (auto & worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    const std::size_t capacity_;
    std::size_t occupied_ = 0;
    deck * deck_ = nullptr;
    bool closing_ = false;
    detail::blocking_notification changed_;
    std::vector<std::shared_ptr<detail::blocking_job>> calls_; // owner only
    std::mutex queue_mutex_;
    std::condition_variable available_;
    std::deque<std::shared_ptr<detail::blocking_job>> queue_;
    bool worker_stop_ = false;
    std::vector<std::thread> workers_;
};

} // namespace nxtrt
