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

using coin_t = std::uint64_t;
using wait_token = coin_t;

namespace op {

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

template<typename Result, fixed_string Name>
struct wish
{
    using result_type = Result;
    static constexpr auto fixed_name = Name;
    static constexpr auto name = fixed_name.view();
};

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

template<typename Wish>
void trace_wish(Wish const & wish)
{
    trace("{}", describe_wish(wish));
}

template<typename Wish>
concept awaitable_wish =
    requires(Wish const & wish) {
        typename Wish::result_type;
        { Wish::name } -> std::convertible_to<std::string_view>;
        wish.args();
    };

} // namespace op

} // namespace nxtrt
