#pragma once

#include "nxtrt/trace.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace nxtrt {

class deck;
class wand;

/// Opaque key a wand hands out for one prepared wish execution.
///
/// The wand returns a coin from `prep`, and the same coin comes back in
/// `wand::suspend` and `wand::cancel`. Its meaning belongs to the wand (the
/// shipped wands encode the address of their execution record). A coin is
/// valid only until the wand retires that execution, so code outside the
/// wand should treat it as a diagnostic label.
using coin_t = std::uint64_t;
/// Alias of @ref nxtrt::coin_t "coin_t" used by the wand implementations.
using wait_token = coin_t;

/**
 * @namespace nxtrt::op
 * Wishes: closed, platform-neutral operation values that task code awaits.
 *
 * A wish is a small value type that says what outside work a task wants and
 * names the type it produces. The families are: byte I/O (`read_some`,
 * `write_some`, `recv_some`, `send_some`), sockets (`connect`, `accept`),
 * readiness and time (`poll`, `timeout`, `poll_until`), files (`openat`;
 * on Linux also `openat2`, `statx`, `getdents64`), child processes
 * (`spawn_piped`, `spawn_pty`, `wait_child`, `signal_child`), and `manual`
 * for tests. The full set is the closed `nxtrt::wish_variant`.
 *
 * `co_await wish` inside a task running on a deck asks that deck's wand to
 * prepare the operation, which yields an @ref nxtrt::urge "urge"; the urge
 * always suspends the task until the wand settles the operation. Failures
 * surface as exceptions from the `co_await`: `errno_error` for a failed
 * syscall, `interrupted_system_call` for `EINTR`, and `operation_cancelled`
 * when a stop request cancelled the wish.
 *
 * Wishes describe the request, not the mechanism, so the same task code runs
 * on io_uring, epoll, and kqueue. Buffers and file descriptors named by a
 * wish are borrowed and must stay valid until the `co_await` returns. Most
 * code uses the stream and filesystem layers built on top of wishes; see
 * @ref rt_wish and @ref rt_wand.
 */
namespace op {

/// One named argument of a wish, used only for trace and debug text.
struct wish_arg
{
    using value_type =
        std::variant<std::intmax_t, std::uintmax_t, std::string_view>;

    std::string_view name;
    value_type value;

    template<std::integral T>
    constexpr wish_arg(std::string_view name, T value) noexcept
        : name(name)
        , value(make_value(value))
    {}

    constexpr wish_arg(std::string_view name, std::string_view value) noexcept
        : name(name)
        , value(value)
    {}

private:
    template<std::integral T>
    static constexpr value_type make_value(T value) noexcept
    {
        if constexpr (std::is_signed_v<T>)
            return static_cast<std::intmax_t>(value);
        else
            return static_cast<std::uintmax_t>(value);
    }
};

/// String literal usable as a template argument; names a wish type.
template<std::size_t N>
struct fixed_string
{
    char value[N]{};

    constexpr fixed_string(char const (&text)[N]) noexcept
    {
        std::copy_n(text, N, value);
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return std::string_view{value, N - 1};
    }
};

/// Base for wish types: fixes the result type and the trace name.
///
/// A wish derives from `wish<Result, "name">`, stores its inputs as public
/// members, and provides `args()` returning a range of `wish_arg` for
/// diagnostics. `co_await` on the derived value produces `Result`. Adding a
/// new wish also means adding it to `nxtrt::wish_variant` and teaching every
/// wand to realize it.
template<typename Result, fixed_string Name>
struct wish
{
    using result_type = Result;
    static constexpr auto fixed_name = Name;
    static constexpr auto name = fixed_name.view();
};

/// Builds the fixed-size array a wish's `args()` returns.
template<typename... Args>
constexpr auto wish_args(Args &&... args)
{
    if constexpr (sizeof...(Args) == 0)
        return std::array<wish_arg, 0>{};
    else
        return std::array{wish_arg{std::forward<Args>(args)}...};
}

inline auto no_args()
{
    return wish_args();
}

inline auto fd_args(int fd)
{
    return wish_args(wish_arg{"fd", fd});
}

inline auto fd_bytes_args(int fd, std::size_t bytes)
{
    return wish_args(
        wish_arg{"fd", fd},
        wish_arg{"bytes", bytes});
}

inline auto path_args(std::string const & path)
{
    return wish_args(wish_arg{"path", std::string_view{path}});
}

inline auto pidfd_args(int pidfd)
{
    return wish_args(wish_arg{"pidfd", pidfd});
}

/// Formats wish arguments as space-separated `name=value` pairs.
template<std::ranges::input_range Args>
std::string format_wish_args(Args const & args)
{
    auto out = std::string{};
    auto first = true;
    for (auto const & arg : args) {
        if (!first)
            out += ' ';
        first = false;
        out += arg.name;
        out += '=';
        std::visit(
            [&](auto const & value) {
                if constexpr (std::same_as<
                                  std::remove_cvref_t<decltype(value)>,
                                  std::string_view>)
                    out += value;
                else
                    out += std::to_string(value);
            },
            arg.value);
    }
    return out;
}

/// One-line description of a wish: its name followed by its arguments.
template<typename Wish>
std::string describe_wish(Wish const & wish)
{
    auto args = wish.args();
    if (std::ranges::empty(args))
        return std::string{Wish::name};
    return std::string{Wish::name}
        + " "
        + format_wish_args(args);
}

/// Writes `describe_wish(wish)` to the runtime trace when tracing is on.
template<typename Wish>
void trace_wish(Wish const & wish)
{
    trace("{}", describe_wish(wish));
}

/// A type that `nxtrt::op::operator co_await` accepts as a wish.
///
/// It needs a `result_type`, a static `name`, and an `args()` member. To be
/// realized by a wand it must also be an alternative of
/// `nxtrt::wish_variant`.
template<typename Wish>
concept awaitable_wish =
    requires(Wish const & wish) {
        typename Wish::result_type;
        { Wish::name } -> std::convertible_to<std::string_view>;
        wish.args();
    };

} // namespace op

} // namespace nxtrt
