// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of the columnar vat in mbrock/wisp's core/heap.zig.
#pragma once

#include "wisp/word.hpp"
#include "nxtrt/land.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace wisp {

// Semantic field identities are independent of physical column positions.
// The names below, not these C++ enum ordinals, identify fields in a
// schema.
enum class field {
    car,
    cdr,
    str,
    pkg,
    val,
    fun,
    dyn,
    env,
    par,
    exp,
    sym,
    cnt,
    idx,
    len,
    nam,
    use,
    hop,
    acc,
    arg,
    err,
    way,
    meta,
};

enum class field_kind { value, offset, length, count, external };

struct column
{
    field id;
    std::string_view name;
    field_kind kind = field_kind::value;
};

template<tag>
struct schema;

template<>
struct schema<tag::duo>
{
    static constexpr std::array columns{
        column{field::car, "car"}, column{field::cdr, "cdr"}};
};

template<>
struct schema<tag::sym>
{
    static constexpr std::array columns{
        column{field::str, "str"},
        column{field::pkg, "pkg"},
        column{field::val, "val"},
        column{field::fun, "fun"},
        column{field::dyn, "dyn"}};
};

template<>
struct schema<tag::fun>
{
    static constexpr std::array columns{
        column{field::env, "env"},
        column{field::par, "par"},
        column{field::exp, "exp"},
        column{field::sym, "sym"},
        column{field::cnt, "cnt", field_kind::count}};
};

template<>
struct schema<tag::mac> : schema<tag::fun>
{};

template<>
struct schema<tag::v08>
{
    static constexpr std::array columns{
        column{field::idx, "idx", field_kind::offset},
        column{field::len, "len", field_kind::length}};
};

template<>
struct schema<tag::v32> : schema<tag::v08>
{};

// A record is a word vector whose first element is its type: a symbol, or
// a type record whose first slot names it (as in Emacs Lisp records).
template<>
struct schema<tag::rec> : schema<tag::v08>
{};

template<>
struct schema<tag::pkg>
{
    static constexpr std::array columns{
        column{field::nam, "nam"},
        column{field::sym, "sym"},
        column{field::use, "use"}};
};

template<>
struct schema<tag::run>
{
    static constexpr std::array columns{
        column{field::exp, "exp"},
        column{field::val, "val"},
        column{field::err, "err"},
        column{field::env, "env"},
        column{field::way, "way"},
        column{field::meta, "meta"}};
};

template<>
struct schema<tag::ktx>
{
    static constexpr std::array columns{
        column{field::hop, "hop"},
        column{field::env, "env"},
        column{field::fun, "fun"},
        column{field::acc, "acc"},
        column{field::arg, "arg"}};
};

template<>
struct schema<tag::ext>
{
    static constexpr std::array columns{
        column{field::idx, "idx", field_kind::external},
        column{field::val, "val"}};
};

template<tag T, field F>
consteval std::size_t column_index()
{
    for (std::size_t i = 0; i < schema<T>::columns.size(); ++i)
        if (schema<T>::columns[i].id == F)
            return i;
    throw "field does not belong to this Wisp schema";
}

template<tag T>
using row = std::array<word, schema<T>::columns.size()>;

/// An append-only columnar table. Rows are values; col() lends a read-only
/// view until the next reserve/growth. Indices survive growth, not GC.
/// Raw capacity belongs to racks; only [0, size()) contains live words.
template<tag T>
class tab
{
public:
    static constexpr tag type = T;
    static constexpr std::size_t width = schema<T>::columns.size();

    static_assert(
        width >= 2); // Tidy's in-row forwarding marker and pointer.
    static_assert(
        [] {
            for (std::size_t i = 0; i < width; ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (schema<T>::columns[i].id
                        == schema<T>::columns[j].id)
                        return false;
            return true;
        }(),
        "repeated Wisp field");

    tab() = default;
    tab(const tab &) = delete;
    tab & operator=(const tab &) = delete;

    tab(tab && other) noexcept
        : columns_(std::move(other.columns_))
        , size_(std::exchange(other.size_, 0))
        , capacity_(std::exchange(other.capacity_, 0))
    {
    }

    tab & operator=(tab && other) noexcept
    {
        if (this != &other) {
            columns_ = std::move(other.columns_);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    void reserve(std::size_t n)
    {
        if (n > std::size_t{max_index} + 1)
            throw std::length_error("Wisp table exceeds 26-bit indices");
        if (n <= capacity_)
            return;
        std::array<nxtrt::rack<word>, width> grown;
        for (std::size_t c = 0; c < width; ++c) {
            grown[c] = nxtrt::rack<word>{n};
            if (size_ != 0)
                std::uninitialized_copy_n(
                    columns_[c].data(), size_, grown[c].data());
        }
        columns_ = std::move(grown);
        capacity_ = n;
    }

    word push(row<T> data)
    {
        if (size_ == capacity_) {
            if (size_ == std::size_t{max_index} + 1)
                throw std::length_error("Wisp table is full");
            reserve(
                std::min(
                    std::size_t{max_index} + 1,
                    capacity_ == 0 ? 8 : capacity_ * 2));
        }
        for (std::size_t c = 0; c < width; ++c)
            std::construct_at(columns_[c].data() + size_, data[c]);
        return static_cast<word>(size_++);
    }

    std::span<const word> col(std::size_t c) const noexcept
    {
        assert(c < width);
        return {columns_[c].data(), size_};
    }

    word get(std::size_t i, std::size_t c) const noexcept
    {
        assert(i < size_);
        return col(c)[i];
    }

    void set(std::size_t i, std::size_t c, word x) noexcept
    {
        assert(i < size_ && c < width);
        columns_[c].data()[i] = x;
    }

    row<T> read(std::size_t i) const noexcept
    {
        row<T> out;
        for (std::size_t c = 0; c < width; ++c)
            out[c] = get(i, c);
        return out;
    }

    void put(std::size_t i, row<T> data) noexcept
    {
        for (std::size_t c = 0; c < width; ++c)
            set(i, c, data[c]);
    }

private:
    std::array<nxtrt::rack<word>, width> columns_;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

// Keep Wisp's vat order explicit; it differs from numerical tag order.
using vat = std::tuple<
    tab<tag::duo>,
    tab<tag::sym>,
    tab<tag::fun>,
    tab<tag::mac>,
    tab<tag::v32>,
    tab<tag::v08>,
    tab<tag::pkg>,
    tab<tag::run>,
    tab<tag::ktx>,
    tab<tag::ext>,
    tab<tag::rec>>;

// Position of tag T's table in the vat.
template<tag T>
consteval std::size_t vat_index()
{
    return []<std::size_t... I>(std::index_sequence<I...>) {
        std::size_t found = sizeof...(I);
        ((std::tuple_element_t<I, vat>::type == T ? (found = I) : 0), ...);
        return found;
    }(std::make_index_sequence<std::tuple_size_v<vat>>{});
}

} // namespace wisp
