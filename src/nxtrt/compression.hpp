#pragma once

#include "nxtrt/buffers.hpp"

#if defined(NXTRT_HAVE_BROTLI)
#include <brotli/decode.h>
#endif
#if defined(NXTRT_HAVE_ZSTD)
#include <zstd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
/// This is a `byte_reader` adapter: compressed bytes are pulled from the wrapped
/// reader, inflated incrementally, and exposed through the ordinary buffered
/// reader hot path. The implementation follows the same Zig-inspired rule as the
/// other readers here: if a destination writer has no immediate capacity, the
/// inflater parks output in its own reader buffer instead of allocating temporary
/// storage.
class zlib_reader final : public byte_reader
{
public:
    explicit zlib_reader(
        byte_reader & reader,
        zlib_format format,
        std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , reader_(&reader)
    {
        init(format);
    }

    explicit zlib_reader(
        byte_reader & reader,
        zlib_format format,
        std::span<std::byte> buffer)
        : byte_reader(buffer)
        , reader_(&reader)
    {
        init(format);
    }

    ~zlib_reader() override
    {
        if (initialized_)
            ::inflateEnd(&stream_);
    }

private:
    static int window_bits(zlib_format format)
    {
        switch (format) {
        case zlib_format::zlib:
            return MAX_WBITS;
        case zlib_format::gzip:
            return MAX_WBITS + 16;
        case zlib_format::raw_deflate:
            return -MAX_WBITS;
        case zlib_format::gzip_or_zlib:
            return MAX_WBITS + 32;
        }
        return MAX_WBITS;
    }

    void init(zlib_format format)
    {
        auto rc = ::inflateInit2(&stream_, window_bits(format));
        if (rc != Z_OK)
            throw compression_error{zlib_message("zlib inflate init failed")};
        initialized_ = true;
    }

    std::string zlib_message(std::string_view fallback) const
    {
        if (stream_.msg != nullptr)
            return stream_.msg;
        return std::string{fallback};
    }

    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});
        if (done_)
            return hope<read_result>::ready(read_result{.eof = true});
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        auto into_reader = false;
        auto out = output_capacity(writer, limit, into_reader);

        while (true) {
            if (out.empty())
                co_return read_result{};

            if (stream_.avail_in == 0)
                co_await refill_input();

            stream_.next_out = reinterpret_cast<Bytef *>(out.data());
            stream_.avail_out = static_cast<uInt>(std::min<std::size_t>(
                out.size(), std::numeric_limits<uInt>::max()));

            auto before = stream_.avail_out;
            auto rc = ::inflate(&stream_, Z_NO_FLUSH);
            auto produced = static_cast<std::size_t>(before - stream_.avail_out);

            if (rc == Z_STREAM_END)
                done_ = true;
            else if (rc != Z_OK && rc != Z_BUF_ERROR)
                throw compression_error{zlib_message("zlib inflate failed")};

            if (produced != 0) {
                if (into_reader) {
                    advance(produced);
                    co_return read_result{};
                }

                writer.advance_constructed(produced);
                co_return read_result{.bytes = produced};
            }

            if (done_)
                co_return read_result{.eof = true};

            if (rc == Z_BUF_ERROR && stream_.avail_in != 0)
                throw compression_error{"zlib inflate made no progress"};
        }
    }

    std::span<std::byte> output_capacity(
        byte_writer & writer,
        std::size_t limit,
        bool & into_reader)
    {
        auto out = writer.unused_capacity();
        if (out.empty()) {
            into_reader = true;
            rebase(1);
            out = unused_capacity();
        }

        return out.first(std::min(limit, out.size()));
    }

    task<void> refill_input()
    {
        while (stream_.avail_in == 0) {
            auto limit = static_cast<std::size_t>(
                std::numeric_limits<uInt>::max());
            auto chunk = co_await reader_->take_some(limit);
            if (!chunk)
                throw compression_error{"unexpected end of compressed stream"};
            if (chunk->empty())
                continue;

            input_ = *chunk;
            stream_.next_in = reinterpret_cast<Bytef *>(
                const_cast<std::byte *>(input_.data()));
            stream_.avail_in = static_cast<uInt>(input_.size());
        }
    }

    byte_reader * reader_;
    z_stream stream_{};
    std::span<const std::byte> input_;
    bool initialized_ = false;
    bool done_ = false;
};

inline zlib_reader gzip_reader(
    byte_reader & reader,
    std::size_t buffer_size = 4096)
{
    return zlib_reader{reader, zlib_format::gzip, buffer_size};
}

inline zlib_reader gzip_reader(
    byte_reader & reader,
    std::span<std::byte> buffer)
{
    return zlib_reader{reader, zlib_format::gzip, buffer};
}

inline zlib_reader deflate_reader(
    byte_reader & reader,
    std::size_t buffer_size = 4096)
{
    return zlib_reader{reader, zlib_format::zlib, buffer_size};
}

inline zlib_reader deflate_reader(
    byte_reader & reader,
    std::span<std::byte> buffer)
{
    return zlib_reader{reader, zlib_format::zlib, buffer};
}

#if defined(NXTRT_HAVE_ZSTD)

