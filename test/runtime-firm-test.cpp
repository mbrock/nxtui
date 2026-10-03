#include "runtime-test.hpp"

namespace nxt::test {

void declare_runtime_firm_tests()
{
    "firms"_group = [] {
        "default firms acquire frame land only on demand"_test = [] {
            auto firm = nxtrt::firm{};
            expect(firm.frame_capacity() == std::size_t{0});
            auto moved = nxtrt::firm{std::move(firm)};
            auto * frame = moved.allocate_frame(64);
            expect(moved.frame_capacity() == std::size_t{4096});
            expect(
                &nxtrt::firm_frame_arena::owner_of(frame)
                == &moved.frame_arena());
            nxtrt::firm_frame_arena::owner_of(frame).deallocate(frame);
            expect(moved.frame_used() == std::size_t{0});
        };

        "owned frame chunks grow beyond 4 MiB without moving frames"_test =
            [] {
                auto arena = nxtrt::firm_frame_arena{};
                auto frames = std::array<void *, 25>{};
                constexpr auto bytes = std::size_t{200000};
                for (auto i = std::size_t{0}; i < frames.size(); ++i) {
                    frames[i] = arena.allocate(bytes);
                    std::fill_n(
                        static_cast<std::byte *>(frames[i]),
                        bytes,
                        static_cast<std::byte>(i + 1));
                }
                expect(arena.used() > std::size_t{4 * 1024 * 1024});
                for (auto i = std::size_t{0}; i < frames.size(); ++i) {
                    expect(
                        &nxtrt::firm_frame_arena::owner_of(frames[i])
                        == &arena);
                    auto payload = std::span{
                        static_cast<std::byte *>(frames[i]), bytes};
                    expect(
                        std::ranges::all_of(payload, [i](std::byte b) {
                            return b == static_cast<std::byte>(i + 1);
                        }));
                    arena.deallocate(frames[i]);
                }
                expect(arena.live_frames() == std::size_t{0});
                expect(arena.top() == std::size_t{0});
                auto const capacity = arena.capacity();
                // Empty chunks remain reusable, including after an
                // empty move.
                auto moved = nxtrt::firm_frame_arena{std::move(arena)};
                for (auto & frame : frames)
                    frame = moved.allocate(bytes);
                expect(moved.capacity() == capacity);
                for (auto * frame : frames) {
                    expect(
                        &nxtrt::firm_frame_arena::owner_of(frame)
                        == &moved);
                    moved.deallocate(frame);
                }
            };

        "owned frames reuse free lists and retract earlier chunk tops"_test =
            [] {
                auto arena = nxtrt::firm_frame_arena{};
                auto * first = arena.allocate(2000);
                auto * second = arena.allocate(2000);
                auto * third = arena.allocate(8000);
                auto const capacity = arena.capacity();
                auto const top = arena.top();
                auto const block = nxtrt::firm_frame_arena::block_size(2000);
                arena.deallocate(first);
                expect(arena.free_listed_bytes() == block);
                auto * reused = arena.allocate(2000);
                expect(reused == first);
                arena.deallocate(second);
                expect(arena.top() == top - block);
                auto * tail = arena.allocate(2000);
                expect(tail == second);
                expect(arena.capacity() == capacity);
                arena.deallocate(reused);
                arena.deallocate(third);
                arena.deallocate(tail);
                expect(arena.used() == std::size_t{0});
                expect(arena.top() == std::size_t{0});
                expect(arena.free_listed_bytes() == std::size_t{0});
            };

        "explicit empty and preallocated frame land stays bounded"_test =
            [] {
                auto empty =
                    nxtrt::firm_frame_arena{nxtrt::frame_storage_ref{}};
                expect(empty.allocate(1) == nullptr);
                auto const bytes = nxtrt::firm_frame_arena::block_size(200);
                auto land = nxtrt::owned_frame_storage{
                    bytes / sizeof(nxtrt::frame_cell)};
                auto arena = nxtrt::firm_frame_arena{land};
                auto * frame = arena.allocate(200);
                expect(frame != nullptr);
                expect(arena.allocate(1) == nullptr);
                expect(arena.capacity() == bytes);
                arena.deallocate(frame);
            };

        "bind the current firm while the body runs"_test = [] {
            auto deck = nxtrt::deck{};

            auto seen = deck.sync_wait([]() -> nxtrt::task<bool> {
                co_return co_await nxtrt::with_firm(
                    []() -> nxtrt::task<bool> {
                        auto * before = nxtrt::current_firm();
                        co_await nxtrt::yield();
                        auto * after = nxtrt::current_firm();
                        co_return before != nullptr && before == after;
                    });
            });

            expect(seen);
        };

        "firm frame land backs tasks created inside the firm"_test = [] {
            struct frame_probe_firm : nxtrt::firm
            {
                frame_probe_firm(
                    nxtrt::frame_storage_ref storage,
                    std::size_t & initial_high_water,
                    std::size_t & child_high_water)
                    : nxtrt::firm(storage)
                    , initial_high_water(&initial_high_water)
                    , child_high_water(&child_high_water)
                {}

                frame_probe_firm(frame_probe_firm &&) noexcept = default;
                frame_probe_firm & operator=(frame_probe_firm &&) = delete;

                nxtrt::task<void> operator()()
                {
                    *initial_high_water = frame_high_water();
                    auto child = []() -> nxtrt::task<void> {
                        co_return;
                    }();
                    *child_high_water = frame_high_water();
                    fork(std::move(child));
                    co_await join();
                }

                std::size_t * initial_high_water = nullptr;
                std::size_t * child_high_water = nullptr;
            };

            auto deck = nxtrt::deck{};
            auto storage = nxtrt::static_frame_storage<64 * 1024>{};
            auto initial_high_water = std::size_t{};
            auto child_high_water = std::size_t{};

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await frame_probe_firm{
                    storage,
                    initial_high_water,
                    child_high_water,
                };
            });

            expect(initial_high_water > std::size_t{0});
            expect(child_high_water > initial_high_water);
        };

