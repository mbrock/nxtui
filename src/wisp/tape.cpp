// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/tape.hpp"
#include "wisp/code.hpp"
#include "nxt/crypto.hpp"

#include <istream>
#include <limits>
#include <ostream>
#include <set>
#include <zlib.h>

namespace wisp {
namespace {

constexpr std::string_view magic = "NXWISP\r\n";
constexpr std::string_view compressed_magic = "NXWISPZ\n";
constexpr word version = 5;

void demand(bool good, const char * message)
{
    if (!good)
        throw tape::error(message);
}

struct tape_writer
{
    std::vector<std::byte> bytes;

    void raw(std::span<const std::byte> data)
    {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    void u32(word x)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(std::byte((x >> shift) & 255));
    }

    void count(std::size_t n)
    {
        demand(
            n <= std::numeric_limits<word>::max(), "tape count overflow");
        u32(static_cast<word>(n));
    }

    void text(std::string_view value)
    {
        count(value.size());
        raw(std::as_bytes(std::span{value.data(), value.size()}));
    }
};

struct tape_reader
{
    std::span<const std::byte> bytes;

    std::span<const std::byte> take(std::size_t n)
    {
        demand(n <= bytes.size(), "truncated tape");
        auto result = bytes.first(n);
        bytes = bytes.subspan(n);
        return result;
    }

    word u32()
    {
        auto data = take(4);
        word result = 0;
        for (unsigned i = 0; i < 4; ++i)
            result |= word(std::to_integer<unsigned char>(data[i]))
                      << (8 * i);
        return result;
    }

    std::string_view text()
    {
        auto data = take(u32());
        return {reinterpret_cast<const char *>(data.data()), data.size()};
    }

    bool flag()
    {
        auto x = u32();
        demand(x <= 1, "invalid tape flag");
        return x != 0;
    }
};

constexpr std::string_view kind_name(field_kind kind)
{
    switch (kind) {
    case field_kind::value:
        return "value";
    case field_kind::offset:
        return "offset";
    case field_kind::length:
        return "length";
    case field_kind::count:
        return "count";
    case field_kind::external:
        return "external";
    }
    return {};
}

} // namespace

// One private boundary owns access to storage and evaluator checkpoint
// state. Table traversal and column identities come from the vat schemas.
struct tape_codec
{
    // A tape root is an evaluator member or an entry in its known table.
    struct saved_root
    {
        std::string_view name;
        root evaluator::* member;
        std::size_t known_index;

        root & in(evaluator & vm) const
        {
            if (member != nullptr)
                return vm.*member;
            return vm.known_[known_index];
        }

        const root & in(const evaluator & vm) const
        {
            if (member != nullptr)
                return vm.*member;
            return vm.known_[known_index];
        }
    };

    static constexpr std::array roots{
        saved_root{"WISP", &evaluator::base_, 0},
        saved_root{"KEYWORD", &evaluator::keywords_, 0},
        saved_root{"KEY", &evaluator::keys_, 0},
        saved_root{"packages", &evaluator::packages_, 0},
        saved_root{"current", &evaluator::current_, 0},
        saved_root{"NIL-name", &evaluator::nil_name_, 0},
        saved_root{"T-name", &evaluator::true_name_, 0},
        saved_root{"DO", nullptr, known_name::find("DO")},
        saved_root{"IF", nullptr, known_name::find("IF")},
        saved_root{"EVAL", nullptr, known_name::find("EVAL")},
        saved_root{"LET", nullptr, known_name::find("LET")},
        saved_root{"PROMPT", nullptr, known_name::find("PROMPT")},
        saved_root{"BINDING", nullptr, known_name::find("BINDING")},
        saved_root{
            "CONTINUATION", nullptr, known_name::find("CONTINUATION")},
        saved_root{"RESUME", nullptr, known_name::find("RESUME")},
        saved_root{"&OPTIONAL", nullptr, known_name::find("&OPTIONAL")},
        saved_root{"&REST", nullptr, known_name::find("&REST")},
        saved_root{"&BODY", nullptr, known_name::find("&BODY")},
    };

