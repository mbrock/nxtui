#include "nxtrt/alloc_trace.hpp"

#include <cstdlib>
#include <new>

namespace {

[[nodiscard]] void * allocate_default(
    std::size_t size,
    std::size_t alignment)
{
    if (size == 0)
        size = 1;

    void * ptr = nullptr;
    if (alignment == 0 || alignment <= alignof(std::max_align_t)) {
        ptr = std::malloc(size);
    } else if (::posix_memalign(&ptr, alignment, size) != 0) {
        ptr = nullptr;
    }

    if (ptr == nullptr)
        throw std::bad_alloc{};

    nxtrt::alloc_trace::event(
        "heap",
        "new",
        ptr,
        size,
        alignment);
    return ptr;
}

void deallocate_default(
    void * ptr,
    std::size_t size,
    std::size_t alignment) noexcept
{
    nxtrt::alloc_trace::event(
        "heap",
        "del",
        ptr,
        size,
        alignment);
    std::free(ptr);
}

} // namespace

void * operator new(std::size_t size)
{
    return allocate_default(size, 0);
}

void * operator new[](std::size_t size)
{
    return allocate_default(size, 0);
}

void * operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate_default(size, static_cast<std::size_t>(alignment));
}

void * operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate_default(size, static_cast<std::size_t>(alignment));
}

void * operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try {
        return allocate_default(size, 0);
    } catch (...) {
        return nullptr;
    }
}

void * operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    try {
        return allocate_default(size, 0);
    } catch (...) {
        return nullptr;
    }
}

void * operator new(
    std::size_t size,
    std::align_val_t alignment,
    const std::nothrow_t &) noexcept
{
    try {
        return allocate_default(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void * operator new[](
    std::size_t size,
    std::align_val_t alignment,
    const std::nothrow_t &) noexcept
{
    try {
        return allocate_default(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void * ptr) noexcept
{
    deallocate_default(ptr, 0, 0);
}

void operator delete[](void * ptr) noexcept
{
    deallocate_default(ptr, 0, 0);
}

void operator delete(void * ptr, std::size_t size) noexcept
{
    deallocate_default(ptr, size, 0);
}

void operator delete[](void * ptr, std::size_t size) noexcept
{
    deallocate_default(ptr, size, 0);
}

void operator delete(void * ptr, std::align_val_t alignment) noexcept
{
    deallocate_default(ptr, 0, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr, std::align_val_t alignment) noexcept
{
    deallocate_default(ptr, 0, static_cast<std::size_t>(alignment));
}

void operator delete(
    void * ptr,
    std::size_t size,
    std::align_val_t alignment) noexcept
{
    deallocate_default(ptr, size, static_cast<std::size_t>(alignment));
}

void operator delete[](
    void * ptr,
    std::size_t size,
    std::align_val_t alignment) noexcept
{
    deallocate_default(ptr, size, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr, const std::nothrow_t &) noexcept
{
    deallocate_default(ptr, 0, 0);
}

void operator delete[](void * ptr, const std::nothrow_t &) noexcept
{
    deallocate_default(ptr, 0, 0);
}

void operator delete(
    void * ptr,
    std::align_val_t alignment,
    const std::nothrow_t &) noexcept
{
    deallocate_default(ptr, 0, static_cast<std::size_t>(alignment));
}

void operator delete[](
    void * ptr,
    std::align_val_t alignment,
    const std::nothrow_t &) noexcept
{
    deallocate_default(ptr, 0, static_cast<std::size_t>(alignment));
}
