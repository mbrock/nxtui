#include "nxtio/subprocess.hpp"

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>

namespace nxt::subprocess {
namespace {

class UniqueFd
{
public:
    explicit UniqueFd(int fd = -1) noexcept
        : fd_(fd)
    {
    }

    ~UniqueFd()
    {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd & operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd && other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {
    }

    UniqueFd & operator=(UniqueFd && other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    [[nodiscard]] int release() noexcept
    {
        return std::exchange(fd_, -1);
    }

    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

[[noreturn]] void throw_errno(std::string_view what)
{
    throw std::system_error(errno, std::generic_category(), std::string{what});
}

void set_cloexec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        throw_errno("fcntl(F_GETFD)");
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        throw_errno("fcntl(F_SETFD)");
}

void set_nonblocking(int fd)
{
    auto flags = ::fcntl(fd, F_GETFL);
    if (flags < 0)
        throw_errno("fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw_errno("fcntl(F_SETFL)");
}

winsize winsize_from(Size size)
{
    return winsize{
        .ws_row = static_cast<unsigned short>(std::max<std::size_t>(
            1,
            size.h.count())),
        .ws_col = static_cast<unsigned short>(std::max<std::size_t>(
            1,
            size.w.count())),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
}

void set_winsize(int fd, Size size)
{
    auto ws = winsize_from(size);
    if (::ioctl(fd, TIOCSWINSZ, &ws) < 0)
        throw_errno("ioctl(TIOCSWINSZ)");
}

UniqueFd open_pty_master()
{
    UniqueFd master(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (master.get() < 0)
        throw_errno("posix_openpt");
    if (::grantpt(master.get()) < 0)
        throw_errno("grantpt");
    if (::unlockpt(master.get()) < 0)
        throw_errno("unlockpt");
    return master;
}

std::string pty_slave_name(int master_fd)
{
    std::array<char, 256> name{};
    if (::ptsname_r(master_fd, name.data(), name.size()) != 0)
        throw_errno("ptsname_r");
    return name.data();
}

std::vector<char *> argv_ptrs(const std::vector<std::string> & argv)
{
    std::vector<char *> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto & arg : argv)
        ptrs.push_back(const_cast<char *>(arg.c_str()));
    ptrs.push_back(nullptr);
    return ptrs;
}

std::vector<char *> environment_ptrs(
    const std::vector<std::string> & environment)
{
    std::vector<char *> ptrs;
    if (environment.empty())
        return ptrs;

    ptrs.reserve(environment.size() + 1);
    for (const auto & entry : environment)
        ptrs.push_back(const_cast<char *>(entry.c_str()));
    ptrs.push_back(nullptr);
    return ptrs;
}

std::string_view environment_name(std::string_view entry)
{
    auto equals = entry.find('=');
    if (equals == std::string_view::npos)
        return {};
    return entry.substr(0, equals);
}

std::vector<std::string> merge_environment(
    std::vector<std::string> environment,
    const std::vector<std::string> & overlay)
{
    for (const auto & entry : overlay) {
        auto name = environment_name(entry);
        if (name.empty())
            continue;

        auto it = std::ranges::find_if(environment, [&](const auto & item) {
            auto item_name = environment_name(item);
            return item_name == name;
        });
        if (it == environment.end())
            environment.push_back(entry);
        else
            *it = entry;
    }
    return environment;
}

void apply_environment_overlay(const std::vector<std::string> & overlay)
{
    for (const auto & entry : overlay) {
        auto name = environment_name(entry);
        if (name.empty())
            continue;
        auto value = std::string_view{entry}.substr(name.size() + 1);
        ::setenv(
            std::string{name}.c_str(),
            std::string{value}.c_str(),
            1);
    }
}

ExitStatus status_from_wait(pid_t pid, int status)
{
    ExitStatus out;
    out.pid = pid;
    out.raw_status = status;
    if (WIFEXITED(status)) {
        out.exited = true;
        out.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out.signaled = true;
        out.signal = WTERMSIG(status);
    }
    return out;
}

VTermModifier to_vterm_modifiers(const nxt::input::Modifiers & mods)
{
    int out = VTERM_MOD_NONE;
    if (mods.shift)
        out |= VTERM_MOD_SHIFT;
    if (mods.alt)
        out |= VTERM_MOD_ALT;
    if (mods.ctrl)
        out |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(out);
}

bool has_unsupported_modifiers(const nxt::input::Modifiers & mods)
{
    return mods.super || mods.hyper || mods.meta;
}

std::optional<char> legacy_ctrl_byte(std::uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z')
        cp = cp - 'A' + 'a';

    if (cp >= 'a' && cp <= 'z')
        return static_cast<char>(cp - 'a' + 1);

    switch (cp) {
    case ' ':
    case '@':
    case '2':
        return '\0';
    case '[':
    case '3':
        return '\x1b';
    case '\\':
    case '4':
        return '\x1c';
    case ']':
    case '5':
        return '\x1d';
    case '^':
    case '6':
        return '\x1e';
    case '_':
    case '7':
    case '/':
        return '\x1f';
    case '?':
    case '8':
        return '\x7f';
    default:
        return std::nullopt;
    }
}

std::uint32_t semantic_control_codepoint(
    const nxt::input::KeyEvent & event)
{
    // Kitty can also report shifted/base-layout alternate codepoints. Those
    // are useful for shortcut disambiguation in native TUIs, but a shell PTY
    // expects Ctrl-letter semantics from the primary key code.
    return event.codepoint;
}

std::string alt_prefix(std::string bytes, bool alt)
{
    if (alt)
        bytes.insert(bytes.begin(), '\x1b');
    return bytes;
}

std::optional<std::string> encode_c0_key(const nxt::input::KeyEvent & event)
{
    using nxt::input::Key;
    switch (event.key) {
    case Key::enter:
        return alt_prefix("\r", event.mods.alt);
    case Key::escape:
        return alt_prefix("\x1b", event.mods.alt);
    case Key::backspace:
        return alt_prefix(
            event.mods.ctrl ? std::string{"\b"} : std::string{"\x7f"},
            event.mods.alt);
    case Key::tab:
        if (event.mods.shift)
            return alt_prefix("\x1b[Z", event.mods.alt);
        return alt_prefix("\t", event.mods.alt);
    default:
        return std::nullopt;
    }
}

std::optional<VTermKey> to_vterm_key(nxt::input::Key key)
{
    using nxt::input::Key;

    switch (key) {
    case Key::enter:
        return VTERM_KEY_ENTER;
    case Key::tab:
        return VTERM_KEY_TAB;
    case Key::backspace:
        return VTERM_KEY_BACKSPACE;
    case Key::escape:
        return VTERM_KEY_ESCAPE;
    case Key::insert:
        return VTERM_KEY_INS;
    case Key::delete_:
        return VTERM_KEY_DEL;
    case Key::left:
        return VTERM_KEY_LEFT;
    case Key::right:
        return VTERM_KEY_RIGHT;
    case Key::up:
        return VTERM_KEY_UP;
    case Key::down:
        return VTERM_KEY_DOWN;
    case Key::home:
        return VTERM_KEY_HOME;
    case Key::end:
        return VTERM_KEY_END;
    case Key::page_up:
        return VTERM_KEY_PAGEUP;
    case Key::page_down:
        return VTERM_KEY_PAGEDOWN;
    case Key::f1:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(1));
    case Key::f2:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(2));
    case Key::f3:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(3));
    case Key::f4:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(4));
    case Key::f5:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(5));
    case Key::f6:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(6));
    case Key::f7:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(7));
    case Key::f8:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(8));
    case Key::f9:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(9));
    case Key::f10:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(10));
    case Key::f11:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(11));
    case Key::f12:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(12));
    default:
        return std::nullopt;
    }
}

