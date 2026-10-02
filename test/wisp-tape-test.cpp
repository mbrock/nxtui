// SPDX-License-Identifier: AGPL-3.0-or-later
#include <wisp/tape.hpp>
#include <wisp/load.hpp>
#include <wisp/nxt.hpp>
#include <wisp/printer.hpp>
#include <nxt/crypto.hpp>

#include "test.hpp"
#include "wisp-base.hpp"
#include <sstream>
#include <zlib.h>

namespace wisp::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;
using bytes = std::vector<std::byte>;

word evaluate(heap & h, evaluator & vm, std::string_view source)
{
    loader input{h, vm, source};
    for (unsigned turns = 0; turns < 10000; ++turns) {
        const auto state = input.advance(2048);
        if (state == evaluation::failed)
            throw std::runtime_error(
                print(h, h.get<tag::run, field::err>(input.run())));
        if (state == evaluation::done)
            return h.get<tag::run, field::val>(input.run());
    }
    throw std::runtime_error("tape fixture exceeded budget");
}

word get32(const bytes & data, std::size_t at)
{
    return std::to_integer<word>(data.at(at))
           | (std::to_integer<word>(data.at(at + 1)) << 8)
           | (std::to_integer<word>(data.at(at + 2)) << 16)
           | (std::to_integer<word>(data.at(at + 3)) << 24);
}

void put32(bytes & data, std::size_t at, word x)
{
    for (unsigned i = 0; i < 4; ++i)
        data.at(at + i) = std::byte((x >> (i * 8)) & 255);
}

void seal(bytes & data)
{
    const auto digest =
        nxt::crypto::sha256(std::span{data}.first(data.size() - 32));
    std::copy(digest.begin(), digest.end(), data.end() - 32);
}

template<typename Exception>
bool throws(auto && action)
{
    try {
        action();
        return false;
    } catch (const Exception &) {
        return true;
    }
}

void rejected(const bytes & data)
{
    expect(throws<tape::error>([&] { (void) tape::decode(data); }));
}

// Construct the compression envelope independently of tape::encode so
// decoder tests do not inherit mistakes in the encoder's framing.
bytes compressed_fixture(const bytes & data)
{
    constexpr std::string_view magic = "NXWISPZ\n";
    const auto header =
        std::as_bytes(std::span{magic.data(), magic.size()});
    bytes result(header.begin(), header.end());
    result.resize(12 + compressBound(data.size()));
    put32(result, 8, static_cast<word>(data.size()));
    uLongf size = result.size() - 12;
    expect(
        compress2(
            reinterpret_cast<Bytef *>(result.data() + 12),
            &size,
            reinterpret_cast<const Bytef *>(data.data()),
            data.size(),
            Z_BEST_SPEED)
        == Z_OK);
    result.resize(12 + size);
    return result;
}

// Independent wire walker for deliberate valid-checksum corruptions and
// reordered directories. It does not ask the codec for its layout.
struct layout
{
    struct table
    {
        std::size_t begin, end, rows, metadata, data;
        std::map<std::string, std::size_t> columns;
    };

    std::map<std::string, std::size_t> roots, jets;
    std::map<tag, table> tables;
    std::vector<std::size_t> values{32};
    std::size_t word_count, pins;