    static void validate(const evaluator & vm, word entry)
    {
        const auto & h = vm.heap_;
        demand(
            h.table<tag::ext>().size() == 0,
            "external resources are not portable");
        demand(
            h.next_pin_ >= 1 && h.next_pin_ <= max_immediate + 1,
            "invalid next pin ID");
        demand(
            vm.next_key_ < (std::uint64_t{1} << 48),
            "invalid fresh key sequence");
        std::array<std::size_t, 32> sizes{};
        std::apply(
            [&](const auto &... tables) {
                ((sizes[word(tables.type)] = tables.size()), ...);
            },
            h.vat_);
        const auto jets = evaluator::jet_manifest();
        const auto value = [&](word x) {
            const auto type = tag_of(x);
            if (is_pointer(type)) {
                demand(
                    era_of(x) == h.era_ && index_of(x) < sizes[word(type)],
                    "invalid tape pointer");
                return;
            }
            switch (type) {
            case tag::integer:
                return;
            case tag::sys:
                demand(
                    x == nil || x == t || x == nah || x == top,
                    "invalid system word");
                return;
            case tag::chr: {
                const auto c = payload_of(x);
                demand(
                    c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff),
                    "invalid character");
                return;
            }
            case tag::jet:
                demand(payload_of(x) < jets.size(), "invalid builtin ID");
                return;
            case tag::pin:
                demand(
                    payload_of(x) > 0 && payload_of(x) < h.next_pin_,
                    "invalid pin handle");
                return; // Released handles may remain in guest data.
            default:
                throw tape::error("unknown word tag");
            }
        };
        value(entry);
        for (const auto & saved : roots)
            value(saved.in(vm).get());
        for (auto [id, x] : h.pins_) {
            demand(id > 0 && id < h.next_pin_, "invalid pin ID");
            value(x);
        }
        for (auto x : h.words_)
            value(x);
        std::apply(
            [&]<tag... Tags>(const tab<Tags> &... tables) {
                const auto check = [&]<tag T>(const tab<T> & table) {
                    for (auto x : table.col(0))
                        demand(
                            x != zap,
                            "collector forwarding marker in tape");
                    for (std::size_t c = 0; c < table.width; ++c)
                        if (schema<T>::columns[c].kind == field_kind::value)
                            for (auto x : table.col(c))
                                value(x);
                    if constexpr (T == tag::v08 || heap::word_payload<T>) {
                        const auto size = T == tag::v08 ? h.bytes_.size()
                                                        : h.words_.size();
                        std::uint64_t total = 0;
                        for (std::size_t i = 0; i < table.size(); ++i) {
                            auto [offset, length] = table.read(i);
                            demand(
                                offset <= size && length <= size - offset,
                                "invalid pool slice");
                            total += length;
                        }
                        demand(
                            total <= std::numeric_limits<word>::max(),
                            "collected payload pool would overflow");
                    }
                };
                (check(tables), ...);
            },
            h.vat_);

        const auto typed = [](word x, tag type) {
            demand(tag_of(x) == type, "invalid machine field type");
        };
        for (word i = 0; i < sizes[word(tag::sym)]; ++i) {
            auto [str, pkg, val, fun, dyn] = h.table<tag::sym>().read(i);
            typed(str, tag::v08);
            if (pkg != nil)
                typed(pkg, tag::pkg);
        }

