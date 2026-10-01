#include <wisp/heap.hpp>

#include "test.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace wisp::test {

using namespace boost::ut;

static_assert(sizeof(word) == 4);
static_assert(!std::is_move_constructible_v<heap>);
static_assert(!std::is_move_constructible_v<root>);
static_assert(column_index<tag::sym, field::fun>() == 3);
static_assert(column_index<tag::ktx, field::arg>() == 4);

static suite wisp_tests{
    "WISP", [] {
        "word packing matches Zig's explicit bit identities"_test = [] {
            expect(nil == 0x88000000u);
            expect(t == 0x88000001u);
            expect(nah == 0x88000002u);
            expect(zap == 0x88000003u);
            expect(top == 0x88000004u);
            expect(fixnum(-1) == 0x7fffffffu);
            expect(fixnum(min_fixnum) == 0x40000000u);
            expect(fixnum(max_fixnum) == 0x3fffffffu);
            for (auto n : {min_fixnum, -1025, -1, 0, 1, 513, max_fixnum}) {
                expect(integer(fixnum(n)) == n);
                expect(tag_of(fixnum(n)) == tag::integer);
            }
            expect(pointer(tag::duo, 0, false) == 0xa8000000u);
            expect(pointer(tag::ktx, 0x123456, true) == 0xe82468adu);
            expect(pointer(tag::ext, max_index, true) == 0xf7ffffffu);
            expect(index_of(0xe82468adu) == 0x123456u);
            expect(era_of(0xe82468adu));
            expect(!era_of(0xe82468acu));
            expect(immediate(tag::pin, max_immediate) == 0xffffffffu);
            expect(!is_pointer(tag_of(0xffffffffu)));
            expect(payload_of(0xffffffffu) == max_immediate);
            expect(immediate(tag::chr, 0x1f680) == 0x9001f680u);
            expect(immediate(tag::jet, 17) == 0x98000011u);
        };

        "tables retain every column across growth and ownership moves"_test =
            [] {
                tab<tag::ktx> table;
                table.reserve(
                    3); // Non-power-of-two capacity, as in NXT land.
                for (word i = 0; i < 4097; ++i)
                    expect(
                        table.push({i, i + 3, i * 7, i ^ 0x55, i + 91})
                        == i);
                expect(table.size() == 4097u);
                auto moved = std::move(table);
                expect(table.size() == 0u && table.capacity() == 0u);
                expect(table.push({1, 2, 3, 4, 5}) == 0u);
                for (word i = 0; i < 4097; ++i) {
                    expect(
                        moved.read(i)
                        == row<tag::ktx>{
                            i, i + 3, i * 7, i ^ 0x55, i + 91});
                    expect(moved.col(3)[i] == (i ^ 0x55));
                }
                moved.put(37, {8, 6, 4, 2, 0});
                expect(moved.get(37, 2) == 4u);
                auto rejected = false;
                try {
                    moved.reserve(std::size_t{max_index} + 2);
                } catch (const std::length_error &) {
                    rejected = true;
                }
                expect(rejected && moved.size() == 4097u);
                expect(moved.read(37) == row<tag::ktx>{8, 6, 4, 2, 0});
            };

        "payload append supports self-borrowing across growth"_test = [] {
            heap h;
            auto empty_bytes = h.newv08({});
            auto empty_words = h.newv32({});
            expect(h.v08slice(empty_bytes).empty());
            expect(h.v32slice(empty_words).empty());
            auto bytes = h.newv08(std::string_view{"a\0b!", 4});
            auto vector =
                h.newv32(std::array<word, 5>{3, nil, fixnum(-7), 11, t});
            for (int i = 0; i < 40; ++i) {
                auto text = h.newv08(h.v08slice(bytes).substr(1, 3));
                expect(h.v08slice(text) == std::string_view{"\0b!", 3});
                auto copy = h.newv32(h.v32slice(vector).subspan(1, 3));
                expect(
                    std::ranges::equal(
                        h.v32slice(copy),
                        std::array<word, 3>{nil, fixnum(-7), 11}));
            }
            auto filled = h.filledv32(19, fixnum(-13));
            expect(h.v32slice(filled).size() == 19u);
            expect(std::ranges::all_of(h.v32slice(filled), [](word x) {
                return x == fixnum(-13);
            }));
            auto before = h.word_count();
            auto rejected = false;
            try {
                h.filledv32(std::numeric_limits<word>::max(), nil);
            } catch (const std::length_error &) {
                rejected = true;
            }
            expect(rejected && h.word_count() == before);
        };

        "row copies alias vector payloads but clonev32 snapshots them"_test =
            [] {
                heap h;
                root original{h, h.newv32(std::array<word, 3>{4, 9, 16})};
                root shallow{h, h.copy<tag::v32>(original.get())};
                root cloned{h, h.clonev32(original.get())};
                h.v32set(original.get(), 1, 27);
                expect(h.v32slice(shallow.get())[1] == 27u);
                expect(h.v32slice(cloned.get())[1] == 9u);
                // This is a peculiarity of Zig Tidy: distinct descriptor
                // rows each copy their payload, so shallow payload aliasing
                // ends at GC.
                h.collect();
                expect(h.word_count() == 9u);
                h.v32set(original.get(), 1, 81);
                expect(h.v32slice(shallow.get())[1] == 27u);
                expect(h.v32slice(cloned.get())[1] == 9u);
            };

        "moving collection preserves cycles sharing and duplicate roots"_test =
            [] {
                heap h;
                (void) h.cons(
                    99, 100); // Dead row forces the live indices to change.
                auto a = h.cons(3, nil);
                auto b = h.cons(a, a);
                h.set<tag::duo, field::cdr>(a, b);
                root first{h, b};
                root duplicate{h, b};
                auto pin = h.make_pin(a);
                for (int pass = 0; pass < 4; ++pass) {
                    auto old = first.get();
                    h.collect();
                    expect(h.era() == (pass % 2 == 0));
                    expect(era_of(old) != era_of(first.get()));
                    expect(first.get() == duplicate.get());
                    expect(h.table<tag::duo>().size() == 2u);
                    auto pair = h.read<tag::duo>(first.get());
                    expect(pair[0] == pair[1]);
                    expect(pair[0] == h.pinned(pin));
                    expect(h.get<tag::duo, field::car>(pair[0]) == 3u);
                    expect(
                        h.get<tag::duo, field::cdr>(pair[0])
                        == first.get());
                }
                first.set(nil);
                duplicate.set(nil);
                h.free_pin(pin);
                h.collect();
                expect(h.table<tag::duo>().size() == 0u);
                expect(h.make_pin(42) == 0xf8000002u);
            };

        "root unlinking supports non-LIFO release and pins retain alone"_test =
            [] {
                heap h;
                auto first = std::make_unique<root>(h, h.cons(1, nil));
                auto middle = std::make_unique<root>(h, h.cons(2, nil));
                auto last = std::make_unique<root>(h, h.cons(3, nil));
                auto pin = h.make_pin(middle->get());
                middle.reset();
                first.reset();
                h.collect();
                expect(h.table<tag::duo>().size() == 2u);
                expect(h.get<tag::duo, field::car>(h.pinned(pin)) == 2u);
                expect(h.get<tag::duo, field::car>(last->get()) == 3u);
                last.reset();
                h.collect();
                expect(h.table<tag::duo>().size() == 1u);
                h.free_pin(pin);
                h.collect();
                expect(h.table<tag::duo>().size() == 0u);
            };

        "Tidy scans nested vector cycles and retains the byte pool"_test =
            [] {
                heap h;
                (void) h.newv08("garbage");
                auto text = h.newv08("live");
                (void) h.filledv32(91, 123);
                auto outer = h.filledv32(3, nil);
                auto inner = h.newv32(std::array<word, 2>{outer, text});
                auto pair = h.cons(inner, outer);
                h.v32set(outer, 0, inner);
                h.v32set(outer, 1, pair);
                h.v32set(outer, 2, immediate(tag::chr, 955));
                root live{h, outer};
                for (int pass = 0; pass < 3; ++pass) {
                    h.collect();
                    expect(h.word_count() == 5u);
                    expect(h.byte_count() == 11u);
                    expect(h.table<tag::v08>().size() == 1u);
                    auto a = h.v32slice(live.get());
                    auto b = h.v32slice(a[0]);
                    expect(b[0] == live.get());
                    expect(h.v08slice(b[1]) == "live");
                    expect(
                        h.read<tag::duo>(a[1])
                        == row<tag::duo>{a[0], live.get()});
                    expect(a[2] == 0x900003bbu);
                }
            };

        "all schema tables trace values but not raw metadata"_test = [] {
            heap h;
            auto text = h.newv08("name");
            auto pkg = h.make<tag::pkg>({text, nil, nil});
            auto sym = h.make<tag::sym>({text, pkg, nah, nil, t});
            auto env = h.newv32(std::array<word, 2>{sym, fixnum(-5)});
            // Pointer-looking metadata must remain raw, not become a GC
            // edge.
            auto fun = h.make<tag::fun>({env, nil, sym, sym, 0xe8001234u});
            auto mac = h.make<tag::mac>({env, sym, fun, sym, 0xa8005678u});
            auto ext = h.make<tag::ext>({0xf0002468u, mac});
            auto frame = h.make<tag::ktx>({top, env, fun, nil, ext});
            auto run = h.make<tag::run>({sym, nah, nil, env, frame});
            h.set<tag::pkg, field::sym>(pkg, h.cons(sym, nil));
            h.set<tag::sym, field::fun>(sym, fun);
            root state{h, run};
            h.collect();
            auto run_row = h.read<tag::run>(state.get());
            auto frame_row = h.read<tag::ktx>(run_row[4]);
            auto fun_row = h.read<tag::fun>(frame_row[2]);
            auto ext_row = h.read<tag::ext>(frame_row[4]);
            auto mac_row = h.read<tag::mac>(ext_row[1]);
            auto sym_row = h.read<tag::sym>(run_row[0]);
            auto pkg_row = h.read<tag::pkg>(sym_row[1]);
            expect(run_row[1] == nah && run_row[2] == nil);
            expect(frame_row[0] == top && frame_row[3] == nil);
            expect(fun_row[0] == run_row[3] && mac_row[0] == run_row[3]);
            expect(fun_row[2] == run_row[0] && fun_row[3] == run_row[0]);
            expect(fun_row[4] == 0xe8001234u);
            expect(mac_row[4] == 0xa8005678u);
            expect(ext_row[0] == 0xf0002468u);
            expect(sym_row[4] == t);
            expect(h.v08slice(pkg_row[0]) == "name");
            expect(h.get<tag::duo, field::car>(pkg_row[1]) == run_row[0]);
            expect(h.v32slice(run_row[3])[0] == run_row[0]);
            expect(h.v32slice(run_row[3])[1] == fixnum(-5));
        };

        "long same-table chains are scanned without recursive traversal"_test =
            [] {
                heap h;
                root list{h};
                for (word i = 0; i < 10000; ++i) {
                    (void) h.cons(37, nil);
                    list.set(h.cons(i, list.get()));
                }
                h.collect();
                expect(h.table<tag::duo>().size() == 10000u);
                auto p = list.get();
                for (word i = 10000; i != 0; --i) {
                    auto pair = h.read<tag::duo>(p);
                    expect(pair[0] == i - 1);
                    p = pair[1];
                }
                expect(p == nil);
                list.set(h.cons(
                    17, list.get())); // Growth after exact GC reservation.
                h.collect();
                expect(h.table<tag::duo>().size() == 10001u);
                expect(h.get<tag::duo, field::car>(list.get()) == 17u);
            };

        "continuation copies share lexical store but snapshot argument state"_test =
            [] {
                heap h;
                auto env = h.newv32(std::array<word, 2>{nil, 7});
                auto acc = h.newv32(std::array<word, 4>{1, 11, nah, nah});
                auto fun = h.make<tag::fun>({env, nil, nil, nil, 0});
                for (auto callee : {fun, immediate(tag::jet, 19)}) {
                    auto frame =
                        h.make<tag::ktx>({top, env, callee, acc, nil});
                    root saved{h, h.copy_continuation_frame(frame)};
                    root one{h, h.copy_continuation_frame(saved.get())};
                    root two{h, h.copy_continuation_frame(saved.get())};
                    auto acc_one = h.get<tag::ktx, field::acc>(one.get());
                    auto acc_two = h.get<tag::ktx, field::acc>(two.get());
                    expect(acc_one != acc_two);
                    h.v32set(acc_one, 2, 29);
                    h.v32set(acc_two, 2, 31);
                    h.v32set(env, 1, 43);
                    h.collect();
                    auto saved_row = h.read<tag::ktx>(saved.get());
                    auto one_row = h.read<tag::ktx>(one.get());
                    auto two_row = h.read<tag::ktx>(two.get());
                    expect(
                        saved_row[1] == one_row[1]
                        && one_row[1] == two_row[1]);
                    expect(h.v32slice(one_row[1])[1] == 43u);
                    expect(h.v32slice(saved_row[3])[2] == nah);
                    expect(h.v32slice(one_row[3])[2] == 29u);
                    expect(h.v32slice(two_row[3])[2] == 31u);
                    // Rebind native temporaries before the next explicit
                    // safepoint.
                    env = saved_row[1];
                    acc = saved_row[3];
                    fun = saved_row[2];
                }
                // A non-call frame's vector accumulator is shared, not
                // snapshotted.
                auto frame = h.make<tag::ktx>({top, env, nil, acc, nil});
                auto copied = h.copy_continuation_frame(frame);
                expect(h.get<tag::ktx, field::acc>(copied) == acc);
                auto unary = h.make<tag::ktx>(
                    {top, env, immediate(tag::jet, 2), nil, nil});
                expect(
                    h.get<tag::ktx, field::acc>(
                        h.copy_continuation_frame(unary))
                    == nil);
            };

        "unreachable externals release once and survivors release on teardown"_test =
            [] {
                struct releases
                {
                    std::array<word, 4> ids{};
                    std::size_t count = 0;
                } log;
                {
                    heap h{{&log, [](void * context, word id) noexcept {
                                auto & out =
                                    *static_cast<releases *>(context);
                                out.ids[out.count++] = id;
                            }}};
                    (void) h.make<tag::ext>({17, nil});
                    root first{h, h.make<tag::ext>({23, h.cons(7, nil)})};
                    root duplicate{h, first.get()};
                    root last{h, h.make<tag::ext>({31, nil})};
                    h.collect();
                    expect(log.count == 1u && log.ids[0] == 17u);
                    expect(first.get() == duplicate.get());
                    auto value = h.get<tag::ext, field::val>(first.get());
                    expect(h.get<tag::duo, field::car>(value) == 7u);
                    first.set(nil);
                    duplicate.set(nil);
                    h.collect();
                    h.collect();
                    expect(log.count == 2u && log.ids[1] == 23u);
                }
                expect(log.count == 3u && log.ids[2] == 31u);
            };
    }};

} // namespace wisp::test
