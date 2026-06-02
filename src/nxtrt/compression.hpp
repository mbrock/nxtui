#pragma once

#include "nxtrt/buffers.hpp"

#if defined(NXTRT_HAVE_BROTLI)
#include <brotli/decode.h>
#endif
#if defined(NXTRT_HAVE_ZSTD)
#include <zstd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <zlib.h>

namespace nxtrt {

struct compression_error : runtime_error
{
    using runtime_error::runtime_error;
};

enum class zlib_format
{
    zlib,
    gzip,
    raw_deflate,
    gzip_or_zlib,
};

/// Streaming inflater reader backed by zlib or zlib-ng's zlib-compatible API.
///
/// This is a `bytefeed` adapter: compressed bytes are pulled from the wrapped
/// reader, inflated incrementally, and exposed through the ordinary buffered
/// reader hot path. The implementation follows the same Zig-inspired rule as the
/// other readers here: if a destination writer has no immediate capacity, the
/// inflater parks output in its own reader buffer instead of allocating temporary
/// storage.
class zlib_reader final : public bytefeed
{
public:
    explicit zlib_reader(
        bytefeed & reader,
        zlib_format format,
        std::size_t buffer_size = 4096);

    explicit zlib_reader(
        bytefeed & reader,
        zlib_format format,
        std::span<std::byte> buffer);

    ~zlib_reader() override;

private:
    static int window_bits(zlib_format format);

    void init(zlib_format format);

    std::string zlib_message(std::string_view fallback) const;

    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override;

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit);

    std::span<std::byte> output_capacity(
        bytesink & writer,
        std::size_t limit,
        bool & into_reader);

    task<void> refill_input();

    bytefeed * reader_;
    z_stream stream_{};
    std::span<const std::byte> input_;
    bool initialized_ = false;
    bool done_ = false;
};

inline zlib_reader gzip_reader(
    bytefeed & reader,
    std::size_t buffer_size = 4096)
{
    return zlib_reader{reader, zlib_format::gzip, buffer_size};
}

inline zlib_reader gzip_reader(
    bytefeed & reader,
    std::span<std::byte> buffer)
{
    return zlib_reader{reader, zlib_format::gzip, buffer};
}

inline zlib_reader deflate_reader(
    bytefeed & reader,
    std::size_t buffer_size = 4096)
{
    return zlib_reader{reader, zlib_format::zlib, buffer_size};
}

inline zlib_reader deflate_reader(
    bytefeed & reader,
    std::span<std::byte> buffer)
{
    return zlib_reader{reader, zlib_format::zlib, buffer};
}

#if defined(NXTRT_HAVE_ZSTD)

class zstd_reader final : public bytefeed
{
public:
    explicit zstd_reader(bytefeed & reader, std::size_t buffer_size = 4096);

    explicit zstd_reader(bytefeed & reader, std::span<std::byte> buffer);

    ~zstd_reader() override;

private:
    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override;

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit);

    std::span<std::byte> output_capacity(
        bytesink & writer,
        std::size_t limit,
        bool & into_reader);

    task<void> refill_input();

    bytefeed * reader_;
    ZSTD_DStream * stream_;
    std::span<const std::byte> input_span_;
    ZSTD_inBuffer input_{};
    bool done_ = false;
};

inline zstd_reader zstd_reader_for(
    bytefeed & reader,
    std::size_t buffer_size = 4096)
{
    return zstd_reader{reader, buffer_size};
}

inline zstd_reader zstd_reader_for(
    bytefeed & reader,
    std::span<std::byte> buffer)
{
    return zstd_reader{reader, buffer};
}

#endif

#if defined(NXTRT_HAVE_BROTLI)

class brotli_reader final : public bytefeed
{
public:
    explicit brotli_reader(bytefeed & reader, std::size_t buffer_size = 4096);

    explicit brotli_reader(bytefeed & reader, std::span<std::byte> buffer);

    ~brotli_reader() override;

private:
    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override;

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit);

    std::span<std::byte> output_capacity(
        bytesink & writer,
        std::size_t limit,
        bool & into_reader);

    task<void> refill_input();

    bytefeed * reader_;
    BrotliDecoderState * state_;
    std::span<const std::byte> input_;
    const std::uint8_t * next_in_ = nullptr;
    std::size_t available_in_ = 0;
    bool done_ = false;
};

inline brotli_reader brotli_reader_for(
    bytefeed & reader,
    std::size_t buffer_size = 4096)
{
    return brotli_reader{reader, buffer_size};
}

inline brotli_reader brotli_reader_for(
    bytefeed & reader,
    std::span<std::byte> buffer)
{
    return brotli_reader{reader, buffer};
}

#endif

} // namespace nxtrt
