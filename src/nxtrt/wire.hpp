#pragma once

#include "nxtrt/bell.hpp"
#include "nxtrt/value-buffers.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace nxtrt {

template<typename T>
class wire_tx;

/// Receive endpoint for a bounded single-receiver typed wire.
///
/// Values are buffered in caller-provided raw storage. Receives use a `hope`
/// fast path when a value is already available and otherwise block by awaiting
/// bell readiness through the active wand. This is intentionally not a
/// broadcast channel. With zero capacity the wire is a rendezvous: a send
/// completes only once the receiver has taken the value.
///
/// Receive with `next()` or `try_next()`; these return `std::nullopt` once the
/// wire is closed and empty, and ring the space bell that wakes a waiting
/// sender. The inherited @ref feed verbs also work and ring the same space
/// bell whenever they consume buffered values.
/// The storage must outlive the receiver; destruction closes the wire.
/// See RFC 0008 (@ref runtime_rfcs).
template<typename T>
class wire_rx : public feed<T>
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = typename feed<T>::storage_ref;

    explicit wire_rx(storage_ref storage)
        : feed<T>(storage)
    {
        this->observe_consumption(
            this, [](void * context, std::size_t) noexcept {
                static_cast<wire_rx *>(context)->space_.ring();
            });
    }

    template<std::size_t Extent>
    explicit wire_rx(std::span<value_type, Extent> storage)
        : wire_rx(storage_ref{std::span<value_type>{storage}})
    {}

    wire_rx(const wire_rx &) = delete;
    wire_rx & operator=(const wire_rx &) = delete;
    wire_rx(wire_rx &&) = delete;
    wire_rx & operator=(wire_rx &&) = delete;

    ~wire_rx()
    {
        close();
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return this->buffered_size() == 0 && !rendezvous_.has_value();
    }

    [[nodiscard]] bool full() const noexcept
    {
        if (capacity() == 0)
            return rendezvous_.has_value();
        return this->unused_capacity_size() == 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return this->storage_capacity();
    }

    [[nodiscard]] std::optional<value_type> try_next()
    {
        if (rendezvous_) {
            auto value = std::optional<value_type>{std::move(*rendezvous_)};
            rendezvous_.reset();
            data_.reset();
            space_.ring();
            return value;
        }

        auto value = this->take_buffered_for_derived();
        if (!value)
            return value;
        if (empty())
            data_.reset();
        return value;
    }

    /// The next value, waiting for one if necessary; `std::nullopt` once
    /// the wire is closed and empty.
    [[nodiscard]] hope<std::optional<value_type>> next()
    {
        if (auto value = try_next())
            return hope<std::optional<value_type>>::ready(std::move(value));
        if (closed_)
            return hope<std::optional<value_type>>::ready(std::nullopt);
        return next_slow();
    }

    /// Close the wire and wake both sides. Buffered values can still be
    /// received; sends fail from now on.
    void close()
    {
        if (closed_)
            return;
        closed_ = true;
        data_.ring();
        space_.ring();
    }

private:
    friend class wire_tx<T>;

    [[nodiscard]] bool can_send_now() const noexcept
    {
        if (closed_)
            return false;
        if (capacity() == 0)
            return false;
        return !full();
    }

    [[nodiscard]] bool try_send(value_type value)
    {
        if (!can_send_now())
            return false;

        push_ready(std::move(value));
        return true;
    }

    [[nodiscard]] hope<bool> send(value_type value)
    {
        if (closed_)
            return hope<bool>::ready(false);
        if (capacity() == 0)
            return send_rendezvous_slow(std::move(value));
        if (can_send_now()) {
            push_ready(std::move(value));
            return hope<bool>::ready(true);
        }
        return send_slow(std::move(value));
    }

    [[nodiscard]] hope<void> flush_sent()
    {
        if (empty() || closed_)
            return hope<void>::ready();
        return flush_slow();
    }

    task<bool> send_slow(value_type value)
    {
        while (true) {
            if (closed_)
                co_return false;
            if (can_send_now()) {
                push_ready(std::move(value));
                co_return true;
            }

            space_.reset();
            if (can_send_now()) {
                push_ready(std::move(value));
                co_return true;
            }
            if (closed_)
                co_return false;
            co_await space_;
        }
    }

    task<bool> send_rendezvous_slow(value_type value)
    {
        while (rendezvous_ && !closed_) {
            space_.reset();
            if (!rendezvous_ || closed_)
                break;
            co_await space_;
        }
        if (closed_)
            co_return false;

        rendezvous_.emplace(std::move(value));
        data_.ring();

        while (rendezvous_ && !closed_) {
            space_.reset();
            if (!rendezvous_ || closed_)
                break;
            co_await space_;
        }

        co_return !closed_;
    }

    task<void> flush_slow()
    {
        while (!empty() && !closed_) {
            space_.reset();
            if (empty() || closed_)
                co_return;
            co_await space_;
        }
    }

    task<std::optional<value_type>> next_slow()
    {
        while (true) {
            if (auto value = try_next())
                co_return std::move(value);
            if (closed_)
                co_return std::nullopt;

            data_.reset();
            if (auto value = try_next())
                co_return std::move(value);
            if (closed_)
                co_return std::nullopt;
            co_await data_;
        }
    }

    void push_ready(value_type value)
    {
        this->emplace(std::move(value));
        data_.ring();
    }

    task<std::optional<value_type>> next_value() override
    {
        co_return co_await next();
    }

    hope<fare_t>
    stream_more(sink<value_type> & output, std::size_t limit) override
    {
        if (capacity() == 0)
            return feed<T>::stream_more(output, limit);
        if (!empty() || limit == 0)
            return hope<fare_t>::ready(0);
        if (closed_)
            return hope<fare_t>::ready(eof);
        return wait_for_data();
    }

    task<fare_t> wait_for_data()
    {
        // send() already constructs values in this feed's ring. A refill
        // waits for that data; it must not consume and reconstruct it
        // through a second sink borrowing the same storage.
        while (empty() && !closed_) {
            data_.reset();
            co_await data_;
        }
        co_return empty() ? fare_t{eof} : fare_t{0};
    }

    bell data_;
    bell space_;
    std::optional<value_type> rendezvous_;
    bool closed_ = false;
};

