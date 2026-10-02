#include <nxtrt/blocking.hpp>
#include <nxtrt/arch.hpp>
#include <nxtrt/pool.hpp>
#if defined(__linux__)
#  include <nxtrt/wand/epoll.hpp>
#endif

#include "test.hpp"

#include <atomic>
#include <barrier>
#include <semaphore>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>

namespace nxt::test {
namespace {

using namespace boost::ut;

struct gate
{
    std::binary_semaphore entered{0}, release{0};

    int operator()()
    {
        entered.release();
        release.acquire();
        return 73;
    }
};

template<typename Wand>
struct host
{
    Wand wand;
    nxtrt::deck deck{&wand};

    template<typename T>
    void finish(nxtrt::root_task<T> & root)
    {
        wand.run_until_done(deck, root.inner());
    }
};

template<typename Fn>
nxtrt::task<int>
observe_resume(Fn recipe, std::thread::id & resumed, nxtrt::deck *& deck)
{
    auto result = co_await recipe();
    resumed = std::this_thread::get_id();
    deck = nxtrt::current_deck();
    co_return result;
}

bool cancelled(nxtrt::task<int> & task)
{
    try {
        (void) task.result();
    } catch (nxtrt::operation_cancelled const &) {
        return true;
    }
    return false;
}

nxtrt::task<void> scope_probe(
    nxtrt::firm & scope,
    nxtrt::blocking_pool & pool,
    gate & running,
    bool & destroyed,
    bool & touched)
{
    struct local_state
    {
        bool & destroyed;

        ~local_state()
        {
            destroyed = true;
        }

        int value = 11;
    } state{destroyed};

    auto child = scope.fork(pool.call([&] {
        (void) running();
        state.value *= 7;
        touched = state.value == 77;
    }));
    co_await scope
        .join(); // locals outlive all worker access, including stop
}

template<typename Wand>
void cases()
{
    "owned recipe runs off deck and resumes on original deck"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{2, 3};
        auto owner = std::this_thread::get_id();
        std::thread::id worker, resumed;
        nxtrt::deck * resumed_deck = nullptr;
        bool worker_env_empty = false;
        auto fn = [value = std::make_unique<int>(19),
                   &worker,
                   &worker_env_empty]() mutable {
            worker = std::this_thread::get_id();
            worker_env_empty = nxtrt::current_env() == nullptr;
            return *value * 3 + 2;
        };
        auto recipe = pool.call(std::move(fn));
        static_assert(nxtrt::idea_of<decltype(recipe), int>);
        nxtrt::root_task root{
            h.deck, [&] {
                return observe_resume(
                    std::move(recipe), resumed, resumed_deck);
            }};
        // Constructing and invoking a recipe does not start the callable.
        expect(worker == std::thread::id{});
        root.start();
        h.finish(root);
        expect(root.inner().result() == 59);
        expect(worker != owner);
        expect(worker_env_empty);
        expect(resumed == owner);
        expect(resumed_deck == &h.deck);
        expect(pool.occupied() == 0ul);
    };

    "void move-only results and original exception transport"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{1, 1};
        int changed = 0;
        nxtrt::root_task void_root{
            h.deck, [&] { return pool.run([&] { changed = 31; }); }};
        void_root.start();
        h.finish(void_root);
        void_root.inner().result();
        expect(changed == 31);
        nxtrt::root_task value{
            h.deck, [&] {
                return pool.run([] { return std::make_unique<int>(97); });
            }};
        value.start();
        h.finish(value);
        expect(*value.inner().result() == 97);
        nxtrt::root_task failure{h.deck, [&] {
                                     return pool.run([]() -> int {
                                         throw std::logic_error{
                                             "worker error 17"};
                                     });
                                 }};
        failure.start();
        h.finish(failure);
        bool original = false;
        try {
            (void) failure.inner().result();
        } catch (std::logic_error const & e) {
            original = std::string_view{e.what()} == "worker error 17";
        }
        expect(original);
        expect(pool.occupied() == 0ul);
    };

