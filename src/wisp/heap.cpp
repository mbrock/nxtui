// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/heap.zig and core/tidy.zig.
#include "wisp/heap.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace wisp {

namespace {

std::size_t pool_size(std::size_t size, std::size_t extra)
{
    constexpr auto limit = std::numeric_limits<word>::max();
    if (extra > limit - size)
        throw std::length_error("Wisp payload exceeds 32-bit offsets");
    return size + extra;
}

template<typename T>
word append(std::vector<T> & pool, std::span<const T> data)
{
    auto start = pool.size();
    if (data.empty())
        return static_cast<word>(start);
    auto size = pool_size(start, data.size());
    // Save an offset, not a pointer, when the source is our own payload.
    auto less = std::less<const T *>{};
    auto own = !pool.empty() && !less(data.data(), pool.data())
               && less(data.data(), pool.data() + pool.size());
    auto offset = own ? std::size_t(data.data() - pool.data()) : 0;
    assert(!own || data.size() <= start - offset);
    pool.resize(size);
    auto source = own ? pool.data() + offset : data.data();
    std::copy_n(source, data.size(), pool.data() + start);
    return static_cast<word>(start);
}

} // namespace

heap::~heap()
{
    assert(roots_ == nullptr);
    release_externals();
}

void heap::release_externals() noexcept
{
    if (host_.release)
        for (word id : table<tag::ext>().col(0))
            if (id != zap)
                host_.release(host_.context, id);
}

word heap::newv08(std::string_view data)
{
    auto idx = append(bytes_, std::span{data.data(), data.size()});
    try {
        return make<tag::v08>({idx, static_cast<word>(data.size())});
    } catch (...) {
        bytes_.resize(idx);
        throw;
    }
}

word heap::newv32(std::span<const word> data)
{
    auto idx = append(words_, data);
    try {
        return make<tag::v32>({idx, static_cast<word>(data.size())});
    } catch (...) {
        words_.resize(idx);
        throw;
    }
}

word heap::filledv32(std::size_t length, word value)
{
    auto idx = words_.size();
    words_.resize(pool_size(idx, length), value);
    try {
        return make<tag::v32>(
            {static_cast<word>(idx), static_cast<word>(length)});
    } catch (...) {
        words_.resize(idx);
        throw;
    }
}

word heap::clonev32(word x)
{
    return newv32(v32slice(x));
}

word heap::copy_continuation_frame(word x)
{
    auto frame = read<tag::ktx>(x);
    auto fun = tag_of(frame[column_index<tag::ktx, field::fun>()]);
    auto & acc = frame[column_index<tag::ktx, field::acc>()];
    // Lexical environments are shared store. Partially filled argument
    // vectors are mutable control state and must be copied per invocation.
    if ((fun == tag::fun || fun == tag::jet) && tag_of(acc) == tag::v32)
        acc = clonev32(acc);
    return make<tag::ktx>(frame);
}

std::string_view heap::v08slice(word x) const noexcept
{
    auto [idx, len] = read<tag::v08>(x);
    assert(idx <= bytes_.size() && len <= bytes_.size() - idx);
    auto slice = std::span{bytes_}.subspan(idx, len);
    return {slice.data(), slice.size()};
}

std::span<const word> heap::v32slice(word x) const noexcept
{
    auto [idx, len] = read<tag::v32>(x);
    assert(idx <= words_.size() && len <= words_.size() - idx);
    return std::span{words_}.subspan(idx, len);
}

void heap::v32set(word x, std::size_t i, word value) noexcept
{
    auto [idx, len] = read<tag::v32>(x);
    assert(i < len && idx <= words_.size() && len <= words_.size() - idx);
    words_[idx + i] = value;
}

word heap::make_pin(word value)
{
    if (next_pin_ > max_immediate)
        throw std::length_error("Wisp pin IDs exhausted");
    pins_.emplace(next_pin_, value);
    return immediate(tag::pin, next_pin_++);
}

word heap::pinned(word pin) const noexcept
{
    assert(tag_of(pin) == tag::pin);
    auto entry = pins_.find(payload_of(pin));
    assert(entry != pins_.end());
    return entry->second;
}

