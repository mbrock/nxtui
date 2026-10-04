// SPDX-License-Identifier: AGPL-3.0-or-later
#include <wisp/reader.hpp>
#include <wisp/printer.hpp>

#include <iostream>

namespace {
using namespace wisp;

void require(bool condition, const char * message)
{
    if (!condition)
        throw std::runtime_error(message);
}

profile evaluate(std::string_view text, std::int32_t expected)
{
    profile p;
    heap h;
    evaluator vm{h};
    root run{h, vm.start(*reader{h, vm, text}.next())};
    h.profiling(&p);
    if (vm.advance(run.get(), 1000) != evaluation::done)
        throw std::runtime_error(
            "run did not finish: " + std::string(text) + ": "
            + print(h, h.get<tag::run, field::err>(run.get())));
    require(
        h.get<tag::run, field::val>(run.get()) == fixnum(expected),
        "wrong value");
    h.profiling(nullptr);
    return p;
}
} // namespace

int main()
{
    try {
        profile p;
        heap h;
        h.profiling(&p);
        root live{h, h.newv32(std::array{h.newv08("abc"), fixnum(9)})};
        h.cons(nil, nil); // Unreachable: never count this as a GC copy.
        h.filledv32(3, t);
        h.collect();
        require(
            h.v08slice(h.v32slice(live.get())[0]) == "abc",
            "GC changed data");
        require(
            h.v32slice(live.get())[1] == fixnum(9), "GC changed number");
        const auto scale = std::uint64_t(profile_enabled);
        require(
            p.allocations[std::size_t(tag::duo)] == scale,
            "duo allocations");
        require(
            p.allocations[std::size_t(tag::v32)] == 2 * scale,
            "vector allocations");
        require(
            p.allocations[std::size_t(tag::v08)] == scale,
            "string allocations");
        require(p.gc_copies[std::size_t(tag::duo)] == 0, "copied garbage");
        require(
            p.gc_copies[std::size_t(tag::v32)] == scale, "vector copies");
        require(
            p.gc_copies[std::size_t(tag::v08)] == scale, "string copies");
        require(
            p.v08_bytes == 3 * scale && p.v32_words == 5 * scale,
            "payload allocations");
        require(
            p.gc_v08_bytes == 3 * scale && p.gc_v32_words == 2 * scale,
            "copied payload");
        require(p.gc_count == scale, "collection count");
        h.profiling(nullptr);
        h.cons(nil, nil);
        require(
            p.allocations[std::size_t(tag::duo)] == scale, "detachment");

        profile copied;
        heap frames;
        const auto acc =
            frames.newv32(std::array{fixnum(11), fixnum(22), fixnum(33)});
        const auto node =
            frames.new_words<tag::rec>(std::array{fixnum(256)});
        const auto frame =
            frames.make<tag::ktx>({top, nil, node, acc, fixnum(1)});
        frames.profiling(&copied);
        const auto clone = frames.copy_continuation_frame(frame);
        const auto progress = frames.get<tag::ktx, field::acc>(clone);
        frames.v32set(progress, 0, fixnum(99));
        require(
            frames.v32slice(acc)[0] == fixnum(11), "progress was shared");
        require(
            copied.continuation_copy_words == 3 * scale,
            "copied progress words");
        frames.copy_continuation_frame(
            frames.make<tag::ktx>({top, nil, node, nil, nil}));
        require(
            copied.continuation_copy_words == 3 * scale,
            "counted absent payload");

        auto atom = evaluate("42", 42);
        require(
            atom.evaluator_steps == scale, "one atom is one transition");
        auto sum = evaluate("(+ 2 5)", 7);
        // Application, first atom, accumulate, second atom, apply.
        require(sum.evaluator_steps == 5 * scale, "arithmetic transitions");
        require(
            sum.jet_calls == scale && sum.function_calls == 0, "call kind");
        require(
            sum.call_arity[2] == scale
                && sum.arguments_accumulated == 2 * scale,
            "call arity");
        require(sum.continuation_pushes == scale, "argument frame");
        require(
            sum.allocations[std::size_t(tag::run)] == 0, "counted setup");
        auto middle = evaluate("(let ((a 11) (b 22) (c 33)) b)", 22);
        require(
            middle.lexical_lookups == scale
                && middle.lexical_frames == scale,
            "lexical frames");
        require(
            middle.lexical_comparisons == 2 * scale,
            "middle binding needs two comparisons");
        require(
            middle.lexical_depth[1] == scale
                && middle.lexical_global_fallbacks == 0,
            "lexical depth");
        auto outer = evaluate("(let ((a 11)) (let ((b 22)) a))", 11);
        require(
            outer.lexical_frames == 2 * scale
                && outer.lexical_depth[2] == scale,
            "outer depth");
        require(
            outer.lexical_comparisons == 2 * scale, "outer comparisons");
        auto wide =
            evaluate("(+ 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17)", 153);
        require(wide.call_arity[16] == scale, "arity histogram saturates");
        auto dynamic = evaluate(
            "(do (set-symbol-dynamic! 'x t)"
            " (call-with-binding 'x 12 (%fn nil ()"
            "  (call-with-prompt 'p (%fn nil () x) (%fn nil (v k) 0)))))",
            12);
        require(
            dynamic.dynamic_lookups == scale
                && dynamic.dynamic_hits == scale,
            "dynamic hit");
        require(
            dynamic.dynamic_hops == 2 * scale,
            "dynamic lookup skips prompt boundary");
        auto effect = evaluate(
            "(call-with-prompt 'pulse"
            " (%fn nil () (+ 3 (send-with-default! 'pulse 5 99)))"
            " (%fn nil (value resume) (call resume value)))",
            8);
        require(
            effect.continuation_searches == scale
                && effect.continuation_boundaries == scale,
            "prompt search");
        require(
            effect.continuation_captures == scale
                && effect.continuation_calls == scale,
            "capture and resume");
        auto missed = evaluate("(send-with-default! 'absent 1 29)", 29);
        require(
            missed.continuation_searches == scale
                && missed.continuation_captures == 0,
            "missing prompt must not count capture");
        std::cout
            << "semantic counters: allocation/GC separation, transitions, arity, lookup depth, dynamic lookup, effects, opt-out OK\n";
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
