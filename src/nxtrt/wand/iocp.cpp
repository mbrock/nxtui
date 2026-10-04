#include "nxtrt/wand/iocp.hpp"

#if defined(_WIN32)
#  include "nxtrt/exec_lifecycle.hpp"

#  include <mswsock.h>
#  include <algorithm>
#  include <array>
#  include <chrono>
#  include <map>
#  include <optional>
#  include <system_error>

namespace nxtrt::detail {
namespace {

[[noreturn]] void windows_error(DWORD code)
{
    throw std::system_error{static_cast<int>(code), std::system_category()};
}

HANDLE socket_file(SOCKET socket)
{
    return reinterpret_cast<HANDLE>(socket);
}

template<typename Function>
Function extension(SOCKET socket, GUID id)
{
    Function function = nullptr;
    DWORD bytes = 0;
    if (WSAIoctl(
            socket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &id,
            sizeof(id),
            &function,
            sizeof(function),
            &bytes,
            nullptr,
            nullptr)
        == SOCKET_ERROR)
        windows_error(WSAGetLastError());
    return function;
}

} // namespace

class iocp_impl
{
    using clock = std::chrono::steady_clock;
    using queued = wand_exec::queued;

    struct submitted
    {
        HANDLE handle;
    };

    struct cancelling
    {
        HANDLE handle;
    };

    struct cancel_queued
    {};

    struct timer
    {
        clock::time_point deadline;
    };

    using lifecycle = wand_exec::lifecycle<
        std::variant<queued, submitted, cancelling, cancel_queued, timer>,
        std::variant<wand_exec::ready_to_retire>>;
    using parked = lifecycle::parked;

    // Inherit so the kernel's OVERLAPPED pointer can be safely downcast.
    // std::map keeps each record's address stable until retirement.
    struct exec : OVERLAPPED
    {
        explicit exec(prepared_wish packet)
            : OVERLAPPED{}
            , packet(std::move(packet))
        {
        }

        ~exec()
        {
            if (accepted != INVALID_SOCKET)
                closesocket(accepted);
        }

        prepared_wish packet;
        lifecycle::state state = wand_exec::prepared{};
        WSABUF buffer{};
        DWORD flags = 0;
        SOCKET accepted = INVALID_SOCKET;
        static constexpr DWORD address_bytes =
            sizeof(sockaddr_storage) + 16;
        std::array<std::byte, address_bytes * 2> addresses{};
    };

public:
    iocp_impl()
        : port_(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1))
    {
        if (!port_)
            windows_error(GetLastError());
    }

