#include <nxt/function-ref.hpp>

#include "task-test.hpp"

#include <memory>
#include <type_traits>

namespace nxt::test {
namespace {

using namespace boost::ut;

int add_one(int value) noexcept
{
    return value + 1;
}

int add_two(int value) noexcept
{
    return value + 2;
}

struct counter
{
    int value = 0;

    int add(int amount) noexcept
    {
        return value += amount;
    }

    int read() const noexcept
    {
        return value;
    }

    int operator()(int amount) noexcept
    {
        return add(amount);
    }
};

using mutable_view = function_ref<int(int)>;
using const_view = function_ref<int(int) const>;
using noexcept_view = function_ref<int(int) noexcept>;
using const_noexcept_view = function_ref<int(int) const noexcept>;

static_assert(std::is_trivially_copyable_v<mutable_view>);
static_assert(std::is_trivially_copyable_v<const_view>);
static_assert(std::is_trivially_copyable_v<noexcept_view>);
static_assert(std::is_trivially_copyable_v<const_noexcept_view>);
static_assert(!std::is_default_constructible_v<mutable_view>);
static_assert(!std::is_constructible_v<mutable_view, std::nullptr_t>);
static_assert(std::is_constructible_v<noexcept_view, counter &>);
static_assert(!std::is_constructible_v<const_view, counter &>);
static_assert(!std::is_constructible_v<mutable_view, const counter &>);
static_assert(
    !std::is_constructible_v<mutable_view, decltype(&counter::add)>);
static_assert(!std::is_assignable_v<mutable_view &, counter &>);
static_assert(std::is_assignable_v<mutable_view &, decltype(&add_one)>);
static_assert(noexcept(std::declval<noexcept_view>()(1)));
static_assert(!noexcept(std::declval<mutable_view>()(1)));

struct mutable_constant
{
    int operator()(int n)
    {
        return n;
    }
};

constexpr auto mutable_target = mutable_constant{};
static_assert(
    !std::is_constructible_v<mutable_view, nontype_t<mutable_target>>);
static_assert(!std::is_constructible_v<
              noexcept_view,
              nontype_t<&counter::add>,
              const counter &>);
static_assert(
    std::is_same_v<decltype(function_ref{add_one}), noexcept_view>);

// This closure remains alive in the suite's stack frame while the test
// framework invokes and awaits it. Captures must survive actual suspension.
void declare_borrowed_coroutine_test()
{
    auto state = std::make_unique<int>(17);
    auto body = [owned = std::move(state)]() -> nxtrt::task<void> {
        co_await nxtrt::yield();
        expect(*owned == 17);
        co_await nxtrt::yield();
        expect(*owned == 17);
    };
    "borrowed coroutine body retains move-only captures across suspension"_test =
        body;
}

static suite function_ref_tests{
    "function references", [] {
        "function pointers are stored by value and copies are independent"_test =
            [] {
                auto pointer = &add_one;
                auto view = mutable_view{pointer};
                pointer = &add_two;
                expect(view(3) == 4);
                auto copy = view;
                view = &add_two;
                expect(view(3) == 5);
                expect(copy(3) == 4);
            };
        "borrows mutable callable state without copying"_test = [] {
            auto fn = [state = std::make_unique<int>(0)](
                          int amount) mutable noexcept {
                return *state += amount;
            };
            auto view = noexcept_view{fn};
            auto copy = view;
            expect(view(2) == 2);
            expect(copy(3) == 5);
            expect(fn(1) == 6);
        };
        "const and noexcept signatures constrain the borrowed callable"_test =
            [] {
                auto fn = [amount = 7](int value) noexcept {
                    return value + amount;
                };
                auto throwing = [](int) -> int { throw 19; };
                static_assert(std::is_constructible_v<
                              const_noexcept_view,
                              decltype(fn) &>);
                static_assert(!std::is_constructible_v<
                              noexcept_view,
                              decltype(throwing) &>);
                const auto & const_fn = fn;
                auto view = const_noexcept_view{const_fn};
                expect(view(2) == 9);
                auto can_throw = mutable_view{throwing};
                try {
                    (void) can_throw(0);
                    expect(false);
                } catch (int value) {
                    expect(value == 19);
                }
            };
        "compile-time targets bind members without adapter closures"_test =
            [] {
                auto object = counter{};
                auto add = noexcept_view{nontype<&counter::add>, object};
                auto add_pointer =
                    noexcept_view{nontype<&counter::add>, &object};
                auto read = function_ref<int() const noexcept>{
                    nontype<&counter::read>, object};
                auto value =
                    function_ref<int &()>{nontype<&counter::value>, object};
                expect(add(3) == 3);
                expect(add_pointer(4) == 7);
                expect(read() == 7);
                value() = 11;
                expect(read() == 11);
                const auto immutable = counter{23};
                auto read_const = function_ref<int() const noexcept>{
                    nontype<&counter::read>, &immutable};
                expect(read_const() == 23);
            };
        "compile-time free functions bind references and pointer values"_test =
            [] {
                auto direct = noexcept_view{nontype<add_one>};
                expect(direct(5) == 6);
                auto first = counter{1};
                auto second = counter{9};
                auto pointer = &first;
                auto fn = [](counter * object, int amount) noexcept {
                    return object->add(amount);
                };
                auto bound = noexcept_view{nontype<fn>, pointer};
                pointer = &second;
                expect(bound(3) == 4);
                expect(first.value == 4 && second.value == 9);
                auto ref_fn = [](counter & object, int amount) noexcept {
                    return object.add(amount);
                };
                auto bound_ref = noexcept_view{nontype<ref_fn>, second};
                expect(bound_ref(2) == 11);
            };
        "preserves reference results and forwards move-only arguments"_test =
            [] {
                auto value = 4;
                auto ref_fn = [&]() -> int & { return value; };
                auto reference = function_ref<int &()>{ref_fn};
                reference() = 8;
                expect(value == 8);
                auto consume = [](std::unique_ptr<int> input) {
                    return *input;
                };
                auto view =
                    function_ref<int(std::unique_ptr<int>)>{consume};
                expect(view(std::make_unique<int>(29)) == 29);
                auto discard = function_ref<void()>{ref_fn};
                discard();
                expect(value == 8);
            };
        "immediate calls can borrow temporary closures"_test = [] {
            auto invoke = [](function_ref<int()> callback) {
                return callback();
            };
            expect(invoke([value = 13] { return value; }) == 13);
        };
        declare_borrowed_coroutine_test();
    }};

// Registration's temporary closure is gone by the time the suite runs.
// Keep its owned capture alive through every filtered or repeated run.
static suite captured_suite{
    "owned suite registration", [value = std::make_shared<int>(31)] {
        "registered suite retains its callable until execution"_test =
            [&value] { expect(*value == 31); };
    }};

} // namespace
} // namespace nxt::test
