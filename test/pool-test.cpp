#include <nxtrt/pool.hpp>
#if defined(__linux__)
#include <nxtrt/wand/epoll.hpp>
#else
#include <nxtrt/wand/kqueue.hpp>
#endif

#include "test.hpp"

#include <array>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace nxt::test {
namespace {

using namespace boost::ut;

template<typename Fn>
struct recipe_feed final : nxtrt::feed<Fn>
{
    explicit recipe_feed(std::vector<Fn> recipes)
        : nxtrt::feed<Fn>(1), recipes(std::move(recipes))
    {}

    std::vector<Fn> recipes;
    std::size_t next = 0;
    std::size_t pause_at = std::size_t(-1);
    bool resume = false;
    bool waiting = false;

private:
    nxtrt::hope<nxtrt::fare_t> stream_more(
        nxtrt::sink<Fn> & sink, std::size_t limit) override
    {
        if (limit == 0)
            return nxtrt::hope<nxtrt::fare_t>::ready(0);
        if (next == pause_at && !resume)
            return delayed(sink);
        return nxtrt::hope<nxtrt::fare_t>::ready(produce(sink));
    }

    nxtrt::fare_t produce(nxtrt::sink<Fn> & sink)
    {
        if (next == recipes.size())
            return nxtrt::eof;
        auto write = sink.write(std::move(recipes[next++]));
        expect(write.is_ready());
        write.take_ready();
        return 1;
    }

    nxtrt::task<nxtrt::fare_t> delayed(nxtrt::sink<Fn> & sink)
    {
        waiting = true;
        while (!resume) {
            if (nxtrt::current_task_stop_token().stop_requested()) {
                waiting = false;
                throw nxtrt::operation_cancelled{};
            }
            co_await nxtrt::yield();
        }
        waiting = false;
        co_return produce(sink);
    }
};

template<typename Fn, std::size_t N>
struct pool_land
{
    std::array<nxtrt::pool_slot<Fn>, N> slots;
    nxtrt::farm<nxtrt::pool_slot<Fn>, N> farm{&slots};
    nxtrt::static_value_storage<nxtrt::pool_result_t<Fn>, N> output;

    void check_returned()
    {
        std::set<nxtrt::pool_slot<Fn> *> returned;
        for (std::size_t i = 0; i != N; ++i) {
            auto * slot = farm.try_alloc();
            expect(slot != nullptr);
            if (slot)
                expect(returned.insert(slot).second);
        }
        expect(farm.try_alloc() == nullptr);
        for (auto * slot : returned)
            farm.release(slot);
    }
};

struct job_state
{
    int invoked = 0;
    int retired = 0;
    int frames_destroyed = 0;
    int cancelled = 0;
    int cancellation_turns = 0;
    int throw_factory = -1;
    std::vector<int> completed;
    std::set<const void *> live_recipes;
    std::function<void(int)> on_invoke;
};

// A coroutine member intentionally borrows the admitted recipe. Moves before
// invocation are allowed; moving or destroying it during execution is not.
struct mixed_recipe
{
    job_state * state;
    int value;
    int turns;
    bool invoked = false;

    mixed_recipe(job_state & state, int value, int turns = -1)
        : state(&state), value(value), turns(turns)
    {}
    mixed_recipe(mixed_recipe && other) noexcept
        : state(other.state), value(other.value), turns(other.turns)
    {
        expect(!other.invoked);
    }
    ~mixed_recipe()
    {
        if (invoked) {
            expect(state->live_recipes.erase(this) == std::size_t{1});
            ++state->retired;
        }
    }

    nxtrt::hope<int> operator()() &
    {
        expect(!invoked);
        invoked = true;
        expect(state->live_recipes.insert(this).second);
        ++state->invoked;
        if (state->on_invoke)
            state->on_invoke(value);
        if (value == state->throw_factory)
            throw std::runtime_error{"factory failed"};
        if (turns < 0) {
            state->completed.push_back(value);
            return nxtrt::hope<int>::ready(value);
        }
        return run();
    }

