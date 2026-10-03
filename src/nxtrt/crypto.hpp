#pragma once

#include "nxtrt/buffers.hpp"

#include <nxt/crypto.hpp>

#include <cstddef>

namespace nxtrt {

/// A byte sink that hashes everything written to it with SHA-256.
///
/// Writes never suspend. `finalize` hashes only bytes the sink has
/// drained, so flush first when the sink has a buffer (BUFFER_SIZE > 0 or
/// a borrowed BUFFER, which must outlive the sink). `finalize` does not
/// reset or consume the state: more writes continue the same message.
class sha256_sink final : public bytesink
{
public:
    explicit sha256_sink(std::size_t buffer_size = 0);

    explicit sha256_sink(std::span<std::byte> buffer);

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha256_len>
    finalize() const;

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override;

    nxt::crypto::sha256_state state_;
};

/// A byte sink that hashes everything written to it with SHA-1; the same
/// contract as `sha256_sink`.
class sha1_sink final : public bytesink
{
public:
    explicit sha1_sink(std::size_t buffer_size = 0);

    explicit sha1_sink(std::span<std::byte> buffer);

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha1_len>
    finalize() const;

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override;

    nxt::crypto::sha1_state state_;
};

/// A byte sink that computes HMAC-SHA-256 under KEY over everything written
/// to it; the same contract as `sha256_sink`. KEY is used during
/// construction only.
class hmac_sha256_sink final : public bytesink
{
public:
    explicit hmac_sha256_sink(
        std::span<const std::byte> key,
        std::size_t buffer_size = 0);

    hmac_sha256_sink(
        std::span<const std::byte> key,
        std::span<std::byte> buffer);

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha256_len>
    finalize() const;

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override;

    nxt::crypto::hmac_sha256_state state_;
};

} // namespace nxtrt
