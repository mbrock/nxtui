#pragma once

#include "nxtrt/value-buffers.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

namespace nxtrt {

using u64 = std::uint64_t;

template<std::size_t N>
concept PowerOfTwo = N >= 1 && std::has_single_bit(N);

template<std::size_t N>
    requires PowerOfTwo<N>
struct mask;

template<std::size_t N>
    requires PowerOfTwo<N> && (N <= 64)
struct mask<N>
{
    u64 bits = 0;
    static constexpr std::size_t width = N;

    [[nodiscard]] bool empty() const noexcept
    {
        return bits == 0;
    }

    [[nodiscard]] std::size_t peek() const noexcept
    {
        if (!bits)
            return N;
        return std::countl_zero(bits) - (64 - N);
    }

    std::size_t take() noexcept
    {
        auto i = peek();
        if (i != N)
            bits &= ~(u64{1} << (N - 1 - i));
        return i;
    }

    void give(std::size_t i) noexcept
    {
        assert(i < N);
        bits |= u64{1} << (N - 1 - i);
    }
};

template<std::size_t N>
    requires PowerOfTwo<N> && (N > 64)
struct mask<N>
{
    using half = mask<N / 2>;

    half hi = {};
    half lo = {};

    static constexpr std::size_t width = N;

    [[nodiscard]] bool empty() const noexcept
    {
        return hi.empty() && lo.empty();
    }

    [[nodiscard]] std::size_t peek() const noexcept
    {
        auto h = hi.peek();
        if (h != N / 2)
            return h;
        return N / 2 + lo.peek();
    }

    std::size_t take() noexcept
    {
        auto h = hi.peek();
        if (h != N / 2)
            return hi.take();
        return N / 2 + lo.take();
    }

    void give(std::size_t i) noexcept
    {
        assert(i < N);
        if (i < N / 2)
            hi.give(i);
        else
            lo.give(i - N / 2);
    }
};

namespace detail {

template<std::size_t N>
struct farm_index_storage
{
    using index_type = std::size_t;

    [[nodiscard]] index_type * data() noexcept
    {
        return std::launder(
            reinterpret_cast<index_type *>(storage.data()));
    }

    alignas(index_type) std::array<
        std::byte,
        sizeof(index_type) * (N == 0 ? 1 : N)>
        storage{};
};

} // namespace detail

template<typename T, std::size_t N>
    requires PowerOfTwo<N>
class farm
    : private detail::farm_index_storage<N < 64 ? N : 64>
    , public feed<std::size_t>
{
public:
    using value_type = std::remove_cv_t<T>;
    using index_type = std::size_t;
    using index_feed = feed<index_type>;
    using index_storage = detail::farm_index_storage<N < 64 ? N : 64>;

    static constexpr std::size_t capacity = N;
    static constexpr std::size_t hot_capacity = N < 64 ? N : 64;

    explicit farm(std::array<value_type, N> * buffer) noexcept
        : index_feed(
              value_storage_ref<index_type>{
                  index_storage::data(),
                  hot_capacity,
              })
        , buffer_(buffer)
    {
        for (auto i = index_type{0}; i < N; ++i)
            cold_.give(i);
    }

    farm(const farm &) = delete;
    farm & operator=(const farm &) = delete;

    [[nodiscard]] std::array<value_type, N> * buffer() noexcept
    {
        return buffer_;
    }

    [[nodiscard]] value_type * data() noexcept
    {
        return buffer_->data();
    }

    [[nodiscard]] value_type * at(index_type index) noexcept
    {
        assert(index < N);
        return data() + index;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return this->buffered_size() == 0 && cold_.empty();
    }

    [[nodiscard]] hope<value_type *> alloc()
    {
        auto index = this->take();
        if (index.is_ready())
            return hope<value_type *>::ready(at_or_null(index.take_ready()));
        return alloc_slow(std::move(index));
    }

    void give(index_type index) noexcept
    {
        assert(index < N);
        if (this->unused_capacity_size() != 0)
            this->emplace(index);
        else
            cold_.give(index);
    }

    void release(value_type * slot) noexcept
    {
        assert(slot >= data());
        assert(slot < data() + N);
        give(static_cast<index_type>(slot - data()));
    }

protected:
    hope<fare_t> stream_more(
        sink<index_type> & sink,
        std::size_t limit) override
    {
        auto dst = sink.uninitialized_capacity().first(limit);
        auto n = std::size_t{0};

        while (n < dst.size()) {
            auto index = cold_.take();
            if (index == N)
                break;
            std::construct_at(dst.data() + n, index);
            ++n;
        }

        sink.advance_constructed(n);
        if (n == 0)
            return hope<fare_t>::ready(eof);
        return hope<fare_t>::ready(n);
    }

private:
    [[nodiscard]] value_type *
    at_or_null(std::optional<index_type> index) noexcept
    {
        if (!index)
            return nullptr;
        return at(*index);
    }

    task<value_type *> alloc_slow(hope<std::optional<index_type>> index)
    {
        co_return at_or_null(co_await index);
    }

    std::array<value_type, N> * buffer_ = nullptr;
    mask<N> cold_ = {};
};

} // namespace nxtrt