void heap::free_pin(word pin) noexcept
{
    assert(tag_of(pin) == tag::pin);
    auto entry = pins_.find(payload_of(pin));
    assert(entry != pins_.end());
    pins_.erase(entry);
}

root::root(heap & owner, word value) noexcept
    : owner_(owner)
    , value_(value)
    , next_(owner.roots_)
{
    if (next_)
        next_->prev_ = this;
    owner_.roots_ = this;
}

root::~root()
{
    if (prev_)
        prev_->next_ = next_;
    else
        owner_.roots_ = next_;
    if (next_)
        next_->prev_ = prev_;
}

struct tidy
{
    heap & old;
    vat next;
    std::vector<word> words;
    std::array<std::size_t, std::tuple_size_v<vat>> scan{};
    std::size_t word_scan = 0;

    explicit tidy(heap & source)
        : old(source)
    {
        // Reserve an upper bound before overwriting any old rows. Even
        // distinct descriptors sharing one payload are copied separately.
        std::apply(
            [&]<tag... Tags>(tab<Tags> &... tables) {
                (tables.reserve(old.table<Tags>().size()), ...);
            },
            next);
        std::size_t total = 0;
        for (word len : old.table<tag::v32>().col(
                 column_index<tag::v32, field::len>()))
            total = pool_size(total, len);
        words.reserve(total);
    }

    template<tag T>
    word push(word x) noexcept
    {
        if (era_of(x) != old.era_)
            return x; // Already relocated through another root or edge.
        auto & from = std::get<tab<T>>(old.vat_);
        auto i = old.check<T>(x);
        if (from.get(i, 0) == zap)
            return from.get(i, 1);
        auto data = from.read(i);
        if constexpr (T == tag::v32) {
            auto payload = old.v32slice(x);
            data[0] = static_cast<word>(words.size());
            words.insert(words.end(), payload.begin(), payload.end());
        }
        auto & to = std::get<tab<T>>(next);
        auto y = pointer(T, to.push(data), !old.era_);
        from.set(i, 0, zap);
        from.set(i, 1, y);
        return y;
    }

    word copy(word x) noexcept
    {
        switch (tag_of(x)) {
        case tag::duo:
            return push<tag::duo>(x);
        case tag::sym:
            return push<tag::sym>(x);
        case tag::fun:
            return push<tag::fun>(x);
        case tag::mac:
            return push<tag::mac>(x);
        case tag::v32:
            return push<tag::v32>(x);
        case tag::v08:
            return push<tag::v08>(x);
        case tag::pkg:
            return push<tag::pkg>(x);
        case tag::run:
            return push<tag::run>(x);
        case tag::ktx:
            return push<tag::ktx>(x);
        case tag::ext:
            return push<tag::ext>(x);
        default:
            return x;
        }
    }

    template<tag T>
    bool pull(tab<T> & table) noexcept
    {
        auto & cursor = scan[word(T) - word(tag::duo)];
        auto changed = cursor < table.size();
        while (cursor < table.size()) {
            for (std::size_t c = 0; c < tab<T>::width; ++c) {
                if (schema<T>::columns[c].kind != field_kind::value)
                    continue;
                auto value = copy(table.get(cursor, c));
                // Resolve again after copying: never hold a column or pool
                // reference over an operation that can append objects.
                table.set(cursor, c, value);
            }
            ++cursor;
        }
        return changed;
    }

    void run() noexcept
    {
        for (auto & [id, value] : old.pins_)
            value = copy(value);
        for (auto * r = old.roots_; r; r = r->next_)
            r->value_ = copy(r->value_);
        bool changed;
        do {
            changed = false;
            std::apply(
                [&](auto &... tables) {
                    ((changed = pull(tables) || changed), ...);
                },
                next);
            while (word_scan < words.size()) {
                auto value = copy(words[word_scan]);
                words[word_scan++] = value;
                changed = true;
            }
        } while (changed);
        old.release_externals(); // Forwarded rows are marked, not released.
        old.vat_ = std::move(next);
        old.words_ = std::move(words);
        old.era_ = !old.era_;
        // Byte payloads deliberately stay put, including unreachable bytes.
    }
};

void heap::collect()
{
    tidy{*this}.run();
}

} // namespace wisp
