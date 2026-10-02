#include <nxtrt/idea.hpp>

#include "test.hpp"

#include <memory>

namespace nxt::test {
namespace {

template<typename Result>
struct recipe
{
    Result operator()() &;
};

struct move_only_recipe
{
    std::unique_ptr<int> state;
    nxtrt::task<int> operator()() &;
};

struct immovable_recipe
{
    immovable_recipe(immovable_recipe &&) = delete;
    nxtrt::task<int> operator()() &;
};

struct rvalue_recipe
{
    nxtrt::task<int> operator()() &&;
};

struct argument_recipe
{
    nxtrt::task<int> operator()(int);
};

struct convertible_result
{
    operator nxtrt::task<int>();
};

template<typename Fn>
concept has_idea_result = requires { typename nxtrt::idea_result_t<Fn>; };

using namespace boost::ut;

static suite idea_tests{"IDEAS", [] {
    "recipes produce tasks or hopes by value"_test = [] {
        static_assert(nxtrt::idea<recipe<nxtrt::task<int>>>);
        static_assert(nxtrt::idea<recipe<nxtrt::task<void>>>);
        static_assert(nxtrt::idea<recipe<nxtrt::hope<int>>>);
        static_assert(nxtrt::idea<recipe<nxtrt::hope<void>>>);
        static_assert(nxtrt::idea_of<recipe<nxtrt::task<int>>, int>);
        static_assert(nxtrt::idea_of<recipe<nxtrt::hope<void>>, void>);
        static_assert(std::same_as<
                      nxtrt::idea_result_t<recipe<nxtrt::hope<int>>>, int>);
        static_assert(!nxtrt::idea_of<recipe<nxtrt::task<int>>, void>);
        static_assert(nxtrt::idea<nxtrt::task<int> (*)()>);
    };

    "recipes use mutable lvalue invocation and allow move-only storage"_test = [] {
        static_assert(nxtrt::idea<move_only_recipe>);
        static_assert(!std::copy_constructible<move_only_recipe>);
        static_assert(!nxtrt::idea<const move_only_recipe>);
        static_assert(!nxtrt::idea<immovable_recipe>);
        static_assert(!nxtrt::idea<rvalue_recipe>);
        static_assert(!nxtrt::idea<argument_recipe>);

        // Ordinary mutable closure, not a capturing coroutine lambda.
        auto ready = [value = std::make_unique<int>(42)]() mutable {
            return nxtrt::hope<int>::ready(*value);
        };
        static_assert(nxtrt::idea_of<decltype(ready), int>);
        expect(ready().await_resume() == 42);
    };

    "invalid result queries fail constraints without hard errors"_test = [] {
        static_assert(!nxtrt::idea<int>);
        static_assert(!nxtrt::idea<void>);
        static_assert(!nxtrt::idea<recipe<int>>);
        static_assert(!nxtrt::idea<recipe<void>>);
        static_assert(!nxtrt::idea<recipe<convertible_result>>);
        static_assert(!nxtrt::idea<recipe<nxtrt::task<int> &>>);
        static_assert(!nxtrt::idea<recipe<nxtrt::task<int> &&>>);
        static_assert(!nxtrt::idea<recipe<const nxtrt::task<int>>>);
        static_assert(!nxtrt::idea<recipe<nxtrt::hope<int> &>>);
        static_assert(!nxtrt::idea<recipe<nxtrt::hope<int> &&>>);
        static_assert(!nxtrt::idea<recipe<const nxtrt::hope<int>>>);
        static_assert(!nxtrt::idea<nxtrt::task<int>>);
        static_assert(!nxtrt::idea<nxtrt::hope<int>>);
        static_assert(!nxtrt::idea_of<int, int>);
        static_assert(!nxtrt::idea_of<recipe<int>, int>);
        static_assert(!has_idea_result<int>);
        static_assert(!has_idea_result<recipe<int>>);
    };
}};

} // namespace
} // namespace nxt::test
