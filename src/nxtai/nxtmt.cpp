#include <nxtrt/app.hpp>
#include <nxtrt/mtproto.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

std::string hex(std::span<const std::byte> bytes)
{
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto out = std::string{};
    out.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        auto value = std::to_integer<unsigned>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0f]);
    }
    return out;
}

struct options
{
    std::string host = "149.154.167.50";
    std::string service = "443";
};

options parse_args(int argc, char ** argv)
{
    auto out = options{};
    for (auto i = 1; i < argc; i++) {
        auto arg = std::string_view{argv[i]};
        if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            out.host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            out.service = argv[++i];
        } else {
            throw nxtrt::runtime_error{
                "usage: nxtmt [--host HOST] [--port PORT]"};
        }
    }
    return out;
}

nxtrt::task<int> run_nxtmt(options opts)
{
    auto session = co_await nxtrt::mtproto::connect_and_auth(
        std::move(opts.host),
        std::move(opts.service));

    std::cout << "MTProto auth ok\n";
    std::cout << "auth_key_id=" << hex(session.key.id) << '\n';
    std::cout << "server_salt=" << session.server_salt << '\n';
    std::cout << "session_id=" << session.session_id << '\n';
    std::cout << "time_offset=" << session.time_offset << '\n';
    co_return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        auto rt = nxtrt::runtime{};
        return rt.run(run_nxtmt(parse_args(argc, argv)));
    } catch (const std::exception & error) {
        std::cerr << "nxtmt: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