        // Only private package indexes are required to be proper lists.
        // Environments, uses, syntax and continuation payloads are mutable
        // guest data. Runtime readers check them on use; a checkpoint must
        // preserve their conditions, not silently impose stronger
        // semantics.
        std::array<std::vector<unsigned char>, 2> lists;
        for (auto & colors : lists)
            colors.resize(sizes[word(tag::duo)]);
        const auto proper = [&](word head, tag element) {
            auto & colors = lists[element == tag::pkg ? 0 : 1];
            auto cur = head;
            while (cur != nil) {
                typed(cur, tag::duo);
                auto & color = colors[index_of(cur)];
                demand(color != 1, "cyclic machine list");
                if (color == 2)
                    break;
                color = 1;
                const auto [item, next] = h.read<tag::duo>(cur);
                typed(item, element);
                cur = next;
            }
            for (cur = head; cur != nil && colors[index_of(cur)] == 1;) {
                colors[index_of(cur)] = 2;
                cur = h.get<tag::duo, field::cdr>(cur);
            }
        };
        for (word i = 0; i < sizes[word(tag::pkg)]; ++i) {
            auto [name, symbols, uses] = h.table<tag::pkg>().read(i);
            typed(name, tag::v08);
            proper(symbols, tag::sym);
        }
        proper(vm.packages_.get(), tag::pkg);
        std::set<std::string_view> package_names;
        std::set<word> packages;
        for (auto cur = vm.packages_.get(); cur != nil;) {
            const auto [pkg, next] = h.read<tag::duo>(cur);
            const auto name = h.v08slice(h.get<tag::pkg, field::nam>(pkg));
            demand(
                packages.insert(pkg).second
                    && package_names.insert(name).second,
                "duplicate package");
            cur = next;
        }
        demand(
            packages.contains(vm.current_.get()),
            "current package missing");
        for (std::size_t i = 0; i < roots.size(); ++i) {
            const auto name = roots[i].name;
            const auto x = roots[i].in(vm).get();
            if (i < 3) {
                demand(packages.contains(x), "canonical package missing");
                demand(
                    h.v08slice(h.get<tag::pkg, field::nam>(x)) == name,
                    "wrong canonical package");
            } else if (i == 5 || i == 6) {
                typed(x, tag::v08);
                demand(
                    h.v08slice(x) == (i == 5 ? "NIL" : "T"),
                    "invalid immediate name");
            } else if (i >= 7) {
                typed(x, tag::sym);
                demand(
                    h.get<tag::sym, field::pkg>(x) == vm.base_.get()
                        && h.v08slice(h.get<tag::sym, field::str>(x))
                               == name,
                    "invalid cached symbol");
            }
        }
        const auto continuation = [&](word x) {
            if (x != top)
                typed(x, tag::ktx);
        };
        const auto boundary_kind = [&](word kind) {
            return kind == vm.known("PROMPT") || kind == vm.known("BINDING")
                   || kind == vm.known("RESUME");
        };
        const auto segment = [&](word x) {
            continuation(x);
            if (x != top) {
                const auto kind = h.get<tag::ktx, field::fun>(x);
                demand(
                    !boundary_kind(kind)
                        && kind != vm.known("CONTINUATION"),
                    "invalid continuation segment");
            }
        };
        const auto meta = [&](word x) {
            continuation(x);
            if (x != top)
                demand(
                    boundary_kind(h.get<tag::ktx, field::fun>(x)),
                    "invalid continuation meta chain");
        };
        // Private control edges now include wrapper registers and suspended
        // segments, not just HOP. Validate their shape and acyclicity
        // without recursion; guest syntax and mutable argument state remain
        // data.
        std::vector<std::array<word, 2>> edges(
            sizes[word(tag::ktx)], {top, top});
        for (word i = 0; i < edges.size(); ++i) {
            const auto [hop, env, kind, acc, arg] =
                h.read<tag::ktx>(pointer(tag::ktx, i, h.era_));
            if (kind == vm.known("CONTINUATION")) {
                demand(hop == top, "invalid continuation wrapper");
                segment(acc);
                meta(arg);
                edges[i] = {acc, arg};
            } else if (boundary_kind(kind)) {
                meta(hop);
                typed(arg, tag::v32);
                const auto xs = h.v32slice(arg);
                demand(xs.size() == 2, "invalid continuation boundary");
                segment(xs[1]);
                edges[i] = {hop, xs[1]};
            } else {
                segment(hop);
                edges[i][0] = hop;
            }
        }
        std::vector<unsigned char> colors(edges.size());
        std::vector<std::pair<word, bool>> pending;
        for (word i = 0; i < edges.size(); ++i) {
            pending.emplace_back(pointer(tag::ktx, i, h.era_), false);
            while (!pending.empty()) {
                const auto [cur, leaving] = pending.back();
                pending.pop_back();
                if (cur == top)
                    continue;
                auto & color = colors[index_of(cur)];
                if (leaving) {
                    color = 2;
                    continue;
                }
                demand(color != 1, "cyclic continuation");
                if (color == 2)
                    continue;
                color = 1;
                pending.emplace_back(cur, true);
                for (auto edge : edges[index_of(cur)])
                    pending.emplace_back(edge, false);
            }
        }
        std::apply(
            [&]<tag... Tags>(const tab<Tags> &... tables) {
                const auto check = [&]<tag T>(const tab<T> & table) {
                    if constexpr (T == tag::run) {
                        for (auto way :
                             table.col(column_index<T, field::way>()))
                            segment(way);
                        for (auto chain :
                             table.col(column_index<T, field::meta>()))
                            meta(chain);
                    }
                    if constexpr (T == tag::fun || T == tag::mac)
                        for (auto name :
                             table.col(column_index<T, field::sym>()))
                            if (name != nil)
                                typed(name, tag::sym);
                };
                (check(tables), ...);
            },
            h.vat_);
    }

