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

/// Interpret library source on a machine, collecting between quanta, and
/// return its tape.
inline std::vector<std::byte>
boot_tape(std::unique_ptr<image> booted, std::string_view source)
{
    auto & h = booted->storage;
    auto & vm = booted->machine;
    loader load{h, vm, source};
    for (;;) {
        const auto state = load.advance(2048);
        vm.collect();
        if (state == evaluation::failed)
            throw std::runtime_error(
                "library failed at " + load.location() + ": "
                + print(h, h.get<tag::run, field::err>(load.run())));
        if (state == evaluation::done)
            break;
    }
    vm.collect();
    return tape::encode(vm);
}

/// The base library, interpreted once per test process and kept as a tape.
inline std::span<const std::byte> base_tape()
{
    static const auto bytes = boot_tape(image::fresh(), base_library());
    return bytes;
}

/// A private machine that has loaded the base library. Each call decodes a
/// separate copy, so tests never share guest state; only the first call in
/// a process pays for interpreting base.wisp.
inline std::unique_ptr<image> base_image()
{
    return tape::decode(base_tape());
}

/// The base tape with the guest compiler loaded on top, interpreted once
/// per test process.
inline std::span<const std::byte> compiler_tape()
{
    static const auto bytes =
        boot_tape(tape::decode(base_tape()), compiler_library());
    return bytes;
}

/// A private machine with the base library and the guest compiler.
inline std::unique_ptr<image> compiler_image()
{
    return tape::decode(compiler_tape());
}

} // namespace wisp::test