    ~iocp_impl()
    {
        // Even during teardown, never free an OVERLAPPED still owned by the
        // kernel. Normal use drains tasks before destroying their wand.
        for (auto & [token, e] : execs_) {
            if (auto * p = std::get_if<parked>(&e.state)) {
                if (auto * s = std::get_if<submitted>(&p->phase))
                    CancelIoEx(s->handle, &e);
            }
        }
        while (in_flight_ != 0) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED * overlapped = nullptr;
            GetQueuedCompletionStatus(
                port_, &bytes, &key, &overlapped, INFINITE);
            if (overlapped)
                --in_flight_;
        }
        CloseHandle(port_);
    }

    coin_t prep(prepared_wish packet)
    {
        auto token = ++next_coin_;
        execs_.try_emplace(token, std::move(packet));
        return token;
    }

    void suspend(coin_t token, need task)
    {
        auto it = execs_.find(token);
        if (it != execs_.end()
            && std::holds_alternative<wand_exec::prepared>(
                it->second.state))
            it->second.state = parked{task, queued{}};
    }

    void cancel(coin_t token)
    {
        auto it = execs_.find(token);
        if (it == execs_.end())
            return;
        auto & e = it->second;
        auto * p = std::get_if<parked>(&e.state);
        if (!p)
            return;
        if (auto * s = std::get_if<submitted>(&p->phase)) {
            auto handle = s->handle;
            // ERROR_NOT_FOUND means the completion may already be queued.
            // In either case only that packet ends kernel buffer ownership.
            CancelIoEx(handle, &e);
            p->phase = cancelling{handle};
        } else if (
            std::holds_alternative<queued>(p->phase)
            || std::holds_alternative<timer>(p->phase)) {
            p->phase = cancel_queued{};
        }
    }

    void wave(deck & d)
    {
        std::erase_if(execs_, [](auto const & entry) {
            return lifecycle::is_retirable(entry.second.state);
        });
        for (auto & [token, e] : execs_) {
            auto * p = std::get_if<parked>(&e.state);
            if (!p)
                continue;
            try {
                if (std::holds_alternative<cancel_queued>(p->phase))
                    throw operation_cancelled{};
                if (std::holds_alternative<queued>(p->phase))
                    std::visit(
                        [&](auto & wish) { submit(d, e, wish); },
                        e.packet.wish);
            } catch (...) {
                finish(d, e, 0, std::current_exception());
            }
        }
        expire(d);
    }

    void poll(deck & d)
    {
        wave(d);
        while (receive(d, 0)) {
        }
        expire(d);
    }

    void wait(deck & d)
    {
        poll(d);
        if (!d.empty())
            return;
        auto deadline = std::optional<clock::time_point>{};
        for (auto & [token, e] : execs_) {
            if (auto * p = std::get_if<parked>(&e.state)) {
                if (auto * t = std::get_if<timer>(&p->phase))
                    if (!deadline || t->deadline < *deadline)
                        deadline = t->deadline;
            }
        }
        if (!deadline && !in_flight_)
            throw runtime_error{"nxtrt iocp wand deadlock"};
        auto timeout = DWORD{INFINITE};
        if (deadline) {
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(
                          *deadline - clock::now())
                          .count();
            timeout = static_cast<DWORD>(
                std::clamp<std::int64_t>(ms, 0, INFINITE - 1));
        }
        receive(d, timeout);
        poll(d);
    }

    void run_until_done(deck & d, std::coroutine_handle<> root)
    {
        while (!root.done()) {
            if (!d.empty())
                d.run_ready();
            poll(d);
            if (d.empty() && !root.done())
                wait(d);
        }
    }

    void associate(HANDLE handle)
    {
        if (!CreateIoCompletionPort(handle, port_, 0, 1))
            windows_error(GetLastError());
    }