    static std::vector<std::byte> encode(const evaluator & vm, word entry)
    {
        validate(vm, entry);
        const auto & h = vm.heap_;
        tape_writer out;
        out.raw(std::as_bytes(std::span{magic.data(), magic.size()}));
        out.u32(version);
        out.u32(h.era_);
        out.u32(h.next_pin_);
        out.u32(word(vm.next_key_));
        out.u32(word(vm.next_key_ >> 32));
        out.u32(vm.collect_);
        out.u32(entry);
        out.count(roots.size());
        for (const auto & saved : roots) {
            out.text(saved.name);
            out.u32(saved.in(vm).get());
        }
        const auto jets = evaluator::jet_manifest();
        out.count(jets.size());
        for (const auto & jet : jets) {
            out.text(jet.name);
            out.u32(jet.control);
        }
        // Numeric code identities are persistent, unlike remapped jets.
        // Reject incompatible layouts, not just different heap schemas.
        out.u32(code_version);
        out.count(code_operations.size());
        for (const auto & operation : code_operations) {
            out.text(operation.name);
            out.u32(code_opcode(operation.op));
            out.count(operation.count);
            for (std::size_t i = 0; i < operation.count; ++i)
                out.text(operand_name(operation.operands[i]));
        }
        out.count(h.bytes_.size());
        out.raw(std::as_bytes(std::span{h.bytes_}));
        out.count(h.words_.size());
        for (auto x : h.words_)
            out.u32(x);
        out.count(std::tuple_size_v<vat>);
        std::apply(
            [&]<tag... Tags>(const tab<Tags> &... tables) {
                const auto write = [&]<tag T>(const tab<T> & table) {
                    out.u32(word(T));
                    out.count(table.size());
                    out.count(table.width);
                    for (auto col : schema<T>::columns) {
                        out.text(col.name);
                        out.text(kind_name(col.kind));
                    }
                    for (std::size_t c = 0; c < table.width; ++c)
                        for (auto x : table.col(c))
                            out.u32(x);
                };
                (write(tables), ...);
            },
            h.vat_);
        out.count(h.pins_.size());
        for (auto [id, x] : h.pins_) {
            out.u32(id);
            out.u32(x);
        }
        out.raw(nxt::crypto::sha256(out.bytes));
        return std::move(out.bytes);
    }

