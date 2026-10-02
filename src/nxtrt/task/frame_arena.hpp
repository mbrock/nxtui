#pragma once

// Aligned frame storage and the reusable coroutine frame arena.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/exceptions.hpp"
#include "nxtrt/land.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace nxtrt {

/// The unit of frame land. Frame blocks are whole cells, so land typed as
/// cells is aligned for any task frame.
struct alignas(std::max_align_t) frame_cell
{
    std::byte bytes[alignof(std::max_align_t)];
};

/// Frame land is raw land of frame cells.
using frame_storage_ref = value_storage_ref<frame_cell>;

/// Inline frame land of at least `Bytes` bytes.
template<std::size_t Bytes>
using static_frame_storage = static_value_storage<
    frame_cell,
    (Bytes + sizeof(frame_cell) - 1) / sizeof(frame_cell)>;

using owned_frame_storage = rack<frame_cell>;
class firm_frame_arena;

namespace detail {

/// Precedes every task frame in firm land.
struct alignas(std::max_align_t) task_frame_header
{
    /// Arena that owns the block, so frame deletion can return it.
    firm_frame_arena * arena = nullptr;
    /// Index of the non-relocating chunk holding the frame (zero if
    /// borrowed).
    std::size_t chunk = 0;
    /// Whole block size: header plus frame, rounded to the block alignment.
    std::size_t block = 0;
};

} // namespace detail

/// Frame land for one firm: lazy owned chunks, or bounded borrowed bytes.
///
/// Owned chunks grow without moving frames, and are reclaimed with the
/// arena. Frames are bump-allocated at each chunk's top. A freed frame on
/// top retracts the top; any other freed frame goes onto a free list for
/// its exact block size and is handed to the next frame of that size.
/// Coroutine frames come in few sizes (one per coroutine function), so
/// long-lived frames low in the arena, like a firm's worker loops, do not
/// stop the frames they keep awaiting from being reused. When no frame is
/// live the whole arena resets.
///
/// A ring with prefix retirement, the shape RFC 0002 first sketched, cannot
/// reclaim anything behind such a long-lived frame.
class firm_frame_arena
{
public:
    static constexpr std::size_t block_alignment =
        alignof(detail::task_frame_header);
    static constexpr std::size_t size_class_capacity = 16;

    firm_frame_arena() = default;

    explicit firm_frame_arena(frame_storage_ref land)
        : storage_(std::as_writable_bytes(std::span{land.data, land.size}))
        , bounded_(true)
        , capacity_(storage_.size())
    {}

    firm_frame_arena(const firm_frame_arena &) = delete;
    firm_frame_arena & operator=(const firm_frame_arena &) = delete;

