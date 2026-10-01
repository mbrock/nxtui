#pragma once

/// Land: bounded regions of storage, and the bookkeeping that hands them out.
///
/// Storage comes in three holdings that lend the same borrowed view:
/// `value_storage_ref<T>` (borrowed), `static_value_storage<T, N>` (inline),
/// and `rack<T>` (owned). All three are raw land: no `T` lives there until a
/// holder constructs one, and `junk<T>` names a window of such land. `mask`
/// keeps track of which slots of some land are free.

#include "nxtrt/alloc_trace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace nxtrt {

template<typename T>
struct value_storage_ref
{
    using value_type = std::remove_cv_t<T>;

    value_storage_ref() = default;

    value_storage_ref(value_type * data, std::size_t size)
        : data(data)
        , size(size)
    {}

    value_storage_ref(std::span<value_type> storage)
        : data(storage.data())
        , size(storage.size())
    {}

    value_type * data = nullptr;
    std::size_t size = 0;
};

template<typename T>
class rack
{
public:
    using value_type = std::remove_cv_t<T>;

    rack() noexcept = default;

    explicit rack(std::size_t size)
        : data_(size == 0 ? nullptr : allocator_.allocate(size))
        , size_(size)
    {
        alloc_trace::event(
            "rack",
            "new",
            data_,
            sizeof(value_type) * size_,
            alignof(value_type),
            size_,
            size_);
    }

    rack(const rack &) = delete;
    rack & operator=(const rack &) = delete;

    rack(rack && other) noexcept
        : data_(std::exchange(other.data_, nullptr))
        , size_(std::exchange(other.size_, 0))
    {}

    rack & operator=(rack && other) noexcept
    {
        if (this != &other) {
            deallocate();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~rack()
    {
        deallocate();
    }

    [[nodiscard]] value_type * data() noexcept
    {
        return data_;
    }

    [[nodiscard]] const value_type * data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] value_storage_ref<value_type> ref() & noexcept
    {
        return {data_, size_};
    }

    [[nodiscard]] operator value_storage_ref<value_type>() & noexcept
    {
        return ref();
    }

    operator value_storage_ref<value_type>() && = delete;

private:
    void deallocate() noexcept
    {
        if (data_ != nullptr)
            alloc_trace::event(
                "rack",
                "del",
                data_,
                sizeof(value_type) * size_,
                alignof(value_type));
        if (data_ != nullptr)
            allocator_.deallocate(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }

    [[no_unique_address]] std::allocator<value_type> allocator_;
    value_type * data_ = nullptr;
    std::size_t size_ = 0;
};

template<typename T, std::size_t N>
class static_value_storage
{
public:
    using value_type = std::remove_cv_t<T>;

    static_value_storage() = default;

    static_value_storage(const static_value_storage &) = delete;
    static_value_storage & operator=(const static_value_storage &) = delete;

    [[nodiscard]] value_type * data() noexcept
    {
        return std::launder(reinterpret_cast<value_type *>(storage_));
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return N;
    }

    [[nodiscard]] value_storage_ref<value_type> ref() & noexcept
    {
        return {data(), N};
    }

    [[nodiscard]] operator value_storage_ref<value_type>() & noexcept
    {
        return ref();
    }

    operator value_storage_ref<value_type>() && = delete;

private:
    alignas(value_type) std::byte storage_[
        sizeof(value_type) * (N == 0 ? 1 : N)];
};

/// View of raw storage where up to `size()` values of `T` may be
/// constructed.
///
/// Unlike `std::span<T>`, this does not claim that live `T` objects already
/// exist. Producers must start object lifetimes before reporting values as
/// constructed to a ring, feed, or sink.
template<typename T>
class junk
{
public:
    using value_type = std::remove_cv_t<T>;

    junk() = default;

    junk(value_type * data, std::size_t size)
        : data_(data)
        , size_(size)
    {
    }

    [[nodiscard]] value_type * data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] junk first(std::size_t n) const noexcept
    {
        return {data_, std::min(n, size_)};
    }

    [[nodiscard]] std::span<std::byte> as_writable_bytes() const noexcept
        requires std::is_trivially_copyable_v<value_type>
    {
        return {
            reinterpret_cast<std::byte *>(data_),
            size_ * sizeof(value_type),
        };
    }

private:
    value_type * data_ = nullptr;
    std::size_t size_ = 0;
};

/// A set of indices `0 .. capacity() - 1` that hands out its lowest member.
///
/// The set is a 64-ary summary tree of words. In a leaf word, bit `j`
/// (counting from the most significant bit) says index `64 * w + j` is in the
/// set; in a word above the leaves, bit `j` says child word `j` is not empty.
/// Each level multiplies capacity by 64 (64, 4096, 262144, ...), and finding
/// the lowest member reads one word per level with `countl_zero`, so `take`
/// and `give` cost one word per level, never a scan.
///
/// `mask<N>` keeps its words inline. `mask<>` borrows them, for capacities
/// known only at run time; `mask<>::words_for(capacity)` says how many.
template<std::size_t N = std::dynamic_extent>
class mask;

namespace detail {

struct mask_layout
{
    static constexpr std::size_t max_depth = 6;
    static constexpr std::size_t max_capacity = std::size_t{1} << 36;

    std::size_t capacity = 0;
    std::size_t depth = 0;
    std::size_t words = 0;
    /// First word of each level, root level first.
    std::array<std::size_t, max_depth> offset{};

