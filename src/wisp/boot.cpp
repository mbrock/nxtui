// SPDX-License-Identifier: AGPL-3.0-or-later
// Build-time bootstrap for the executable host, not the portable library.
#include "wisp/load.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"

#include <fstream>
#include <iostream>

namespace {

constexpr unsigned char host_bytes[] = {
#embed "host.wisp"
};
const std::string_view host_source{
    reinterpret_cast<const char *>(host_bytes), sizeof(host_bytes)};

} // namespace

int main(int argc, char ** argv)
{
    using namespace wisp;
    try {
        if (argc != 2)
            throw std::runtime_error("expected output tape path");
        auto booted = image::fresh();
        for (auto source : {base_library(), host_source}) {
            {
                loader load{booted->storage, booted->machine, source};
                auto state = evaluation::runnable;
                while (state == evaluation::runnable)
                    state = load.advance(2048);
                if (state == evaluation::failed)
                    throw std::runtime_error(print(
                        booted->storage,
                        booted->storage.get<tag::run, field::err>(
                            load.run())));
            }
            booted->machine.collect();
        }
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(argv[1], std::ios::binary);
        tape::write(output, booted->machine);
        output.close();
        return 0;
    } catch (const std::exception & error) {
        std::cerr << "wisp-boot: " << error.what() << '\n';
        return 1;
    }
}
