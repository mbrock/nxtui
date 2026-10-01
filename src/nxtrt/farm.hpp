#pragma once

#include "nxtrt/land.hpp"
#include "nxtrt/value-buffers.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace nxtrt {

/// Land for a farm's free-index bookkeeping: a hot ring of recently given
/// indices and the words of the cold mask.
struct farm_index_storage_ref
{
    std::span<std::size_t> hot;
    std::span<std::uint64_t> cold;
};

/// Inline farm index land for `N` slots.
template<std::size_t N>
class static_farm_index_storage
{
public:
    static constexpr std::size_t hot_capacity = N < 64 ? N : 64;

    [[nodiscard]] farm_index_storage_ref ref() noexcept
    {
        return {
            std::span{hot_.data(), hot_capacity},
            std::span{cold_},
        };
    }

private:
    std::array<std::size_t, hot_capacity == 0 ? 1 : hot_capacity> hot_{};
    std::array<std::uint64_t, mask<>::words_for(N)> cold_{};
};

/// A pool of slots that hands out free slots by streaming their indices.
///
/// The farm is a `feed<std::size_t>` of free indices: `alloc()` reads the next
/// free index and `give()` writes one back. Indices given back recently wait
/// in the feed's hot ring and come out first in, first out; the rest wait in
/// a cold `mask`, which refills the ring with the lowest indices first, so
/// busy slots stay packed toward the front.
///
/// `farm<T>` borrows its slots and index land, so its capacity can be chosen
/// at run time. `farm<T, N>` keeps its index land inline.
template<typename T, std::size_t N = std::dynamic_extent>
class farm;

template<typename T>
class farm<T, std::dynamic_extent> : public feed<std::size_t>
{
public:
    using value_type = std::remove_cv_t<T>;
    using index_type = std::size_t;
    using index_feed = feed<index_type>;

    /// Index land needed for `slots` slots: `hot_capacity` ring entries
    /// (any nonzero size works; 64 is plenty) and `mask<>::words_for(slots)`
    /// mask words.
    [[nodiscard]] static constexpr std::size_t hot_capacity_for(
        std::size_t slots) noexcept
    {
        return slots < 64 ? (slots == 0 ? 1 : slots) : 64;
    }

    /// Every slot starts free.
    farm(std::span<value_type> slots, farm_index_storage_ref indices) noexcept
        : index_feed(
              value_storage_ref<index_type>{
                  indices.hot.data(),
                  indices.hot.size(),
              })
        , slots_(slots)
        , cold_(indices.cold, slots.size())
    {
        cold_.fill();
    }

    farm(const farm &) = delete;
    farm & operator=(const farm &) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return slots_.size();
    }

    [[nodiscard]] value_type * data() noexcept
    {
        return slots_.data();
    }

    [[nodiscard]] value_type * at(index_type index) noexcept
    {
        assert(index < slots_.size());
        return slots_.data() + index;
    }

    /// True when every slot is handed out.
    [[nodiscard]] bool empty() const noexcept
    {
        return this->buffered_size() == 0 && cold_.empty();
    }

    /// The next free slot, or null when every slot is handed out.
    [[nodiscard]] hope<value_type *> alloc()
    {
        auto index = this->take();
        if (index.is_ready())
            return hope<value_type *>::ready(at_or_null(index.take_ready()));
        return alloc_slow(std::move(index));
    }

    void give(index_type index) noexcept
    {
        assert(index < slots_.size());
        if (this->unused_capacity_size() != 0)
            this->emplace(index);
        else
            cold_.give(index);
    }

    void release(value_type * slot) noexcept
    {
        assert(slot >= slots_.data());
        assert(slot < slots_.data() + slots_.size());
        give(static_cast<index_type>(slot - slots_.data()));
    }

protected:
    hope<fare_t> stream_more(
        sink<index_type> & sink,
        std::size_t limit) override
    {
        auto dst = sink.uninitialized_capacity().first(limit);
        auto n = std::size_t{0};

        while (n < dst.size() && !cold_.empty()) {
            std::construct_at(dst.data() + n, cold_.take());
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

    std::span<value_type> slots_;
    mask<> cold_;
};

template<typename T, std::size_t N>
class farm
    : private static_farm_index_storage<N>
    , public farm<T>
{
public:
    using value_type = typename farm<T>::value_type;

    explicit farm(std::array<value_type, N> * buffer) noexcept
        : farm<T>(std::span{*buffer}, static_farm_index_storage<N>::ref())
        , buffer_(buffer)
    {}

    [[nodiscard]] std::array<value_type, N> * buffer() noexcept
    {
        return buffer_;
    }

private:
    std::array<value_type, N> * buffer_ = nullptr;
};

} // namespace nxtrt
