#pragma once

#include "nxtrt/task.hpp"

namespace nxtrt {

inline constexpr bool has_iocp_wand =
#if defined(_WIN32)
    true;
#else
    false;
#endif

#if defined(_WIN32)
namespace detail {
class iocp_impl;
}

/// Windows/UWP completion-port backend, including Xbox apps built by
/// nixbox.
///
/// Implements manual, timeout, read/write, recv/send, ConnectEx and
/// AcceptEx. File/pipe wishes borrow native HANDLEs opened with
/// FILE_FLAG_OVERLAPPED; sockets borrow SOCKETs created with
/// WSA_FLAG_OVERLAPPED. The host must attach each handle once before I/O;
/// accepted sockets are already attached. Handles cannot use another port.
/// Do not enable
/// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS on these handles. The host
/// initializes Winsock and closes handles after wishes finish. Disk I/O
/// requires explicit nonnegative offsets; byte-mode pipes use -1. Accept
/// flags must be zero; the returned socket belongs to the caller. Readiness
/// poll wishes are rejected: IOCP completes I/O, not readiness. POSIX opens
/// and child-process wishes are not exposed on Windows.
///
/// Timers use steady-clock deadlines and the completion-port wait timeout,
/// rounded up to milliseconds. poll is nonblocking for UI-loop integration.
/// CancelIoEx requests cancellation but does not release the OVERLAPPED or
/// borrowed buffer: settlement waits for the operation's completion packet.
/// A successful completion racing cancellation still delivers its result.
/// Like the other wands, confined to the deck's thread.
class iocp_wand final : public wand
{
public:
    iocp_wand();
    ~iocp_wand() override;

    iocp_wand(const iocp_wand &) = delete;
    iocp_wand & operator=(const iocp_wand &) = delete;
    iocp_wand(iocp_wand &&) = delete;
    iocp_wand & operator=(iocp_wand &&) = delete;

    /// Associate a newly opened, unassociated handle with this port, once
    /// per handle lifetime, before awaiting I/O. Does not take ownership.
    /// Reattaching even to the same port fails. Close/reopen requires a new
    /// attachment, including when Windows reuses the numeric handle value.
    void attach(io_handle handle);
    void attach(socket_handle socket);

    void suspend(coin_t token, need task) override;
    void cancel(coin_t token) override;
    void wave(deck & d) override;
    void poll(deck & d);
    void wait(deck & d);
    void run_until_done(deck & d, std::coroutine_handle<> root);

    template<typename T>
    void run_until_done(deck & d, task<T> & root)
    {
        run_until_done(d, std::coroutine_handle<>{root.handle()});
    }

protected:
    coin_t
    prep(deck &, detail::promise_base &, detail::prepared_wish) override;

private:
    std::unique_ptr<detail::iocp_impl> impl_;
};

/// Create the root inside a fresh runtime environment, then drive its port.
/// The stored factory outlives the task, including capturing coroutine
/// lambdas.
template<task_factory Fn>
[[nodiscard]] inline task_result_t<std::invoke_result_t<Fn>>
run_with_iocp(Fn && fn)
{
    auto wand = iocp_wand{};
    auto d = deck{&wand};
    auto env = runtime_env{};
    auto guard = detail::env_guard{env, &d, nullptr};
    auto factory = std::decay_t<Fn>{std::forward<Fn>(fn)};
    auto root = std::invoke(factory);
    d.start(root);
    wand.run_until_done(d, root);
    return std::move(root).result();
}
#endif

} // namespace nxtrt
