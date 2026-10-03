// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of mbrock/wisp core/benchmark.zig,
// a282b936867fcf7d5d2926ebee9afbaf3e153f5b.
#include <wisp/load.hpp>
#include <wisp/printer.hpp>
#include <wisp/tape.hpp>
#include "wisp-revision.hpp"

#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace wisp;
using clock_type = std::chrono::steady_clock;

// #embed is intentionally used as a C++23 extension.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
constexpr unsigned char micro_bytes[] = {
#embed "wisp/micro.wisp"
};
constexpr unsigned char program_bytes[] = {
#embed "wisp/benchmarks.wisp"
};
constexpr unsigned char repo_bytes[] = {
#embed "wisp/repo-benchmarks.wisp"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

template<std::size_t N>
std::string_view source(const unsigned char (&bytes)[N])
{
    return {reinterpret_cast<const char *>(bytes), N};
}

struct benchmark
{
    std::string_view name, entry, arguments = "", input = "count";
    unsigned iterations = 25'000;
    std::string_view setup = source(micro_bytes), expected = "0";
    bool count_last = false;
};

const auto cases = std::to_array<benchmark>({
    {"call-1", "%bench-call-1"},
    {"call-2", "%bench-call-2", " 0"},
    {"call-5", "%bench-call-5", " 0 0 0 0"},
    {"call-16", "%bench-call-16", " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"},
    {"jet-add-2", "%bench-jet-add-2"},
    {"closure-leaf-2", "%bench-closure-leaf-2"},
    {"lookup-first-16",
     "%bench-lookup-first-16",
     " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"},
    {"lookup-last-16",
     "%bench-lookup-last-16",
     " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
     "count",
     25'000,
     source(micro_bytes),
     "0",
     true},
    {"lookup-inner-8", "%bench-lookup-inner-8"},
    {"lookup-outer-8", "%bench-lookup-outer-8"},
    {"effect-shallow",
     "%bench-effects",
     " 0",
     "count",
     1000,
     source(micro_bytes),
     "5"},
    {"effect-deep",
     "%bench-effects",
     " 64",
     "count",
     1000,
     source(micro_bytes),
     "69"},
    {"tak", "%benchmark-tak", "", "18/12/6", 1, source(program_bytes), "7"},
    {"deriv",
     "%benchmark-deriv",
     "",
     "canonical-expression",
     100,
     source(program_bytes),
     "(+ (* (* 3 x x) (+ (/ 0 3) (/ 1 x) (/ 1 x)))"
     " (* (* a x x) (+ (/ 0 a) (/ 1 x) (/ 1 x)))"
     " (* (* b x) (+ (/ 0 b) (/ 1 x))) 0)"},
    {"diviter",
     "%benchmark-diviter",
     "",
     "1000-cell-list",
     100,
     source(program_bytes),
     ""},
    {"divrec",
     "%benchmark-divrec",
     "",
     "1000-cell-list",
     100,
     source(program_bytes),
     ""},
    {"stdlib-list",
     "%benchmark-stdlib",
     "",
     "64-element-list-pipeline",
     100,
     source(repo_bytes),
     "66"},
    {"backquote",
     "%benchmark-backquote",
     "",
     "nested-unquote-splice",
     100,
     source(repo_bytes),
     "(append (list (quote a))"
     " (list (append (list (quote b)) (list c) (quote nil)))"
     " d (list (quote e)) (quote nil))"},
    {"router-hit",
     "%benchmark-router-hit",
     "",
     "8-pattern-late-hit",
     100,
     source(repo_bytes),
     "(\"alice\")"},
    {"router-miss",
     "%benchmark-router-miss",
     "",
     "8-pattern-miss",
     100,
     source(repo_bytes),
     "not-found"},
});

// This is a benchmark host, not a new evaluator GC policy. Match the NXT
// host's allocation-proportional policy, including its committed
// safepoints.
constexpr std::size_t gc_floor = 1024 * 1024, gc_poll_steps = 4096;

std::size_t heap_bytes(const heap & h)
{
    const auto rows = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return (
            (h.table<std::tuple_element_t<I, vat>::type>().size()
             * sizeof(row<std::tuple_element_t<I, vat>::type>))
            + ...);
    }(std::make_index_sequence<std::tuple_size_v<vat>>{});
    return rows + h.byte_count() + h.word_count() * sizeof(word);
}

struct runner
{
    std::unique_ptr<image> state = image::fresh();
    heap & h = state->storage;
    evaluator & vm = state->machine;
    std::size_t threshold = gc_floor;

    void collect_if_needed()
    {
        if (vm.collection_requested() || heap_bytes(h) >= threshold) {
            vm.collect();
            threshold = 2 * heap_bytes(h) + gc_floor;
        }
    }

    void check(evaluation status, word run)
    {
        if (status == evaluation::failed)
            throw std::runtime_error(
                print(h, h.get<tag::run, field::err>(run)));
    }

    void load(std::string_view text)
    {
        loader input{h, vm, text};
        evaluation status;
        do {
            status = input.advance(gc_poll_steps);
            collect_if_needed();
            check(status, input.run());
        } while (status == evaluation::runnable);
    }

    void evaluate(root & run)
    {
        evaluation status;
        do {
            status = vm.advance(run.get(), gc_poll_steps);
            collect_if_needed();
            check(status, run.get());
        } while (status == evaluation::runnable);
    }

    word invocation(const benchmark & b, unsigned count)
    {
        const auto n = " " + std::to_string(count);
        const auto text = "(" + std::string(b.entry)
                          + (b.count_last ? std::string(b.arguments) + n
                                          : n + std::string(b.arguments))
                          + ")";
        return *reader{h, vm, text}.next();
    }

