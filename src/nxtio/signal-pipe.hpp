#pragma once

#include <array>
#include <optional>

namespace nxt::ui {

/// Portable async-signal-safe signal delivery via pipe.
/// Write end is written to from signal handlers (async-signal-safe).
/// Read end can be polled by any event loop.
class SignalPipe
{
public:
    SignalPipe();
    ~SignalPipe();

    // Non-copyable
    SignalPipe(const SignalPipe &) = delete;
    SignalPipe & operator=(const SignalPipe &) = delete;

    // Moveable
    SignalPipe(SignalPipe && other) noexcept;
    SignalPipe & operator=(SignalPipe && other) noexcept;

    /// File descriptor to poll for readability.
    [[nodiscard]] int read_fd() const noexcept
    {
        return fds_[0];
    }

    /// Called from signal handler context — async-signal-safe.
    /// Writes signal number as a single byte.
    static void notify(int signum);

    /// Read pending signals (non-blocking). Returns nullopt if no signal
    /// ready.
    std::optional<int> try_read();

    /// Install this pipe as the handler for the given signals.
    template<typename... Signals>
    void watch(Signals... signals)
    {
        const int sigs[] = {signals...};
        for (int sig : sigs)
            install_handler(sig);
    }

private:
    void install_handler(int sig);
    void close_fds();

    std::array<int, 2> fds_{-1, -1};

    // Singleton write-end for signal handler (must be static for signal
    // handler)
    static int s_write_fd;
};

} // namespace nxt::ui