    nxtrt::task<int> run()
    {
        struct frame_guard
        {
            job_state & state;
            const void * recipe;
            ~frame_guard()
            {
                expect(state.live_recipes.contains(recipe));
                ++state.frames_destroyed;
            }
        } guard{*state, this};
        for (int i = 0; i != turns; ++i) {
            co_await nxtrt::yield();
            if (nxtrt::current_task_stop_token().stop_requested()) {
                ++state->cancelled;
                for (int turn = 0; turn != state->cancellation_turns; ++turn)
                    co_await nxtrt::yield();
                throw nxtrt::operation_cancelled{};
            }
        }
        state->completed.push_back(value);
        co_return value;
    }
};

struct task_recipe : mixed_recipe
{
    using mixed_recipe::mixed_recipe;
    nxtrt::task<int> operator()() &
    {
        expect(!invoked);
        invoked = true;
        expect(state->live_recipes.insert(this).second);
        ++state->invoked;
        return run();
    }
};

template<typename Fn>
nxtrt::task<std::vector<nxtrt::pool_result_t<Fn>>> collect(nxtrt::pool<Fn> & pool)
{
    std::vector<nxtrt::pool_result_t<Fn>> values;
    while (auto item = co_await pool.take())
        values.push_back(std::move(*item));
    co_return values;
}

std::vector<mixed_recipe> ready_recipes(job_state & state, int count)
{
    std::vector<mixed_recipe> recipes;
    for (int i = 0; i != count; ++i)
        recipes.emplace_back(state, i);
    return recipes;
}

nxtrt::task<void> async_upstream_case(
    nxtrt::pool<mixed_recipe> & pool, recipe_feed<mixed_recipe> & input)
{
    auto first = co_await pool.take();
    expect(first && *first == 10);
    // The second upstream read cannot finish until the consumer releases it.
    // Serially awaiting that read before publishing 10 would deadlock.
    expect(input.waiting);
    expect(!input.resume);
    input.resume = true;
    expect((co_await pool.take()) == std::optional<int>{20});
    expect(!(co_await pool.take()));
}

template<typename Fn>
nxtrt::task<void> abandon_after_one(nxtrt::pool<Fn> & pool)
{
    expect((co_await pool.take()) == std::optional<int>{0});
    throw std::runtime_error{"consumer failed"};
}

nxtrt::task<void> partial_lookahead_case(
    nxtrt::pool<mixed_recipe> & pool, job_state & state, bool short_input)
{
    if (short_input) {
        bool caught = false;
        try {
            (void)co_await pool.peek(3);
        } catch (const nxtrt::value_end_of_stream &) {
            caught = true;
        }
        expect(caught);
    } else {
        auto view = co_await pool.peek(2);
        std::vector<int> values;
        for (auto chunk : view)
            values.insert(values.end(), chunk.begin(), chunk.end());
        expect(values == std::vector<int>{0, 1});
    }
    auto * first = co_await pool.peek();
    for (int i = 0; i != 8; ++i) {
        co_await nxtrt::yield();
        expect((co_await pool.peek()) == first);
        expect(state.invoked == 2);
        expect(state.retired == 0);
    }
    expect(pool.occupied() == std::size_t{2});
    expect((co_await collect(pool)) == std::vector<int>{0, 1});
}

nxtrt::task<void> close_after_peek(nxtrt::pool<mixed_recipe> & pool)
{
    expect(*(co_await pool.peek()) == 0);
    co_await pool.close();
    co_await pool.close();
    expect(pool.occupied() == std::size_t{0});
}

nxtrt::task<void> overlapping_close_case(nxtrt::pool<mixed_recipe> & pool)
{
    expect((co_await pool.take()) == std::optional<int>{0});
    auto closing = pool.close();
    nxtrt::current_deck()->start(closing);
    co_await nxtrt::yield();
    expect(!closing.done());
    closing.request_stop();
    bool caught = false;
    try {
        co_await pool.close();
    } catch (const nxtrt::runtime_error & error) {
        caught = std::string_view{error.what()}
                 == "nxtrt pool has overlapping operations";
    }
    expect(caught);
    while (!closing.done())
        co_await nxtrt::yield();
    closing.result();
}

struct move_recipe
{
    int value;
    nxtrt::hope<std::unique_ptr<int>> operator()() &
    {
        return nxtrt::hope<std::unique_ptr<int>>::ready(
            std::make_unique<int>(value));
    }
};

struct void_recipe
{
    int * invoked;
    nxtrt::hope<void> operator()() &
    {
        ++*invoked;
        return nxtrt::hope<void>::ready();
    }
};

struct throwing_value
{
    bool * fail;
    int * live;
    int * moves_left;
    throwing_value(bool & fail, int & live, int * moves_left = nullptr)
        : fail(&fail), live(&live), moves_left(moves_left)
    {
        ++live;
    }
    throwing_value(throwing_value && other)
        : fail(other.fail), live(other.live), moves_left(other.moves_left)
    {
        if (*fail || (moves_left && --*moves_left == 0))
            throw std::runtime_error{"result move failed"};
        ++*live;
    }
    ~throwing_value() { --*live; }
};

struct throwing_recipe
{
    bool * fail;
    int * live;
    int * moves_left = nullptr;
    nxtrt::hope<throwing_value> operator()() &
    {
        return nxtrt::hope<throwing_value>::ready(
            throwing_value{*fail, *live, moves_left});
    }
};

struct stopping_value
{
    std::function<void()> * on_move;

