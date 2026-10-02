#pragma once

// Borrowed, static, and owned firm bookkeeping storage.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/deed.hpp"
#include "nxtrt/task/frame_arena.hpp"

namespace nxtrt {

struct firm_child_storage_ref
{
    firm_child_storage_ref() = default;

    explicit firm_child_storage_ref(
        std::span<detail::firm_child_slot> slots)
        : slots(slots)
    {}

    std::span<detail::firm_child_slot> slots;
};

template<std::size_t N>
class static_firm_child_storage
{
public:
    [[nodiscard]] firm_child_storage_ref ref() noexcept
    {
        return firm_child_storage_ref{
            std::span<detail::firm_child_slot>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_child_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::firm_child_slot, N == 0 ? 1 : N> storage_{};
};

class owned_firm_child_storage
{
public:
    owned_firm_child_storage() = default;

    explicit owned_firm_child_storage(std::size_t capacity)
        : slots_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::firm_child_slot[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_child_storage(const owned_firm_child_storage &) = delete;
    owned_firm_child_storage & operator=(
        const owned_firm_child_storage &) = delete;
    owned_firm_child_storage(owned_firm_child_storage &&) noexcept = default;
    owned_firm_child_storage & operator=(
        owned_firm_child_storage &&) noexcept = default;

    [[nodiscard]] firm_child_storage_ref ref() noexcept
    {
        return firm_child_storage_ref{
            std::span<detail::firm_child_slot>{slots_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_child_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::firm_child_slot[]> slots_;
    std::size_t capacity_ = 0;
};

struct firm_deed_storage_ref
{
    firm_deed_storage_ref() = default;

    explicit firm_deed_storage_ref(
        std::span<detail::firm_deed_record> records)
        : records(records)
    {}

    std::span<detail::firm_deed_record> records;
};

template<std::size_t N>
class static_firm_deed_storage
{
public:
    [[nodiscard]] firm_deed_storage_ref ref() noexcept
    {
        return firm_deed_storage_ref{
            std::span<detail::firm_deed_record>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_deed_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::firm_deed_record, N == 0 ? 1 : N> storage_{};
};

class owned_firm_deed_storage
{
public:
    owned_firm_deed_storage() = default;

    explicit owned_firm_deed_storage(std::size_t capacity)
        : records_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::firm_deed_record[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_deed_storage(const owned_firm_deed_storage &) = delete;
    owned_firm_deed_storage & operator=(
        const owned_firm_deed_storage &) = delete;
    owned_firm_deed_storage(owned_firm_deed_storage &&) noexcept = default;
    owned_firm_deed_storage & operator=(
        owned_firm_deed_storage &&) noexcept = default;

    [[nodiscard]] firm_deed_storage_ref ref() noexcept
    {
        return firm_deed_storage_ref{
            std::span<detail::firm_deed_record>{
                records_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_deed_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::firm_deed_record[]> records_;
    std::size_t capacity_ = 0;
};

struct firm_completion_storage_ref
{
    firm_completion_storage_ref() = default;

    explicit firm_completion_storage_ref(
        std::span<detail::child_completion> completions)
        : completions(completions)
    {}

    std::span<detail::child_completion> completions;
};

template<std::size_t N>
class static_firm_completion_storage
{
public:
    [[nodiscard]] firm_completion_storage_ref ref() noexcept
    {
        return firm_completion_storage_ref{
            std::span<detail::child_completion>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_completion_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::child_completion, N == 0 ? 1 : N> storage_{};
};

class owned_firm_completion_storage
{
public:
    owned_firm_completion_storage() = default;

    explicit owned_firm_completion_storage(std::size_t capacity)
        : completions_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::child_completion[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_completion_storage(
        const owned_firm_completion_storage &) = delete;
    owned_firm_completion_storage & operator=(
        const owned_firm_completion_storage &) = delete;
    owned_firm_completion_storage(
        owned_firm_completion_storage &&) noexcept = default;
    owned_firm_completion_storage & operator=(
        owned_firm_completion_storage &&) noexcept = default;

    [[nodiscard]] firm_completion_storage_ref ref() noexcept
    {
        return firm_completion_storage_ref{
            std::span<detail::child_completion>{
                completions_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_completion_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::child_completion[]> completions_;
    std::size_t capacity_ = 0;
};

struct firm_join_storage_ref
{
    firm_join_storage_ref() = default;

    explicit firm_join_storage_ref(
        std::span<std::exception_ptr> failures)
        : failures(failures)
    {}

    std::span<std::exception_ptr> failures;
};

template<std::size_t N>
class static_firm_join_storage
{
public:
    [[nodiscard]] firm_join_storage_ref ref() noexcept
    {
        return firm_join_storage_ref{
            std::span<std::exception_ptr>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_join_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<std::exception_ptr, N == 0 ? 1 : N> storage_{};
};

class owned_firm_join_storage
{
public:
    owned_firm_join_storage() = default;

    explicit owned_firm_join_storage(std::size_t capacity)
        : failures_(
            capacity == 0
                ? nullptr
                : std::make_unique<std::exception_ptr[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_join_storage(const owned_firm_join_storage &) = delete;
    owned_firm_join_storage & operator=(
        const owned_firm_join_storage &) = delete;
    owned_firm_join_storage(owned_firm_join_storage &&) noexcept = default;
    owned_firm_join_storage & operator=(
        owned_firm_join_storage &&) noexcept = default;

    [[nodiscard]] firm_join_storage_ref ref() noexcept
    {
        return firm_join_storage_ref{
            std::span<std::exception_ptr>{failures_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_join_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<std::exception_ptr[]> failures_;
    std::size_t capacity_ = 0;
};

struct firm_bookkeeping_storage_ref
{
    firm_bookkeeping_storage_ref() = default;

    firm_bookkeeping_storage_ref(
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds,
        firm_completion_storage_ref completions,
        firm_join_storage_ref joins)
        : children(children)
        , deeds(deeds)
        , completions(completions)
        , joins(joins)
    {}

    firm_child_storage_ref children;
    firm_deed_storage_ref deeds;
    firm_completion_storage_ref completions;
    firm_join_storage_ref joins;
};

struct firm_storage_ref
{
    firm_storage_ref() = default;

    firm_storage_ref(
        frame_storage_ref frames,
        firm_bookkeeping_storage_ref bookkeeping)
        : frames(frames)
        , bookkeeping(bookkeeping)
    {}

    firm_storage_ref(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds,
        firm_completion_storage_ref completions,
        firm_join_storage_ref joins)
        : firm_storage_ref(
              frames,
              firm_bookkeeping_storage_ref{
                  children,
                  deeds,
                  completions,
                  joins})
    {}

    [[nodiscard]] firm_child_storage_ref children() const noexcept
    {
        return bookkeeping.children;
    }

    [[nodiscard]] firm_deed_storage_ref deeds() const noexcept
    {
        return bookkeeping.deeds;
    }

    [[nodiscard]] firm_completion_storage_ref completions() const noexcept
    {
        return bookkeeping.completions;
    }

    [[nodiscard]] firm_join_storage_ref joins() const noexcept
    {
        return bookkeeping.joins;
    }

    frame_storage_ref frames;
    firm_bookkeeping_storage_ref bookkeeping;
};

template<
    std::size_t ChildSlots,
    std::size_t JoinFailureSlots = ChildSlots,
    std::size_t CompletionSlots = ChildSlots,
    std::size_t DeedSlots = ChildSlots>
class static_firm_bookkeeping_storage
{
public:
    [[nodiscard]] firm_bookkeeping_storage_ref ref() noexcept
    {
        return firm_bookkeeping_storage_ref{
            children_, deeds_, completions_, joins_};
    }

    [[nodiscard]] operator firm_bookkeeping_storage_ref() noexcept
    {
        return ref();
    }

    [[nodiscard]] firm_child_storage_ref children() noexcept
    {
        return children_;
    }

    [[nodiscard]] firm_deed_storage_ref deeds() noexcept
    {
        return deeds_;
    }

    [[nodiscard]] firm_join_storage_ref joins() noexcept
    {
        return joins_;
    }

    [[nodiscard]] firm_completion_storage_ref completions() noexcept
    {
        return completions_;
    }

private:
    static_firm_child_storage<ChildSlots> children_;
    static_firm_deed_storage<DeedSlots> deeds_;
    static_firm_completion_storage<CompletionSlots> completions_;
    static_firm_join_storage<JoinFailureSlots> joins_;
};

template<
    std::size_t FrameBytes,
    std::size_t ChildSlots,
    std::size_t JoinFailureSlots = ChildSlots,
    std::size_t CompletionSlots = ChildSlots,
    std::size_t DeedSlots = ChildSlots>
class static_firm_storage
{
public:
    [[nodiscard]] firm_storage_ref ref() noexcept
    {
        return firm_storage_ref{
            frames_, bookkeeping_};
    }

    [[nodiscard]] operator firm_storage_ref() noexcept
    {
        return ref();
    }

    [[nodiscard]] frame_storage_ref frames() noexcept
    {
        return frames_;
    }

    [[nodiscard]] firm_child_storage_ref children() noexcept
    {
        return bookkeeping_.children();
    }

    [[nodiscard]] firm_deed_storage_ref deeds() noexcept
    {
        return bookkeeping_.deeds();
    }

    [[nodiscard]] firm_join_storage_ref joins() noexcept
    {
        return bookkeeping_.joins();
    }

    [[nodiscard]] firm_completion_storage_ref completions() noexcept
    {
        return bookkeeping_.completions();
    }

private:
    static_frame_storage<FrameBytes> frames_;
    static_firm_bookkeeping_storage<
        ChildSlots,
        JoinFailureSlots,
        CompletionSlots,
        DeedSlots> bookkeeping_;
};

} // namespace nxtrt
