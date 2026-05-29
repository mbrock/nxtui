#pragma once

#include "nxtrt/bell.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <type_traits>
#include <utility>

namespace nxtrt {

/// Bounded single-receiver typed wire.
///
/// Values are buffered up to `capacity`, clamped to at least one slot.
/// `flush()` waits until accepted values have been consumed. Receives use a
/// `hope` fast path when a value is already available and otherwise block by
/// awaiting bell readiness through the active wand. This is intentionally not a
/// broadcast channel.
template<typename T>
class wire
{
public:
    using value_type = std::remove_cv_t<T>;

    /// Write endpoint for a wire.
    ///
    /// This is a non-owning view; the parent `wire` owns the queue, bells, and
    /// closed state. It exists to make producer/consumer direction explicit at
    /// call sites without changing the wire's scoped lifetime model.
    class tx_side
    {
    public:
        explicit tx_side(wire & owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] bool closed() const noexcept
        {
            return owner_->closed();
        }

        [[nodiscard]] bool full() const noexcept
        {
            return owner_->full();
        }

        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return owner_->capacity();
        }

        [[nodiscard]] bool try_send(T value)
        {
            return owner_->try_send(std::move(value));
        }

        [[nodiscard]] hope<bool> send(T value)
        {
            return owner_->send(std::move(value));
        }

        [[nodiscard]] hope<void> flush()
        {
            return owner_->flush();
        }

        void close()
        {
            owner_->close();
        }

    private:
        wire * owner_;
    };

    /// Read endpoint for a wire.
    ///
    /// This is also a non-owning view.
    class rx_side
    {
    public:
        explicit rx_side(wire & owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] bool closed() const noexcept
        {
            return owner_->closed();
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return owner_->empty();
        }

        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return owner_->capacity();
        }

        [[nodiscard]] std::optional<value_type> try_next()
        {
            return owner_->try_next();
        }

        [[nodiscard]] hope<std::optional<value_type>> next()
        {
            return owner_->next();
        }

    private:
        wire * owner_;
    };

    explicit wire(std::size_t capacity = 64)
        : capacity_(capacity == 0 ? 1 : capacity)
    {}

    wire(const wire &) = delete;
    wire & operator=(const wire &) = delete;
    wire(wire &&) = delete;
    wire & operator=(wire &&) = delete;

    ~wire()
    {
        close();
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return values_.empty();
    }

    [[nodiscard]] bool full() const noexcept
    {
        return values_.size() >= capacity_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] tx_side tx() noexcept
    {
        return tx_side{*this};
    }

    [[nodiscard]] rx_side rx() noexcept
    {
        return rx_side{*this};
    }

    [[nodiscard]] std::optional<value_type> try_next()
    {
        if (values_.empty())
            return std::nullopt;

        auto value = std::move(values_.front());
        values_.pop_front();
        if (values_.empty())
            data_.reset();
        space_.ring();
        return value;
    }

    [[nodiscard]] bool try_send(T value)
    {
        if (!can_send_now())
            return false;

        push_ready(std::move(value));
        return true;
    }

    [[nodiscard]] hope<bool> send(T value)
    {
        if (closed_)
            return hope<bool>::ready(false);
        if (can_send_now()) {
            push_ready(std::move(value));
            return hope<bool>::ready(true);
        }
        return send_slow(std::move(value));
    }

    [[nodiscard]] hope<void> flush()
    {
        if (values_.empty() || closed_)
            return hope<void>::ready();
        return flush_slow();
    }

    [[nodiscard]] hope<std::optional<value_type>> next()
    {
        if (auto value = try_next())
            return hope<std::optional<value_type>>::ready(std::move(value));
        if (closed_)
            return hope<std::optional<value_type>>::ready(std::nullopt);
        return next_slow();
    }

    void close()
    {
        if (closed_)
            return;
        closed_ = true;
        data_.ring();
        space_.ring();
    }

private:
    task<bool> send_slow(T value)
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

    task<void> flush_slow()
    {
        while (!values_.empty() && !closed_) {
            space_.reset();
            if (values_.empty() || closed_)
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

    void push_ready(T value)
    {
        values_.push_back(std::move(value));
        data_.ring();
    }

    [[nodiscard]] bool can_send_now() const noexcept
    {
        if (closed_)
            return false;
        return !full();
    }

    std::deque<value_type> values_;
    std::size_t capacity_ = 64;
    bell data_;
    bell space_;
    bool closed_ = false;
};

} // namespace nxtrt
