#pragma once

#include <linux/io_uring.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct io_uring
{
    int ring_fd = -1;

    void * sq_ring_ptr = MAP_FAILED;
    std::size_t sq_ring_size = 0;
    void * cq_ring_ptr = MAP_FAILED;
    std::size_t cq_ring_size = 0;
    io_uring_sqe * sqes = nullptr;
    std::size_t sqes_size = 0;

    std::uint32_t * sq_head = nullptr;
    std::uint32_t * sq_tail = nullptr;
    std::uint32_t * sq_mask = nullptr;
    std::uint32_t * sq_entries = nullptr;
    std::uint32_t * sq_array = nullptr;

    std::uint32_t * cq_head = nullptr;
    std::uint32_t * cq_tail = nullptr;
    std::uint32_t * cq_mask = nullptr;
    std::uint32_t * cq_entries = nullptr;
    io_uring_cqe * cqes = nullptr;

    std::uint32_t sqe_tail = 0;
};

namespace nxtrt::raw_uring_detail {

template<typename T>
T load_acquire(T * ptr) noexcept
{
    return std::atomic_ref<T>{*ptr}.load(std::memory_order_acquire);
}

template<typename T>
void store_release(T * ptr, T value) noexcept
{
    std::atomic_ref<T>{*ptr}.store(value, std::memory_order_release);
}

inline int negative_errno() noexcept
{
    return errno == 0 ? -EIO : -errno;
}

inline void * checked_mmap(
    std::size_t size,
    int fd,
    off_t offset) noexcept
{
    return ::mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd,
        offset);
}

inline unsigned prep_poll_mask(unsigned poll_mask) noexcept
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return ((poll_mask & 0x0000ffffU) << 16) | ((poll_mask & 0xffff0000U) >> 16);
#else
    return poll_mask;
#endif
}

inline std::uint64_t ptr_to_u64(const void * ptr) noexcept
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
}

inline void prep_rw(
    int op,
    io_uring_sqe * sqe,
    int fd,
    const void * addr,
    unsigned len,
    std::uint64_t offset) noexcept
{
    sqe->opcode = static_cast<std::uint8_t>(op);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = ptr_to_u64(addr);
    sqe->len = len;
}

} // namespace nxtrt::raw_uring_detail

inline int io_uring_queue_init(
    unsigned entries,
    io_uring * ring,
    unsigned flags) noexcept
{
    *ring = {};
    auto params = io_uring_params{};
    params.flags = flags;

    auto fd = static_cast<int>(
        ::syscall(SYS_io_uring_setup, entries, &params));
    if (fd < 0)
        return nxtrt::raw_uring_detail::negative_errno();

    ring->ring_fd = fd;
    ring->sq_ring_size =
        params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
    ring->cq_ring_size =
        params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

    ring->sq_ring_ptr = nxtrt::raw_uring_detail::checked_mmap(
        ring->sq_ring_size,
        fd,
        IORING_OFF_SQ_RING);
    if (ring->sq_ring_ptr == MAP_FAILED)
        goto fail;

    ring->cq_ring_ptr = nxtrt::raw_uring_detail::checked_mmap(
        ring->cq_ring_size,
        fd,
        IORING_OFF_CQ_RING);
    if (ring->cq_ring_ptr == MAP_FAILED)
        goto fail;

    ring->sqes_size = params.sq_entries * sizeof(io_uring_sqe);
    ring->sqes = static_cast<io_uring_sqe *>(
        nxtrt::raw_uring_detail::checked_mmap(
            ring->sqes_size,
            fd,
            IORING_OFF_SQES));
    if (ring->sqes == MAP_FAILED)
        goto fail;

    {
    auto * sq = static_cast<std::byte *>(ring->sq_ring_ptr);
    ring->sq_head = reinterpret_cast<std::uint32_t *>(sq + params.sq_off.head);
    ring->sq_tail = reinterpret_cast<std::uint32_t *>(sq + params.sq_off.tail);
    ring->sq_mask =
        reinterpret_cast<std::uint32_t *>(sq + params.sq_off.ring_mask);
    ring->sq_entries =
        reinterpret_cast<std::uint32_t *>(sq + params.sq_off.ring_entries);
    ring->sq_array =
        reinterpret_cast<std::uint32_t *>(sq + params.sq_off.array);
    ring->sqe_tail =
        nxtrt::raw_uring_detail::load_acquire(ring->sq_tail);

    auto * cq = static_cast<std::byte *>(ring->cq_ring_ptr);
    ring->cq_head = reinterpret_cast<std::uint32_t *>(cq + params.cq_off.head);
    ring->cq_tail = reinterpret_cast<std::uint32_t *>(cq + params.cq_off.tail);
    ring->cq_mask =
        reinterpret_cast<std::uint32_t *>(cq + params.cq_off.ring_mask);
    ring->cq_entries =
        reinterpret_cast<std::uint32_t *>(cq + params.cq_off.ring_entries);
    ring->cqes = reinterpret_cast<io_uring_cqe *>(cq + params.cq_off.cqes);
    }
    return 0;

fail:
    auto saved = nxtrt::raw_uring_detail::negative_errno();
    if (ring->sqes != nullptr && ring->sqes != MAP_FAILED)
        ::munmap(ring->sqes, ring->sqes_size);
    if (ring->cq_ring_ptr != MAP_FAILED)
        ::munmap(ring->cq_ring_ptr, ring->cq_ring_size);
    if (ring->sq_ring_ptr != MAP_FAILED)
        ::munmap(ring->sq_ring_ptr, ring->sq_ring_size);
    ::close(fd);
    *ring = {};
    return saved;
}