    static std::unique_ptr<image>
    decode(std::span<const std::byte> data, std::size_t limit)
    {
        demand(data.size() <= limit, "tape exceeds input limit");
        demand(
            data.size() >= magic.size() + 4 + nxt::crypto::sha256_len,
            "truncated tape");
        const auto body = data.first(data.size() - nxt::crypto::sha256_len);
        demand(
            std::ranges::equal(
                nxt::crypto::sha256(body),
                data.last(nxt::crypto::sha256_len)),
            "tape checksum mismatch");
        tape_reader in{body};
        demand(
            std::ranges::equal(
                in.take(magic.size()),
                std::as_bytes(std::span{magic.data(), magic.size()})),
            "unknown tape magic");
        demand(in.u32() == version, "unsupported tape version");
        auto result = std::unique_ptr<image>(new image);
        auto & h = result->storage;
        auto & vm = result->machine;
        h.era_ = in.flag();
        h.next_pin_ = in.u32();
        vm.next_key_ = in.u32();
        vm.next_key_ |= std::uint64_t(in.u32()) << 32;
        vm.collect_ = in.flag();
        result->entry.set(in.u32());
        demand(in.u32() == roots.size(), "wrong root directory");
        std::array<bool, roots.size()> seen_roots{};
        for (std::size_t i = 0; i < roots.size(); ++i) {
            const auto name = in.text();
            const auto found =
                std::ranges::find(roots, name, &saved_root::name);
            demand(found != roots.end(), "unknown root");
            const auto index = std::size_t(found - roots.begin());
            demand(!seen_roots[index], "duplicate root");
            seen_roots[index] = true;
            found->in(vm).set(in.u32());
        }
        const auto jets = evaluator::jet_manifest();
        const auto jet_count = in.u32();
        demand(jet_count <= jets.size(), "unsupported builtin manifest");
        std::vector<word> jet_map;
        std::set<word> seen_jets;
        for (word i = 0; i < jet_count; ++i) {
            const auto name = in.text();
            const auto control = in.flag();
            const auto found =
                std::ranges::find(jets, name, &evaluator::jet_info::name);
            demand(
                found != jets.end() && found->control == control,
                "unsupported builtin");
            const auto id = word(found - jets.begin());
            demand(seen_jets.insert(id).second, "duplicate builtin");
            jet_map.push_back(id);
        }
        demand(in.u32() == code_version, "unsupported code version");
        demand(
            in.u32() == code_operations.size(),
            "unsupported code manifest");
        for (const auto & operation : code_operations) {
            demand(
                in.text() == operation.name, "unsupported code operation");
            demand(
                in.u32() == code_opcode(operation.op),
                "unsupported code opcode");
            demand(in.u32() == operation.count, "unsupported code layout");
            for (std::size_t i = 0; i < operation.count; ++i)
                demand(
                    in.text() == operand_name(operation.operands[i]),
                    "unsupported code operand");
        }
        const auto bytes = in.take(in.u32());
        h.bytes_.assign(
            reinterpret_cast<const char *>(bytes.data()),
            reinterpret_cast<const char *>(bytes.data()) + bytes.size());
        const auto words = in.u32();
        demand(words <= in.bytes.size() / 4, "truncated word pool");
        h.words_.reserve(words);
        for (word i = 0; i < words; ++i)
            h.words_.push_back(in.u32());
        demand(in.u32() == std::tuple_size_v<vat>, "wrong table directory");
        std::set<word> seen_tables;
        for (std::size_t n = 0; n < std::tuple_size_v<vat>; ++n) {
            const auto type = in.u32();
            demand(seen_tables.insert(type).second, "duplicate table");
            bool found = false;
            std::apply(
                [&]<tag... Tags>(tab<Tags> &... tables) {
                    const auto read = [&]<tag T>(tab<T> & table) {
                        if (type != word(T))
                            return;
                        found = true;
                        const auto rows = in.u32();
                        demand(
                            rows <= max_index + 1, "table count overflow");
                        if constexpr (T == tag::ext)
                            demand(
                                rows == 0,
                                "external resources are not portable");
                        demand(
                            in.u32() == table.width, "wrong column count");
                        std::array<std::size_t, table.width> columns{};
                        std::array<bool, table.width> seen{};
                        for (auto & c : columns) {
                            const auto name = in.text();
                            const auto kind = in.text();
                            const auto & schema_cols = schema<T>::columns;
                            const auto col = std::ranges::find(
                                schema_cols, name, &column::name);
                            demand(
                                col != schema_cols.end()
                                    && kind_name(col->kind) == kind,
                                "unsupported column schema");
                            c = std::size_t(col - schema_cols.begin());
                            demand(!seen[c], "duplicate column");
                            seen[c] = true;
                        }
                        demand(
                            rows <= in.bytes.size() / (4 * table.width),
                            "truncated table");
                        table.reserve(rows);
                        for (word i = 0; i < rows; ++i)
                            table.push({});
                        for (auto c : columns)
                            for (word i = 0; i < rows; ++i)
                                table.set(i, c, in.u32());
                    };
                    (read(tables), ...);
                },
                h.vat_);
            demand(found, "unknown table tag");
        }
        const auto pins = in.u32();
        demand(pins <= in.bytes.size() / 8, "truncated pin map");
        for (word i = 0; i < pins; ++i) {
            const auto id = in.u32(), x = in.u32();
            demand(h.pins_.emplace(id, x).second, "duplicate pin ID");
        }
        demand(in.bytes.empty(), "trailing tape bytes");
        const auto remap = [&](word x) {
            if (tag_of(x) != tag::jet)
                return x;
            const auto id = payload_of(x);
            demand(id < jet_map.size(), "invalid saved builtin ID");
            return immediate(tag::jet, jet_map[id]);
        };
        result->entry.set(remap(result->entry.get()));
        for (const auto & saved : roots)
            saved.in(vm).set(remap(saved.in(vm).get()));
        for (auto & [id, x] : h.pins_)
            x = remap(x);
        for (auto & x : h.words_)
            x = remap(x);
        std::apply(
            [&]<tag... Tags>(tab<Tags> &... tables) {
                const auto rewrite = [&]<tag T>(tab<T> & table) {
                    for (std::size_t c = 0; c < table.width; ++c)
                        if (schema<T>::columns[c].kind == field_kind::value)
                            for (std::size_t i = 0; i < table.size(); ++i)
                                table.set(i, c, remap(table.get(i, c)));
                };
                (rewrite(tables), ...);
            },
            h.vat_);
        validate(vm, result->entry.get());
        // Saved names were checked above; the rest of the known table is
        // derived from the restored packages, as Zig's tape loader does.
        vm.install_known();
        h.freeze_continuations();
        return result;
    }
};