    "completed result can be unwanted before owner delivery"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{1, 2};
        gate first_gate, second_gate;
        nxtrt::root_task first{
            h.deck, [&] { return pool.run([&] { return first_gate(); }); }};
        nxtrt::root_task second{
            h.deck,
            [&] { return pool.run([&] { return second_gate(); }); }};
        first.start();
        h.deck.run_until_idle();
        first_gate.entered.acquire();
        second.start();
        h.deck.run_until_idle();
        first_gate.release.release();
        // Starting the next job proves the worker published the first
        // result. No deck turn has consumed that result yet.
        second_gate.entered.acquire();
        expect(pool.occupied() == 2ul);
        first.inner().request_stop();
        h.finish(first);
        expect(cancelled(first.inner()));
        second_gate.release.release();
        h.finish(second);
        expect(second.inner().result() == 73);
        second.inner().request_stop();
        expect(second.inner().result() == 73); // delivery cannot be revoked
    };

    "pre-stopped calls do not invoke and parked destruction fails fast"_test =
        [] {
            // Fork before creating workers; never reuse a multithreaded
            // service copied by fork. The child exits via abort, without
            // unwinding.
            auto pid = ::fork();
            expect(pid >= 0);
            if (pid == 0) {
                rlimit limit{0, 0};
                (void) ::setrlimit(RLIMIT_CORE, &limit);
                host<Wand> child_host;
                nxtrt::blocking_pool child_pool{1, 1};
                gate running;
                nxtrt::root_task child{child_host.deck, [&] {
                                           return child_pool.run(
                                               [&] { return running(); });
                                       }};
                child.start();
                child_host.deck.run_until_idle();
                running.entered.acquire();
                child.inner() =
                    {}; // invalid raw destruction, never silent detach
                ::_exit(10);
            }
            if (pid > 0) {
                int status = 0;
                expect(::waitpid(pid, &status, 0) == pid);
                expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
            }
            host<Wand> h;
            nxtrt::blocking_pool pool{1, 1};
            int invoked = 0;
            std::stop_source stop;
            stop.request_stop();
            nxtrt::root_task root{h.deck, [&] {
                                      return pool.run(
                                          [&] {
                                              ++invoked;
                                              return 8;
                                          },
                                          stop.get_token());
                                  }};
            root.start();
            h.finish(root);
            expect(cancelled(root.inner()));
            expect(invoked == 0);
        };

    "bounded credit and cancelled admission never invoke"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{1, 1};
        gate running;
        int invoked = 0;
        std::stop_source stop;
        nxtrt::root_task first{
            h.deck, [&] { return pool.run([&] { return running(); }); }};
        nxtrt::root_task second{h.deck, [&] {
                                    return pool.run(
                                        [&] {
                                            ++invoked;
                                            return 9;
                                        },
                                        stop.get_token());
                                }};
        first.start();
        h.deck.run_until_idle();
        running.entered.acquire();
        second.start();
        h.deck.run_until_idle();
        expect(pool.occupied() == 1ul);
        expect(invoked == 0);
        // The external token's callback writes the job pipe, never
        // wand.cancel.
        std::thread stopper{[&] { stop.request_stop(); }};
        stopper.join();
        h.finish(second);
        expect(cancelled(second.inner()));
        expect(invoked == 0);
        expect(!first.inner().done());
        running.release.release();
        h.finish(first);
        expect(first.inner().result() == 73);
    };

    "credit returns only after settlement and wakes admission"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{1, 1};
        gate running;
        std::atomic<int> invoked = 0;
        nxtrt::root_task first{
            h.deck, [&] { return pool.run([&] { return running(); }); }};
        nxtrt::root_task second{h.deck, [&] {
                                    return pool.run([&] {
                                        ++invoked;
                                        return 29;
                                    });
                                }};
        first.start();
        h.deck.run_until_idle();
        running.entered.acquire();
        second.start();
        h.deck.run_until_idle();
        expect(pool.occupied() == 1ul);
        expect(invoked.load() == 0);
        first.inner().request_stop();
        h.deck.run_until_idle();
        expect(!first.inner().done());
        expect(pool.occupied() == 1ul);
        running.release.release();
        h.finish(second);
        h.finish(first);
        expect(cancelled(first.inner()));
        expect(second.inner().result() == 29);
        expect(invoked.load() == 1);
    };

    "stopped firm retains frame locals until worker settlement"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool pool{1, 1};
        gate running;
        bool destroyed = false, touched = false;
        nxtrt::root_task root{
            h.deck, [&] {
                return nxtrt::with_firm([&](nxtrt::firm & scope) {
                    return scope_probe(
                        scope, pool, running, destroyed, touched);
                });
            }};
        root.start();
        h.deck.run_until_idle();
        running.entered.acquire();
        root.inner().request_stop();
        h.deck.run_until_idle();
        expect(!root.inner().done());
        expect(!destroyed);
        running.release.release();
        h.finish(root);
        try {
            root.inner().result();
        } catch (...) {
        }
        expect(touched);
        expect(destroyed);
        expect(pool.occupied() == 0ul);
    };

    "blocking recipes compose with bounded NXT feed pool"_test = [] {
        host<Wand> h;
        nxtrt::blocking_pool workers{2, 2};
        using fn = std::function<int()>;
        auto jobs = std::array{
            workers.call(fn{[] { return 13; }}),
            workers.call(fn{[] { return 37; }}),
            workers.call(fn{[] { return 61; }})};
        using recipe = decltype(jobs)::value_type;
        nxtrt::static_value_storage<recipe, 1> input_land;
        nxtrt::value_range_source input{jobs, input_land.ref()};
        std::array<nxtrt::pool_slot<recipe>, 2> slots;
        nxtrt::farm<nxtrt::pool_slot<recipe>, 2> available{&slots};
        nxtrt::static_value_storage<int, 2> output;
        nxtrt::pool results{input, available, output.ref()};
        auto collect =
            [](nxtrt::feed<int> & feed) -> nxtrt::task<std::vector<int>> {
            std::vector<int> values;
            while (auto value = co_await feed.take())
                values.push_back(*value);
            co_return values;
        }; // captureless coroutine with explicit parameter
        nxtrt::root_task root{h.deck, [&] {
                                  return nxtrt::finally(
                                      collect(results),
                                      [&] { return results.close(); });
                              }};
        root.start();
        h.finish(root);
        auto values = std::move(root.inner()).result();
        std::ranges::sort(values);
        expect(values == std::vector<int>{13, 37, 61});
        expect(workers.occupied() == 0ul);
    };

    "queued cancellation skips invocation while running cancellation drains"_test =
        [] {
            host<Wand> h;
            nxtrt::blocking_pool pool{1, 2};
            gate running;
            int invoked = 0;
            nxtrt::root_task first{
                h.deck,
                [&] { return pool.run([&] { return running(); }); }};
            nxtrt::root_task queued{h.deck, [&] {
                                        return pool.run([&] {
                                            ++invoked;
                                            return 9;
                                        });
                                    }};
            first.start();
            h.deck.run_until_idle();
            running.entered.acquire();
            queued.start();
            h.deck.run_until_idle();
            expect(pool.occupied() == 2ul);
            queued.inner().request_stop();
            first.inner().request_stop();
            h.deck.run_until_idle();
            expect(!first.inner().done());
            expect(!queued.inner().done());
            running.release.release();
            h.finish(queued);
            h.finish(first);
            expect(cancelled(first.inner()));
            expect(cancelled(queued.inner()));
            expect(invoked == 0);
            expect(pool.occupied() == 0ul);
        };

    "close drains in-flight calls and rejects admission even when stopped"_test =
        [] {
            host<Wand> h;
            nxtrt::blocking_pool pool{1, 1};
            gate running;
            nxtrt::root_task first{
                h.deck,
                [&] { return pool.run([&] { return running(); }); }};
            nxtrt::root_task pending{
                h.deck, [&] { return pool.run([] { return 5; }); }};
            nxtrt::root_task close{h.deck, [&] { return pool.close(); }};
            first.start();
            h.deck.run_until_idle();
            running.entered.acquire();
            pending.start();
            close.start();
            h.deck.run_until_idle();
            close.inner().request_stop();
            h.deck.run_until_idle();
            expect(!close.inner().done());
            running.release.release();
            h.finish(close);
            close.inner().result();
            expect(first.inner().done());
            expect(pending.inner().done());
            expect(cancelled(first.inner()));
            expect(cancelled(pending.inner()));
            expect(pool.occupied() == 0ul);
            nxtrt::root_task late{
                h.deck, [&] { return pool.run([] { return 7; }); }};
            late.start();
            h.finish(late);
            expect(cancelled(late.inner()));
            nxtrt::root_task again{h.deck, [&] { return pool.close(); }};
            again.start();
            h.finish(again);
            again.inner().result();
        };

    "cancellation and completion race keeps storage until worker exits"_test =
        [] {
            for (int i = 0; i != 32; ++i) {
                host<Wand> h;
                nxtrt::blocking_pool pool{1, 1};
                std::barrier rendezvous{2};
                std::binary_semaphore entered{0};
                bool touched = false;
                std::stop_source stop;
                nxtrt::root_task root{
                    h.deck, [&] {
                        return pool.run(
                            [&] {
                                entered.release();
                                rendezvous.arrive_and_wait();
                                touched = true;
                                if (i % 2)
                                    throw std::runtime_error{"unwanted"};
                                return 43;
                            },
                            stop.get_token());
                    }};
                root.start();
                h.deck.run_until_idle();
                entered.acquire();
                std::thread stopper{[&] {
                    rendezvous.arrive_and_wait();
                    stop.request_stop();
                }};
                stopper.join(); // stop definitely precedes owner result
                                // delivery
                h.finish(root);
                expect(cancelled(root.inner()));
                expect(touched);
            }
        };

    "single worker state initialization calls destruction have affinity and FIFO"_test =
        [] {
            host<Wand> h;
            nxtrt::blocking_pool pool{1, 3};
            struct state
            {
                std::thread::id thread = std::this_thread::get_id();
                int value = 2;
            };
            state * owned = nullptr;
            std::thread::id initialized, called, destroyed;
            std::vector<int> order;
            nxtrt::root_task init{h.deck, [&] {
                                      return pool.run([&] {
                                          owned = new state;
                                          initialized = owned->thread;
                                      });
                                  }};
            init.start();
            h.finish(init);
            init.inner().result();
            auto make = [&](int digit) {
                return pool.run([&, digit] {
                    called = std::this_thread::get_id();
                    owned->value = owned->value * 10 + digit;
                    order.push_back(digit);
                    return owned->value;
                });
            };
            nxtrt::root_task a{h.deck, [&] { return make(3); }};
            nxtrt::root_task b{h.deck, [&] { return make(7); }};
            a.start();
            b.start();
            h.finish(b);
            h.finish(a);
            expect(a.inner().result() == 23);
            expect(b.inner().result() == 237);
            expect(order == std::vector<int>{3, 7});
            nxtrt::root_task destroy{h.deck, [&] {
                                         return pool.run([&] {
                                             destroyed =
                                                 std::this_thread::get_id();
                                             delete owned;
                                             owned = nullptr;
                                         });
                                     }};
            destroy.start();
            h.finish(destroy);
            destroy.inner().result();
            expect(initialized == called);
            expect(called == destroyed);
            expect(initialized != std::this_thread::get_id());
        };
}

static suite blocking_tests{
    "BLOCKING", [] {
        "application wand"_group = [] { cases<nxtrt::arch::wand>(); };
#if defined(__linux__)
        "epoll wand"_group = [] { cases<nxtrt::epoll_wand>(); };
#endif
    }};

} // namespace
} // namespace nxt::test
