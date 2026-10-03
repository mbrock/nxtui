#include "nxtrt/compression.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace nxtrt {

zlib_reader::zlib_reader(
    bytefeed & reader,
    zlib_format format,
    std::size_t buffer_size)
    : bytefeed(buffer_size)
    , reader_(&reader)
{
    init(format);
}

zlib_reader::zlib_reader(
    bytefeed & reader,
    zlib_format format,
    std::span<std::byte> buffer)
    : bytefeed(buffer)
    , reader_(&reader)
{
    init(format);
}

zlib_reader::~zlib_reader()
{
    if (initialized_)
        ::inflateEnd(&stream_);
}

int zlib_reader::window_bits(zlib_format format)
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

void zlib_reader::init(zlib_format format)
{
    auto rc = ::inflateInit2(&stream_, window_bits(format));
    if (rc != Z_OK)
        throw compression_error{zlib_message("zlib inflate init failed")};
    initialized_ = true;
}

std::string zlib_reader::zlib_message(std::string_view fallback) const
{
    if (stream_.msg != nullptr)
        return stream_.msg;
    return std::string{fallback};
}

hope<fare_t> zlib_reader::stream_more(bytesink & writer, std::size_t limit)
{
    if (limit == 0)
        return hope<fare_t>::ready(0);
    if (done_)
        return hope<fare_t>::ready(eof);
    return stream_more_task(writer, limit);
}

task<fare_t> zlib_reader::stream_more_task(
    bytesink & writer,
    std::size_t limit)
{
    auto into_reader = false;
    auto out = output_capacity(writer, limit, into_reader);

    while (true) {
        if (out.empty())
            co_return 0;

        if (stream_.avail_in == 0)
            co_await refill_input();

        stream_.next_out = reinterpret_cast<Bytef *>(out.data());
        stream_.avail_out = static_cast<uInt>(std::min<std::size_t>(
            out.size(), std::numeric_limits<uInt>::max()));

        auto before = stream_.avail_out;
        auto input_before = stream_.avail_in;
        auto rc = ::inflate(&stream_, Z_NO_FLUSH);
        auto produced = static_cast<std::size_t>(before - stream_.avail_out);
        auto consumed =
            static_cast<std::size_t>(input_before - stream_.avail_in);
        if (consumed != 0) {
            auto discard = reader_->discard(consumed);
            if (!discard.is_ready())
                co_await std::move(discard);
            if (stream_.avail_in != 0) {
                reader_->rebase(1);
                input_ = reader_->buffered_span();
                stream_.next_in = reinterpret_cast<Bytef *>(
                    const_cast<std::byte *>(input_.data()));
            }
        }

        if (rc == Z_STREAM_END)
            done_ = true;
        else if (rc != Z_OK && rc != Z_BUF_ERROR)
            throw compression_error{zlib_message("zlib inflate failed")};

        if (produced != 0) {
            if (into_reader) {
                advance_constructed(produced);
                co_return 0;
            }

            writer.advance_constructed(produced);
            co_return produced;
        }

        if (done_)
            co_return eof;

        if (rc == Z_BUF_ERROR && stream_.avail_in != 0)
            throw compression_error{"zlib inflate made no progress"};
    }
}