class zstd_reader final : public byte_reader
{
public:
    explicit zstd_reader(byte_reader & reader, std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , reader_(&reader)
        , stream_(ZSTD_createDStream())
    {
        if (stream_ == nullptr)
            throw compression_error{"zstd decompressor init failed"};
    }

    explicit zstd_reader(byte_reader & reader, std::span<std::byte> buffer)
        : byte_reader(buffer)
        , reader_(&reader)
        , stream_(ZSTD_createDStream())
    {
        if (stream_ == nullptr)
            throw compression_error{"zstd decompressor init failed"};
    }

    ~zstd_reader() override
    {
        ZSTD_freeDStream(stream_);
    }

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});
        if (done_)
            return hope<read_result>::ready(read_result{.eof = true});
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        auto into_reader = false;
        auto out = output_capacity(writer, limit, into_reader);

        while (true) {
            if (out.empty())
                co_return read_result{};

            if (input_.pos == input_.size)
                co_await refill_input();

            auto output = ZSTD_outBuffer{
                .dst = out.data(),
                .size = std::min(out.size(), std::numeric_limits<std::size_t>::max()),
                .pos = 0,
            };
            auto rc = ZSTD_decompressStream(stream_, &output, &input_);
            if (ZSTD_isError(rc))
                throw compression_error{ZSTD_getErrorName(rc)};
            if (rc == 0)
                done_ = true;

            if (output.pos != 0) {
                if (into_reader) {
                    advance(output.pos);
                    co_return read_result{};
                }

                writer.advance_constructed(output.pos);
                co_return read_result{.bytes = output.pos};
            }

            if (done_)
                co_return read_result{.eof = true};
        }
    }

    std::span<std::byte> output_capacity(
        byte_writer & writer,
        std::size_t limit,
        bool & into_reader)
    {
        auto out = writer.unused_capacity();
        if (out.empty()) {
            into_reader = true;
            rebase(1);
            out = unused_capacity();
        }

        return out.first(std::min(limit, out.size()));
    }

    task<void> refill_input()
    {
        while (input_.pos == input_.size) {
            auto chunk = co_await reader_->take_some();
            if (!chunk)
                throw compression_error{"unexpected end of zstd stream"};
            if (chunk->empty())
                continue;

            input_span_ = *chunk;
            input_ = ZSTD_inBuffer{
                .src = input_span_.data(),
                .size = input_span_.size(),
                .pos = 0,
            };
        }
    }

    byte_reader * reader_;
    ZSTD_DStream * stream_;
    std::span<const std::byte> input_span_;
    ZSTD_inBuffer input_{};
    bool done_ = false;
};

inline zstd_reader zstd_reader_for(
    byte_reader & reader,
    std::size_t buffer_size = 4096)
{
    return zstd_reader{reader, buffer_size};
}

inline zstd_reader zstd_reader_for(
    byte_reader & reader,
    std::span<std::byte> buffer)
{
    return zstd_reader{reader, buffer};
}

#endif

#if defined(NXTRT_HAVE_BROTLI)

class brotli_reader final : public byte_reader
{
public:
    explicit brotli_reader(byte_reader & reader, std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , reader_(&reader)
        , state_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr))
    {
        if (state_ == nullptr)
            throw compression_error{"brotli decompressor init failed"};
    }

    explicit brotli_reader(byte_reader & reader, std::span<std::byte> buffer)
        : byte_reader(buffer)
        , reader_(&reader)
        , state_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr))
    {
        if (state_ == nullptr)
            throw compression_error{"brotli decompressor init failed"};
    }

    ~brotli_reader() override
    {
        BrotliDecoderDestroyInstance(state_);
    }

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});
        if (done_)
            return hope<read_result>::ready(read_result{.eof = true});
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        auto into_reader = false;
        auto out = output_capacity(writer, limit, into_reader);

        while (true) {
            if (out.empty())
                co_return read_result{};

            if (available_in_ == 0)
                co_await refill_input();

            auto next_out = reinterpret_cast<std::uint8_t *>(out.data());
            auto available_out = out.size();
            auto result = BrotliDecoderDecompressStream(
                state_,
                &available_in_,
                &next_in_,
                &available_out,
                &next_out,
                nullptr);
            auto produced = out.size() - available_out;

            if (result == BROTLI_DECODER_RESULT_ERROR) {
                auto code = BrotliDecoderGetErrorCode(state_);
                throw compression_error{
                    BrotliDecoderErrorString(code)};
            }
            if (result == BROTLI_DECODER_RESULT_SUCCESS)
                done_ = true;

            if (produced != 0) {
                if (into_reader) {
                    advance(produced);
                    co_return read_result{};
                }

                writer.advance_constructed(produced);
                co_return read_result{.bytes = produced};
            }

            if (done_)
                co_return read_result{.eof = true};
        }
    }

    std::span<std::byte> output_capacity(
        byte_writer & writer,
        std::size_t limit,
        bool & into_reader)
    {
        auto out = writer.unused_capacity();
        if (out.empty()) {
            into_reader = true;
            rebase(1);
            out = unused_capacity();
        }

        return out.first(std::min(limit, out.size()));
    }

    task<void> refill_input()
    {
        while (available_in_ == 0) {
            auto chunk = co_await reader_->take_some();
            if (!chunk)
                throw compression_error{"unexpected end of brotli stream"};
            if (chunk->empty())
                continue;

            input_ = *chunk;
            next_in_ =
                reinterpret_cast<const std::uint8_t *>(input_.data());
            available_in_ = input_.size();
        }
    }

    byte_reader * reader_;
    BrotliDecoderState * state_;
    std::span<const std::byte> input_;
    const std::uint8_t * next_in_ = nullptr;
    std::size_t available_in_ = 0;
    bool done_ = false;
};

inline brotli_reader brotli_reader_for(
    byte_reader & reader,
    std::size_t buffer_size = 4096)
{
    return brotli_reader{reader, buffer_size};
}

inline brotli_reader brotli_reader_for(
    byte_reader & reader,
    std::span<std::byte> buffer)
{
    return brotli_reader{reader, buffer};
}

#endif

} // namespace nxtrt