std::string default_shell()
{
    if (const char * shell = std::getenv("SHELL");
        shell != nullptr && *shell != '\0')
        return shell;
    return "/bin/sh";
}

} // namespace

std::string ExitStatus::describe() const
{
    if (exited)
        return std::format("exited {}", exit_code);
    if (signaled)
        return std::format("signal {}", signal);
    return "unknown";
}

PtySession::PtySession(int master_fd, pid_t child_pid, Size size)
    : master_fd_(master_fd)
    , child_pid_(child_pid)
    , size_(size)
    , terminal_(
          static_cast<int>(std::max<std::size_t>(1, size.h.count())),
          static_cast<int>(std::max<std::size_t>(1, size.w.count())))
{
}

PtySession::~PtySession()
{
    terminate(SIGHUP);
    close_master();
    (void) try_reap();
}

PtySession::PtySession(PtySession && other) noexcept
    : master_fd_(std::exchange(other.master_fd_, -1))
    , child_pid_(std::exchange(other.child_pid_, -1))
    , size_(other.size_)
    , terminal_(std::move(other.terminal_))
    , exit_status_(std::move(other.exit_status_))
{
}

PtySession & PtySession::operator=(PtySession && other) noexcept
{
    if (this != &other) {
        terminate(SIGHUP);
        close_master();
        (void) try_reap();

        master_fd_ = std::exchange(other.master_fd_, -1);
        child_pid_ = std::exchange(other.child_pid_, -1);
        size_ = other.size_;
        terminal_ = std::move(other.terminal_);
        exit_status_ = std::move(other.exit_status_);
    }

    return *this;
}

