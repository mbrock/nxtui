#include "nxtio/signal-pipe.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace nxt::ui {

int SignalPipe::s_write_fd = -1;

SignalPipe::SignalPipe()
{
    if (pipe(fds_.data()) < 0)
        throw std::runtime_error("Failed to create signal pipe");

    // Make both ends non-blocking and close-on-exec
    for (const int fd : fds_) {
        const int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    s_write_fd = fds_[1];
}

SignalPipe::~SignalPipe()
{
    close_fds();
}

SignalPipe::SignalPipe(SignalPipe && other) noexcept
    : fds_(other.fds_)
{
    const int old_write_fd = other.fds_[1];
    other.fds_ = {-1, -1};
    if (s_write_fd == old_write_fd)
        s_write_fd = fds_[1];
}

SignalPipe & SignalPipe::operator=(SignalPipe && other) noexcept
{
    if (this != &other) {
        close_fds();
        fds_ = other.fds_;
        const int old_write_fd = other.fds_[1];
        other.fds_ = {-1, -1};
        if (s_write_fd == old_write_fd)
            s_write_fd = fds_[1];
    }
    return *this;
}

void SignalPipe::close_fds()
{
    if (fds_[0] >= 0)
        close(fds_[0]);
    if (fds_[1] >= 0)
        close(fds_[1]);
    if (s_write_fd == fds_[1])
        s_write_fd = -1;
    fds_ = {-1, -1};
}

void SignalPipe::notify(const int signum)
{
    // This is called from signal handler — only async-signal-safe ops!
    if (s_write_fd >= 0) {
        const char c = static_cast<char>(signum);
        // Ignore errors — best effort, pipe might be full
        (void) write(s_write_fd, &c, 1);
    }
}

std::optional<int> SignalPipe::try_read()
{
    char c;
    const ssize_t n = read(fds_[0], &c, 1);
    if (n == 1)
        return static_cast<int>(static_cast<unsigned char>(c));
    return std::nullopt;
}

void SignalPipe::install_handler(const int sig)
{
    struct sigaction sa{};
    sa.sa_handler = [](const int s) { SignalPipe::notify(s); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, nullptr);
}

} // namespace nxt::ui
