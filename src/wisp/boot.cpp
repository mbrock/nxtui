// SPDX-License-Identifier: AGPL-3.0-or-later
// Build-time bootstrap for the executable host, not the portable library.
#include "wisp/load.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"

#include <fstream>
#include <iostream>

namespace {

// #embed is intentionally used as a C++23 extension.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
constexpr unsigned char host_bytes[] = {
#embed "host.wisp"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
const std::string_view host_source{
    reinterpret_cast<const char *>(host_bytes), sizeof(host_bytes)};

} // namespace

int main(int argc, char ** argv)
{
    using namespace wisp;
    try {
        if (argc != 3)
            throw std::runtime_error(
                "expected output tape path and none|zlib");
        const std::string_view format{argv[2]};
        if (format != "none" && format != "zlib")
            throw std::runtime_error("expected compression none|zlib");
        auto booted = image::fresh();
        for (auto [source, path] :
             {std::pair{base_library(), "base.wisp"},
              std::pair{host_source, "host.wisp"}}) {
            {
                loader load{booted->storage, booted->machine, source, path};
                auto state = evaluation::runnable;
                while (state == evaluation::runnable)
                    state = load.advance(2048);
                if (state == evaluation::failed)
                    throw std::runtime_error(
                        load.location() + ": "
                        + print(
                            booted->storage,
                            booted->storage.get<tag::run, field::err>(
                                load.run())));
            }
            booted->machine.collect();
        }
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(argv[1], std::ios::binary);
        tape::write(
            output,
            booted->machine,
            nil,
            format == "zlib" ? tape::compression::zlib
                             : tape::compression::none);
        output.close();
        return 0;
    } catch (const std::exception & error) {
        std::cerr << "wisp-boot: " << error.what() << '\n';
        return 1;
    }
}