PtySession PtySession::spawn(const SpawnOptions & options)
{
    if (options.argv.empty())
        throw std::invalid_argument("SpawnOptions::argv must not be empty");

    auto master = open_pty_master();
    auto slave_name = pty_slave_name(master.get());
    UniqueFd slave(::open(slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (slave.get() < 0)
        throw_errno("open pty slave");

    set_winsize(slave.get(), options.size);

    auto child_environment = options.environment.empty()
        ? std::vector<std::string>{}
        : merge_environment(options.environment, options.environment_overlay);
    auto argv = argv_ptrs(options.argv);
    auto env = environment_ptrs(child_environment);

    const pid_t pid = ::fork();
    if (pid < 0)
        throw_errno("fork");

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

        if (options.cwd && ::chdir(options.cwd->c_str()) < 0)
            _exit(127);

        if (options.environment.empty())
            apply_environment_overlay(options.environment_overlay);

        if (env.empty())
            ::execvp(argv[0], argv.data());
        else
            ::execve(argv[0], argv.data(), env.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    slave.reset();
    set_cloexec(master.get());
    set_nonblocking(master.get());

    return PtySession(master.release(), pid, options.size);
}

PtySession PtySession::shell(
    Size size,
    std::string shell_path,
    std::optional<std::filesystem::path> cwd)
{
    if (shell_path.empty())
        shell_path = default_shell();

    return spawn(SpawnOptions{
        .argv = {shell_path, "-i"},
        .cwd = std::move(cwd),
        .environment = {},
        .environment_overlay = {
            "TERM=xterm-256color",
            "COLORTERM=truecolor"},
        .size = size,
    });
}

void PtySession::resize(Size size)
{
    if (size.w == 0 * ch || size.h == 0 * ln)
        return;
    if (size.w == size_.w && size.h == size_.h)
        return;

    size_ = size;
    if (master_fd_ >= 0) {
        auto ws = winsize_from(size);
        if (::ioctl(master_fd_, TIOCSWINSZ, &ws) < 0
            && errno != EBADF && errno != EIO && errno != ENOTTY)
            throw_errno("ioctl(TIOCSWINSZ)");
    }

    std::scoped_lock lock{terminal_mutex_};
    terminal_.set_size(
        static_cast<int>(size.h.count()),
        static_cast<int>(size.w.count()));
}

void PtySession::terminate(int signal) noexcept
{
    if (child_pid_ > 0 && !exit_status_)
        (void) ::kill(-child_pid_, signal);
}

void PtySession::close_master() noexcept
{
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
}

void PtySession::write(std::string_view bytes)
{
    while (!bytes.empty()) {
        auto n = ::write(master_fd_, bytes.data(), bytes.size());
        if (n > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            throw std::runtime_error("pty write would block");
        throw_errno("write pty");
    }
}

nxt::task<> PtySession::write_all(
    nxt::scheduler & scheduler,
    std::string bytes,
    std::stop_token stop)
{
    std::size_t offset = 0;
    while (offset < bytes.size() && !stop.stop_requested()) {
        auto n = ::write(master_fd_, bytes.data() + offset, bytes.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto status = co_await scheduler.poll(
                master_fd_,
                nxt::poll_op::write,
                std::chrono::milliseconds{100});
            if (status == nxt::poll_status::closed
                || status == nxt::poll_status::error)
                co_return;
            continue;
        }
        throw_errno("write pty");
    }
}

std::string PtySession::encode_key(const nxt::input::KeyEvent & event)
{
    // nxt reads rich Kitty CSI-u key events from the outer terminal, but an
    // ordinary shell PTY expects legacy terminal input bytes. Normalize here
    // and drop chords that would otherwise leak untranslated CSI-u text into
    // readline.
    if (event.type == nxt::input::EventType::release)
        return {};

    if (has_unsupported_modifiers(event.mods))
        return {};

    if (event.is_text())
        return event.text;

    if (event.key == nxt::input::Key::character) {
        if (event.mods.ctrl) {
            if (event.mods.shift)
                return {};
            auto cp = semantic_control_codepoint(event);
            if (auto byte = legacy_ctrl_byte(cp))
                return alt_prefix(std::string{byte.value()}, event.mods.alt);
            return {};
        }

        if (!event.text.empty())
            return alt_prefix(event.text, event.mods.alt);

        if (event.codepoint != 0 && !event.mods.shift) {
            std::scoped_lock lock{terminal_mutex_};
            terminal_.keyboard_unichar(
                event.codepoint,
                to_vterm_modifiers(event.mods));
            return terminal_.read_pending_output();
        }

        return {};
    }

    if (auto bytes = encode_c0_key(event))
        return *bytes;

    std::scoped_lock lock{terminal_mutex_};
    const auto modifiers = to_vterm_modifiers(event.mods);

    if (auto key = to_vterm_key(event.key)) {
        terminal_.keyboard_key(*key, modifiers);
        return terminal_.read_pending_output();
    }

    return {};
}

nxt::task<> PtySession::send_key(
    nxt::scheduler & scheduler,
    const nxt::input::KeyEvent & event,
    std::stop_token stop)
{
    auto bytes = encode_key(event);
    if (!bytes.empty())
        co_await write_all(scheduler, std::move(bytes), stop);
}

std::optional<ExitStatus> PtySession::try_reap() noexcept
{
    if (exit_status_)
        return exit_status_;
    if (child_pid_ <= 0)
        return std::nullopt;

    int status = 0;
    auto pid = ::waitpid(child_pid_, &status, WNOHANG);
    if (pid == child_pid_) {
        exit_status_ = status_from_wait(child_pid_, status);
        child_pid_ = -1;
        return exit_status_;
    }

    if (pid < 0 && errno == ECHILD) {
        exit_status_ = ExitStatus{.pid = child_pid_};
        child_pid_ = -1;
        return exit_status_;
    }

    return std::nullopt;
}

nxt::task<ExitStatus> PtySession::finish_reap(
    nxt::scheduler & scheduler,
    std::stop_token stop)
{
    if (auto status = try_reap())
        co_return *status;

    auto wait_for_reap =
        [this, &scheduler, stop](int attempts) -> nxt::task<bool> {
        for (int i = 0; i < attempts; ++i) {
            if (auto status = try_reap()) {
                (void) status;
                co_return true;
            }
            if (stop.stop_requested() && i > 0)
                co_return false;
            co_await scheduler.yield_for(std::chrono::milliseconds{25});
        }
        co_return false;
    };

    if (co_await wait_for_reap(8)) {
        co_return *exit_status_;
    }

    if (child_pid_ > 0) {
        terminate(stop.stop_requested() ? SIGHUP : SIGTERM);
        if (co_await wait_for_reap(8))
            co_return *exit_status_;
    }

    if (child_pid_ > 0) {
        terminate(SIGKILL);
        if (co_await wait_for_reap(40))
            co_return *exit_status_;
    }

    if (!exit_status_)
        exit_status_ = ExitStatus{};
    co_return *exit_status_;
}

nxt::task<ExitStatus> PtySession::read_loop(
    nxt::scheduler & scheduler,
    std::stop_token stop,
    DamageCallback on_damage)
{
    std::array<char, 8192> buffer{};
    bool eof = false;

    while (!eof && !stop.stop_requested()) {
        (void) try_reap();

        auto status = co_await scheduler.poll(
            master_fd_,
            nxt::poll_op::read,
            std::chrono::milliseconds{100});
        if (status == nxt::poll_status::timeout)
            continue;
        if (status == nxt::poll_status::closed
            || status == nxt::poll_status::error)
            break;

        while (!eof && !stop.stop_requested()) {
            auto n = ::read(master_fd_, buffer.data(), buffer.size());
            if (n > 0) {
                std::string reply;
                {
                    std::scoped_lock lock{terminal_mutex_};
                    terminal_.write(std::string_view{
                        buffer.data(),
                        static_cast<std::size_t>(n)});
                    reply = terminal_.read_pending_output();
                }

                if (on_damage)
                    on_damage();

                if (!reply.empty())
                    co_await write_all(scheduler, std::move(reply), stop);
                continue;
            }

            if (n == 0) {
                eof = true;
                break;
            }

            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EIO) {
                eof = true;
                break;
            }

            throw_errno("read pty");
        }
    }

    if (stop.stop_requested())
        terminate(SIGHUP);

    auto status = co_await finish_reap(scheduler, stop);
    if (on_damage)
        on_damage();
    co_return status;
}

} // namespace nxt::subprocess
