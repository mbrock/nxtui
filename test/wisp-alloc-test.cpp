// Kept in its own executable: allocation failure must not affect unrelated
// tests or replace nxt-core's process-wide allocation tracing.
#include <wisp/heap.hpp>

#include "test.hpp"

#include <cstdlib>
#include <limits>
#include <new>

namespace {

constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
auto remaining = unlimited;

struct fail_after
{
    explicit fail_after(std::size_t n)
    {
        remaining = n;
    }

    ~fail_after()
    {
        remaining = unlimited;
    }
};

} // namespace

void * operator new(std::size_t size)
{
    if (remaining == 0)
        throw std::bad_alloc{};
    if (remaining != unlimited)
        --remaining;
    if (auto * p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc{};
}

void operator delete(void * p) noexcept
{
    std::free(p);
}

void operator delete(void * p, std::size_t) noexcept
{
    std::free(p);
}

namespace wisp::test {

using namespace boost::ut;

static suite allocation_tests{
    "WISP ALLOCATION", [] {
        "every partial column allocation leaves old rows intact"_test = [] {
            tab<tag::sym> table;
            table.push({3, 5, 7, 11, 13});
            auto capacity = table.capacity();
            for (std::size_t n = 0; n < 5; ++n) {
                bool failed = false;
                try {
                    fail_after fault{n};
                    table.reserve(100);
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
                expect(failed);
                expect(table.size() == 1u && table.capacity() == capacity);
                expect(table.read(0) == row<tag::sym>{3, 5, 7, 11, 13});
            }
            table.reserve(100);
            expect(table.capacity() == 100u);
        };

        "collection fails only before forwarding roots pins or rows"_test =
            [] {
                std::size_t failures = 0;
                bool succeeded = false;
                for (std::size_t n = 0; n < 64 && !succeeded; ++n) {
                    std::size_t releases = 0;
                    heap h{{&releases, [](void * p, word) noexcept {
                                ++*static_cast<std::size_t *>(p);
                            }}};
                    (void) h.make<tag::ext>({19, nil});
                    auto pair = h.cons(7, nil);
                    auto vec = h.newv32(std::array<word, 2>{pair, pair});
                    h.set<tag::duo, field::cdr>(pair, vec);
                    root live{h, vec};
                    auto pin = h.make_pin(pair);
                    try {
                        fail_after fault{n};
                        h.collect();
                        succeeded = true;
                    } catch (const std::bad_alloc &) {
                        ++failures;
                    }
                    if (!succeeded) {
                        expect(!h.era());
                        expect(live.get() == vec && h.pinned(pin) == pair);
                        expect(
                            h.read<tag::duo>(pair)
                            == row<tag::duo>{7, vec});
                        expect(
                            h.v32slice(vec)[0] == pair
                            && h.v32slice(vec)[1] == pair);
                        expect(releases == 0u);
                        h.collect(); // Recover using the very same heap.
                    }
                    expect(h.era() && releases == 1u);
                    expect(h.v32slice(live.get())[0] == h.pinned(pin));
                    expect(
                        h.get<tag::duo, field::cdr>(h.pinned(pin))
                        == live.get());
                }
                expect(succeeded && failures >= 3u);
            };

        "failed vector append rolls back its payload and descriptor"_test =
            [] {
                bool succeeded = false;
                std::size_t failures = 0;
                for (std::size_t n = 0; n < 16 && !succeeded; ++n) {
                    heap h;
                    auto vec = h.newv32(std::array<word, 3>{7, 11, 19});
                    // Fill descriptor capacity to force column growth on
                    // clone.
                    while (h.table<tag::v32>().size()
                           < h.table<tag::v32>().capacity())
                        (void) h.copy<tag::v32>(vec);
                    auto rows = h.table<tag::v32>().size();
                    word clone = nil;
                    try {
                        fail_after fault{n};
                        clone = h.clonev32(vec);
                        succeeded = true;
                    } catch (const std::bad_alloc &) {
                        ++failures;
                    }
                    if (!succeeded) {
                        expect(h.word_count() == 3u);
                        expect(h.table<tag::v32>().size() == rows);
                        clone = h.clonev32(vec);
                    }
                    expect(h.word_count() == 6u);
                    expect(h.table<tag::v32>().size() == rows + 1);
                    expect(h.v32slice(clone)[2] == 19u);
                    expect(h.v32slice(vec)[1] == 11u);
                }
                expect(succeeded && failures >= 3u);
            };
    }};

} // namespace wisp::test