namespace tape {

std::vector<std::byte>
encode(const evaluator & vm, word entry, compression format)
{
    auto data = tape_codec::encode(vm, entry);
    if (format == compression::none)
        return data;
    tape_writer out;
    out.raw(
        std::as_bytes(
            std::span{compressed_magic.data(), compressed_magic.size()}));
    out.count(data.size());
    const auto header_size = out.bytes.size();
    uLongf size = compressBound(data.size());
    out.bytes.resize(header_size + size);
    const auto status = compress2(
        reinterpret_cast<Bytef *>(out.bytes.data() + header_size),
        &size,
        reinterpret_cast<const Bytef *>(data.data()),
        data.size(),
        Z_BEST_COMPRESSION);
    if (status == Z_MEM_ERROR)
        throw std::bad_alloc{};
    demand(status == Z_OK, "tape compression failed");
    out.bytes.resize(header_size + size);
    return std::move(out.bytes);
}

std::unique_ptr<image>
decode(std::span<const std::byte> data, std::size_t limit)
{
    if (data.size() >= compressed_magic.size()
        && std::ranges::equal(
            data.first(compressed_magic.size()),
            std::as_bytes(
                std::span{
                    compressed_magic.data(), compressed_magic.size()}))) {
        demand(data.size() <= limit, "tape exceeds input limit");
        tape_reader in{data.subspan(compressed_magic.size())};
        const auto decoded_size = in.u32();
        demand(decoded_size <= limit, "tape exceeds decoded limit");
        std::vector<std::byte> decoded(decoded_size);
        uLongf size = decoded.size();
        uLong source_size = in.bytes.size();
        const auto status = uncompress2(
            reinterpret_cast<Bytef *>(decoded.data()),
            &size,
            reinterpret_cast<const Bytef *>(in.bytes.data()),
            &source_size);
        if (status == Z_MEM_ERROR)
            throw std::bad_alloc{};
        demand(status == Z_OK, "invalid compressed tape");
        demand(size == decoded_size, "wrong decoded tape size");
        demand(
            source_size == in.bytes.size(),
            "trailing compressed tape data");
        return tape_codec::decode(decoded, limit);
    }
    return tape_codec::decode(data, limit);
}

void write(
    std::ostream & output,
    const evaluator & vm,
    word entry,
    compression format)
{
    const auto data = encode(vm, entry, format);
    // Bounded chunks also work where streamsize is narrower than size_t.
    for (std::size_t pos = 0; pos < data.size();) {
        const auto n = std::min(std::size_t{8192}, data.size() - pos);
        output.write(reinterpret_cast<const char *>(data.data() + pos), n);
        demand(bool(output), "tape write failed");
        pos += n;
    }
}

std::unique_ptr<image> read(std::istream & input, std::size_t limit)
{
    std::vector<std::byte> bytes;
    std::array<char, 8192> buffer;
    while (true) {
        try {
            input.read(buffer.data(), buffer.size());
        } catch (const std::ios_base::failure &) {
            // read() sets failbit on a normal short final block too.
            if (!input.eof() || input.bad())
                throw;
        }
        const auto n = std::size_t(input.gcount());
        demand(n <= limit - bytes.size(), "tape exceeds input limit");
        auto chunk = std::as_bytes(std::span{buffer.data(), n});
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        demand(!input.bad(), "tape read failed");
        if (input.eof())
            break;
        demand(bool(input), "tape read failed");
    }
    return decode(bytes, limit);
}

} // namespace tape
} // namespace wisp