private:
    void submitted_io(exec & e, HANDLE handle, bool success, DWORD error)
    {
        if (!success && error != ERROR_IO_PENDING)
            windows_error(error);
        // Synchronous success also queues a packet (skip-on-success is
        // banned).
        std::get<parked>(e.state).phase = submitted{handle};
        ++in_flight_;
    }

    void submit(deck & d, exec & e, op::manual &)
    {
        finish(d, e, 0);
    }

    void submit(deck &, exec & e, op::timeout & wish)
    {
        auto duration = std::chrono::seconds{wish.duration.tv_sec}
                        + std::chrono::nanoseconds{wish.duration.tv_nsec};
        std::get<parked>(e.state).phase = timer{clock::now() + duration};
    }

    template<typename Wish>
        requires(
            std::same_as<Wish, op::read_some>
            || std::same_as<Wish, op::write_some>)
    void submit(deck & d, exec & e, Wish & wish)
    {
        if (wish.offset < 0 && GetFileType(wish.fd) != FILE_TYPE_PIPE)
            throw invalid_argument{
                "IOCP disk I/O requires an explicit offset"};
        if (wish.offset >= 0) {
            auto offset = static_cast<std::uint64_t>(wish.offset);
            e.Offset = static_cast<DWORD>(offset);
            e.OffsetHigh = static_cast<DWORD>(offset >> 32);
        }
        auto size = static_cast<DWORD>(
            std::min<std::size_t>(wish.buffer.size(), MAXDWORD));
        BOOL success;
        if constexpr (std::same_as<Wish, op::read_some>)
            success =
                ReadFile(wish.fd, wish.buffer.data(), size, nullptr, &e);
        else
            success =
                WriteFile(wish.fd, wish.buffer.data(), size, nullptr, &e);
        auto error = success ? ERROR_SUCCESS : GetLastError();
        if constexpr (std::same_as<Wish, op::read_some>) {
            if (!success
                && (error == ERROR_HANDLE_EOF
                    || error == ERROR_BROKEN_PIPE)) {
                finish(d, e, 0);
                return;
            }
        }
        submitted_io(e, wish.fd, success, error);
    }

    template<typename Wish>
        requires(
            std::same_as<Wish, op::recv_some>
            || std::same_as<Wish, op::send_some>)
    void submit(deck &, exec & e, Wish & wish)
    {
        e.buffer.len = static_cast<ULONG>(
            std::min<std::size_t>(wish.buffer.size(), MAXDWORD));
        e.buffer.buf = reinterpret_cast<char *>(
            const_cast<std::byte *>(wish.buffer.data()));
        e.flags = static_cast<DWORD>(wish.flags);
        int result;
        if constexpr (std::same_as<Wish, op::recv_some>)
            result = WSARecv(
                wish.fd, &e.buffer, 1, nullptr, &e.flags, &e, nullptr);
        else
            result = WSASend(
                wish.fd, &e.buffer, 1, nullptr, e.flags, &e, nullptr);
        submitted_io(
            e,
            socket_file(wish.fd),
            result == 0,
            result == 0 ? ERROR_SUCCESS : WSAGetLastError());
    }

    void submit(deck &, exec & e, op::connect & wish)
    {
        if (wish.address_size <= 0
            || static_cast<std::size_t>(wish.address_size)
                   > sizeof(wish.address))
            throw invalid_argument{"invalid ConnectEx address size"};
        auto function = extension<LPFN_CONNECTEX>(wish.fd, WSAID_CONNECTEX);
        sockaddr_storage local{};
        int length = sizeof(local);
        if (getsockname(
                wish.fd, reinterpret_cast<sockaddr *>(&local), &length)
            == SOCKET_ERROR) {
            auto error = WSAGetLastError();
            if (error != WSAEINVAL)
                windows_error(error);
            local.ss_family = wish.address.ss_family;
            if (local.ss_family == AF_INET)
                length = sizeof(sockaddr_in);
            else if (local.ss_family == AF_INET6)
                length = sizeof(sockaddr_in6);
            else
                throw invalid_argument{"ConnectEx requires IPv4 or IPv6"};
            if (bind(wish.fd, reinterpret_cast<sockaddr *>(&local), length)
                == SOCKET_ERROR)
                windows_error(WSAGetLastError());
        }
        auto success = function(
            wish.fd,
            wish.sockaddr_ptr(),
            wish.address_size,
            nullptr,
            0,
            nullptr,
            &e);
        submitted_io(
            e,
            socket_file(wish.fd),
            success,
            success ? ERROR_SUCCESS : WSAGetLastError());
    }

    void submit(deck &, exec & e, op::accept & wish)
    {
        if (wish.flags != 0)
            throw invalid_argument{"IOCP accept flags must be zero"};
        auto function = extension<LPFN_ACCEPTEX>(wish.fd, WSAID_ACCEPTEX);
        WSAPROTOCOL_INFOW protocol{};
        int length = sizeof(protocol);
        if (getsockopt(
                wish.fd,
                SOL_SOCKET,
                SO_PROTOCOL_INFOW,
                reinterpret_cast<char *>(&protocol),
                &length)
            == SOCKET_ERROR)
            windows_error(WSAGetLastError());
        e.accepted = WSASocketW(
            FROM_PROTOCOL_INFO,
            FROM_PROTOCOL_INFO,
            FROM_PROTOCOL_INFO,
            &protocol,
            0,
            WSA_FLAG_OVERLAPPED);
        if (e.accepted == INVALID_SOCKET)
            windows_error(WSAGetLastError());
        DWORD bytes = 0;
        auto success = function(
            wish.fd,
            e.accepted,
            e.addresses.data(),
            0,
            exec::address_bytes,
            exec::address_bytes,
            &bytes,
            &e);
        submitted_io(
            e,
            socket_file(wish.fd),
            success,
            success ? ERROR_SUCCESS : WSAGetLastError());
    }

    template<typename Wish>
        requires(
            std::same_as<Wish, op::poll>
            || std::same_as<Wish, op::poll_until>)
    void submit(deck &, exec &, Wish &)
    {
        throw runtime_error{
            "IOCP does not support readiness polling; await I/O instead"};
    }

    void
    finish(deck & d, exec & e, DWORD bytes, std::exception_ptr failure = {})
    {
        auto continuation = std::get<parked>(e.state).continuation;
        std::visit(
            [&](auto const & wish) {
                using Wish = std::remove_cvref_t<decltype(wish)>;
                using Result = typename Wish::result_type;
                auto state = std::static_pointer_cast<urge_state<Result>>(
                    e.packet.state);
                if (failure) {
                    state->set_exception(failure);
                } else if constexpr (std::same_as<Wish, op::accept>) {
                    state->set_value(
                        std::exchange(e.accepted, INVALID_SOCKET));
                } else if constexpr (std::is_void_v<Result>) {
                    state->set_value();
                } else if constexpr (std::same_as<Result, std::size_t>) {
                    state->set_value(bytes);
                }
            },
            e.packet.wish);
        e.state = lifecycle::settled{};
        continuation.resume(d);
    }

    bool receive(deck & d, DWORD timeout)
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED * overlapped = nullptr;
        auto success = GetQueuedCompletionStatus(
            port_, &bytes, &key, &overlapped, timeout);
        auto error = success ? ERROR_SUCCESS : GetLastError();
        if (!overlapped) {
            if (!success && error != WAIT_TIMEOUT)
                windows_error(error);
            return false;
        }
        auto & e = *static_cast<exec *>(overlapped);
        --in_flight_;
        try {
            if (!success) {
                if (error == ERROR_OPERATION_ABORTED)
                    throw operation_cancelled{};
                if (!(std::holds_alternative<op::read_some>(e.packet.wish)
                      && (error == ERROR_HANDLE_EOF
                          || error == ERROR_BROKEN_PIPE)))
                    windows_error(error);
                bytes = 0;
            }
            if (auto * wish = std::get_if<op::connect>(&e.packet.wish)) {
                if (setsockopt(
                        wish->fd,
                        SOL_SOCKET,
                        SO_UPDATE_CONNECT_CONTEXT,
                        nullptr,
                        0)
                    == SOCKET_ERROR)
                    windows_error(WSAGetLastError());
            } else if (
                auto * wish = std::get_if<op::accept>(&e.packet.wish)) {
                associate(socket_file(e.accepted));
                if (setsockopt(
                        e.accepted,
                        SOL_SOCKET,
                        SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<char *>(&wish->fd),
                        sizeof(wish->fd))
                    == SOCKET_ERROR)
                    windows_error(WSAGetLastError());
            }
            finish(d, e, bytes);
        } catch (...) {
            finish(d, e, 0, std::current_exception());
        }
        return true;
    }

    void expire(deck & d)
    {
        auto now = clock::now();
        for (auto & [token, e] : execs_) {
            if (auto * p = std::get_if<parked>(&e.state))
                if (auto * t = std::get_if<timer>(&p->phase))
                    if (t->deadline <= now)
                        finish(d, e, 0);
        }
    }

    HANDLE port_;
    std::map<coin_t, exec> execs_;
    coin_t next_coin_ = 0;
    std::size_t in_flight_ = 0;
};

} // namespace nxtrt::detail

namespace nxtrt {
iocp_wand::iocp_wand()
    : impl_(std::make_unique<detail::iocp_impl>())
{
}

iocp_wand::~iocp_wand() = default;

void iocp_wand::attach(io_handle handle)
{
    impl_->associate(handle);
}

void iocp_wand::attach(socket_handle socket)
{
    impl_->associate(detail::socket_file(socket));
}

coin_t iocp_wand::prep(
    deck &, detail::promise_base &, detail::prepared_wish packet)
{
    return impl_->prep(std::move(packet));
}

void iocp_wand::suspend(coin_t token, need task)
{
    impl_->suspend(token, task);
}

void iocp_wand::cancel(coin_t token)
{
    impl_->cancel(token);
}

void iocp_wand::wave(deck & d)
{
    impl_->wave(d);
}

void iocp_wand::poll(deck & d)
{
    impl_->poll(d);
}

void iocp_wand::wait(deck & d)
{
    impl_->wait(d);
}

void iocp_wand::run_until_done(deck & d, std::coroutine_handle<> root)
{
    impl_->run_until_done(d, root);
}
} // namespace nxtrt
#endif