    void validate(const benchmark & b, word value)
    {
        if (b.expected.empty()) {
            // Stronger than the reference's length-only check: every cell
            // must be NIL and the 500th tail must terminate.
            for (unsigned i = 0; i < 500; ++i) {
                if (tag_of(value) != tag::duo)
                    throw std::runtime_error("expected 500 NIL cells");
                auto [car, cdr] = h.read<tag::duo>(value);
                if (car != nil)
                    throw std::runtime_error("expected NIL list element");
                value = cdr;
            }
            if (value != nil)
                throw std::runtime_error(
                    "expected list to end at 500 cells");
        } else {
            const auto actual = print(h, value);
            const auto expected =
                print(h, *reader{h, vm, b.expected}.next());
            if (actual != expected)
                throw std::runtime_error(
                    "expected " + expected + ", got " + actual);
        }
    }
};

template<typename T>
void field(std::string_view name, const T & value)
{
    std::cout << ',' << std::quoted(name) << ':' << value;
}

template<std::size_t N>
void array_field(
    std::string_view name, const std::array<std::uint64_t, N> & values)
{
    std::cout << ',' << std::quoted(name) << ":[";
    for (std::size_t i = 0; i < N; ++i)
        std::cout << (i ? "," : "") << values[i];
    std::cout << ']';
}

void run(const benchmark & b, unsigned iterations, unsigned warmup)
{
    profile counters;
    runner r;
    r.load(base_library());
    r.load(b.setup);
    if (warmup) {
        root run{r.h, r.vm.start(r.invocation(b, warmup))};
        r.evaluate(run);
        r.validate(b, r.h.get<tag::run, field::val>(run.get()));
    }
    root run{r.h, r.vm.start(r.invocation(b, iterations))};
    const auto before = heap_bytes(r.h);
    r.h.profiling(&counters);
    const auto start = clock_type::now();
    r.evaluate(run);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_type::now() - start)
            .count();
    r.h.profiling(nullptr);
    const auto after = heap_bytes(r.h);
    r.validate(b, r.h.get<tag::run, field::val>(run.get()));

    std::cout << "{\"benchmark\":" << std::quoted(b.name);
    field("input", std::quoted(b.input));
    field("runtime_revision", std::quoted(WISP_BENCH_REVISION));
    field("buildtype", std::quoted(WISP_BENCH_BUILDTYPE));
    field("compiler", std::quoted(__VERSION__));
    field("timing", std::quoted("evaluator-only"));
    if (b.name.starts_with("router-"))
        field(
            "router_semantics", std::quoted("guest-continuation-prompts"));
    field("profile_enabled", profile_enabled ? "true" : "false");
    field("iterations", iterations);
    field("warmup_iterations", warmup);
    field("elapsed_ns", elapsed);
    field("ns_per_iteration", elapsed / iterations);
    field("heap_bytes_start", before);
    field("heap_bytes_end", after);
    field("gc_poll_steps", gc_poll_steps);
    field("gc_floor_bytes", gc_floor);
    field("gc_growth_factor", 2);
    field("gc_threshold_end", r.threshold);
    field("checked", "true");
    if constexpr (profile_enabled) {
#define COUNTER(name) field(#name, counters.name)
        COUNTER(evaluator_steps);
        COUNTER(jet_calls);
        COUNTER(function_calls);
        COUNTER(macro_calls);
        COUNTER(continuation_calls);
        COUNTER(continuation_searches);
        COUNTER(continuation_captures);
        COUNTER(continuation_boundaries);
        COUNTER(continuation_pushes);
        COUNTER(arguments_accumulated);
        COUNTER(lists_scanned);
        COUNTER(list_cells_scanned);
        COUNTER(lexical_lookups);
        COUNTER(lexical_frames);
        COUNTER(lexical_comparisons);
        COUNTER(lexical_global_fallbacks);
        COUNTER(dynamic_lookups);
        COUNTER(dynamic_hops);
        COUNTER(dynamic_hits);
        COUNTER(v08_bytes);
        COUNTER(v32_words);
        COUNTER(gc_v08_bytes);
        COUNTER(gc_v32_words);
        COUNTER(gc_count);
        COUNTER(gc_nanoseconds);
#undef COUNTER
        array_field("call_arity", counters.call_arity);
        array_field("lexical_depth", counters.lexical_depth);
        array_field("allocations", counters.allocations);
        array_field("gc_copies", counters.gc_copies);
    }
    std::cout << "}\n";
}

unsigned count(std::string_view text, bool zero_allowed)
{
    unsigned value = 0;
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()
        || value > unsigned(max_fixnum) || (!value && !zero_allowed))
        throw std::runtime_error(
            "invalid iteration count: " + std::string(text));
    return value;
}
} // namespace

int main(int argc, char ** argv)
{
    try {
        if (argc > 4)
            throw std::runtime_error(
                "usage: wisp-bench [all|NAME|--list] [ITERATIONS [WARMUP]]");
        const std::string_view selection = argc > 1 ? argv[1] : "all";
        bool matched = false;
        for (const auto & b : cases) {
            if (selection == "--list") {
                std::cout << b.name << '\n';
                matched = true;
            } else if (selection == "all" || selection == b.name) {
                matched = true;
                run(b,
                    argc > 2 ? count(argv[2], false) : b.iterations,
                    argc > 3 ? count(argv[3], true) : 0);
            }
        }
        if (!matched)
            throw std::runtime_error(
                "unknown benchmark: " + std::string(selection));
    } catch (const std::exception & error) {
        std::cerr << "wisp-bench: " << error.what() << '\n';
        return 1;
    }
}
