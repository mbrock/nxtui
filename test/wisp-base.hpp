#pragma once

#include <wisp/load.hpp>
#include <wisp/printer.hpp>
#include <wisp/tape.hpp>

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wisp::test {

/// The base library, interpreted once per test process and kept as a tape.
inline std::span<const std::byte> base_tape()
{
    static const auto bytes = [] {
        auto booted = image::fresh();
        auto & h = booted->storage;
        auto & vm = booted->machine;
        loader source{h, vm, base_library()};
        for (;;) {
            const auto state = source.advance(2048);
            vm.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    "base library failed at byte "
                    + std::to_string(source.position()) + ": "
                    + print(h, h.get<tag::run, field::err>(source.run())));
            if (state == evaluation::done)
                break;
        }
        vm.collect();
        return tape::encode(vm);
    }();
    return bytes;
}

/// A private machine that has loaded the base library. Each call decodes a
/// separate copy, so tests never share guest state; only the first call in
/// a process pays for interpreting base.wisp.
inline std::unique_ptr<image> base_image()
{
    return tape::decode(base_tape());
}

} // namespace wisp::test