inline void io_uring_queue_exit(io_uring * ring) noexcept
{
    if (ring->sqes != nullptr && ring->sqes != MAP_FAILED)
        ::munmap(ring->sqes, ring->sqes_size);
    if (ring->cq_ring_ptr != MAP_FAILED)
        ::munmap(ring->cq_ring_ptr, ring->cq_ring_size);
    if (ring->sq_ring_ptr != MAP_FAILED)
        ::munmap(ring->sq_ring_ptr, ring->sq_ring_size);
    if (ring->ring_fd >= 0)
        ::close(ring->ring_fd);
    *ring = {};
}

inline unsigned io_uring_sq_space_left(io_uring * ring) noexcept
{
    auto head = nxtrt::raw_uring_detail::load_acquire(ring->sq_head);
    return *ring->sq_entries - (ring->sqe_tail - head);
}

inline io_uring_sqe * io_uring_get_sqe(io_uring * ring) noexcept
{
    if (io_uring_sq_space_left(ring) == 0)
        return nullptr;

    auto index = ring->sqe_tail & *ring->sq_mask;
    auto * sqe = &ring->sqes[index];
    std::memset(sqe, 0, sizeof(*sqe));
    ring->sq_array[index] = index;
    ring->sqe_tail++;
    return sqe;
}

inline int io_uring_submit(io_uring * ring) noexcept
{
    auto ktail = nxtrt::raw_uring_detail::load_acquire(ring->sq_tail);
    auto to_submit = ring->sqe_tail - ktail;
    if (to_submit == 0)
        return 0;

    nxtrt::raw_uring_detail::store_release(ring->sq_tail, ring->sqe_tail);
    auto rc = static_cast<int>(::syscall(
        SYS_io_uring_enter,
        ring->ring_fd,
        to_submit,
        0,
        0,
        nullptr,
        0));
    if (rc < 0)
        return nxtrt::raw_uring_detail::negative_errno();
    return rc;
}

inline int io_uring_peek_cqe(io_uring * ring, io_uring_cqe ** cqe) noexcept
{
    auto head = nxtrt::raw_uring_detail::load_acquire(ring->cq_head);
    auto tail = nxtrt::raw_uring_detail::load_acquire(ring->cq_tail);
    if (head == tail) {
        *cqe = nullptr;
        return -EAGAIN;
    }

    *cqe = &ring->cqes[head & *ring->cq_mask];
    return 0;
}

inline int io_uring_wait_cqe(io_uring * ring, io_uring_cqe ** cqe) noexcept
{
    while (true) {
        auto rc = io_uring_peek_cqe(ring, cqe);
        if (rc != -EAGAIN)
            return rc;

        rc = static_cast<int>(::syscall(
            SYS_io_uring_enter,
            ring->ring_fd,
            0,
            1,
            IORING_ENTER_GETEVENTS,
            nullptr,
            0));
        if (rc < 0) {
            auto err = nxtrt::raw_uring_detail::negative_errno();
            if (err == -EINTR)
                return err;
            return err;
        }
    }
}

inline void io_uring_cqe_seen(io_uring * ring, io_uring_cqe * cqe) noexcept
{
    (void)cqe;
    auto head = nxtrt::raw_uring_detail::load_acquire(ring->cq_head);
    nxtrt::raw_uring_detail::store_release(ring->cq_head, head + 1);
}