        "firm frame land reports borrowed storage overflow"_test = [] {
            struct overflowing_firm : nxtrt::firm
            {
                explicit overflowing_firm(nxtrt::frame_storage_ref storage)
                    : nxtrt::firm(storage)
                {}

                overflowing_firm(overflowing_firm &&) noexcept = default;
                overflowing_firm & operator=(overflowing_firm &&) = delete;

                nxtrt::task<void> operator()()
                {
                    co_return;
                }
            };

            auto deck = nxtrt::deck{};
            auto storage = nxtrt::static_frame_storage<1>{};
            auto failed = false;

            try {
                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await overflowing_firm{storage};
                });
            } catch (const std::exception & e) {
                failed = std::string_view{e.what()}.contains(
                    "firm frame arena");
            }

            expect(failed);
        };

        "frame land retracts its top when the newest frame dies"_test = [] {
            auto storage = nxtrt::static_frame_storage<1024>{};
            auto arena = nxtrt::firm_frame_arena{storage};
            auto * a = arena.allocate(40);
            auto * b = arena.allocate(40);
            auto const top_after_a =
                nxtrt::firm_frame_arena::block_size(40);

            arena.deallocate(b);
            expect(arena.top() == top_after_a);
            expect(arena.live_frames() == std::size_t{1});
            expect(arena.free_listed_bytes() == std::size_t{0});

            arena.deallocate(a);
            expect(arena.top() == std::size_t{0});
            expect(arena.used() == std::size_t{0});
            expect(arena.high_water() == 2 * top_after_a);
        };

        "frame land hands a freed block to the next frame of its size"_test = [] {
            auto storage = nxtrt::static_frame_storage<1024>{};
            auto arena = nxtrt::firm_frame_arena{storage};
            auto * oldest = arena.allocate(40);
            auto * middle = arena.allocate(40);
            auto * newest = arena.allocate(40);
            auto const top = arena.top();

            arena.deallocate(middle);
            expect(arena.free_listed_bytes()
                   == nxtrt::firm_frame_arena::block_size(40));

            auto * other_size = arena.allocate(200);
            expect(other_size != middle);
            auto * same_size = arena.allocate(40);
            expect(same_size == middle);
            expect(arena.free_listed_bytes() == std::size_t{0});
            expect(arena.top()
                   == top + nxtrt::firm_frame_arena::block_size(200));

            arena.deallocate(other_size);
            arena.deallocate(same_size);
            arena.deallocate(newest);
            arena.deallocate(oldest);
            expect(arena.live_frames() == std::size_t{0});
            expect(arena.top() == std::size_t{0});
        };

        "frame land resets once no frame is live"_test = [] {
            auto storage = nxtrt::static_frame_storage<4096>{};
            auto arena = nxtrt::firm_frame_arena{storage};
            auto frames = std::array<void *, 20>{};
            for (auto i = std::size_t{0}; i < frames.size(); ++i)
                frames[i] = arena.allocate(16 * (i + 1));

            // Free oldest first: none of these is on top, and 20 sizes
            // overflow the 16 size classes, so some blocks strand.
            for (auto i = std::size_t{0}; i + 1 < frames.size(); ++i)
                arena.deallocate(frames[i]);
            expect(arena.stranded_bytes() > std::size_t{0});
            expect(arena.free_listed_bytes() > std::size_t{0});

            arena.deallocate(frames.back());
            expect(arena.top() == std::size_t{0});
            expect(arena.free_listed_bytes() == std::size_t{0});
            expect(arena.stranded_bytes() == std::size_t{0});
        };

        "frame land reports exhaustion without throwing"_test = [] {
            auto storage = nxtrt::static_frame_storage<256>{};
            auto arena = nxtrt::firm_frame_arena{storage};
            auto * fits = arena.allocate(200);
            expect(fits != nullptr);
            expect(arena.allocate(200) == nullptr);
            arena.deallocate(fits);
            expect(arena.allocate(200) != nullptr);
        };

        "firm frame land is reused by long-lived workers' awaits"_test = [] {
            auto deck = nxtrt::deck{};
            auto storage = nxtrt::static_frame_storage<16 * 1024>{};
            auto totals = std::array<int, 4>{};
            auto high_water = std::size_t{};
            constexpr auto iterations = 5000;

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await frame_reuse_firm{
                    nxtrt::firm{storage},
                    iterations,
                    totals,
                    high_water,
                };
            });

            for (auto total : totals)
                expect(total == iterations);
            // 20000 step frames would need far more than 16 KiB without
            // reuse; the footprint stays near the live set instead.
            expect(high_water < std::size_t{16 * 1024});
        };

        "lazy frame chunks are reused by long-lived workers"_test = [] {
            auto deck = nxtrt::deck{};
            auto totals = std::array<int, 4>{};
            auto high_water = std::size_t{};
            constexpr auto iterations = 5000;

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await frame_reuse_firm{
                    nxtrt::firm{}, iterations, totals, high_water};
            });

            for (auto total : totals)
                expect(total == iterations);
            expect(high_water < std::size_t{16 * 1024});
        };

        "firm frame arena overflow names the land it ran out of"_test = [] {
            struct overflowing_firm : nxtrt::firm
            {
                explicit overflowing_firm(nxtrt::frame_storage_ref storage)
                    : nxtrt::firm(storage)
                {}

                overflowing_firm(overflowing_firm &&) noexcept = default;
                overflowing_firm & operator=(overflowing_firm &&) = delete;

                nxtrt::task<void> operator()()
                {
                    co_return;
                }
            };

            auto deck = nxtrt::deck{};
            auto storage = nxtrt::static_frame_storage<1>{};
            auto message = std::string{};

            try {
                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await overflowing_firm{storage};
                });
            } catch (const std::exception & e) {
                message = e.what();
            }

            expect(message.contains("bytes for a"));
            expect(message.contains("live frames"));
            expect(message.contains("high water"));
        };

        "nursery grows beyond the former child limit with stable deeds"_test = [] {
            auto deck = nxtrt::deck{};
            auto deeds = deck.sync_wait([] {
                return nxtrt::with_firm(
                    [](nxtrt::firm & scope)
                        -> nxtrt::task<std::vector<nxtrt::deed<int>>> {
                        auto deeds = std::vector<nxtrt::deed<int>>{};
                        for (auto i = 0; i < 4100; ++i) {
                            deeds.push_back(
                                scope.fork(value_after_yield(i)));
                            // Reuse frames while retaining linked
                            // results.
                            if (i % 64 == 63)
                                co_await scope.join();
                        }
                        co_await scope.join();
                        expect(scope.child_count() == 4100);
                        co_return deeds;
                    });
            });
            for (auto i = 0; i < 4100; ++i)
                expect(std::move(deeds[i]).get() == i);
        };

        "join forked tasks before the firm exits"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        scope.fork(record_after_yield(events, 1));
                        events.push_back(2);
                        co_await scope.join();
                        co_return;
                    });
                events.push_back(3);
                co_return;
            });

            expect(events == std::vector<int>{2, 11, 12, 3});
        };

        "reject firm bodies that return before joining"_test = [] {
            auto deck = nxtrt::deck{};
            auto message = std::string{};

            try {
                deck.sync_wait([]() -> nxtrt::task<void> {
                    co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope) -> nxtrt::task<void> {
                            scope.fork(value_after_yield(1));
                            co_return;
                        });
                });
            } catch (const std::exception & e) {
                message = e.what();
            }

            expect(message.contains("unjoined children"));
            expect(message.contains("co_await scope.join()"));
        };

        "firm subclasses are directly awaitable"_test = [] {
            struct child_firm : nxtrt::firm
            {
                explicit child_firm(std::vector<int> & events)
                    : events(events)
                {}

                std::vector<int> & events;

                nxtrt::task<int> operator()()
                {
                    auto child = fork(value_after_yield(7));
                    events.push_back(1);
                    co_await join();
                    events.push_back(2);
                    co_return std::move(child).get();
                }
            };

            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto result =
                deck.sync_wait([&]() -> nxtrt::task<int> {
                co_return co_await child_firm{events};
            });

            expect(result == 7_i);
            expect(events == std::vector<int>{1, 2});
        };

        "policy firms can be subclassed"_test = [] {
            struct first_success_firm : nxtrt::stop_on_success
            {
                explicit first_success_firm(std::vector<int> & events)
                    : events(events)
                {}

                std::vector<int> & events;

                nxtrt::task<nxtrt::deed<int>> operator()()
                {
                    auto child = fork(value_after_yield(5));
                    fork(record_stop_state_after_two_yields(events, 9));
                    co_await join();
                    co_return std::move(child);
                }
            };

            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto child =
                deck.sync_wait([&]()
                    -> nxtrt::task<nxtrt::deed<int>> {
                    co_return co_await first_success_firm{events};
                });

            expect(std::move(child).get() == 5_i);
            expect(events == std::vector<int>{9});
        };

        "let forked tasks inherit the current firm"_test = [] {
            auto deck = nxtrt::deck{};
            auto firms = std::vector<nxtrt::firm *>{};
            auto expected = static_cast<nxtrt::firm *>(nullptr);

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        expected = &scope;
                        scope.fork(record_current_firm(firms));
                        co_await scope.join();
                        co_return;
                    });
                co_return;
            });

            expect(expected != nullptr);
            expect(firms == std::vector<nxtrt::firm *>{expected});
        };

        "explicit owner overrides inner scope but inherits caller values"_test = [] {
            auto deck = nxtrt::deck{};
            auto seen = std::vector<nxtrt::firm *>{};
            auto calls = 0;
            deck.sync_wait([&] {
                return nxtrt::with_firm(
                    [&](nxtrt::firm & outer) -> nxtrt::task<void> {
                        auto prepared = record_current_firm(seen);
                        auto result = nxtrt::deed<int>{};
                        co_await nxtrt::with_firm(
                            [&](nxtrt::firm & inner)
                                -> nxtrt::task<void> {
                                expect(nxtrt::current_firm() == &inner);
                                co_await nxtrt::with_env<
                                    ambient_int_key>(
                                    29, [&]() -> nxtrt::task<void> {
                                        outer.fork(std::move(prepared));
                                        result = outer.fork([&] {
                                            ++calls;
                                            expect(
                                                nxtrt::current_firm()
                                                == &outer);
                                            auto task =
                                                read_ambient_int_after_yield();
                                            expect(
                                                &nxtrt::firm_frame_arena::
                                                    owner_of(
                                                        task.handle()
                                                            .address())
                                                == &outer
                                                        .frame_arena());
                                            return task;
                                        });
                                        expect(
                                            nxtrt::current_firm()
                                            == &inner);
                                        expect(
                                            inner.child_count() == 0);
                                        co_return;
                                    });
                            });
                        // The inner scope is gone before the outer
                        // joins.
                        co_await outer.join();
                        expect(calls == 1);
                        expect(std::move(result).get() == 29);
                        expect(
                            seen == std::vector<nxtrt::firm *>{&outer});
                    });
            });
        };

        "inner owner may adopt a frame from an enclosing scope"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto seen = std::vector<nxtrt::firm *>{};
                deck.sync_wait([&] {
                    return nxtrt::with_firm(
                        [&](nxtrt::firm & outer) -> nxtrt::task<void> {
                            auto task = record_current_firm(seen);
                            expect(
                                &nxtrt::firm_frame_arena::owner_of(
                                    task.handle().address())
                                == &outer.frame_arena());
                            co_await nxtrt::with_firm(
                                [&](nxtrt::firm & inner)
                                    -> nxtrt::task<void> {
                                    inner.fork(std::move(task));
                                    co_await inner.join();
                                    expect(
                                        seen
                                        == std::vector<nxtrt::firm *>{
                                            &inner});
                                });
                            expect(outer.child_count() == 0);
                        });
                });
            };

        "outer owner rejects a shorter lived inner frame"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto rejected = false;
            deck.sync_wait([&] {
                return nxtrt::with_firm(
                    [&](nxtrt::firm & outer) -> nxtrt::task<void> {
                        co_await nxtrt::with_firm(
                            [&]() -> nxtrt::task<void> {
                                try {
                                    outer.fork(
                                        record_after_yield(events, 7));
                                } catch (const nxtrt::runtime_error &
                                             error) {
                                    rejected =
                                        std::string_view{error.what()}
                                            .contains(
                                                "non-enclosing scope");
                                }
                                co_return;
                            });
                        expect(outer.child_count() == 0);
                    });
            });
            expect(rejected);
            expect(events.empty());
        };

        "firm child records remember deck task ids"_test = [] {
            struct task_id_observer_firm : nxtrt::firm
            {
                task_id_observer_firm(
                    std::vector<nxtrt::task_id> & running_ids,
                    std::vector<nxtrt::task_id> & deed_ids,
                    std::vector<nxtrt::task_id> & completed_ids)
                    : running_ids(&running_ids)
                    , deed_ids(&deed_ids)
                    , completed_ids(&completed_ids)
                {}

                task_id_observer_firm(
                    task_id_observer_firm &&) noexcept = default;
                task_id_observer_firm & operator=(
                    task_id_observer_firm &&) = delete;

                nxtrt::task<void> operator()()
                {
                    auto child =
                        fork(record_current_task_id_after_yield(
                            *running_ids));
                    deed_ids->push_back(child.child_task_id());
                    co_await join();
                }

                void completed(
                    nxtrt::task_id child,
                    std::exception_ptr) noexcept override
                {
                    completed_ids->push_back(child);
                }

            private:
                std::vector<nxtrt::task_id> * running_ids = nullptr;
                std::vector<nxtrt::task_id> * deed_ids = nullptr;
                std::vector<nxtrt::task_id> * completed_ids = nullptr;
            };

            auto deck = nxtrt::deck{};
            auto running_ids = std::vector<nxtrt::task_id>{};
            auto deed_ids = std::vector<nxtrt::task_id>{};
            auto completed_ids = std::vector<nxtrt::task_id>{};

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await task_id_observer_firm{
                    running_ids,
                    deed_ids,
                    completed_ids,
                };
            });

            expect(running_ids.size() == std::size_t{1});
            expect(deed_ids.size() == std::size_t{1});
            expect(completed_ids.size() == std::size_t{1});
            expect(static_cast<bool>(running_ids.front()));
            expect(deed_ids.front() == running_ids.front());
            expect(completed_ids.front() == running_ids.front());
        };

        "firm fork unwinds child record after deck registry overflow"_test =
            [] {
                // Two scope scaffolds occupy six task IDs; this probe
                // occupies the seventh. Only the fork must overflow.
                auto storage = nxtrt::static_deck_task_storage<7>{};
                auto deck = nxtrt::deck{storage};
                auto events = std::vector<int>{};
                auto overflowed = false;
                auto child_count_after_failure = std::size_t{42};

                deck.sync_wait(fork_deck_overflow_root{
                    &events,
                    &overflowed,
                    &child_count_after_failure,
                });

                expect(overflowed);
                expect(child_count_after_failure == std::size_t{0});
                expect(events.empty());
            };

        "allow children to fork more work into the same firm"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};

            auto parent =
                [&events](nxtrt::firm & scope) -> nxtrt::task<void> {
                events.push_back(1);
                co_await nxtrt::yield();
                scope.fork(record_after_yield(events, 2));
                events.push_back(3);
                co_return;
            };

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        scope.fork(parent(scope));
                        co_await scope.join();
                        co_return;
                    });
                events.push_back(4);
                co_return;
            });

            expect(events == std::vector<int>{1, 3, 21, 22, 4});
        };

        "tasks require a firm at construction"_test = [] {
            auto rejected = false;
            try {
                (void)[]() -> nxtrt::task<void> {
                    co_return;
                }();
            } catch (const nxtrt::runtime_error &) {
                rejected = true;
            }

            expect(rejected);
        };

        "propagate child exceptions after joining siblings"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto threw = false;

            try {
                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_firm(
                        [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                            scope.fork(throw_after_yield(events, 0));
                            scope.fork(record_after_yield(events, 2));
                            co_await scope.join();
                            co_return;
                        });
                    co_return;
                });
            } catch (const std::exception &) {
                threw = true;
            }

            expect(threw);
            expect(events == std::vector<int>{1, 21, 22});
        };

        "group multiple child exceptions"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto grouped = false;

            try {
                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_firm(
                        [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                            scope.fork(throw_after_yield(events, 1));
                            scope.fork(throw_after_yield(events, 2));
                            scope.fork(record_after_yield(events, 3));
                            co_await scope.join();
                            co_return;
                        });
                    co_return;
                });
            } catch (const nxtrt::exception_group & group) {
                grouped = true;
                expect(group.exceptions().size() == std::size_t{2});
            }

            expect(grouped);
            expect(events == std::vector<int>{11, 21, 31, 32});
        };

        "return forked task results after joining"_test = [] {
            auto deck = nxtrt::deck{};

            auto child =
                deck.sync_wait([]() -> nxtrt::task<nxtrt::deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::deed<int>> {
                            auto child =
                                scope.fork(value_after_yield(42));
                            co_await scope.join();
                            co_return std::move(child);
                        });
                });

            expect(std::move(child).get() == 42_i);
        };

        "evacuate forked task results into external deed storage"_test = [] {
            auto deck = nxtrt::deck{};
            auto target = 0;

            auto child = deck.sync_wait(
                external_result_root{.target = &target});

            expect(target == 64_i);
            expect(std::move(child).get() == 64_i);
        };

        "evacuate move-only results into borrowed deed storage"_test = [] {
            auto deck = nxtrt::deck{};
            auto target =
                nxtrt::deed_result_storage<firm_result_value>{};

            auto child = deck.sync_wait(
                external_result_cell_root{.target = &target});

            expect(target.ready());
            auto value = std::move(child).get();
            expect(value.value == 71_i);
            expect(!target.ready());
        };

        "evacuate results into borrowed deed storage pools"_test = [] {
            auto deck = nxtrt::deck{};
            auto storage =
                nxtrt::static_deed_result_storage_pool<
                    firm_result_value,
                    1>{};
            auto pool = storage.ref();

            auto child = deck.sync_wait(
                pooled_result_cell_root{.pool = &pool});

            expect(pool.capacity() == std::size_t{1});
            expect(pool.used() == std::size_t{1});
            expect(pool.high_water() == std::size_t{1});
            auto value = std::move(child).get();
            expect(value.value == 72_i);

            auto another_pool_ref = storage.ref();
            expect(another_pool_ref.used() == std::size_t{1});
            expect(another_pool_ref.high_water() == std::size_t{1});

            auto overflowed = false;
            try {
                (void)another_pool_ref.borrow();
            } catch (const std::exception & e) {
                overflowed = std::string_view{e.what()}.contains(
                    "deed result storage pool is full");
            }
            expect(overflowed);

            auto empty_storage =
                nxtrt::static_deed_result_storage_pool<
                    firm_result_value,
                    0>{};
            auto empty_pool = empty_storage.ref();
            expect(empty_pool.capacity() == std::size_t{0});
            expect(empty_pool.used() == std::size_t{0});

            auto empty_overflowed = false;
            try {
                (void)empty_pool.borrow();
            } catch (const std::exception & e) {
                empty_overflowed =
                    std::string_view{e.what()}.contains(
                        "deed result storage pool is full");
            }
            expect(empty_overflowed);
        };

        "move live deeds before child completion"_test = [] {
            auto deck = nxtrt::deck{};

            auto child =
                deck.sync_wait([]() -> nxtrt::task<nxtrt::deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::deed<int>> {
                            auto first =
                                scope.fork(value_after_yield(77));
                            auto id = first.child_task_id();
                            auto second = std::move(first);
                            expect(second.child_task_id() == id);
                            co_await scope.join();
                            co_return std::move(second);
                        });
                });

            expect(static_cast<bool>(child.child_task_id()));
            expect(std::move(child).get() == 77_i);
        };

        "fork task factories with explicit arguments"_test = [] {
            auto deck = nxtrt::deck{};

            auto child =
                deck.sync_wait([]() -> nxtrt::task<nxtrt::deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::deed<int>> {
                            auto child =
                                scope.fork(value_after_yield, 88);
                            co_await scope.join();
                            co_return std::move(child);
                        });
                });

            expect(std::move(child).get() == 88_i);
        };

        "return several forked task results"_test = [] {
            auto deck = nxtrt::deck{};
            using children_type = std::tuple<
                nxtrt::deed<int>,
                nxtrt::deed<int>>;

            auto children =
                deck.sync_wait([]() -> nxtrt::task<children_type> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<children_type> {
                            auto first =
                                scope.fork(value_after_yield(10));
                            auto second =
                                scope.fork(value_after_yield(20));
                            co_await scope.join();
                            co_return children_type{
                                std::move(first),
                                std::move(second)};
                        });
                });

            auto [first, second] = std::move(children);
            expect(std::move(first).get() == 10_i);
            expect(std::move(second).get() == 20_i);
        };

        "return failed deeds for caller observation"_test = [] {
            auto deck = nxtrt::deck{};

            auto child =
                deck.sync_wait([]() -> nxtrt::task<nxtrt::deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::deed<int>> {
                            auto child =
                                scope.fork(throw_int_after_yield());
                            co_await scope.join();
                            co_return std::move(child);
                        });
                });

            auto threw = false;
            try {
                (void)std::move(child).get();
            } catch (const std::exception &) {
                threw = true;
            }

            expect(threw);
        };

        "allow observed deed failures inside the firm"_test = [] {
            auto deck = nxtrt::deck{};
            auto observed = false;

            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        auto child =
                            scope.fork(throw_int_after_yield());
                        co_await nxtrt::yield();
                        co_await nxtrt::yield();
                        observed = child.exception() != nullptr;
                        co_await scope.join();
                        co_return;
                    });
            });

            expect(observed);
        };

        "dropped deeds do not hide child failures"_test = [] {
            auto deck = nxtrt::deck{};
            auto threw = false;

            try {
                deck.sync_wait([]() -> nxtrt::task<void> {
                    co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope) -> nxtrt::task<void> {
                            {
                                auto child =
                                    scope.fork(throw_int_after_yield());
                            }
                            co_await scope.join();
                        });
                });
            } catch (const std::exception &) {
                threw = true;
            }

            expect(threw);
        };

        "let coped deeds report failure as expected"_test = [] {
            auto deck = nxtrt::deck{};

            auto child = deck.sync_wait(
                []() -> nxtrt::task<nxtrt::catching_deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::catching_deed<int>> {
                            auto child =
                                scope.fork(throw_int_after_yield())
                                    .cope();
                            co_await scope.join();
                            co_return std::move(child);
                        });
                });

            expect(static_cast<bool>(child.child_task_id()));
            auto result = std::move(child).get();
            expect(!result.has_value());
        };

        "let coped deeds report success as expected"_test = [] {
            auto deck = nxtrt::deck{};

            auto child = deck.sync_wait(
                []() -> nxtrt::task<nxtrt::catching_deed<int>> {
                    co_return co_await nxtrt::with_firm(
                        [](nxtrt::firm & scope)
                            -> nxtrt::task<nxtrt::catching_deed<int>> {
                            auto child =
                                scope.fork(value_after_yield(99))
                                    .cope();
                            co_await scope.join();
                            co_return std::move(child);
                        });
                });

            expect(static_cast<bool>(child.child_task_id()));
            auto result = std::move(child).get();
            expect(result.has_value());
            expect(*result == 99_i);
        };

        declare_runtime_firm_stop_tests();
    };
}

} // namespace nxt::test