    firm_frame_arena(firm_frame_arena && other) noexcept
    {
        // Headers retain their arena pointer; only an empty arena may move.
        if (other.live_frames_ != 0) {
            std::fputs(
                "nxtrt: moved a firm while task frames live in its land\n",
                stderr);
            std::abort();
        }
        storage_ = std::exchange(other.storage_, {});
        bounded_ = other.bounded_;
        chunks_ = std::move(other.chunks_);
        capacity_ = std::exchange(other.capacity_, 0);
        high_water_ = std::exchange(other.high_water_, 0);
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    /// Bytes held by live frames, headers included.
    [[nodiscard]] std::size_t used() const noexcept
    {
        return live_bytes_;
    }

    [[nodiscard]] std::size_t live_frames() const noexcept
    {
        return live_frames_;
    }

    /// Sum of chunk bump offsets (the borrowed region has just one).
    [[nodiscard]] std::size_t top() const noexcept
    {
        return top_;
    }

    /// Highest sum of bump offsets since construction, excluding unused
    /// tails.
    [[nodiscard]] std::size_t high_water() const noexcept
    {
        return high_water_;
    }

    /// Freed blocks waiting on a size-class free list.
    [[nodiscard]] std::size_t free_listed_bytes() const noexcept
    {
        return free_listed_bytes_;
    }

    /// Freed blocks no size class could take because every class was busy;
    /// they come back when the arena resets.
    [[nodiscard]] std::size_t stranded_bytes() const noexcept
    {
        return stranded_bytes_;
    }

    /// The borrowed region, or an empty view for non-contiguous owned land.
    [[nodiscard]] frame_storage_ref storage() const noexcept
    {
        return {
            reinterpret_cast<frame_cell *>(storage_.data()),
            storage_.size() / sizeof(frame_cell),
        };
    }

    [[nodiscard]] static constexpr std::size_t block_size(
        std::size_t frame_size) noexcept
    {
        return align_up(
            sizeof(detail::task_frame_header) + frame_size,
            block_alignment);
    }

    /// Returns the frame address, or nullptr when bounded land cannot fit
    /// it. Owned growth may throw std::bad_alloc.
    [[nodiscard]] void * allocate(std::size_t frame_size)
    {
        auto const block = block_size(frame_size);
        auto * reused = take_free_block(block);
        auto chunk_index = std::size_t{0};
        auto * bytes = static_cast<std::byte *>(nullptr);
        if (reused != nullptr) {
            chunk_index = reused->chunk;
            bytes = reinterpret_cast<std::byte *>(reused);
        } else {
            if (bounded_) {
                if (block > storage_.size() - top_)
                    return nullptr;
                bytes = storage_.data() + top_;
            } else {
                chunk_index = chunks_.size();
                for (auto i = chunks_.size(); i != 0; --i) {
                    auto & chunk = chunks_[i - 1];
                    if (block <= chunk.land.size() * sizeof(frame_cell)
                                     - chunk.top) {
                        chunk_index = i - 1;
                        break;
                    }
                }
                if (chunk_index == chunks_.size()) {
                    // Double ordinary chunks from 4 KiB up to 64 KiB;
                    // a larger frame gets a chunk large enough for itself.
                    auto const growth =
                        chunks_.empty() ? std::size_t{4096}
                                        : std::min(
                                              chunks_.back().land.size()
                                                  * sizeof(frame_cell) * 2,
                                              std::size_t{64 * 1024});
                    auto const chunk_bytes = std::max(block, growth);
                    chunks_.push_back(
                        chunk{
                            owned_frame_storage{
                                chunk_bytes / sizeof(frame_cell)},
                            0});
                    capacity_ += chunk_bytes;
                }
                auto & chunk = chunks_[chunk_index];
                bytes = reinterpret_cast<std::byte *>(chunk.land.data())
                        + chunk.top;
                chunk.top += block;
            }
            top_ += block;
            high_water_ = std::max(high_water_, top_);
        }

        auto * header =
            ::new (static_cast<void *>(bytes)) detail::task_frame_header{
                .arena = this, .chunk = chunk_index, .block = block};
        live_bytes_ += block;
        ++live_frames_;
        return header + 1;
    }

    /// The arena a frame from `allocate` belongs to.
    [[nodiscard]] static firm_frame_arena & owner_of(void * frame) noexcept
    {
        return *header_of(frame)->arena;
    }

    void deallocate(void * frame) noexcept
    {
        auto * header = header_of(frame);
        auto const block = header->block;
        auto const chunk_index = header->chunk;
        auto * bytes = reinterpret_cast<std::byte *>(header);
        live_bytes_ -= block;
        if (--live_frames_ == 0) {
            reset();
            return;
        }

        auto * start = bounded_ ? storage_.data()
                                : reinterpret_cast<std::byte *>(
                                      chunks_[chunk_index].land.data());
        auto & chunk_top = bounded_ ? top_ : chunks_[chunk_index].top;
        auto const offset = static_cast<std::size_t>(bytes - start);
        if (offset + block == chunk_top) {
            chunk_top = offset;
            if (!bounded_)
                top_ -= block;
            return;
        }
        give_free_block(bytes, block, chunk_index);
    }

private:
    struct chunk
    {
        owned_frame_storage land;
        std::size_t top = 0;
    };

    struct free_block
    {
        free_block * next = nullptr;
        std::size_t chunk = 0;
    };

    struct size_class
    {
        std::size_t block = 0;
        free_block * head = nullptr;
    };

    [[nodiscard]] static constexpr std::size_t align_up(
        std::size_t value,
        std::size_t alignment) noexcept
    {
        auto mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    [[nodiscard]] static detail::task_frame_header * header_of(
        void * frame) noexcept
    {
        return static_cast<detail::task_frame_header *>(frame) - 1;
    }

    [[nodiscard]] free_block * take_free_block(std::size_t block) noexcept
    {
        for (auto & entry : size_classes_) {
            if (entry.block != block || entry.head == nullptr)
                continue;
            auto * taken = entry.head;
            entry.head = taken->next;
            free_listed_bytes_ -= block;
            return taken;
        }
        return nullptr;
    }

    /// Files a freed block under its size class, claiming an empty class
    /// when none matches.
    void give_free_block(
        std::byte * bytes,
        std::size_t block,
        std::size_t chunk_index) noexcept
    {
        auto * slot = static_cast<size_class *>(nullptr);
        for (auto & entry : size_classes_) {
            if (entry.block == block) {
                slot = &entry;
                break;
            }
            if (slot == nullptr && entry.head == nullptr)
                slot = &entry;
        }
        if (slot == nullptr) {
            stranded_bytes_ += block;
            return;
        }

        slot->block = block;
        slot->head = ::new (static_cast<void *>(bytes)) free_block{
            .next = slot->head,
            .chunk = chunk_index,
        };
        free_listed_bytes_ += block;
    }

    void reset() noexcept
    {
        top_ = 0;
        for (auto & chunk : chunks_)
            chunk.top = 0;
        free_listed_bytes_ = 0;
        stranded_bytes_ = 0;
        size_classes_ = {};
    }

    std::span<std::byte> storage_;
    bool bounded_ = false;
    std::vector<chunk> chunks_;
    std::size_t capacity_ = 0;
    std::size_t top_ = 0;
    std::size_t high_water_ = 0;
    std::size_t live_bytes_ = 0;
    std::size_t live_frames_ = 0;
    std::size_t free_listed_bytes_ = 0;
    std::size_t stranded_bytes_ = 0;
    std::array<size_class, size_class_capacity> size_classes_{};
};

} // namespace nxtrt