inline void io_uring_sqe_set_data64(
    io_uring_sqe * sqe,
    std::uint64_t data) noexcept
{
    sqe->user_data = data;
}

inline std::uint64_t io_uring_cqe_get_data64(io_uring_cqe const * cqe) noexcept
{
    return cqe->user_data;
}

inline void io_uring_prep_nop(io_uring_sqe * sqe) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(IORING_OP_NOP, sqe, -1, nullptr, 0, 0);
}

inline void io_uring_prep_openat(
    io_uring_sqe * sqe,
    int dfd,
    const char * path,
    int flags,
    mode_t mode) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_OPENAT, sqe, dfd, path, mode, 0);
    sqe->open_flags = static_cast<std::uint32_t>(flags);
}

inline void io_uring_prep_statx(
    io_uring_sqe * sqe,
    int dfd,
    const char * path,
    int flags,
    unsigned mask,
    struct statx * statxbuf) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_STATX,
        sqe,
        dfd,
        path,
        mask,
        nxtrt::raw_uring_detail::ptr_to_u64(statxbuf));
    sqe->statx_flags = static_cast<std::uint32_t>(flags);
}

inline void io_uring_prep_waitid(
    io_uring_sqe * sqe,
    idtype_t idtype,
    id_t id,
    siginfo_t * infop,
    int options,
    unsigned int flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_WAITID,
        sqe,
        id,
        nullptr,
        static_cast<unsigned>(idtype),
        0);
    sqe->waitid_flags = flags;
    sqe->file_index = static_cast<std::uint32_t>(options);
    sqe->addr2 = nxtrt::raw_uring_detail::ptr_to_u64(infop);
}

inline void io_uring_prep_read(
    io_uring_sqe * sqe,
    int fd,
    void * buf,
    unsigned nbytes,
    std::uint64_t offset) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_READ, sqe, fd, buf, nbytes, offset);
}

inline void io_uring_prep_write(
    io_uring_sqe * sqe,
    int fd,
    const void * buf,
    unsigned nbytes,
    std::uint64_t offset) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_WRITE, sqe, fd, buf, nbytes, offset);
}

inline void io_uring_prep_recv(
    io_uring_sqe * sqe,
    int fd,
    void * buf,
    std::size_t len,
    int flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_RECV,
        sqe,
        fd,
        buf,
        static_cast<unsigned>(len),
        0);
    sqe->msg_flags = static_cast<std::uint32_t>(flags);
}

inline void io_uring_prep_send(
    io_uring_sqe * sqe,
    int fd,
    const void * buf,
    std::size_t len,
    int flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_SEND,
        sqe,
        fd,
        buf,
        static_cast<unsigned>(len),
        0);
    sqe->msg_flags = static_cast<std::uint32_t>(flags);
}

inline void io_uring_prep_connect(
    io_uring_sqe * sqe,
    int fd,
    const sockaddr * addr,
    socklen_t addrlen) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_CONNECT, sqe, fd, addr, 0, addrlen);
}

inline void io_uring_prep_accept(
    io_uring_sqe * sqe,
    int fd,
    sockaddr * addr,
    socklen_t * addrlen,
    int flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_ACCEPT,
        sqe,
        fd,
        addr,
        0,
        nxtrt::raw_uring_detail::ptr_to_u64(addrlen));
    sqe->accept_flags = static_cast<std::uint32_t>(flags);
}

inline void io_uring_prep_poll_add(
    io_uring_sqe * sqe,
    int fd,
    unsigned poll_mask) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_POLL_ADD, sqe, fd, nullptr, 0, 0);
    sqe->poll32_events = nxtrt::raw_uring_detail::prep_poll_mask(poll_mask);
}

inline void io_uring_prep_timeout(
    io_uring_sqe * sqe,
    const __kernel_timespec * ts,
    unsigned count,
    unsigned flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_TIMEOUT, sqe, -1, ts, 1, count);
    sqe->timeout_flags = flags;
}

inline void io_uring_prep_cancel64(
    io_uring_sqe * sqe,
    std::uint64_t user_data,
    int flags) noexcept
{
    nxtrt::raw_uring_detail::prep_rw(
        IORING_OP_ASYNC_CANCEL, sqe, -1, nullptr, 0, 0);
    sqe->addr = user_data;
    sqe->cancel_flags = static_cast<std::uint32_t>(flags);
}
