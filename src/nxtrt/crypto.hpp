#pragma once

#include "nxtrt/buffers.hpp"

#include <nxt/crypto.hpp>

#include <cstddef>

namespace nxtrt {

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