    explicit stopping_value(std::function<void()> & on_move)
        : on_move(&on_move)
    {}

    stopping_value(stopping_value && other)
        : on_move(other.on_move)
    {
        (*on_move)();
    }
};

struct stopping_recipe
{
    std::function<void()> * on_move;

    nxtrt::hope<stopping_value> operator()()
    {
        return nxtrt::hope<stopping_value>::ready(stopping_value{*on_move});
    }
};

#if NXT_RT_HAS_EPOLL || NXT_RT_HAS_KQUEUE
#if NXT_RT_HAS_EPOLL
using timer_wand = nxtrt::epoll_wand;
#else
using timer_wand = nxtrt::kqueue_wand;
#endif

struct timer_recipe
{
    job_state * state;
    int value;
    std::chrono::nanoseconds delay;

    nxtrt::task<int> operator()() &
    {
        ++state->invoked;
        try {
            co_await nxtrt::op::timeout::after(delay);
        } catch (const nxtrt::operation_cancelled &) {
            ++state->cancelled;
            throw;
        }
        state->completed.push_back(value);
        co_return value;
    }
};

void timer_cancellation_case(bool early_consumer_failure)
{
    using namespace std::chrono_literals;
    job_state state;
    recipe_feed input{std::vector<timer_recipe>{
        {&state, 0, early_consumer_failure ? 1ms : 10s},
        {&state, 1, 10s},
    }};
    pool_land<timer_recipe, 2> land;
    nxtrt::pool<timer_recipe> pool{input, land.farm, land.output};
    timer_wand wand;
    nxtrt::deck deck{&wand};
    if (early_consumer_failure) {
        nxtrt::root_task root{deck, [&] {
            return nxtrt::finally(
                abandon_after_one(pool), [&] { return pool.close(); });
        }};
        root.start();
        wand.run_until_done(deck, root.inner());
        bool caught = false;
        try {
            root.inner().result();
        } catch (const std::runtime_error & error) {
            caught = std::string_view{error.what()} == "consumer failed";
        }
        expect(caught);
        expect(state.completed == std::vector<int>{0});
        expect(state.cancelled == 1);
    } else {
        nxtrt::root_task root{deck, [&] { return collect(pool); }};
        root.start();
        // Let both wishes actually park in the backend before cancelling the
        // consumer, rather than merely cancelling unstarted coroutine frames.
        for (int round = 0; round != 32 && state.invoked != 2; ++round)
            deck.run_ready(wand);
        expect(state.invoked == 2);
        expect(!root.inner().done());
        root.inner().request_stop();
        wand.run_until_done(deck, root.inner());
        bool caught = false;
        try {
            (void)std::move(root.inner()).result();
        } catch (const nxtrt::operation_cancelled &) {
            caught = true;
        }
        expect(caught);
        expect(state.completed.empty());
        expect(state.cancelled == 2);
    }
    expect(pool.occupied() == std::size_t{0});
    land.check_returned();
}
#endif

static suite pool_tests{"POOLS", [] {
    "ready hopes batch without a deck and retain credit through peek"_test = [] {
        expect(nxtrt::current_deck() == nullptr);
        expect(nxtrt::current_firm() == nullptr);
        job_state state;
        recipe_feed input{ready_recipes(state, 41)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        expect(pool.capacity() == std::size_t{3});
        // Scalar peek fills synchronously; feed::peek(n) intentionally uses a
        // coroutine when it must fill, even for an otherwise synchronous feed.
        auto initial = pool.peek();
        expect(initial.is_ready());
        expect(*initial.take_ready() == 0);
        auto peek = pool.peek(3);
        expect(peek.is_ready());
        expect(peek.take_ready().size() == std::size_t{3});
        expect(state.invoked == 3);
        expect(pool.occupied() == std::size_t{3});
        expect(land.farm.empty());
        auto * first = pool.peek().take_ready();
        for (int i = 0; i != 5; ++i)
            expect(pool.peek().take_ready() == first);
        expect(state.invoked == 3);
        expect(state.retired == 0);
        for (int i = 0; i != 41; ++i) {
            auto next = pool.take();
            expect(next.is_ready());
            expect(next.take_ready() == std::optional<int>{i});
            // Taking a buffered value returns credit, never invokes a recipe.
            if (i == 0) {
                expect(state.invoked == 3);
                expect(state.retired == 1);
                expect(pool.occupied() == std::size_t{2});
            }
        }
        expect(!pool.take().take_ready());
        expect(state.invoked == 41);
        expect(state.retired == 41);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "pending tasks publish completion order and retain their recipes"_test = [] {
        job_state state;
        std::vector<task_recipe> recipes;
        recipes.emplace_back(state, 0, 8);
        recipes.emplace_back(state, 1, 1);
        recipes.emplace_back(state, 2, 4);
        recipe_feed input{std::move(recipes)};
        pool_land<task_recipe, 4> land;
        nxtrt::pool<task_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        auto values = deck.sync_wait([&] { return collect(pool); });
        expect(values == std::vector<int>{1, 2, 0});
        expect(values == state.completed);
        expect(state.invoked == 3);
        expect(state.frames_destroyed == 3);
        expect(state.retired == 3);
        expect(input.next == std::size_t{3});
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "mixed ready and pending hopes reuse a small farm many times"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        for (int i = 0; i != 97; ++i)
            recipes.emplace_back(state, i, i % 3 == 0 ? -1 : i % 5);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        auto values = deck.sync_wait([&] { return collect(pool); });
        expect(values.size() == std::size_t{97});
        expect(std::set<int>(values.begin(), values.end()).size()
               == std::size_t{97});
        expect(values == state.completed);
        expect(state.invoked == 97);
        expect(state.retired == 97);
        expect(state.frames_destroyed == 64);
        land.check_returned();
    };

    "a pending upstream read cannot hide a completed job"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 10, 2);
        recipes.emplace_back(state, 20, 1);
        recipe_feed input{std::move(recipes)};
        input.pause_at = 1;
        pool_land<mixed_recipe, 2> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        deck.sync_wait([&] { return async_upstream_case(pool, input); });
        expect(state.retired == 2);
        land.check_returned();
    };

    "lookahead waits for pending output rather than spinning on ready output"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0);
        recipes.emplace_back(state, 1, 2);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 2> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        deck.sync_wait([&] { return partial_lookahead_case(pool, state, false); });
        land.check_returned();
    };

