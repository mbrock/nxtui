#pragma once

#include "nxt/rt/task.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>

namespace nxt::rt {

class ema_rate
{
public:
    explicit ema_rate(
        std::chrono::nanoseconds half_life = std::chrono::milliseconds{500})
        : half_life_(std::max(half_life, std::chrono::nanoseconds{1}))
    {}

    [[nodiscard]] double value() const noexcept
    {
        return value_;
    }

    template<typename Rep, typename Period>
    double sample(
        std::size_t delta,
        std::chrono::duration<Rep, Period> elapsed)
    {
        auto seconds = std::chrono::duration<double>(elapsed).count();
        if (seconds <= 0.0)
            return value_;

        auto instant = static_cast<double>(delta) / seconds;
        if (!initialized_) {
            value_ = instant;
            initialized_ = true;
            return value_;
        }

        auto half_life_seconds =
            std::chrono::duration<double>(half_life_).count();
        auto alpha = 1.0 - std::exp(-std::log(2.0) * seconds
                                    / half_life_seconds);
        value_ += alpha * (instant - value_);
        return value_;
    }

private:
    std::chrono::nanoseconds half_life_;
    double value_ = 0.0;
    bool initialized_ = false;
};

template<typename ReadTotal, typename PublishRate, typename Rep, typename Period>
task<void> sample_ema_rate(
    std::chrono::duration<Rep, Period> interval,
    ReadTotal read_total,
    PublishRate publish_rate,
    std::chrono::nanoseconds half_life = std::chrono::milliseconds{500})
{
    auto filter = ema_rate{half_life};
    auto last_time = std::chrono::steady_clock::now();
    auto last_total = std::invoke(read_total);

    try {
        while (!stop_requested()) {
            co_await op::timeout::after(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    interval));
            if (stop_requested())
                break;

            auto now = std::chrono::steady_clock::now();
            auto total = std::invoke(read_total);
            auto delta = total >= last_total ? total - last_total : 0;
            auto rate = filter.sample(delta, now - last_time);
            std::invoke(publish_rate, rate);
            last_total = total;
            last_time = now;
        }
    } catch (const operation_cancelled &) {
    }
}

} // namespace nxt::rt
