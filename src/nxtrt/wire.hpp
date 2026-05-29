#pragma once

#include "nxtrt/bell.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace nxtrt {

/// Bounded single-receiver typed wire.
///
/// Values are buffered up to `capacity`. Receives use a `hope` fast path when a
/// value is already queued and otherwise block by awaiting bell readiness
/// through the active wand. This is intentionally not a broadcast channel.
template<typename T>
class wire
{
public:
    using value_type = std::remove_cv_t<T>;

    explicit wire(std::size_t capacity = 64)
        : capacity_(capacity == 0 ? 1 : capacity)
    {}

    wire(const wire &) = delete;
    wire & operator=(const wire &) = delete;
    wire(wire &&) = delete;
    wire & operator=(wire &&) = delete;

    ~wire()
    {
        cancel();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_.get_token();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_.stop_requested();
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

    [[nodiscard]] std::optional<value_type> try_next()
    {
        if (values_.empty())
            return std::nullopt;

        auto was_full = full();
        auto value = std::move(values_.front());
        values_.pop_front();
        if (values_.empty())
            data_.reset();
        if (was_full)
            space_.ring();
        return value;
    }

    [[nodiscard]] bool try_send(T value)
    {
        if (closed_ || stop_.stop_requested() || full())
            return false;

        push_ready(std::move(value));
        return true;
    }

    [[nodiscard]] hope<bool> send(T value)
    {
        if (closed_ || stop_.stop_requested())
            return hope<bool>::ready(false);
        if (!full()) {
            push_ready(std::move(value));
            return hope<bool>::ready(true);
        }
        return send_slow(std::move(value));
    }

    [[nodiscard]] hope<std::optional<value_type>> next()
    {
        if (auto value = try_next())
            return hope<std::optional<value_type>>::ready(std::move(value));
        if (closed_ || stop_.stop_requested())
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

    void cancel()
    {
        stop_.request_stop();
        close();
    }

private:
    task<bool> send_slow(T value)
    {
        while (true) {
            if (closed_ || stop_.stop_requested())
                co_return false;
            if (!full()) {
                push_ready(std::move(value));
                co_return true;
            }

            space_.reset();
            if (closed_ || stop_.stop_requested())
                co_return false;
            co_await space_;
        }
    }

    task<std::optional<value_type>> next_slow()
    {
        while (true) {
            if (auto value = try_next())
                co_return std::move(value);
            if (closed_ || stop_.stop_requested())
                co_return std::nullopt;

            data_.reset();
            if (auto value = try_next())
                co_return std::move(value);
            if (closed_ || stop_.stop_requested())
                co_return std::nullopt;
            co_await data_;
        }
    }

    void push_ready(T value)
    {
        values_.push_back(std::move(value));
        data_.ring();
    }

    std::deque<value_type> values_;
    std::size_t capacity_ = 64;
    bell data_;
    bell space_;
    std::stop_source stop_;
    bool closed_ = false;
};

} // namespace nxtrt
