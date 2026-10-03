#pragma once

#include <nxtrt/task.hpp>

#include "test.hpp"

#include <functional>

/// Coroutine test bodies, `"name"_test = []() -> nxtrt::task<void> { ... }`,
/// run to completion on a fresh deck. Every test shares this one entry, so a
/// coroutine test costs no more to compile than its own coroutine.
template<>
struct boost::ut::test_body_runner<nxtrt::task<void>>
{
    static void run(std::function<nxtrt::task<void>()> body)
    {
        auto deck = nxtrt::deck{};
        deck.sync_wait(std::ref(body));
    }
};