    explicit layout(const bytes & data)
    {
        std::size_t at = 36;
        auto u32 = [&] {
            auto x = get32(data, at);
            at += 4;
            return x;
        };
        auto text = [&] {
            auto n = u32();
            std::string s(
                reinterpret_cast<const char *>(data.data() + at), n);
            at += n;
            return s;
        };
        auto count = u32();
        for (word i = 0; i < count; ++i) {
            auto name = text();
            roots[name] = at;
            values.push_back(at);
            at += 4;
        }
        count = u32();
        for (word i = 0; i < count; ++i) {
            const auto start = at + 4;
            jets[text()] = start;
            at += 4;
        }
        const auto byte_count = u32();
        at += byte_count;
        word_count = at;
        count = u32();
        for (word i = 0; i < count; ++i) {
            values.push_back(at);
            at += 4;
        }
        count = u32();
        for (word i = 0; i < count; ++i) {
            table t{};
            t.begin = at;
            const auto type = tag(u32());
            t.rows = u32();
            const auto width = u32();
            t.metadata = at;
            std::vector<std::pair<std::string, std::string>> columns;
            for (word c = 0; c < width; ++c) {
                auto name = text();
                columns.emplace_back(name, text());
            }
            t.data = at;
            for (auto [name, kind] : columns) {
                t.columns[name] = at;
                for (std::size_t row = 0; row < t.rows; ++row) {
                    if (kind == "value")
                        values.push_back(at);
                    at += 4;
                }
            }
            t.end = at;
            tables[type] = t;
        }
        pins = at;
        count = u32();
        for (word i = 0; i < count; ++i) {
            at += 4;
            values.push_back(at);
            at += 4;
        }
        expect(at + 32 == data.size());
    }
};

word resume(image & restored, word value)
{
    auto & h = restored.storage;
    auto & vm = restored.machine;
    const auto packet = h.v32slice(restored.entry.get());
    const auto continuation = packet[1];
    const auto quote =
        h.cons(vm.intern("QUOTE"), h.cons(continuation, nil));
    root run{
        h,
        vm.start(
            h.cons(vm.intern("CALL"), h.cons(quote, h.cons(value, nil))))};
    nxtrt::deck deck;
    expect(deck.sync_wait([&] {
        return drive(vm, run, 3);
    }) == evaluation::done);
    return h.get<tag::run, field::val>(run.get());
}