std::span<std::byte> zlib_reader::output_capacity(
    bytesink & writer,
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

task<void> zlib_reader::refill_input()
{
    while (stream_.avail_in == 0) {
        try {
            co_await reader_->fill(1);
        } catch (const value_end_of_stream &) {
            throw compression_error{"unexpected end of compressed stream"};
        }
        reader_->rebase(1);
        input_ = reader_->buffered_span();
        stream_.next_in = reinterpret_cast<Bytef *>(
            const_cast<std::byte *>(input_.data()));
        stream_.avail_in = static_cast<uInt>(std::min<std::size_t>(
            input_.size(), std::numeric_limits<uInt>::max()));
    }
}

#if defined(NXTRT_HAVE_ZSTD)

zstd_reader::zstd_reader(bytefeed & reader, std::size_t buffer_size)
    : bytefeed(buffer_size)
    , reader_(&reader)
    , stream_(ZSTD_createDStream())
{
    if (stream_ == nullptr)
        throw compression_error{"zstd decompressor init failed"};
}

zstd_reader::zstd_reader(bytefeed & reader, std::span<std::byte> buffer)
    : bytefeed(buffer)
    , reader_(&reader)
    , stream_(ZSTD_createDStream())
{
    if (stream_ == nullptr)
        throw compression_error{"zstd decompressor init failed"};
}

zstd_reader::~zstd_reader()
{
    ZSTD_freeDStream(stream_);
}

hope<fare_t> zstd_reader::stream_more(bytesink & writer, std::size_t limit)
{
    if (limit == 0)
        return hope<fare_t>::ready(0);
    if (done_)
        return hope<fare_t>::ready(eof);
    return stream_more_task(writer, limit);
}

task<fare_t> zstd_reader::stream_more_task(
    bytesink & writer,
    std::size_t limit)
{
    auto into_reader = false;
    auto out = output_capacity(writer, limit, into_reader);

    while (true) {
        if (out.empty())
            co_return 0;

        if (input_.pos == input_.size)
            co_await refill_input();
        if (done_)
            co_return eof;

        auto output = ZSTD_outBuffer{
            .dst = out.data(),
            .size = std::min(out.size(), std::numeric_limits<std::size_t>::max()),
            .pos = 0,
        };
        auto before_input = input_.pos;
        auto rc = ZSTD_decompressStream(stream_, &output, &input_);
        auto consumed = input_.pos - before_input;
        if (consumed != 0) {
            auto discard = reader_->discard(consumed);
            if (!discard.is_ready())
                co_await std::move(discard);
            input_.pos = 0;
            input_.size = 0;
        }
        if (ZSTD_isError(rc))
            throw compression_error{ZSTD_getErrorName(rc)};
        if (rc == 0)
            frame_complete_ = true;

        if (output.pos != 0) {
            if (into_reader) {
                advance_constructed(output.pos);
                co_return 0;
            }

            writer.advance_constructed(output.pos);
            co_return output.pos;
        }

        if (done_)
            co_return eof;
    }
}

std::span<std::byte> zstd_reader::output_capacity(
    bytesink & writer,
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

task<void> zstd_reader::refill_input()
{
    while (input_.pos == input_.size) {
        auto minimum = frame_complete_ ? std::size_t{4} : std::size_t{1};
        try {
            co_await reader_->fill(minimum);
        } catch (const value_end_of_stream &) {
            if (frame_complete_) {
                done_ = true;
                co_return;
            }
            throw compression_error{"unexpected end of zstd stream"};
        }

        reader_->rebase(1);
        input_span_ = reader_->buffered_span();
        if (frame_complete_) {
            if (input_span_.size() < sizeof(std::uint32_t)) {
                done_ = true;
                co_return;
            }
            constexpr auto magic = std::array{
                std::byte{0x28},
                std::byte{0xb5},
                std::byte{0x2f},
                std::byte{0xfd},
            };
            auto skippable =
                (std::to_integer<unsigned>(input_span_[0]) & 0xf0) == 0x50
                && input_span_[1] == std::byte{0x2a}
                && input_span_[2] == std::byte{0x4d}
                && input_span_[3] == std::byte{0x18};
            if (!skippable
                && !std::equal(
                    magic.begin(), magic.end(), input_span_.begin())) {
                done_ = true;
                co_return;
            }
        }
        input_ = ZSTD_inBuffer{
            .src = input_span_.data(),
            .size = input_span_.size(),
            .pos = 0,
        };
        frame_complete_ = false;
    }
}

#endif

#if defined(NXTRT_HAVE_BROTLI)

brotli_reader::brotli_reader(bytefeed & reader, std::size_t buffer_size)
    : bytefeed(buffer_size)
    , reader_(&reader)
    , state_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr))
{
    if (state_ == nullptr)
        throw compression_error{"brotli decompressor init failed"};
}

brotli_reader::brotli_reader(bytefeed & reader, std::span<std::byte> buffer)
    : bytefeed(buffer)
    , reader_(&reader)
    , state_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr))
{
    if (state_ == nullptr)
        throw compression_error{"brotli decompressor init failed"};
}

brotli_reader::~brotli_reader()
{
    BrotliDecoderDestroyInstance(state_);
}

hope<fare_t> brotli_reader::stream_more(bytesink & writer, std::size_t limit)
{
    if (limit == 0)
        return hope<fare_t>::ready(0);
    if (done_)
        return hope<fare_t>::ready(eof);
    return stream_more_task(writer, limit);
}

task<fare_t> brotli_reader::stream_more_task(
    bytesink & writer,
    std::size_t limit)
{
    auto into_reader = false;
    auto out = output_capacity(writer, limit, into_reader);

    while (true) {
        if (out.empty())
            co_return 0;

        if (available_in_ == 0)
            co_await refill_input();

        auto next_out = reinterpret_cast<std::uint8_t *>(out.data());
        auto available_out = out.size();
        auto input_before = available_in_;
        auto result = BrotliDecoderDecompressStream(
            state_,
            &available_in_,
            &next_in_,
            &available_out,
            &next_out,
            nullptr);
        auto produced = out.size() - available_out;
        auto consumed = input_before - available_in_;
        if (consumed != 0) {
            auto discard = reader_->discard(consumed);
            if (!discard.is_ready())
                co_await std::move(discard);
            if (available_in_ != 0) {
                reader_->rebase(1);
                input_ = reader_->buffered_span();
                next_in_ =
                    reinterpret_cast<const std::uint8_t *>(input_.data());
            }
        }

        if (result == BROTLI_DECODER_RESULT_ERROR) {
            auto code = BrotliDecoderGetErrorCode(state_);
            throw compression_error{
                BrotliDecoderErrorString(code)};
        }
        if (result == BROTLI_DECODER_RESULT_SUCCESS)
            done_ = true;

        if (produced != 0) {
            if (into_reader) {
                advance_constructed(produced);
                co_return 0;
            }

            writer.advance_constructed(produced);
            co_return produced;
        }

        if (done_)
            co_return eof;
    }
}

std::span<std::byte> brotli_reader::output_capacity(
    bytesink & writer,
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

task<void> brotli_reader::refill_input()
{
    while (available_in_ == 0) {
        try {
            co_await reader_->fill(1);
        } catch (const value_end_of_stream &) {
            throw compression_error{"unexpected end of brotli stream"};
        }

        reader_->rebase(1);
        input_ = reader_->buffered_span();
        next_in_ = reinterpret_cast<const std::uint8_t *>(input_.data());
        available_in_ = input_.size();
    }
}

#endif

} // namespace nxtrt