/// Transmit endpoint for a bounded typed wire; a borrowed view of a
/// @ref wire_rx.
///
/// `send()` resolves to `true` once the value is in the wire (or, with zero
/// capacity, once the receiver took it), and `false` if the wire is closed.
/// `try_send()` never waits. As a @ref sink it has no buffer of its own:
/// `write()` sends each value and throws @ref nxtrt::value_buffer_error "value_buffer_error" when the
/// wire is closed before any value was accepted. `flush()` waits until the
/// receiver has emptied the wire or closed it. The receiver must outlive
/// the transmitter.
template<typename T>
class wire_tx : public sink<std::remove_cv_t<T>>
{
public:
    using value_type = std::remove_cv_t<T>;
    using base = sink<value_type>;
    using storage_ref = typename base::storage_ref;

    explicit wire_tx(wire_rx<value_type> & receiver) noexcept
        : base(storage_ref{})
        , receiver_(&receiver)
    {}

    using base::write;

    [[nodiscard]] bool closed() const noexcept
    {
        return receiver_->closed();
    }

    [[nodiscard]] bool full() const noexcept
    {
        return receiver_->full();
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return receiver_->capacity();
    }

    [[nodiscard]] bool try_send(value_type value)
    {
        return receiver_->try_send(std::move(value));
    }

    [[nodiscard]] hope<bool> send(value_type value)
    {
        return receiver_->send(std::move(value));
    }

    [[nodiscard]] hope<void> flush()
    {
        return receiver_->flush_sent();
    }

    void close()
    {
        receiver_->close();
    }

private:
    hope<std::size_t> drain_more(
        typename base::value_chunk_view values,
        std::size_t splat) override
    {
        return drain_more_task(values, splat);
    }

    task<std::size_t> drain_more_task(
        typename base::value_chunk_view values,
        std::size_t splat)
    {
        auto accepted = std::size_t{0};
        auto chunks = values.chunks();

        for (auto chunk : chunks.first(chunks.size() - 1)) {
            for (auto & value : chunk) {
                if (!co_await send(std::move(value)))
                    co_return accepted;
                ++accepted;
            }
        }

        if (chunks.empty())
            co_return accepted;

        auto last = chunks.back();
        if (splat <= 1) {
            for (auto & value : last) {
                if (!co_await send(std::move(value)))
                    co_return accepted;
                ++accepted;
            }
            co_return accepted;
        }

        if constexpr (!std::copy_constructible<value_type>) {
            throw value_buffer_error{"wire sink cannot splat move-only values"};
        } else {
            for (auto i = std::size_t{0}; i < splat; ++i) {
                for (auto & value : last) {
                    if (!co_await send(value_type{value}))
                        co_return accepted;
                    ++accepted;
                }
            }
            co_return accepted;
        }
    }

    wire_rx<value_type> * receiver_;
};

/// A @ref wire_rx and a @ref wire_tx bound to it, in one object.
///
/// The storage is borrowed and must outlive the wire. The members forward to
/// the two endpoints. See RFC 0008 (@ref runtime_rfcs).
template<typename T>
class wire
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;

    wire_rx<value_type> rx_endpoint;
    wire_tx<value_type> tx_endpoint;

    explicit wire(storage_ref storage)
        : rx_endpoint(storage)
        , tx_endpoint(rx_endpoint)
    {}

    template<std::size_t Extent>
    explicit wire(std::span<value_type, Extent> storage)
        : wire(storage_ref{std::span<value_type>{storage}})
    {}

    wire(const wire &) = delete;
    wire & operator=(const wire &) = delete;
    wire(wire &&) = delete;
    wire & operator=(wire &&) = delete;

    [[nodiscard]] wire_tx<value_type> & tx() noexcept
    {
        return tx_endpoint;
    }

    [[nodiscard]] wire_rx<value_type> & rx() noexcept
    {
        return rx_endpoint;
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return rx_endpoint.closed();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return rx_endpoint.empty();
    }

    [[nodiscard]] bool full() const noexcept
    {
        return rx_endpoint.full();
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return rx_endpoint.capacity();
    }

    [[nodiscard]] std::optional<value_type> try_next()
    {
        return rx_endpoint.try_next();
    }

    [[nodiscard]] hope<std::optional<value_type>> next()
    {
        return rx_endpoint.next();
    }

    [[nodiscard]] bool try_send(value_type value)
    {
        return tx_endpoint.try_send(std::move(value));
    }

    [[nodiscard]] hope<bool> send(value_type value)
    {
        return tx_endpoint.send(std::move(value));
    }

    [[nodiscard]] hope<void> flush()
    {
        return tx_endpoint.flush();
    }

    void close()
    {
        rx_endpoint.close();
    }
};

} // namespace nxtrt