static suite tape_tests{
    "WISP TAPE", [] {
        "zlib tapes preserve the raw image and work through stream I/O"_test =
            [] {
                auto original = image::fresh();
                auto & h = original->storage;
                std::string text(8192, 'Q');
                text.replace(31, 5, std::string{"a\0\xffz", 4});
                original->entry.set(
                    h.newv32(std::array{fixnum(-173), h.newv08(text)}));
                const auto raw =
                    tape::encode(original->machine, original->entry.get());
                const auto packed = tape::encode(
                    original->machine,
                    original->entry.get(),
                    tape::compression::zlib);
                expect(packed.size() < raw.size());
                expect(
                    std::string_view(
                        reinterpret_cast<const char *>(packed.data()), 8)
                    == "NXWISPZ\n");
                expect(get32(packed, 8) == raw.size());
                bytes decoded(raw.size());
                uLongf size = decoded.size();
                uLong source_size = packed.size() - 12;
                expect(
                    uncompress2(
                        reinterpret_cast<Bytef *>(decoded.data()),
                        &size,
                        reinterpret_cast<const Bytef *>(packed.data() + 12),
                        &source_size)
                    == Z_OK);
                expect(
                    size == raw.size()
                    && source_size == packed.size() - 12);
                expect(decoded == raw);
                auto restored = tape::decode(packed);
                expect(
                    tape::encode(restored->machine, restored->entry.get())
                    == raw);
                const auto entry =
                    restored->storage.v32slice(restored->entry.get());
                expect(integer(entry[0]) == -173);
                expect(restored->storage.v08slice(entry[1]) == text);
                std::stringstream stream;
                tape::write(
                    stream,
                    original->machine,
                    original->entry.get(),
                    tape::compression::zlib);
                const auto encoded = stream.str();
                expect(
                    std::ranges::equal(
                        std::as_bytes(
                            std::span{encoded.data(), encoded.size()}),
                        packed));
                restored = tape::read(stream, raw.size());
                expect(
                    tape::encode(restored->machine, restored->entry.get())
                    == raw);
            };

        "compressed tapes enforce framing, decoded limits, and inner validation"_test =
            [] {
                heap h;
                evaluator vm{h};
                root entry{h, h.newv08(std::string(8192, 'R'))};
                const auto raw = tape::encode(vm, entry.get());
                const auto packed = compressed_fixture(raw);
                expect(packed.size() < raw.size());
                auto restored = tape::decode(packed, raw.size());
                expect(
                    tape::encode(restored->machine, restored->entry.get())
                    == raw);
                for (auto [limit, message] :
                     {std::pair{
                          packed.size() - 1, "tape exceeds input limit"},
                      std::pair{
                          raw.size() - 1, "tape exceeds decoded limit"}}) {
                    try {
                        (void) tape::decode(packed, limit);
                        expect(false);
                    } catch (const tape::error & error) {
                        expect(std::string_view{error.what()} == message);
                    }
                }
                for (auto length :
                     {std::size_t{0},
                      std::size_t{7},
                      std::size_t{8},
                      std::size_t{11},
                      std::size_t{12},
                      packed.size() - 1})
                    rejected(
                        bytes(packed.begin(), packed.begin() + length));
                for (auto length :
                     {word{0},
                      word(raw.size() - 1),
                      word(raw.size() + 1),
                      word(tape::default_limit + 1)}) {
                    auto bad = packed;
                    put32(bad, 8, length);
                    rejected(bad);
                }
                for (auto offset : {std::size_t{12}, packed.size() - 1}) {
                    auto bad = packed;
                    bad[offset] ^= std::byte{1};
                    rejected(bad);
                }
                auto bad = packed;
                bad.push_back(std::byte{0});
                rejected(bad);
                bad = packed;
                bad.insert(bad.end(), packed.begin() + 12, packed.end());
                rejected(bad);
                bad = raw;
                bad.back() ^= std::byte{1};
                rejected(compressed_fixture(bad));
                bad = raw;
                put32(bad, 8, 999);
                seal(bad);
                rejected(compressed_fixture(bad));
                std::stringstream limited{std::string(
                    reinterpret_cast<const char *>(packed.data()),
                    packed.size())};
                expect(throws<tape::error>([&] {
                    (void) tape::read(limited, raw.size() - 1);
                }));
            };

        "little-endian tapes preserve cycles, aliasing, raw fields, pins, and both eras"_test =
            [] {
                for (bool collect_first : {false, true}) {
                    heap h;
                    evaluator vm{h};
                    if (collect_first)
                        vm.collect();
                    const auto cycle = h.cons(fixnum(-19), nil);
                    h.set<tag::duo, field::cdr>(cycle, cycle);
                    const auto text = h.newv08(std::string_view{"a\0z", 3});
                    const auto vector = h.newv32(
                        std::array{
                            cycle, text, immediate(tag::chr, 0x1f40b)});
                    const auto alias = h.copy<tag::v32>(vector);
                    const auto released = h.make_pin(nil);
                    h.free_pin(released);
                    const auto pin = h.make_pin(h.cons(73, 11));
                    root entry{
                        h,
                        h.newv32(std::array{vector, alias, released, pin})};
                    const auto data = tape::encode(vm, entry.get());
                    expect(tape::encode(vm, entry.get()) == data);
                    expect(
                        get32(data, 8) == 3u
                        && get32(data, 12) == word(collect_first));
                    expect(
                        get32(data, 16) == 3u
                        && get32(data, 32) == entry.get());
                    expect(
                        data[8] == std::byte{3} && data[9] == std::byte{0});
                    expect(
                        std::string_view(
                            reinterpret_cast<const char *>(data.data()), 8)
                        == "NXWISP\r\n");
                    auto saved = tape::decode(data);
                    expect(
                        tape::encode(saved->machine, saved->entry.get())
                        == data);
                    auto & copy = saved->storage;
                    auto entries = copy.v32slice(saved->entry.get());
                    const auto xs = copy.v32slice(entries[0]);
                    expect(
                        (copy.get<tag::duo, field::cdr>(xs[0]) == xs[0]));
                    expect(
                        (integer(copy.get<tag::duo, field::car>(xs[0]))
                         == -19));
                    expect(
                        copy.v08slice(xs[1])
                        == std::string_view{"a\0z", 3});
                    expect(xs[2] == immediate(tag::chr, 0x1f40b));
                    copy.v32set(entries[0], 2, 97);
                    expect(copy.v32slice(entries[1])[2] == 97u);
                    expect(
                        (copy.get<tag::duo, field::car>(copy.pinned(pin))
                         == 73u));
                    expect(payload_of(copy.make_pin(nil)) == 3u);
                    saved->machine.collect();
                    entries = copy.v32slice(saved->entry.get());
                    expect(copy.v32slice(entries[0])[2] == 97u);
                    expect(
                        (copy.get<tag::duo, field::cdr>(copy.pinned(pin))
                         == 11u));
                }
            };

        "current package, definitions, and fresh key sequence survive without reinstallation"_test =
            [] {
                heap h;
                evaluator vm{h};
                evaluate(h, vm, R"(
            (genkey!) (genkey!)
            (set-symbol-function! '+ (%fn nil (a b) (- a b)))
            (%defpackage "P")
            (package-set-uses! (find-package "P") (list (find-package "WISP")))
            (in-package P))");
                h.set<tag::pkg, field::sym>(vm.find_package("KEY"), nil);
                auto restored = tape::decode(tape::encode(vm));
                auto & copy = restored->storage;
                auto & language = restored->machine;
                language.collect();
                expect(
                    language.current_package()
                    == language.find_package("P"));
                expect(evaluate(copy, language, "(+ 19 7)") == 12u);
                auto key = evaluate(copy, language, "(genkey!)");
                expect(
                    copy.v08slice(copy.get<tag::sym, field::str>(key))
                    == "~20220101.DYYYYYYYYY"); // Third serial is 3 (D).
            };

        "pending GC and a partially evaluated run resume through NXT"_test =
            [] {
                heap h;
                evaluator vm{h};
                root run{
                    h,
                    vm.start(
                        reader{h, vm, "(do (gc) (+ 11 (* 3 7)))"}
                            .next()
                            .value())};
                vm.advance(run.get(), 100);
                expect(vm.collection_requested());
                auto restored = tape::decode(tape::encode(vm, run.get()));
                expect(restored->machine.collection_requested());
                nxtrt::deck deck;
                expect(deck.sync_wait([&] {
                    return drive(restored->machine, restored->entry, 1);
                }) == evaluation::done);
                expect(!restored->machine.collection_requested());
                expect(
                    (restored->storage.get<tag::run, field::val>(
                         restored->entry.get())
                     == 32u));
            };

        "mid-argument accumulation survives restore and corrupt cursors signal conditions"_test =
            [] {
                heap h;
                evaluator vm{h};
                root run{
                    h,
                    vm.start(
                        reader{h, vm, "(- 91 (+ 7 13) 5)"}.next().value())};
                word acc = nil;
                for (unsigned i = 0; i < 100 && acc == nil; ++i) {
                    expect(vm.step(run.get()) == evaluation::runnable);
                    const auto way = h.get<tag::run, field::way>(run.get());
                    if (way != top) {
                        const auto candidate =
                            h.get<tag::ktx, field::acc>(way);
                        if (tag_of(candidate) == tag::v32
                            && h.v32slice(candidate)[0] == 1)
                            acc = candidate;
                    }
                }
                expect(acc != nil);
                if (acc == nil)
                    return;
                expect(h.v32slice(acc)[1] == 91u);
                auto restored = tape::decode(tape::encode(vm, run.get()));
                restored->machine.collect();
                expect(
                    restored->machine.advance(restored->entry.get(), 100)
                    == evaluation::done);
                expect(
                    (restored->storage.get<tag::run, field::val>(
                         restored->entry.get())
                     == 66u));
                h.v32set(acc, 0, 999);
                restored = tape::decode(tape::encode(vm, run.get()));
                restored->machine.collect();
                expect(
                    restored->machine.advance(restored->entry.get(), 100)
                    == evaluation::failed);
                expect(print(
                           restored->storage,
                           restored->storage.get<tag::run, field::err>(
                               restored->entry.get()))
                           .contains("INVALID-CONTINUATION"));
            };

        "segmented tapes preserve run meta and frozen multi-shot snapshots"_test =
            [] {
                for (bool collect_first : {false, true}) {
                    heap h;
                    evaluator vm{h};
                    evaluate(h, vm, R"(
                  (set-symbol-dynamic! 'dyn t) (set-symbol-value! 'dyn 5)
                  (call-with-prompt 'park
                    (%fn nil () (call-with-binding 'dyn 10
                      (%fn nil () (call-with-prompt 'inner
                        (%fn nil () (+ dyn (send-with-default! 'park nil nil)))
                        (%fn nil (v k) 999)))))
                    (%fn nil (v k) (set-symbol-value! 'saved k))))");
                    root run{
                        h,
                        vm.start(
                            reader{h, vm, R"(
                  (call-with-prompt 'done
                    (%fn nil () (call-with-binding 'dyn 20
                      (%fn nil () (+ 1 2 3))))
                    (%fn nil (v k) 999)))"}
                                .next()
                                .value())};
                    for (unsigned i = 0; i < 100; ++i) {
                        if (h.get<tag::run, field::exp>(run.get()) == 2u
                            && h.get<tag::run, field::way>(run.get()) != top
                            && h.get<tag::run, field::meta>(run.get())
                                   != top)
                            break;
                        expect(vm.step(run.get()) == evaluation::runnable);
                    }
                    expect((h.get<tag::run, field::exp>(run.get()) == 2u));
                    h.set<tag::sym, field::val>(
                        vm.intern("TARGET"), run.get());
                    evaluate(
                        h,
                        vm,
                        "(set-symbol-value! 'snapshot (run-way target))");
                    if (collect_first)
                        vm.collect();
                    auto restored =
                        tape::decode(tape::encode(vm, run.get()));
                    auto & copy = restored->storage;
                    auto & machine = restored->machine;
                    // No post-restore GC: decode itself must freeze frames.
                    expect(
                        machine.advance(restored->entry.get(), 100)
                        == evaluation::done);
                    expect(
                        (copy.get<tag::run, field::val>(
                             restored->entry.get())
                         == 6u));
                    expect(
                        (copy.get<tag::run, field::meta>(
                             restored->entry.get())
                         == top));
                    expect(
                        print(copy, evaluate(copy, machine, R"(
                  (list (ktx-pos snapshot)
                    (ktx-fun (ktx-hop snapshot))
                    (ktx-arg (ktx-hop snapshot))
                    (ktx-fun (ktx-hop (ktx-hop snapshot)))
                    (top? (ktx-hop (ktx-hop (ktx-hop snapshot))))))"))
                        == "(1 BINDING 20 PROMPT T)");
                    expect(
                        print(copy, evaluate(copy, machine, R"(
                  (list (call snapshot 7) (call snapshot 9)
                    (call saved 7) (call saved 9) dyn (ktx-pos snapshot)))"))
                        == "(11 13 17 19 5 1)");
                }
            };

        "mutable environment keys and invalid uses retain their runtime meaning"_test =
            [] {
                heap h;
                evaluator vm{h};
                root run{
                    h,
                    vm.start(
                        vm.intern("X"),
                        h.cons(
                            h.newv32(
                                std::array{17u, 31u, vm.intern("X"), 42u}),
                            nil))};
                auto restored = tape::decode(tape::encode(vm, run.get()));
                expect(
                    restored->machine.advance(restored->entry.get(), 10)
                    == evaluation::done);
                expect(
                    (restored->storage.get<tag::run, field::val>(
                         restored->entry.get())
                     == 42u));
                evaluate(h, vm, R"(
                (%defpackage "P")
                (set-symbol-value! 'uses (list (find-package "WISP")))
                (package-set-uses! (find-package "P") uses)
                (set-tail! uses uses))");
                run.set(vm.start(
                    reader{
                        h,
                        vm,
                        R"((intern "NOT-PRESENT" (find-package "P")))"}
                        .next()
                        .value()));
                restored = tape::decode(tape::encode(vm, run.get()));
                restored->machine.collect();
                expect(
                    restored->machine.advance(restored->entry.get(), 100)
                    == evaluation::failed);
                expect(print(
                           restored->storage,
                           restored->storage.get<tag::run, field::err>(
                               restored->entry.get()))
                           .contains("INVALID-PACKAGE-USES"));
            };

        "a guest library continuation outlives its source machine and resumes on NXT"_test
            .with_timeout(10s) = [] {
            bytes data;
            word uninterrupted = nil;
            {
                auto base = base_image();
                heap & h = base->storage;
                evaluator & vm = base->machine;
                root packet{h, evaluate(h, vm, R"(
                (call-with-effect-handler 'host
                  (fn () (+ 7 (send! 'host 35)))
                  (fn (request resume raise) (vector request resume))))")};
                vm.collect();
                data = tape::encode(vm, packet.get());
                const auto closure = h.v32slice(packet.get())[1];
                const auto sym = vm.intern("SAVED");
                h.set<tag::sym, field::val>(sym, closure);
                uninterrupted = evaluate(h, vm, "(call saved 35)");
            }
            auto restored = tape::decode(data);
            restored->machine.collect();
            expect(
                restored->storage.v32slice(restored->entry.get())[0]
                == 35u);
            expect(uninterrupted == 42u);
            expect(resume(*restored, 35) == 42u);
            restored->machine.collect();
            expect(resume(*restored, 36) == 43u);
        };

        "builtin names remap every value location, never raw metadata"_test =
            [] {
                heap h;
                evaluator vm{h};
                const auto plus =
                    h.get<tag::sym, field::fun>(vm.intern("+"));
                const auto minus =
                    h.get<tag::sym, field::fun>(vm.intern("-"));
                const auto closure =
                    h.make<tag::fun>({nil, nil, nil, nil, plus});
                const auto frame =
                    h.make<tag::ktx>({top, nil, plus, nil, nil});
                const auto run =
                    h.make<tag::run>({nah, plus, nil, nil, frame, top});
                h.make_pin(plus);
                const auto entry = h.newv32(
                    std::array{plus, h.cons(minus, nil), closure, run});
                const auto original = tape::encode(vm, entry);
                auto reordered = original;
                layout wire{reordered};
                std::swap(
                    reordered[wire.jets.at("+")],
                    reordered[wire.jets.at("-")]);
                for (auto offset : wire.values) {
                    const auto value = get32(reordered, offset);
                    if (value == plus)
                        put32(reordered, offset, minus);
                    if (value == minus)
                        put32(reordered, offset, plus);
                }
                seal(reordered);
                auto restored = tape::decode(reordered);
                expect(
                    tape::encode(restored->machine, restored->entry.get())
                    == original);
                expect(
                    evaluate(
                        restored->storage, restored->machine, "(+ 19 23)")
                    == 42u);
                // The entry may itself be a builtin, not a heap pointer.
                put32(reordered, 32, minus);
                seal(reordered);
                expect(tape::decode(reordered)->entry.get() == plus);
            };

        "table and column identities are independent of physical order"_test =
            [] {
                heap h;
                evaluator vm{h};
                const auto original = tape::encode(vm, h.cons(17, 93));
                auto reordered = original;
                layout wire{reordered};
                const auto & duo = wire.tables.at(tag::duo);
                const auto & sym = wire.tables.at(tag::sym);
                std::swap_ranges(
                    reordered.begin() + duo.metadata,
                    reordered.begin() + duo.metadata + 16,
                    reordered.begin() + duo.metadata + 16);
                std::swap_ranges(
                    reordered.begin() + duo.data,
                    reordered.begin() + duo.data + 4 * duo.rows,
                    reordered.begin() + duo.data + 4 * duo.rows);
                std::rotate(
                    reordered.begin() + duo.begin,
                    reordered.begin() + sym.begin,
                    reordered.begin() + sym.end);
                seal(reordered);
                auto restored = tape::decode(reordered);
                expect(
                    tape::encode(restored->machine, restored->entry.get())
                    == original);
                expect(
                    print(restored->storage, restored->entry.get())
                    == "(17 . 93)");
            };

        "malformed tapes with valid checksums fail before publishing a machine"_test =
            [] {
                heap h;
                evaluator vm{h};
                const auto frame =
                    h.make<tag::ktx>({top, nil, vm.intern("DO"), nil, nil});
                const auto vector = h.newv32(std::array{17u, 31u});
                const auto pin = h.make_pin(vector);
                const auto original = tape::encode(vm, frame);
                layout wire{original};
                const auto package = vm.find_package("WISP");
                const auto symbols = h.get<tag::pkg, field::sym>(package);
                const std::pair<std::size_t, word> corruptions[]{
                    {8, 1}, // Pre-segmentation tapes are not migrated.
                    {8, 2}, // Old variable/EVAL scope is not migrated.
                    {8, 4},
                    {12, 2},
                    {16, 0},
                    {28, 2},
                    {32, pointer(tag::duo, max_index, h.era())},
                    {32, pointer(tag::ktx, index_of(frame), !h.era())},
                    {32, zap},
                    {32, 0x80000000u},
                    {32, immediate(tag::jet, max_immediate)},
                    {32, immediate(tag::pin, payload_of(pin) + 1)},
                    {wire.word_count, 0xffffffffu},
                    {wire.roots.at("current"), nil},
                    {wire.tables.at(tag::duo).begin + 4, max_index + 2},
                    {wire.tables.at(tag::ext).begin + 4, 1},
                    {wire.tables.at(tag::v08).columns.at("idx"),
                     0xffffffffu},
                    {wire.tables.at(tag::v32).columns.at("len"),
                     0xffffffffu},
                    {wire.tables.at(tag::ktx).columns.at("hop"), frame},
                    {wire.tables.at(tag::duo).columns.at("cdr")
                         + 4 * index_of(symbols),
                     symbols},
                    {wire.tables.at(tag::sym).columns.at("str"), 7},
                    {wire.pins + 4, 0},
                };
                for (auto [offset, value] : corruptions) {
                    auto bad = original;
                    put32(bad, offset, value);
                    seal(bad);
                    rejected(bad);
                }
                auto bad = original;
                bad[wire.jets.at("QUOTE")] = std::byte{'X'};
                seal(bad);
                rejected(bad);
                bad = original;
                bad[wire.tables.at(tag::duo).metadata + 4] = std::byte{'X'};
                seal(bad);
                rejected(bad);
                bad = original;
                bad.insert(bad.end() - 32, std::byte{0});
                seal(bad);
                rejected(bad);
                expect(tape::encode(vm, frame) == original);
            };

        "segmented private links and boundary payloads are validated"_test =
            [] {
                heap h;
                evaluator vm{h};
                const auto segment =
                    h.make<tag::ktx>({top, nil, vm.intern("DO"), nil, nil});
                const auto payload = h.newv32(std::array{nil, segment});
                const auto boundary = h.make<tag::ktx>(
                    {top, nil, vm.intern("PROMPT"), nil, payload});
                const auto snapshot = h.make<tag::ktx>(
                    {top,
                     nil,
                     vm.intern("CONTINUATION"),
                     segment,
                     boundary});
                const auto run =
                    h.make<tag::run>({nah, 7, nil, nil, segment, boundary});
                const auto data =
                    tape::encode(vm, h.newv32(std::array{run, snapshot}));
                layout wire{data};
                const auto ktx = [&](std::string name, word ptr) {
                    return wire.tables.at(tag::ktx).columns.at(name)
                           + 4 * index_of(ptr);
                };
                const std::pair<std::size_t, word> corruptions[]{
                    {ktx("hop", boundary), boundary},
                    {ktx("hop", boundary), segment},
                    {ktx("arg", boundary), nil},
                    {ktx("acc", snapshot), boundary},
                    {ktx("arg", snapshot), segment},
                    {wire.tables.at(tag::run).columns.at("meta"), segment},
                    {wire.tables.at(tag::run).columns.at("way"), snapshot},
                    {wire.tables.at(tag::v32).columns.at("len")
                         + 4 * index_of(payload),
                     1},
                    {wire.word_count + 4
                         + 4 * h.get<tag::v32, field::idx>(payload) + 4,
                     boundary},
                };
                for (auto [offset, value] : corruptions) {
                    auto bad = data;
                    put32(bad, offset, value);
                    seal(bad);
                    rejected(bad);
                }
                expect(tape::decode(data)->entry.get() != nil);
            };

        "checksum, truncation, input limits, and stream errors are distinct failures"_test =
            [] {
                heap h;
                evaluator vm{h};
                const auto data = tape::encode(vm, 0x01020304);
                expect(
                    data[32] == std::byte{4} && data[35] == std::byte{1});
                for (auto length :
                     {std::size_t{0},
                      std::size_t{7},
                      std::size_t{40},
                      data.size() - 1})
                    rejected(bytes(data.begin(), data.begin() + length));
                auto bad = data;
                bad[32] ^= std::byte{1};
                rejected(bad);
                expect(throws<tape::error>([&] {
                    (void) tape::decode(data, data.size() - 1);
                }));
                expect(
                    tape::decode(data, data.size())->entry.get()
                    == 0x01020304u);
                std::stringstream stream;
                tape::write(stream, vm, 0x01020304);
                expect(
                    tape::read(stream, data.size())->entry.get()
                    == 0x01020304u);
                std::stringstream throwing{stream.str()};
                throwing.exceptions(std::ios::badbit | std::ios::failbit);
                expect(tape::read(throwing)->entry.get() == 0x01020304u);
                std::stringstream limited{stream.str()};
                expect(throws<tape::error>([&] {
                    (void) tape::read(limited, data.size() - 1);
                }));
                std::stringstream broken;
                broken.setstate(std::ios::badbit);
                expect(
                    throws<tape::error>([&] { tape::write(broken, vm); }));
                expect(throws<tape::error>([&] {
                    (void) tape::read(broken);
                }));
            };

        "external rows are rejected without acquiring or releasing host resources"_test =
            [] {
                unsigned released = 0;
                heap h{{&released, [](void * context, word) noexcept {
                            ++*static_cast<unsigned *>(context);
                        }}};
                evaluator vm{h};
                h.make<tag::ext>({17, nil});
                expect(
                    throws<tape::error>([&] { (void) tape::encode(vm); }));
                expect(released == 0u);
                vm.collect();
                expect(released == 1u);
                expect(tape::decode(tape::encode(vm))->entry.get() == nil);
            };

        "deep continuation validation and restore do not use the native stack"_test =
            [] {
                heap h;
                evaluator vm{h};
                word chain = top;
                for (int i = 0; i < 12000; ++i)
                    chain = h.make<tag::ktx>(
                        {chain, nil, vm.intern("DO"), nil, nil});
                auto restored = tape::decode(tape::encode(vm, chain));
                restored->machine.collect();
                unsigned length = 0;
                for (auto cur = restored->entry.get(); cur != top;
                     cur = restored->storage.get<tag::ktx, field::hop>(cur))
                    ++length;
                expect(length == 12000u);
            };
    }};

} // namespace
} // namespace wisp::test
