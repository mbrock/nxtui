// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/heap.zig and core/tidy.zig.
#pragma once

#include "wisp/vat.hpp"
#include "wisp/profile.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace wisp {

class root;
struct tidy;

/// A host binding, never a guest pointer or part of a portable image.
/// One release per unreachable ext row, and per remaining row at teardown.
/// The callback must not throw, allocate guest objects, or reenter this
/// heap.
struct externals
{
    void * context = nullptr;
    void (*release)(void *, word) noexcept = nullptr;
};

/// Single-threaded guest storage. Allocation grows but never collects;
/// collect() is an explicit safepoint. Only registered roots and pins
/// survive it. The heap must outlive its roots. Guest words are not native
/// addresses.
class heap
{
public:
    explicit heap(externals host = {}) noexcept
        : host_(host)
    {
    }

    ~heap();
    heap(const heap &) = delete;
    heap & operator=(const heap &) = delete;
    heap(heap &&) = delete;
    heap & operator=(heap &&) = delete;

    bool era() const noexcept
    {
        return era_;
    }

    /// The caller owns the counters and must detach them before
    /// destruction. No per-operation recording or pointer test in
    /// non-profile builds.
    void profiling(profile * counters) noexcept
    {
        profile_ = counters;
    }

    profile * profiling() const noexcept
    {
        if constexpr (profile_enabled)
            return profile_;
        return nullptr;
    }

    template<tag T>
    const tab<T> & table() const noexcept
    {
        return std::get<tab<T>>(vat_);
    }

    /// Low-level schema operation: payload descriptors must describe valid
    /// pool slices. Use newv08/newv32 for fresh payloads. zap is reserved
    /// for the collector and cannot occupy a live row's first column.
    template<tag T>
    word make(row<T> data)
    {
        assert(data[0] != zap);
        auto result = pointer(T, std::get<tab<T>>(vat_).push(data), era_);
        if (auto * p = profiling())
            ++p->allocations[std::size_t(T)];
        return result;
    }

    template<tag T>
    row<T> read(word x) const noexcept
    {
        return table<T>().read(check<T>(x));
    }

    template<tag T, field F>
    word get(word x) const noexcept
    {
        return table<T>().get(check<T>(x), column_index<T, F>());
    }

    template<tag T, field F>
    void set(word x, word value) noexcept
    {
        constexpr auto c = column_index<T, F>();
        assert(c != 0 || value != zap);
        std::get<tab<T>>(vat_).set(check<T>(x), c, value);
    }

    template<tag T>
    void put(word x, row<T> data) noexcept
    {
        assert(data[0] != zap);
        std::get<tab<T>>(vat_).put(check<T>(x), data);
    }

    /// Shallow copy: vector descriptors share payload until collection.
    template<tag T>
    word copy(word x)
    {
        return make<T>(read<T>(x));
    }

    word cons(word car, word cdr)
    {
        return make<tag::duo>({car, cdr});
    }

    /// Tags whose payload is a slice of the word pool.
    template<tag T>
    static constexpr bool word_payload = T == tag::v32 || T == tag::rec;

    word newv08(std::string_view data);
    template<tag T>
        requires word_payload<T>
    word new_words(std::span<const word> data)
    {
        const auto idx = append_words(data);
        try {
            auto result = make<T>({idx, static_cast<word>(data.size())});
            if (auto * p = profiling())
                p->v32_words += data.size(); // word-pool use, records too
            return result;
        } catch (...) {
            words_.resize(idx);
            throw;
        }
    }
    word newv32(std::span<const word> data)
    {
        return new_words<tag::v32>(data);
    }
    word filledv32(std::size_t length, word value);
    word clonev32(word x);
    word copy_continuation_frame(word x);

    // Allocation watermark: snapshots share frames until the evaluator
    // needs to write one. Collection and restore freeze all survivors.
    void freeze_continuations() noexcept
    {
        frozen_ktx_ = table<tag::ktx>().size();
    }

    bool continuation_frozen(word x) const noexcept
    {
        return check<tag::ktx>(x) < frozen_ktx_;
    }

    /// Borrowed payloads expire on pool growth or collection. Mutation goes
    /// through operations, not writable spans. Appending a borrowed slice
    /// back into this same heap is supported, including across growth.
    std::string_view v08slice(word x) const noexcept;
    template<tag T>
        requires word_payload<T>
    std::span<const word> words(word x) const noexcept
    {
        auto [idx, len] = read<T>(x);
        assert(idx <= words_.size() && len <= words_.size() - idx);
        return std::span{words_}.subspan(idx, len);
    }
    template<tag T>
        requires word_payload<T>
    void set_word(word x, std::size_t i, word value) noexcept
    {
        auto [idx, len] = read<T>(x);
        assert(
            i < len && idx <= words_.size() && len <= words_.size() - idx);
        words_[idx + i] = value;
    }
    std::span<const word> v32slice(word x) const noexcept
    {
        return words<tag::v32>(x);
    }
    void v32set(word x, std::size_t i, word value) noexcept
    {
        set_word<tag::v32>(x, i, value);
    }

    std::size_t byte_count() const noexcept
    {
        return bytes_.size();
    }

    std::size_t word_count() const noexcept
    {
        return words_.size();
    }

    /// Pins are stable immediate IDs, not fixed-address objects. They keep
    /// their value reachable until free_pin(), even if no guest holds the
    /// ID.
    word make_pin(word value);
    word pinned(word pin) const noexcept;
    /// Idempotent: releasing an already freed pin has no effect.
    void free_pin(word pin) noexcept;

    /// Tidy's era-flipping copying collector, with in-row forwarding.
    /// Copies live byte and word payloads per descriptor, rewriting
    /// offsets; distinct descriptors no longer share payload after
    /// collection. All destination capacity is reserved before forwarding
    /// begins: an allocation failure leaves the old heap and host roots
    /// untouched. Reservation covers every old row and the sum of
    /// descriptor lengths, including unreachable and shared slices, before
    /// reclaiming garbage.
    void collect();

private:
    /// Appends to the word pool, returning the slice's offset.
    word append_words(std::span<const word> data);

    friend class root;
    friend struct tidy;
    friend struct tape_codec;

    template<tag T>
    word check(word x) const noexcept
    {
        assert(tag_of(x) == T && era_of(x) == era_);
        auto i = index_of(x);
        assert(i < table<T>().size());
        return i;
    }

    void release_externals() noexcept;

    vat vat_;
    std::vector<char> bytes_;
    std::vector<word> words_;
    std::map<word, word> pins_;
    word next_pin_ = 1;
    root * roots_ = nullptr;
    bool era_ = false;
    std::size_t frozen_ktx_ = 0;
    externals host_;
    profile * profile_ = nullptr;
};

/// An address-stable host slot rewritten by collection. Nonmovable so its
/// intrusive registration cannot dangle; destruction unlinks in any order.
class root
{
public:
    explicit root(heap & owner, word value = nil) noexcept;
    ~root();
    root(const root &) = delete;
    root & operator=(const root &) = delete;
    root(root &&) = delete;
    root & operator=(root &&) = delete;

    word get() const noexcept
    {
        return value_;
    }

    void set(word value) noexcept
    {
        value_ = value;
    }

private:
    friend struct tidy;
    heap & owner_;
    word value_;
    root * prev_ = nullptr;
    root * next_ = nullptr;
};

} // namespace wisp