    [[nodiscard]] static constexpr mask_layout of(std::size_t capacity) noexcept
    {
        auto counts = std::array<std::size_t, max_depth>{};
        auto depth = std::size_t{0};
        auto width = (capacity + 63) / 64;
        do {
            counts[depth++] = width == 0 ? 1 : width;
            width = (width + 63) / 64;
        } while (counts[depth - 1] > 1 && depth < max_depth);

        auto layout = mask_layout{.capacity = capacity, .depth = depth};
        for (auto level = std::size_t{0}; level < depth; ++level) {
            layout.offset[level] = layout.words;
            layout.words += counts[depth - 1 - level];
        }
        return layout;
    }

    [[nodiscard]] static constexpr std::uint64_t bit(std::size_t j) noexcept
    {
        return std::uint64_t{1} << (63 - j);
    }

    [[nodiscard]] std::size_t peek(const std::uint64_t * words) const noexcept
    {
        if (words[0] == 0)
            return capacity;
        auto index = std::size_t{0};
        for (auto level = std::size_t{0}; level < depth; ++level) {
            auto word = words[offset[level] + index];
            index = (index << 6)
                | static_cast<std::size_t>(std::countl_zero(word));
        }
        return index;
    }

    [[nodiscard]] bool contains(
        const std::uint64_t * words,
        std::size_t index) const noexcept
    {
        return index < capacity
            && (words[offset[depth - 1] + (index >> 6)] & bit(index & 63))
            != 0;
    }

    /// Clears `index`, then each ancestor bit whose word just emptied.
    void erase(std::uint64_t * words, std::size_t index) const noexcept
    {
        for (auto level = depth; level-- > 0;) {
            auto & word = words[offset[level] + (index >> 6)];
            word &= ~bit(index & 63);
            if (word != 0)
                return;
            index >>= 6;
        }
    }

    /// Sets `index`, then each ancestor bit whose word was empty.
    void insert(std::uint64_t * words, std::size_t index) const noexcept
    {
        for (auto level = depth; level-- > 0;) {
            auto & word = words[offset[level] + (index >> 6)];
            auto const was_empty = word == 0;
            word |= bit(index & 63);
            if (!was_empty)
                return;
            index >>= 6;
        }
    }

    /// Puts every index in the set.
    void fill(std::uint64_t * words) const noexcept
    {
        auto members = capacity;
        for (auto level = depth; level-- > 0;) {
            auto const first = offset[level];
            auto const count = (members + 63) / 64;
            for (auto w = std::size_t{0}; w < count; ++w) {
                auto const live = std::min<std::size_t>(64, members - w * 64);
                words[first + w] = live == 64
                    ? ~std::uint64_t{0}
                    : ~(~std::uint64_t{0} >> live);
            }
            members = count;
        }
    }
};

} // namespace detail

template<std::size_t N>
class mask
{
    static constexpr auto layout_ = detail::mask_layout::of(N);
    static_assert(N <= detail::mask_layout::max_capacity);

public:
    static constexpr std::size_t words = layout_.words;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept
    {
        return N;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return words_[0] == 0;
    }

    /// Lowest member, or `capacity()` when empty.
    [[nodiscard]] std::size_t peek() const noexcept
    {
        return layout_.peek(words_.data());
    }

    /// Removes and returns the lowest member, or `capacity()` when empty.
    std::size_t take() noexcept
    {
        auto index = peek();
        if (index != N)
            layout_.erase(words_.data(), index);
        return index;
    }

    void give(std::size_t index) noexcept
    {
        assert(index < N);
        layout_.insert(words_.data(), index);
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept
    {
        return layout_.contains(words_.data(), index);
    }

    void fill() noexcept
    {
        layout_.fill(words_.data());
    }

private:
    std::array<std::uint64_t, words> words_{};
};

template<>
class mask<std::dynamic_extent>
{
public:
    [[nodiscard]] static constexpr std::size_t words_for(
        std::size_t capacity) noexcept
    {
        return detail::mask_layout::of(capacity).words;
    }

    mask() = default;

    /// Borrows `words`, which must hold at least `words_for(capacity)`, and
    /// starts empty.
    mask(std::span<std::uint64_t> words, std::size_t capacity) noexcept
        : words_(words.data())
        , layout_(detail::mask_layout::of(capacity))
    {
        assert(capacity <= detail::mask_layout::max_capacity);
        assert(words.size() >= layout_.words);
        std::fill_n(words_, layout_.words, std::uint64_t{0});
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return layout_.capacity;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return words_ == nullptr || words_[0] == 0;
    }

    [[nodiscard]] std::size_t peek() const noexcept
    {
        return words_ == nullptr ? 0 : layout_.peek(words_);
    }

    std::size_t take() noexcept
    {
        auto index = peek();
        if (index != layout_.capacity)
            layout_.erase(words_, index);
        return index;
    }

    void give(std::size_t index) noexcept
    {
        assert(index < layout_.capacity);
        layout_.insert(words_, index);
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept
    {
        return words_ != nullptr && layout_.contains(words_, index);
    }

    void fill() noexcept
    {
        if (words_ != nullptr)
            layout_.fill(words_);
    }

private:
    std::uint64_t * words_ = nullptr;
    detail::mask_layout layout_ = detail::mask_layout::of(0);
};

} // namespace nxtrt
