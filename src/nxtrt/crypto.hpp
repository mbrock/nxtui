#pragma once

#include "nxtrt/buffers.hpp"

#include <nxt/crypto.hpp>

#include <cstddef>

namespace nxtrt {

class sha256_sink final : public bytesink
{
public:
    explicit sha256_sink(std::size_t buffer_size = 0)
        : bytesink(buffer_size)
    {}

    explicit sha256_sink(std::span<std::byte> buffer)
        : bytesink(buffer)
    {}

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha256_len>
    finalize() const
    {
        return state_.finalize();
    }

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        auto accepted = std::size_t{0};
        if (chunks.empty())
            return hope<std::size_t>::ready(0);

        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            state_.update(chunk);
            accepted += chunk.size();
        }

        auto last = spans.back();
        for (auto i = std::size_t{0}; i < splat; i++) {
            state_.update(last);
            accepted += last.size();
        }

        return hope<std::size_t>::ready(accepted);
    }

    nxt::crypto::sha256_state state_;
};

class sha1_sink final : public bytesink
{
public:
    explicit sha1_sink(std::size_t buffer_size = 0)
        : bytesink(buffer_size)
    {}

    explicit sha1_sink(std::span<std::byte> buffer)
        : bytesink(buffer)
    {}

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha1_len>
    finalize() const
    {
        return state_.finalize();
    }

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        auto accepted = std::size_t{0};
        if (chunks.empty())
            return hope<std::size_t>::ready(0);

        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            state_.update(chunk);
            accepted += chunk.size();
        }

        auto last = spans.back();
        for (auto i = std::size_t{0}; i < splat; i++) {
            state_.update(last);
            accepted += last.size();
        }

        return hope<std::size_t>::ready(accepted);
    }

    nxt::crypto::sha1_state state_;
};

class hmac_sha256_sink final : public bytesink
{
public:
    explicit hmac_sha256_sink(
        std::span<const std::byte> key,
        std::size_t buffer_size = 0)
        : bytesink(buffer_size)
        , state_(key)
    {}

    hmac_sha256_sink(
        std::span<const std::byte> key,
        std::span<std::byte> buffer)
        : bytesink(buffer)
        , state_(key)
    {}

    [[nodiscard]] std::array<std::byte, nxt::crypto::sha256_len>
    finalize() const
    {
        return state_.finalize();
    }

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        auto accepted = std::size_t{0};
        if (chunks.empty())
            return hope<std::size_t>::ready(0);

        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            state_.update(chunk);
            accepted += chunk.size();
        }

        auto last = spans.back();
        for (auto i = std::size_t{0}; i < splat; i++) {
            state_.update(last);
            accepted += last.size();
        }

        return hope<std::size_t>::ready(accepted);
    }

    nxt::crypto::hmac_sha256_state state_;
};

} // namespace nxtrt
