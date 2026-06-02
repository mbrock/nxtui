#include "nxtrt/crypto.hpp"

namespace nxtrt {
namespace {

template<typename State>
std::size_t update_hash_state(
    State & state,
    bytesink::value_chunk_view chunks,
    std::size_t splat)
{
    auto accepted = std::size_t{0};
    if (chunks.empty())
        return 0;

    auto spans = chunks.chunks();
    for (auto chunk : spans.first(spans.size() - 1)) {
        state.update(chunk);
        accepted += chunk.size();
    }

    auto last = spans.back();
    for (auto i = std::size_t{0}; i < splat; i++) {
        state.update(last);
        accepted += last.size();
    }

    return accepted;
}

} // namespace

sha256_sink::sha256_sink(std::size_t buffer_size)
    : bytesink(buffer_size)
{}

sha256_sink::sha256_sink(std::span<std::byte> buffer)
    : bytesink(buffer)
{}

std::array<std::byte, nxt::crypto::sha256_len>
sha256_sink::finalize() const
{
    return state_.finalize();
}

hope<std::size_t>
sha256_sink::drain_more(value_chunk_view chunks, std::size_t splat)
{
    return hope<std::size_t>::ready(
        update_hash_state(state_, chunks, splat));
}

sha1_sink::sha1_sink(std::size_t buffer_size)
    : bytesink(buffer_size)
{}

sha1_sink::sha1_sink(std::span<std::byte> buffer)
    : bytesink(buffer)
{}

std::array<std::byte, nxt::crypto::sha1_len>
sha1_sink::finalize() const
{
    return state_.finalize();
}

hope<std::size_t>
sha1_sink::drain_more(value_chunk_view chunks, std::size_t splat)
{
    return hope<std::size_t>::ready(
        update_hash_state(state_, chunks, splat));
}

hmac_sha256_sink::hmac_sha256_sink(
    std::span<const std::byte> key,
    std::size_t buffer_size)
    : bytesink(buffer_size)
    , state_(key)
{}

hmac_sha256_sink::hmac_sha256_sink(
    std::span<const std::byte> key,
    std::span<std::byte> buffer)
    : bytesink(buffer)
    , state_(key)
{}

std::array<std::byte, nxt::crypto::sha256_len>
hmac_sha256_sink::finalize() const
{
    return state_.finalize();
}

hope<std::size_t>
hmac_sha256_sink::drain_more(value_chunk_view chunks, std::size_t splat)
{
    return hope<std::size_t>::ready(
        update_hash_state(state_, chunks, splat));
}

} // namespace nxtrt
