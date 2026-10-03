#pragma once

#include <wisp/load.hpp>
#include <wisp/printer.hpp>

#include "test.hpp"
#include "wisp-base.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wisp::test {

/// A machine that loads Wisp source text, collecting after every quantum,
/// and reports the value of the last top-level form.
struct source_machine
{
    explicit source_machine(std::unique_ptr<image> from = image::fresh())
        : owner(std::move(from))
    {
    }

    std::unique_ptr<image> owner;
    heap & h = owner->storage;
    evaluator & vm = owner->machine;
    root last{h};

    word load(std::string_view text, std::size_t quantum = 257)
    {
        loader source{h, vm, text};
        for (std::size_t turns = 0; turns < 40000; ++turns) {
            const auto state = source.advance(quantum);
            h.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    "Wisp source failed at byte "
                    + std::to_string(source.position()) + ": "
                    + print(h, h.get<tag::run, field::err>(source.run())));
            if (state == evaluation::done) {
                last.set(source.run());
                return last.get() == nil
                           ? nil
                           : h.get<tag::run, field::val>(last.get());
            }
        }
        throw std::runtime_error("Wisp source exhausted its test budget");
    }

    void check(std::string_view source, std::string_view expected)
    {
        boost::ut::expect(print(h, load(source)) == expected);
    }
};

} // namespace wisp::test