    "short lookahead reports EOF without discarding buffered outcomes"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0);
        recipes.emplace_back(state, 1, 2);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        deck.sync_wait([&] { return partial_lookahead_case(pool, state, true); });
        land.check_returned();
    };

    "extra output storage cannot request lookahead beyond admission capacity"_test = [] {
        job_state state;
        recipe_feed input{ready_recipes(state, 4)};
        pool_land<mixed_recipe, 2> land;
        nxtrt::static_value_storage<int, 4> oversized_output;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, oversized_output};
        bool caught = false;
        try {
            (void)pool.peek(3);
        } catch (const nxtrt::value_buffer_error &) {
            caught = true;
        }
        expect(caught);
        expect(state.invoked == 0);
        expect(pool.capacity() == std::size_t{2});
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "move-only results transfer ownership and void becomes monostate"_test = [] {
        recipe_feed input{std::vector<move_recipe>{{7}, {9}, {11}}};
        pool_land<move_recipe, 2> land;
        nxtrt::pool<move_recipe> pool{input, land.farm, land.output};
        for (int expected : {7, 9, 11}) {
            auto value = pool.take().take_ready();
            expect(value && **value == expected);
        }
        expect(!pool.take().take_ready());
        land.check_returned();

        int invoked = 0;
        recipe_feed void_input{
            std::vector<void_recipe>{{&invoked}, {&invoked}, {&invoked}}};
        pool_land<void_recipe, 2> void_land;
        nxtrt::pool<void_recipe> void_pool{
            void_input, void_land.farm, void_land.output};
        static_assert(std::derived_from<
            decltype(void_pool), nxtrt::feed<std::monostate>>);
        for (int i = 0; i != 3; ++i)
            expect(void_pool.take().take_ready().has_value());
        expect(!void_pool.take().take_ready());
        expect(invoked == 3);
        void_land.check_returned();
    };

    "factory failure drains already running jobs and returns every slot"_test = [] {
        job_state state;
        state.throw_factory = 1;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0, 100);
        recipes.emplace_back(state, 1, 0);
        recipes.emplace_back(state, 2, 0);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        bool caught = false;
        try {
            (void)deck.sync_wait([&] { return collect(pool); });
        } catch (const std::runtime_error & error) {
            caught = std::string_view{error.what()} == "factory failed";
        }
        expect(caught);
        expect(state.invoked == 2);
        expect(state.retired == 2);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "throwing consumption leaves its credit available for close"_test = [] {
        bool fail = false;
        int live = 0;
        recipe_feed input{std::vector<throwing_recipe>{{&fail, &live}}};
        pool_land<throwing_recipe, 1> land;
        nxtrt::pool<throwing_recipe> pool{input, land.farm, land.output};
        expect(pool.peek().take_ready() != nullptr);
        fail = true;
        bool caught = false;
        try {
            (void)pool.take();
        } catch (const std::runtime_error & error) {
            caught = std::string_view{error.what()} == "result move failed";
        }
        expect(caught);
        nxtrt::deck deck;
        deck.sync_wait([&] { return pool.close(); });
        expect(live == 0);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "stop from a result move prevents further input admission"_test = [] {
        bool reached_end = false;
        for (int stop_at = 1; stop_at != 32 && !reached_end; ++stop_at) {
            int moves = 0;
            bool stopped = false;
            std::size_t admitted_at_stop = 0;
            std::function<void()> on_move;
            recipe_feed input{std::vector<stopping_recipe>{
                {&on_move}, {&on_move}, {&on_move}}};
            pool_land<stopping_recipe, 3> land;
            {
                nxtrt::pool<stopping_recipe> pool{
                    input, land.farm, land.output};
                on_move = [&] {
                    if (++moves == stop_at) {
                        stopped = true;
                        admitted_at_stop = input.next;
                        pool.stop();
                    }
                };
                bool cancelled = false;
                try {
                    auto value = pool.peek();
                    expect(value.is_ready());
                } catch (const nxtrt::operation_cancelled &) {
                    cancelled = true;
                }
                expect(cancelled == stopped);
                if (stopped) {
                    expect(input.next == admitted_at_stop);
                    expect(pool.occupied() == std::size_t{0});
                } else {
                    reached_end = true;
                }
            }
            land.check_returned();
        }
        expect(reached_end);
    };

    "failure at every result move through publication returns all credit"_test = [] {
        // Walk the move boundaries instead of pinning a compiler-dependent
        // move count. The first successful run has passed every throwing site.
        int failing_sites = 0;
        bool reached_success = false;
        for (int fail_at = 1; fail_at != 17; ++fail_at) {
            bool fail = false;
            int live = 0;
            int moves_left = fail_at;
            recipe_feed input{
                std::vector<throwing_recipe>{{&fail, &live, &moves_left}}};
            pool_land<throwing_recipe, 1> land;
            nxtrt::pool<throwing_recipe> pool{input, land.farm, land.output};
            try {
                expect(pool.peek().take_ready() != nullptr);
                reached_success = true;
            } catch (const std::runtime_error & error) {
                expect(std::string_view{error.what()} == "result move failed");
                ++failing_sites;
                expect(pool.occupied() == std::size_t{0});
                expect(live == 0);
            }
            nxtrt::deck deck;
            deck.sync_wait([&] { return pool.close(); });
            expect(live == 0);
            land.check_returned();
            if (reached_success)
                break;
        }
        expect(reached_success);
        expect(failing_sites >= 3);
    };

    "early consumer failure uses finally to cancel and drain siblings"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0, 1);
        recipes.emplace_back(state, 1, 100);
        recipes.emplace_back(state, 2, 100);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        bool caught = false;
        try {
            deck.sync_wait([&] {
                return nxtrt::finally(
                    abandon_after_one(pool), [&] { return pool.close(); });
            });
        } catch (const std::runtime_error & error) {
            caught = std::string_view{error.what()} == "consumer failed";
        }
        expect(caught);
        expect(state.cancelled == 2);
        expect(state.frames_destroyed == 3);
        expect(state.retired == 3);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "close discards peeked output and is idempotent"_test = [] {
        job_state state;
        recipe_feed input{ready_recipes(state, 8)};
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        deck.sync_wait([&] { return close_after_peek(pool); });
        expect(state.invoked == 3);
        expect(state.retired == 3);
        land.check_returned();
    };

    "overlapping close is rejected without disrupting a stopped close drain"_test = [] {
        job_state state;
        state.cancellation_turns = 8;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0, 1);
        recipes.emplace_back(state, 1, 100);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 2> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        deck.sync_wait([&] { return overlapping_close_case(pool); });
        expect(state.cancelled == 1);
        expect(state.retired == 2);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "cancellation drains both a pending upstream read and running jobs"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        recipes.emplace_back(state, 0, 100);
        recipes.emplace_back(state, 1, 100);
        recipe_feed input{std::move(recipes)};
        input.pause_at = 1;
        pool_land<mixed_recipe, 3> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::deck deck;
        nxtrt::root_task root{deck, [&] { return collect(pool); }};
        root.start();
        for (int round = 0; round != 32 && !input.waiting; ++round)
            deck.run_ready();
        expect(input.waiting);
        expect(state.invoked == 1);
        expect(pool.occupied() == std::size_t{2});
        root.inner().request_stop();
        deck.run_until_idle();
        expect(root.inner().done());
        bool caught = false;
        try {
            (void)std::move(root.inner()).result();
        } catch (const nxtrt::operation_cancelled &) {
            caught = true;
        }
        expect(caught);
        expect(!input.waiting);
        expect(state.cancelled == 1);
        expect(state.retired == 1);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "a factory stopping its pool cannot start its returned task or admit more"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        for (int i = 0; i != 4; ++i)
            recipes.emplace_back(state, i, 100);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 4> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        state.on_invoke = [&](int value) {
            if (value == 1)
                pool.stop();
        };
        nxtrt::deck deck;
        bool caught = false;
        try {
            (void)deck.sync_wait([&] { return collect(pool); });
        } catch (const nxtrt::operation_cancelled &) {
            caught = true;
        }
        expect(caught);
        expect(state.invoked == 2);
        expect(state.frames_destroyed == 1);
        expect(state.cancelled == 1);
        expect(state.retired == 2);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

    "deck exhaustion drains admitted tasks before propagating failure"_test = [] {
        job_state state;
        std::vector<mixed_recipe> recipes;
        for (int i = 0; i != 12; ++i)
            recipes.emplace_back(state, i, 100);
        recipe_feed input{std::move(recipes)};
        pool_land<mixed_recipe, 12> land;
        nxtrt::pool<mixed_recipe> pool{input, land.farm, land.output};
        nxtrt::static_deck_task_storage<8> task_land;
        nxtrt::deck deck{task_land};
        bool caught = false;
        try {
            (void)deck.sync_wait([&] { return collect(pool); });
        } catch (const nxtrt::runtime_error & error) {
            caught = std::string_view{error.what()}
                     == "nxtrt deck task table is full";
        }
        expect(caught);
        expect(state.invoked > 1 && state.invoked < 12);
        expect(state.retired == state.invoked);
        expect(pool.occupied() == std::size_t{0});
        land.check_returned();
    };

#if NXT_RT_HAS_EPOLL || NXT_RT_HAS_KQUEUE
    "consumer cancellation drains actual parked timer wishes"_test = [] {
        timer_cancellation_case(false);
    };

    "finally closes a pool with an actual timer still parked"_test = [] {
        timer_cancellation_case(true);
    };
#endif
}};

} // namespace
} // namespace nxt::test
