// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#include "wisp/eval.hpp"
#include "wisp/code.hpp"
#include "wisp/printer.hpp"
#include "wisp/reader.hpp"

#include <cassert>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>

namespace wisp {
struct eval_step;

namespace {

using values = std::span<const word>;

// Native scratch for one synchronous operation, never suspended guest
// state. Borrowing a guest payload across call() is unsafe: binding or a
// builtin may grow its pool. Small snapshots need no native allocation;
// unusually wide calls get one exact-sized rack, not geometric growth.
template<typename Use>
void with_words(std::size_t count, Use && use)
{
    std::array<word, 32> local;
    if (count <= local.size()) {
        use(std::span{local}.first(count));
    } else {
        nxtrt::rack<word> storage{count};
        std::uninitialized_default_construct_n(storage.data(), count);
        use(std::span{storage.data(), count});
    }
}

struct builtin
{
    std::string_view name;
    bool control;
    bool observes;
    std::size_t minimum;
    std::size_t maximum;
    void (*invoke)(eval_step &, values);

    // A word consumes one argument; a final values parameter consumes the
    // rest. Signature, arity, and invocation cannot drift apart. No erased
    // function-pointer casts or compiler reflection are needed.
    template<auto Function>
    static consteval builtin
    bind(std::string_view name, bool control = false, bool observes = false)
    {
        return bind<Function>(name, control, observes, Function);
    }

private:
    template<typename Arg, std::size_t I>
    static Arg argument(values args)
    {
        if constexpr (std::same_as<Arg, word>)
            return args[I];
        else
            return args.subspan(I);
    }

    template<auto Function, typename... Args>
    static consteval builtin bind(
        std::string_view name,
        bool control,
        bool observes,
        void (eval_step::*)(Args...))
    {
        static_assert(
            ((std::same_as<Args, word> || std::same_as<Args, values>)
             && ...));
        constexpr auto rest = (0 + ... + int(std::same_as<Args, values>));
        static_assert(rest <= 1);
        if constexpr (rest != 0)
            static_assert(std::same_as<
                          std::tuple_element_t<
                              sizeof...(Args) - 1,
                              std::tuple<Args...>>,
                          values>);
        return {
            name,
            control,
            observes,
            sizeof...(Args) - rest,
            rest ? std::size_t(-1) : sizeof...(Args),
            [](eval_step & step, values args) {
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (step.*Function)(argument<Args, I>(args)...);
                }(std::index_sequence_for<Args...>{});
            }};
    }
};

std::span<const builtin> builtins();

constexpr std::string_view type_name(tag type)
{
    switch (type) {
    case tag::integer:
        return "INTEGER";
    case tag::chr:
        return "CHARACTER";
    case tag::duo:
        return "CONS";
    case tag::sym:
        return "SYMBOL";
    case tag::fun:
    case tag::jet:
        return "FUNCTION";
    case tag::mac:
        return "MACRO";
    case tag::v32:
        return "VECTOR";
    case tag::rec:
        return "RECORD";
    case tag::v08:
        return "STRING";
    case tag::pkg:
        return "PACKAGE";
    case tag::ktx:
        return "CONTINUATION";
    case tag::run:
        return "EVALUATOR";
    case tag::ext:
        return "EXTERNAL";
    case tag::pin:
        return "PIN";
    case tag::sys:
        return "SYSTEM";
    }
    return "UNKNOWN";
}

// Index in known_names of each tag's TYPE-OF name, or npos when it has
// none.
constexpr auto type_symbols = [] {
    std::array<std::size_t, 32> table{};
    for (word t = 0; t < table.size(); ++t) {
        table[t] = std::string_view::npos;
        const auto name = type_name(tag(t));
        for (std::size_t k = 0; k < known_names.size(); ++k)
            if (known_names[k] == name)
                table[t] = k;
    }
    return table;
}();

word list(heap & h, std::span<const word> xs)
{
    word result = nil;
    for (auto i = xs.size(); i != 0; --i)
        result = h.cons(xs[i - 1], result);
    return result;
}

struct condition
{
    word value;
};

} // namespace

evaluator::evaluator(heap & storage, std::nullptr_t)
    : heap_(storage)
    , base_(storage)
    , keywords_(storage)
    , keys_(storage)
    , packages_(storage)
    , current_(storage)
    , nil_name_(storage)
    , true_name_(storage)
    , known_([&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<root, sizeof...(I)>{
            (static_cast<void>(I), root{storage})...};
    }(std::make_index_sequence<known_names.size()>{}))
{
}

void evaluator::install_known()
{
    for (std::size_t i = 0; i < known_names.size(); ++i)
        known_[i].set(intern(known_names[i]));
}

evaluator::evaluator(heap & storage)
    : evaluator(storage, nullptr)
{
    base_.set(storage.make<tag::pkg>({storage.newv08("WISP"), nil, nil}));
    keywords_.set(
        storage.make<tag::pkg>({storage.newv08("KEYWORD"), nil, nil}));
    keys_.set(storage.make<tag::pkg>({storage.newv08("KEY"), nil, nil}));
    packages_.set(list(
        storage, std::array{keys_.get(), keywords_.get(), base_.get()}));
    current_.set(base_.get());
    nil_name_.set(storage.newv08("NIL"));
    true_name_.set(storage.newv08("T"));
    install_known();
    const auto jets = builtins();
    for (word i = 0; i < jets.size(); ++i)
        heap_.set<tag::sym, field::fun>(
            intern(jets[i].name), immediate(tag::jet, i));
}

std::vector<evaluator::jet_info> evaluator::jet_manifest()
{
    std::vector<jet_info> result;
    for (const auto & jet : builtins())
        result.push_back({jet.name, jet.control});
    return result;
}

word evaluator::intern(std::string_view name, word package)
{
    if (package == base_.get()) {
        if (name == "NIL")
            return nil;
        if (name == "T")
            return t;
    }
    const auto find = [&](word pkg) -> std::optional<word> {
        for (auto cur = heap_.get<tag::pkg, field::sym>(pkg); cur != nil;) {
            const auto [sym, next] = heap_.read<tag::duo>(cur);
            if (heap_.v08slice(heap_.get<tag::sym, field::str>(sym))
                == name)
                return sym;
            cur = next;
        }
        return std::nullopt;
    };
    if (auto own = find(package))
        return *own;
    auto cur = heap_.get<tag::pkg, field::use>(package), slow = cur;
    bool move_slow = false;
    while (cur != nil) {
        if (tag_of(cur) != tag::duo)
            throw std::invalid_argument("malformed package uses list");
        const auto [used, next] = heap_.read<tag::duo>(cur);
        if (tag_of(used) != tag::pkg)
            throw std::invalid_argument("non-package in uses list");
        if (auto inherited = find(used))
            return *inherited;
        cur = next;
        if (move_slow)
            slow = heap_.get<tag::duo, field::cdr>(slow);
        move_slow = !move_slow;
        if (cur != nil && cur == slow)
            throw std::invalid_argument("cyclic package uses list");
    }
    const auto symbols = heap_.get<tag::pkg, field::sym>(package);
    const auto sym =
        heap_.make<tag::sym>({heap_.newv08(name), package, nah, nil, nil});
    heap_.set<tag::pkg, field::sym>(package, heap_.cons(sym, symbols));
    return sym;
}

word evaluator::intern(std::string_view name)
{
    return intern(name, base_.get());
}

word evaluator::keyword(std::string_view name)
{
    return intern(name, keywords_.get());
}

word evaluator::find_package(std::string_view name) const noexcept
{
    for (auto cur = packages_.get(); cur != nil;) {
        const auto [pkg, next] = heap_.read<tag::duo>(cur);
        if (heap_.v08slice(heap_.get<tag::pkg, field::nam>(pkg)) == name)
            return pkg;
        cur = next;
    }
    return nil;
}

word evaluator::define_package(std::string_view name)
{
    if (find_package(name) != nil)
        throw std::invalid_argument("package already exists");
    const auto pkg = heap_.make<tag::pkg>({heap_.newv08(name), nil, nil});
    packages_.set(heap_.cons(pkg, packages_.get()));
    return pkg;
}

void evaluator::collect()
{
    heap_.collect();
    collect_ = false;
}

word evaluator::start(word expression, word environment)
{
    return heap_.make<tag::run>(
        {expression, nah, nil, environment, top, top});
}

evaluation evaluator::status(word run) const noexcept
{
    if (heap_.get<tag::run, field::err>(run) != nil)
        return evaluation::failed;
    if (heap_.get<tag::run, field::way>(run) == top
        && heap_.get<tag::run, field::meta>(run) == top
        && heap_.get<tag::run, field::val>(run) != nah)
        return evaluation::done;
    return evaluation::runnable;
}

// Scratch registers for a batch of transitions. No guest allocation
// collects; no borrowed row or payload survives a call that can grow it.
struct eval_step
{
    evaluator & vm;
    heap & h;
    word exp, val, err, env, way, meta;
    values active_runs;
    word step_target = nil;
    row<tag::run> entry{};
    std::size_t depth = 0, entry_depth = 0;
    std::optional<evaluator::cached_frame> popped_entry = std::nullopt;

    evaluation state() const
    {
        if (err != nil)
            return evaluation::failed;
        return val != nah && segment_empty() && meta == top
                   ? evaluation::done
                   : evaluation::runnable;
    }

    void begin_transition()
    {
        entry = {exp, val, err, env, way, meta};
        entry_depth = depth;
        popped_entry.reset();
    }

    void observe()
    {
        // Self-inspection sees the same transition-entry run row as the
        // one-step evaluator, including in-place heap progress mutations.
        flush(cache_flush::observation);
        if (popped_entry) {
            auto & entry_way = entry[column_index<tag::run, field::way>()];
            const auto & f = *popped_entry;
            entry_way =
                h.make<tag::ktx>({entry_way, f.env, f.fun, f.acc, f.arg});
            if (auto * p = h.profiling())
                ++p->cache_flushed[std::size_t(cache_flush::observation)];
            popped_entry.reset();
        }
        h.put<tag::run>(active_runs.back(), entry);
    }

    void commit()
    {
        flush(cache_flush::batch);
        h.put<tag::run>(
            active_runs.back(), {exp, val, err, env, way, meta});
    }

    [[noreturn]] void
    fail(known_name name, std::initializer_list<word> details = {})
    {
        // Conditions are records typed by their name, so TYPE-OF says
        // which condition it is: #S(TYPE-MISMATCH VECTOR 42).
        std::vector<word> xs{vm.known(name)};
        xs.insert(xs.end(), details.begin(), details.end());
        throw condition{h.new_words<tag::rec>(xs)};
    }

    // The TYPE-OF symbol for a type tag other than sys.
    word type_symbol(tag type) const noexcept
    {
        return vm.known_[type_symbols[std::size_t(type)]].get();
    }

    void require(word x, tag type)
    {
        if (tag_of(x) != type)
            fail("TYPE-MISMATCH", {type_symbol(type), x});
    }

    // Validate the whole spine before using any element, even if a lookup
    // will stop early. Guest code may have mutated it since the last step.
    std::size_t scan_count(word x)
    {
        std::size_t count = 0;
        auto slow = x;
        while (x != nil) {
            require(x, tag::duo);
            x = h.get<tag::duo, field::cdr>(x);
            if (++count % 2 == 0)
                slow = h.get<tag::duo, field::cdr>(slow);
            if (x != nil && x == slow)
                fail("CYCLIC-LIST");
        }
        if (auto * p = h.profiling()) {
            ++p->lists_scanned;
            p->list_cells_scanned += count;
        }
        return count;
    }

    // Only after scan_count, with no intervening guest execution.
    void copy_list(word x, std::span<word> into)
    {
        for (auto & value : into) {
            const auto [car, cdr] = h.read<tag::duo>(x);
            value = car;
            x = cdr;
        }
    }

    template<typename Use>
    void with_list(word x, Use && use)
    {
        with_words(scan_count(x), [&](std::span<word> xs) {
            copy_list(x, xs);
            use(xs);
        });
    }

    void give(word x)
    {
        exp = nah;
        val = x;
    }

    void enter(word x)
    {
        exp = x;
        val = nah;
    }

    void eval(word x)
    {
        // Public source evaluation excludes implicit caller locals, but
        // retains dynamic bindings, effects, and the continuation. Macro
        // results use enter() with their restored call-site environment.
        env = nil;
        enter(x);
    }

    // * Compiled calls

    std::size_t vector_size(word owner, word x)
    {
        if (tag_of(x) != tag::v32)
            fail("INVALID-EXPRESSION", {owner});
        return h.v32slice(x).size();
    }

    // A frame's position, which must name an element of a vector.
    std::size_t frame_index(word arg, std::size_t limit)
    {
        if (tag_of(arg) != tag::integer || integer(arg) < 0
            || std::size_t(integer(arg)) >= limit)
            fail("INVALID-CONTINUATION", {frame_identity()});
        return std::size_t(integer(arg));
    }

    bool control_jet(word fun) const noexcept
    {
        return tag_of(fun) == tag::jet
               && payload_of(fun) < builtins().size()
               && builtins()[payload_of(fun)].control;
    }

    // A lowered closure keeps its FUNCTION node in the code slot; calls run
    // the function's body.
    word executable_body(word code)
    {
        if (!lowered_code(code))
            return code;
        if (lowered_operation(code) != code_op::function)
            fail("INVALID-FUNCTION", {code});
        return code_operand(code, 2);
    }

    // CODE shows a lowered closure's source snapshot, not its nodes.
    word source_body(word code)
    {
        if (!lowered_code(code))
            return code;
        if (lowered_operation(code) != code_op::function)
            fail("INVALID-FUNCTION", {code});
        return code_operand(code, 3);
    }

    // The value of an operand that needs no transitions of its own: a
    // constant, a lexical load, or a function cell. None can suspend or
    // have an effect, so a call evaluates them in place, in order.
    bool immediate(word x, word & value)
    {
        switch (lowered_operation(x)) {
        case code_op::constant:
            value = code_operand(x, 0);
            return true;
        case code_op::lexical_load: {
            const auto [scope, at] = lowered_address(x);
            value = h.v32slice(scope)[at];
            return true;
        }
        case code_op::function_cell:
            value = h.get<tag::sym, field::fun>(code_operand(x, 0));
            return true;
        default:
            return false;
        }
    }

    // As in source application, the callee is resolved before any
    // argument runs and kept for the rest of the call, even across
    // suspension.
    void start_arguments(word x, word fun, word args)
    {
        const auto size = vector_size(x, args);
        with_words(size, [&](std::span<word> xs) {
            std::size_t ready = 0;
            while (ready < size
                   && immediate(h.v32slice(args)[ready], xs[ready]))
                ++ready;
            if (ready == size) {
                call(fun, xs);
                return;
            }
            // A lone argument needs no progress vector: the frame keeps
            // the callee itself. Otherwise it is [callee, arguments...].
            if (size == 1) {
                push(x, fun, fixnum(0));
            } else {
                const auto progress = h.filledv32(size + 1, nil);
                h.v32set(progress, 0, fun);
                for (std::size_t i = 0; i < ready; ++i)
                    h.v32set(progress, i + 1, xs[i]);
                push(x, progress, fixnum(int(ready)));
            }
            enter(h.v32slice(args)[ready]);
        });
    }

    void proceed_arguments(word x, word args, word acc, word arg)
    {
        const auto size = vector_size(x, args);
        const auto i = frame_index(arg, size);
        if (size == 1) {
            const std::array one{val};
            pop();
            call(acc, one);
            return;
        }
        writable_frame();
        const auto progress = frame_acc();
        if (tag_of(progress) != tag::v32
            || h.v32slice(progress).size() != size + 1)
            fail("INVALID-CONTINUATION", {frame_identity()});
        h.v32set(progress, i + 1, val);
        auto next = i + 1;
        for (word value;
             next < size && immediate(h.v32slice(args)[next], value);
             ++next)
            h.v32set(progress, next + 1, value);
        if (next < size) {
            set_position(fixnum(int(next)));
            enter(h.v32slice(args)[next]);
            return;
        }
        with_words(size, [&](std::span<word> xs) {
            const auto saved = h.v32slice(progress);
            const auto fun = saved[0];
            std::ranges::copy(saved.subspan(1), xs.begin());
            pop();
            call(fun, xs);
        });
    }

    // * Compact executable nodes (RFC 0021)

    bool lowered_code(word x) const
    {
        return code_record_op(h, x).has_value();
    }

    // A code record's shape was checked when it was made or restored, and
    // it cannot change, so dispatch reads only its opcode.
    code_op
    lowered_operation(word x, known_name error = "INVALID-EXPRESSION")
    {
        const auto op = code_record_op(h, x);
        if (!op)
            fail(error, {x});
        return *op;
    }

    // Only for a code record of a known operation. Reacquire pool slices
    // after allocations.
    word code_operand(word x, std::size_t at) const
    {
        return h.words<tag::rec>(x)[at + 1];
    }

    void enter_lowered(word x)
    {
        if (!lowered_code(x))
            fail("INVALID-EXPRESSION", {x});
        enter(x);
    }

    std::pair<word, std::size_t> lowered_address(word x)
    {
        auto cur = env;
        for (auto depth = integer(code_operand(x, 0)); depth > 0; --depth) {
            if (tag_of(cur) != tag::duo)
                fail("INVALID-EXPRESSION", {x});
            cur = h.get<tag::duo, field::cdr>(cur);
        }
        if (tag_of(cur) != tag::duo)
            fail("INVALID-EXPRESSION", {x});
        const auto scope = h.get<tag::duo, field::car>(cur);
        const auto at = 2 * std::size_t(integer(code_operand(x, 1))) + 1;
        if (tag_of(scope) != tag::v32 || h.v32slice(scope).size() % 2 != 0
            || at >= h.v32slice(scope).size())
            fail("INVALID-EXPRESSION", {x});
        return {scope, at};
    }

    void lowered(word x, code_op op)
    {
        switch (op) {
        case code_op::constant:
            give(code_operand(x, 0));
            return;
        case code_op::lexical_load: {
            const auto [scope, at] = lowered_address(x);
            give(h.v32slice(scope)[at]);
            return;
        }
        case code_op::global_load:
            give(lookup(code_operand(x, 0)));
            return;
        case code_op::function_cell:
            function(code_operand(x, 0));
            return;
        case code_op::lexical_store:
            push(x, nil, nil);
            enter(code_operand(x, 2));
            return;
        case code_op::global_store:
            push(x, nil, nil);
            enter(code_operand(x, 1));
            return;
        case code_op::branch:
            push(x, nil, nil);
            enter(code_operand(x, 0));
            return;
        case code_op::sequence: {
            const auto forms = code_operand(x, 0);
            const auto size = h.v32slice(forms).size();
            if (size == 0)
                fail("INVALID-EXPRESSION", {x});
            if (size > 1)
                push(x, nil, fixnum(1));
            enter_lowered(h.v32slice(forms)[0]);
            return;
        }
        case code_op::let: {
            const auto names = code_operand(x, 0),
                       inits = code_operand(x, 1);
            const auto size = h.v32slice(inits).size();
            if (h.v32slice(names).size() != size)
                fail("INVALID-EXPRESSION", {x});
            if (size == 0) {
                enter(code_operand(x, 2));
                return;
            }
            push(x, h.filledv32(size, nil), fixnum(0));
            enter_lowered(h.v32slice(inits)[0]);
            return;
        }
        case code_op::call: {
            const auto name = code_operand(x, 0);
            const auto fun = h.get<tag::sym, field::fun>(name);
            if (fun == nil)
                fail("UNDEFINED-FUNCTION", {name});
            if (tag_of(fun) != tag::fun
                && (tag_of(fun) != tag::jet || control_jet(fun)))
                fail("INVALID-FUNCTION", {fun});
            start_arguments(x, fun, code_operand(x, 1));
            return;
        }
        case code_op::closure: {
            const auto code = code_operand(x, 0);
            if (lowered_operation(code) != code_op::function)
                fail("INVALID-EXPRESSION", {x});
            give(h.make<tag::fun>(
                {env,
                 code_operand(code, 1),
                 code,
                 code_operand(code, 0),
                 0}));
            return;
        }
        case code_op::source:
            enter(code_operand(x, 0));
            return;
        case code_op::function:
            enter(code_operand(x, 2));
            return;
        }
    }

    void proceed_lowered(word x, word acc, word arg)
    {
        switch (lowered_operation(x, "INVALID-CONTINUATION")) {
        case code_op::branch:
            pop();
            enter(code_operand(x, val == nil ? 2 : 1));
            return;
        case code_op::lexical_store: {
            const auto [scope, at] = lowered_address(x);
            h.v32set(scope, at, val);
            pop();
            give(val);
            return;
        }
        case code_op::global_store:
            give(lookup(code_operand(x, 0), true, val));
            pop();
            return;
        case code_op::sequence: {
            const auto forms = code_operand(x, 0);
            const auto size = h.v32slice(forms).size();
            const auto i = frame_index(arg, size);
            if (i + 1 == size)
                pop();
            else {
                writable_frame();
                set_position(fixnum(int(i + 1)));
            }
            enter_lowered(h.v32slice(forms)[i]);
            return;
        }
        case code_op::let: {
            const auto names = code_operand(x, 0),
                       inits = code_operand(x, 1);
            const auto size = h.v32slice(inits).size();
            if (h.v32slice(names).size() != size)
                fail("INVALID-CONTINUATION", {frame_identity()});
            const auto i = frame_index(arg, size);
            if (tag_of(acc) != tag::v32 || h.v32slice(acc).size() != size)
                fail("INVALID-CONTINUATION", {frame_identity()});
            writable_frame();
            const auto progress = frame_acc();
            h.v32set(progress, i, val);
            if (i + 1 < size) {
                set_position(fixnum(int(i + 1)));
                enter_lowered(h.v32slice(inits)[i + 1]);
                return;
            }
            with_words(2 * size, [&](std::span<word> scope) {
                for (std::size_t k = 0; k < size; ++k) {
                    const auto j = size - 1 - k;
                    const auto name = h.v32slice(names)[j];
                    require(name, tag::sym);
                    scope[2 * k] = name;
                    scope[2 * k + 1] = h.v32slice(progress)[j];
                }
                env = h.cons(h.newv32(scope), env);
            });
            pop();
            enter(code_operand(x, 2));
            return;
        }
        case code_op::call:
            proceed_arguments(x, code_operand(x, 1), acc, arg);
            return;
        default:
            fail("INVALID-CONTINUATION", {frame_identity()});
        }
    }

    // The only constructor of code records: OPCODE with OPERANDS that fit
    // its schema.
    void make_code(word opcode, values operands)
    {
        const auto op = code_operation_of(opcode);
        if (!op || !code_operands_valid(h, *op, operands))
            fail("INVALID-CODE", {opcode, list(h, operands)});
        std::vector<word> xs{opcode};
        xs.insert(xs.end(), operands.begin(), operands.end());
        give(h.new_words<tag::rec>(xs));
    }

    void code_operations_()
    {
        word result = nil;
        for (auto i = code_operations.size(); i > 0; --i) {
            const auto & shape = code_operations[i - 1];
            word kinds = nil;
            for (auto j = shape.count; j > 0; --j)
                kinds = h.cons(
                    vm.keyword(operand_name(shape.operands[j - 1])), kinds);
            const std::array entry{
                vm.keyword(shape.name), code_opcode(shape.op), kinds};
            result = h.cons(list(h, entry), result);
        }
        give(result);
    }

    // Ordinary-frame access is shared by source and lowered transitions.
    // Boundary traversal and public continuation views stay heap-based.
    bool segment_empty() const
    {
        return depth == 0 && way == top;
    }

    word frame_identity()
    {
        flush(cache_flush::condition);
        return way;
    }

    evaluator::cached_frame frame() const
    {
        if (depth != 0)
            return vm.frames_[depth - 1];
        const auto [hop, saved_env, fun, acc, arg] = h.read<tag::ktx>(way);
        return {saved_env, fun, acc, arg};
    }

    word frame_acc() const
    {
        return depth != 0 ? vm.frames_[depth - 1].acc
                          : h.get<tag::ktx, field::acc>(way);
    }

    void set_acc(word acc)
    {
        if (depth != 0) {
            vm.frames_[depth - 1].acc = acc;
            return;
        }
        assert(!h.continuation_frozen(way));
        h.set<tag::ktx, field::acc>(way, acc);
    }

    void set_position(word arg)
    {
        if (depth != 0) {
            vm.frames_[depth - 1].arg = arg;
            return;
        }
        assert(!h.continuation_frozen(way));
        h.set<tag::ktx, field::arg>(way, arg);
    }

    void pop()
    {
        if (depth != 0) {
            // A reflective callee can still read the entry run's popped
            // frame. Retain it lazily, with its completed progress.
            if (depth == entry_depth) {
                assert(!popped_entry);
                popped_entry = vm.frames_[depth - 1];
                --entry_depth;
            }
            --depth;
            return;
        }
        way = h.get<tag::ktx, field::hop>(way);
    }

    void writable_frame()
    {
        if (depth != 0 || !h.continuation_frozen(way))
            return;
        auto f = frame();
        const auto kind = tag_of(f.fun);
        if ((kind == tag::fun || kind == tag::jet || kind == tag::rec)
            && tag_of(f.acc) == tag::v32) {
            f.acc = h.clonev32(f.acc);
            if (auto * p = h.profiling())
                p->continuation_copy_words += h.v32slice(f.acc).size();
        }
        // The frozen row remains the transition-entry view. Its private
        // replacement is not part of that view's cached prefix.
        way = h.get<tag::ktx, field::hop>(way);
        vm.frames_[depth++] = f;
        if (auto * p = h.profiling())
            ++p->cache_pulls;
    }

    void push(word fun, word acc, word arg)
    {
        if (depth == vm.frames_.size()) {
            spill(depth / 2, cache_flush::spill);
            if (auto * p = h.profiling())
                ++p->cache_spills;
        }
        vm.frames_[depth++] = {env, fun, acc, arg};
        if (auto * p = h.profiling()) {
            ++p->continuation_pushes;
            ++p->cached_pushes;
        }
    }

    void spill(std::size_t count, cache_flush reason)
    {
        assert(count <= depth && entry_depth <= depth);
        for (std::size_t i = 0; i < count; ++i) {
            const auto & f = vm.frames_[i];
            way = h.make<tag::ktx>({way, f.env, f.fun, f.acc, f.arg});
            if (i < entry_depth)
                entry[column_index<tag::run, field::way>()] = way;
        }
        entry_depth -= std::min(count, entry_depth);
        std::move(
            vm.frames_.begin() + count,
            vm.frames_.begin() + depth,
            vm.frames_.begin());
        depth -= count;
        if (auto * p = h.profiling())
            p->cache_flushed[std::size_t(reason)] += count;
    }

    void flush(cache_flush reason)
    {
        spill(depth, reason);
    }

    word lookup(word sym, bool assign = false, word value = nil)
    {
        require(sym, tag::sym);
        if (!assign
            && h.get<tag::sym, field::pkg>(sym) == vm.keywords_.get())
            return sym;
        auto * p = h.profiling();
        std::size_t depth = 0;
        if (p)
            ++p->lexical_lookups;
        // Explicit lexical binders stay lexical even when the symbol's
        // dynamic declaration changes after a closure captures them.
        (void) scan_count(env);
        for (auto cur = env; cur != nil;) {
            const auto [scope, rest] = h.read<tag::duo>(cur);
            cur = rest;
            if (p) {
                ++p->lexical_frames;
                ++depth;
            }
            require(scope, tag::v32);
            const auto xs = h.v32slice(scope);
            if (xs.size() % 2 != 0)
                fail("INVALID-ENVIRONMENT", {scope});
            for (std::size_t i = 0; i < xs.size(); i += 2) {
                if (p)
                    ++p->lexical_comparisons;
                if (xs[i] == sym) {
                    if (p)
                        ++p->lexical_depth[std::min(
                            depth, std::size_t{16})];
                    if (assign)
                        h.v32set(scope, i + 1, value);
                    return assign ? value : xs[i + 1];
                }
            }
        }
        if (p) {
            ++p->lexical_global_fallbacks;
            ++p->lexical_depth[std::min(depth, std::size_t{16})];
        }
        if (h.get<tag::sym, field::dyn>(sym) != nil) {
            if (p)
                ++p->dynamic_lookups;
            for (auto cur = meta; cur != top;) {
                if (p)
                    ++p->dynamic_hops;
                const auto [hop, saved_env, fun, name, binding] =
                    h.read<tag::ktx>(cur);
                if (fun == vm.known("BINDING") && name == sym) {
                    if (p)
                        ++p->dynamic_hits;
                    const auto xs = h.v32slice(binding);
                    const auto old = xs[0], segment = xs[1];
                    if (assign) {
                        auto entry = h.read<tag::ktx>(cur);
                        entry[column_index<tag::ktx, field::arg>()] =
                            h.newv32(std::array{value, segment});
                        meta =
                            append_meta(meta, cur, h.make<tag::ktx>(entry));
                    }
                    return assign ? value : old;
                }
                cur = hop;
            }
        }
        const auto old = h.get<tag::sym, field::val>(sym);
        if (old == nah)
            fail("UNBOUND-VARIABLE", {sym});
        if (assign)
            h.set<tag::sym, field::val>(sym, value);
        return assign ? value : old;
    }

    void sequence(word body)
    {
        // Body spines are guest data: an earlier form may have changed
        // the rest since the application first checked it.
        (void) scan_count(body);
        if (body == nil) {
            give(nil);
            return;
        }
        const auto [first, rest] = h.read<tag::duo>(body);
        // The last form is in tail position, including a singleton DO.
        if (rest != nil)
            push(vm.known("DO"), nil, rest);
        enter(first);
    }

    void bind(
        word fun, const row<tag::fun> & closure, std::span<const word> args)
    {
        const auto [captured, parameters, body, name, count] = closure;
        const auto size = scan_count(parameters);
        with_words(2 * size, [&](std::span<word> scope) {
            bool optional = false;
            std::size_t used = 0, bound = 0;
            auto cur = parameters;
            for (std::size_t i = 0; i < size; ++i) {
                const auto [p, rest] = h.read<tag::duo>(cur);
                cur = rest;
                if (p == vm.known("&OPTIONAL")) {
                    optional = true;
                    continue;
                }
                if (p == vm.known("&REST") || p == vm.known("&BODY")) {
                    if (i + 2 != size)
                        fail("INVALID-PARAMETERS", {parameters});
                    const auto tail = h.get<tag::duo, field::car>(cur);
                    require(tail, tag::sym);
                    scope[bound++] = tail;
                    scope[bound++] = list(h, args.subspan(used));
                    used = args.size();
                    break;
                }
                require(p, tag::sym);
                if (used == args.size() && !optional)
                    fail(
                        "PROGRAM-ERROR",
                        {vm.known("INVALID-ARGUMENT-COUNT"), fun});
                scope[bound++] = p;
                scope[bound++] = used < args.size() ? args[used++] : nil;
            }
            if (used != args.size())
                fail(
                    "PROGRAM-ERROR",
                    {vm.known("INVALID-ARGUMENT-COUNT"), fun});
            env = h.cons(h.newv32(scope.first(bound)), captured);
        });
        if (tag_of(fun) == tag::fun)
            h.set<tag::fun, field::cnt>(fun, count + 1);
        else
            h.set<tag::mac, field::cnt>(fun, count + 1);
        enter(executable_body(body));
    }

    void call(word fun, std::span<const word> args)
    {
        if (auto * p = h.profiling()) {
            ++p->call_arity[std::min(args.size(), std::size_t{16})];
            if (tag_of(fun) == tag::jet)
                ++p->jet_calls;
            if (tag_of(fun) == tag::fun)
                ++p->function_calls;
            if (tag_of(fun) == tag::mac)
                ++p->macro_calls;
            if (tag_of(fun) == tag::ktx || fun == top)
                ++p->continuation_calls;
        }
        if (tag_of(fun) == tag::ktx || fun == top) {
            if (args.size() != 1)
                fail(
                    "PROGRAM-ERROR", {vm.known("CONTINUATION-CALL-ERROR")});
            install(compose(context(fun)));
            give(args[0]);
            return;
        }
        switch (tag_of(fun)) {
        case tag::jet:
            operate(fun, args);
            break;
        case tag::fun:
            bind(fun, h.read<tag::fun>(fun), args);
            break;
        case tag::mac:
            bind(fun, h.read<tag::mac>(fun), args);
            break;
        default:
            fail("INVALID-FUNCTION", {fun});
        }
    }

    void application(word form)
    {
        const auto [callee, arguments] = h.read<tag::duo>(form);
        if (tag_of(callee) != tag::sym)
            fail("INVALID-CALLEE", {callee});
        const auto fun = h.get<tag::sym, field::fun>(callee);
        if (fun == nil)
            fail("UNDEFINED-FUNCTION", {callee});
        const auto size = scan_count(arguments);
        if (tag_of(fun) == tag::mac
            || (tag_of(fun) == tag::jet
                && payload_of(fun) < builtins().size()
                && builtins()[payload_of(fun)].control)) {
            with_words(size, [&](std::span<word> args) {
                copy_list(arguments, args);
                if (tag_of(fun) == tag::mac)
                    push(vm.known("EVAL"), nil, nil);
                call(fun, args);
            });
        } else if (tag_of(fun) != tag::fun && tag_of(fun) != tag::jet) {
            fail("INVALID-FUNCTION", {fun});
        } else if (arguments == nil) {
            call(fun, {});
        } else {
            const auto [first, rest] = h.read<tag::duo>(arguments);
            push(fun, nil, rest);
            enter(first);
        }
    }

    void proceed()
    {
        if (segment_empty()) {
            const auto entry = h.read<tag::ktx>(meta);
            install(outer(entry));
            env = entry[column_index<tag::ktx, field::env>()];
            return;
        }
        const auto [saved_env, fun, acc, arg] = frame();
        env = saved_env;
        if (fun == vm.known("DO")) {
            pop();
            sequence(arg);
        } else if (fun == vm.known("IF")) {
            require(arg, tag::duo);
            const auto [yes, no] = h.read<tag::duo>(arg);
            pop();
            enter(val == nil ? no : yes);
        } else if (fun == vm.known("EVAL")) {
            pop();
            enter(val);
        } else if (fun == vm.known("LET")) {
            // Reverse accumulator: name, value, name, ..., body.
            with_list(acc, [&](std::span<word> xs) {
                if (xs.empty() || xs.size() % 2 != 0)
                    fail("INVALID-CONTINUATION", {frame_identity()});
                for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
                    require(xs[i], tag::sym);
                (void) scan_count(arg);
                if (arg == nil) {
                    const auto body = xs.back();
                    // Replace the body with val at the front, then rotate
                    // value/name pairs into name/value bindings.
                    std::move_backward(xs.begin(), xs.end() - 1, xs.end());
                    xs[0] = val;
                    for (std::size_t i = 0; i < xs.size(); i += 2)
                        std::swap(xs[i], xs[i + 1]);
                    env = h.cons(h.newv32(xs), saved_env);
                    pop();
                    enter(body);
                } else {
                    const auto [clause, rest] = h.read<tag::duo>(arg);
                    const auto [name, value] = binding(clause);
                    auto next_acc = h.cons(name, h.cons(val, acc));
                    writable_frame();
                    set_acc(next_acc);
                    set_position(rest);
                    enter(value);
                }
            });
        } else if (tag_of(fun) == tag::fun || tag_of(fun) == tag::jet) {
            if (auto * p = h.profiling())
                ++p->arguments_accumulated;
            if (acc == nil && arg == nil) {
                const std::array args{val};
                pop();
                call(fun, args);
                return;
            }
            const auto remaining = scan_count(arg);
            writable_frame();
            auto vector = frame_acc();
            if (vector == nil) {
                vector = h.filledv32(2 + remaining, nil);
                h.v32set(vector, 0, 0);
                set_acc(vector);
            }
            require(vector, tag::v32);
            const auto xs = h.v32slice(vector);
            if (xs.size() < 2 || xs[0] >= xs.size() - 1
                || remaining != xs.size() - xs[0] - 2)
                fail("INVALID-CONTINUATION", {frame_identity()});
            const auto pos = xs[0];
            h.v32set(vector, pos + 1, val);
            h.v32set(vector, 0, pos + 1);
            if (arg == nil) {
                // Binding and slice-taking builtins can grow the word pool.
                const auto slice = h.v32slice(vector).subspan(1);
                with_words(slice.size(), [&](std::span<word> args) {
                    std::ranges::copy(slice, args.begin());
                    pop();
                    call(fun, args);
                });
            } else {
                const auto [first, rest] = h.read<tag::duo>(arg);
                set_position(rest);
                enter(first);
            }
        } else if (tag_of(fun) == tag::rec) {
            proceed_lowered(fun, acc, arg);
        } else {
            fail("INVALID-CONTINUATION", {frame_identity()});
        }
    }

    void operate(word jet, std::span<const word> args)
    {
        const auto id = payload_of(jet);
        if (id >= builtins().size())
            fail("INVALID-FUNCTION", {jet});
        try {
            const auto & def = builtins()[id];
            if (args.size() < def.minimum || args.size() > def.maximum)
                fail(
                    "PROGRAM-ERROR",
                    {vm.known("INVALID-ARGUMENT-COUNT"), jet});
            if (def.observes)
                observe();
            def.invoke(*this, args);
        } catch (const condition & c) {
            fail("BUILTIN-FAILURE", {jet, c.value});
        }
    }

    std::int64_t number(word x)
    {
        require(x, tag::integer);
        return integer(x);
    }

    void quote(word x)
    {
        give(x);
    }

    void function(word sym)
    {
        require(sym, tag::sym);
        give(h.get<tag::sym, field::fun>(sym));
    }

    void fn(word name, word parameters, word body)
    {
        if (name != nil)
            require(name, tag::sym);
        give(h.make<tag::fun>({env, parameters, body, name, 0}));
    }

    void macro_fn(word parameters, word body)
    {
        give(h.make<tag::mac>({env, parameters, body, nil, 0}));
    }

    void if_(word test, word yes, word no)
    {
        push(vm.known("IF"), nil, h.cons(yes, no));
        enter(test);
    }

    void do_(values body)
    {
        sequence(list(h, body));
    }

    std::array<word, 2> binding(word clause)
    {
        if (scan_count(clause) != 2)
            fail("INVALID-BINDING", {clause});
        std::array<word, 2> pair;
        copy_list(clause, pair);
        require(pair[0], tag::sym);
        return pair;
    }

    void let(word clauses, values forms)
    {
        // All initializers use the caller's environment, left to right.
        (void) scan_count(clauses);
        for (auto cur = clauses; cur != nil;) {
            const auto [clause, rest] = h.read<tag::duo>(cur);
            (void) binding(clause);
            cur = rest;
        }
        const auto body = h.cons(vm.known("DO"), list(h, forms));
        if (clauses == nil) {
            enter(body);
            return;
        }
        const auto first = binding(h.get<tag::duo, field::car>(clauses));
        push(
            vm.known("LET"),
            h.cons(first[0], h.cons(body, nil)),
            h.get<tag::duo, field::cdr>(clauses));
        enter(first[1]);
    }

    template<typename Operation>
    void arithmetic(std::int64_t result, values args)
    {
        for (auto x : args) {
            result = Operation{}(result, number(x));
            if (result < min_fixnum || result > max_fixnum)
                fail("FIXNUM-OVERFLOW");
        }
        give(fixnum(static_cast<std::int32_t>(result)));
    }

    void add(values args)
    {
        arithmetic<std::plus<>>(0, args);
    }

    void multiply(values args)
    {
        arithmetic<std::multiplies<>>(1, args);
    }

    void subtract(word first, values rest)
    {
        // Like Zig Wisp, unary subtraction is the identity, not negation.
        arithmetic<std::minus<>>(number(first), rest);
    }

    void divide(word first, values rest)
    {
        auto result = rest.empty() ? 1 : number(first);
        const std::array reciprocal{first};
        for (auto x : rest.empty() ? values{reciprocal} : rest) {
            const auto divisor = number(x);
            if (divisor == 0 || (result == min_fixnum && divisor == -1))
                fail("BAD-FIXNUM-DIVISION", {fixnum(result), x});
            // C++ truncates toward zero; Wisp rounds toward -infinity.
            const auto remainder = result % divisor;
            result /= divisor;
            if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
                --result;
        }
        give(fixnum(static_cast<std::int32_t>(result)));
    }

    void mod(word x, word y)
    {
        const auto dividend = number(x), divisor = number(y);
        if (divisor <= 0)
            fail("BAD-MODULO", {y});
        auto result = dividend % divisor;
        if (result < 0)
            result += divisor;
        give(fixnum(static_cast<std::int32_t>(result)));
    }

    void less(word x, word y)
    {
        give(number(x) < number(y) ? t : nil);
    }

    void greater(word x, word y)
    {
        give(number(x) > number(y) ? t : nil);
    }

    void eq(word x, word y)
    {
        give(x == y ? t : nil);
    }

    void cons(word car, word cdr)
    {
        give(h.cons(car, cdr));
    }

    template<field F>
    void pair_part(word x)
    {
        if (x == nil)
            give(nil);
        else {
            require(x, tag::duo);
            give(h.get<tag::duo, F>(x));
        }
    }

    void list_(values args)
    {
        give(list(h, args));
    }

    void apply(word fun, word arglist)
    {
        with_list(arglist, [&](values args) { call(fun, args); });
    }

    void symbol_function(word sym)
    {
        if (tag_of(sym) == tag::sys)
            give(nil);
        else
            function(sym);
    }

    void type_of(word x)
    {
        if (x == nil)
            give(vm.known("NULL"));
        else if (x == t)
            give(vm.known("BOOLEAN"));
        else if (x == top)
            give(vm.known("CONTINUATION"));
        else if (tag_of(x) == tag::sys)
            fail("INVALID-VALUE", {x});
        else if (tag_of(x) == tag::rec)
            give(record_name(x));
        else
            give(type_symbol(tag_of(x)));
    }

    // As in Emacs Lisp, a record's type is its first word: a symbol names
    // it directly, and a type record (such as a DEFSTRUCT descriptor) names
    // it in its own first slot. Anything else is just a RECORD.
    word record_name(word r)
    {
        auto type = h.words<tag::rec>(r)[0];
        if (tag_of(type) == tag::rec && h.words<tag::rec>(type).size() > 1)
            type = h.words<tag::rec>(type)[1];
        return tag_of(type) == tag::sym ? type : vm.known("RECORD");
    }

    template<bool Control>
    void is_jet(word x)
    {
        auto yes = tag_of(x) == tag::jet;
        if constexpr (Control)
            yes = yes && payload_of(x) < builtins().size()
                  && builtins()[payload_of(x)].control;
        give(yes ? t : nil);
    }

    void prognify(word forms)
    {
        const auto size = scan_count(forms);
        give(
            size == 0   ? nil
            : size == 1 ? h.get<tag::duo, field::car>(forms)
                        : h.cons(vm.known("DO"), forms));
    }

    void macroexpand_1(word form)
    {
        if (tag_of(form) == tag::duo) {
            const auto [head, tail] = h.read<tag::duo>(form);
            if (tag_of(head) == tag::sym) {
                const auto fun = h.get<tag::sym, field::fun>(head);
                if (tag_of(fun) == tag::mac) {
                    with_list(tail, [&](values args) { call(fun, args); });
                    return;
                }
            }
        }
        give(form);
    }

    template<tag T, field F>
    void get_field(word x)
    {
        require(x, T);
        if constexpr (T == tag::pkg && F == field::sym)
            // As with PACKAGES, expose a snapshot, not the private index
            // that interning (including condition creation) must trust.
            with_list(
                h.get<T, F>(x), [&](values xs) { give(list(h, xs)); });
        else
            give(h.get<T, F>(x));
    }

    template<tag T, field F>
    void set_field(word x, word value)
    {
        require(x, T);
        h.set<T, F>(x, value);
        give(x);
    }

    void is_symbol(word x)
    {
        give(x == nil || x == t || tag_of(x) == tag::sym ? t : nil);
    }

    template<field F>
    void symbol_part(word x)
    {
        if (x == nil || x == t) {
            if constexpr (F == field::pkg)
                give(vm.base_.get());
            else
                give(x == nil ? vm.nil_name_.get() : vm.true_name_.get());
        } else
            get_field<tag::sym, F>(x);
    }

    template<field F>
    void closure_part(word fun)
    {
        word result;
        if (tag_of(fun) == tag::fun)
            result = h.get<tag::fun, F>(fun);
        else if (tag_of(fun) == tag::mac)
            result = h.get<tag::mac, F>(fun);
        else if (tag_of(fun) == tag::jet) {
            if constexpr (F == field::sym) {
                if (payload_of(fun) >= builtins().size())
                    fail("INVALID-FUNCTION", {fun});
                result = vm.intern(builtins()[payload_of(fun)].name);
            } else
                result = F == field::cnt ? 0 : nil;
        } else if constexpr (F == field::sym)
            result = nil;
        else
            fail("PROGRAM-ERROR");
        if constexpr (F == field::exp)
            result = source_body(result);
        if constexpr (F == field::cnt)
            give_count(result);
        else
            give(result);
    }

    template<field F>
    void set_closure(word fun, word value)
    {
        if constexpr (F == field::sym)
            if (value != nil)
                require(value, tag::sym);
        if (tag_of(fun) == tag::fun)
            h.set<tag::fun, F>(fun, value);
        else if (tag_of(fun) == tag::mac)
            h.set<tag::mac, F>(fun, value);
        else
            fail("PROGRAM-ERROR");
        // Zig SET-FUNCTION-NAME! omits give, leaving the machine stuck.
        // Both closure setters finish and return the modified closure.
        give(fun);
    }

    template<field F>
    void set_symbol(word sym, word value)
    {
        require(sym, tag::sym);
        h.set<tag::sym, F>(sym, value);
        if constexpr (F == field::fun) {
            if (tag_of(value) == tag::fun)
                h.set<tag::fun, field::sym>(value, sym);
            if (tag_of(value) == tag::mac)
                h.set<tag::mac, field::sym>(value, sym);
        }
        give(value);
    }

    void set(word sym, word value)
    {
        give(lookup(sym, true, value));
    }

    void environment()
    {
        give(env);
    }

    void give_count(std::size_t count)
    {
        if (count > std::size_t{max_fixnum})
            fail("FIXNUM-OVERFLOW");
        give(fixnum(static_cast<std::int32_t>(count)));
    }

    template<tag T>
    void length(word x)
    {
        require(x, T);
        give_count(h.get<T, field::len>(x));
    }

    void vector(values xs)
    {
        give(h.newv32(xs));
    }

    std::size_t vector_index(word vec, word idx)
    {
        require(vec, tag::v32);
        const auto index = number(idx);
        if (index < 0 || std::size_t(index) >= h.v32slice(vec).size())
            fail("TYPE-MISMATCH", {vm.known("INTEGER"), idx});
        return static_cast<std::size_t>(index);
    }

    void vector_get(word vec, word idx)
    {
        const auto index = vector_index(vec, idx);
        give(h.v32slice(vec)[index]);
    }

    void vector_set(word vec, word idx, word value)
    {
        const auto index = vector_index(vec, idx);
        h.v32set(vec, index, value);
        give(value);
    }

    void record(word type, values slots)
    {
        if (code_operation_of(type))
            fail("INVALID-CODE", {type});
        std::vector<word> xs{type};
        xs.insert(xs.end(), slots.begin(), slots.end());
        give(h.new_words<tag::rec>(xs));
    }

    void is_record(word x)
    {
        give(tag_of(x) == tag::rec ? t : nil);
    }

    void record_type(word r)
    {
        require(r, tag::rec);
        give(h.words<tag::rec>(r)[0]);
    }

    void record_length(word r)
    {
        require(r, tag::rec);
        give_count(h.words<tag::rec>(r).size() - 1);
    }

    // Slot indices start after the type word.
    std::size_t record_index(word r, word idx)
    {
        require(r, tag::rec);
        const auto index = number(idx);
        if (index < 0
            || std::size_t(index) + 1 >= h.words<tag::rec>(r).size())
            fail("TYPE-MISMATCH", {vm.known("INTEGER"), idx});
        return static_cast<std::size_t>(index) + 1;
    }

    void record_get(word r, word idx)
    {
        const auto index = record_index(r, idx);
        give(h.words<tag::rec>(r)[index]);
    }

    void record_set(word r, word idx, word value)
    {
        if (lowered_code(r))
            fail("IMMUTABLE-CODE", {r});
        const auto index = record_index(r, idx);
        h.set_word<tag::rec>(r, index, value);
        give(value);
    }

    void vector_append(values xs)
    {
        // Accumulate outside the guest pool: allocating the destination
        // cannot invalidate any source slice, even when xs alias.
        std::vector<word> result;
        for (auto x : xs) {
            require(x, tag::v32);
            const auto piece = h.v32slice(x);
            result.insert(result.end(), piece.begin(), piece.end());
        }
        give(h.newv32(result));
    }

    void vector_from_list(word xs)
    {
        with_list(xs, [&](values items) { give(h.newv32(items)); });
    }

    std::string_view string(word x)
    {
        require(x, tag::v08);
        return h.v08slice(x);
    }

    void string_equal(word x, word y)
    {
        // Validate both before borrowing; fail() can grow the byte pool.
        require(x, tag::v08);
        require(y, tag::v08);
        give(h.v08slice(x) == h.v08slice(y) ? t : nil);
    }

    void string_append(values xs)
    {
        std::string result;
        for (auto x : xs)
            result.append(string(x));
        give(h.newv08(result));
    }

    void string_search(word x, word y)
    {
        require(x, tag::v08);
        require(y, tag::v08);
        const auto pos = h.v08slice(x).find(h.v08slice(y));
        if (pos == std::string_view::npos)
            give(nil);
        else
            give_count(pos);
    }

    void string_slice(word x, word i, word j)
    {
        require(x, tag::v08);
        const auto start = number(i), end = number(j);
        const auto bytes = h.v08slice(x);
        if (start < 0 || end < start || std::size_t(end) > bytes.size()) {
            give_count(bytes.size());
            fail("BOUNDS-ERROR", {x, i, j, val});
        }
        give(h.newv08(bytes.substr(start, end - start)));
    }

    void string_uppercase(word x)
    {
        std::string result{string(x)};
        for (auto & c : result)
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
        give(h.newv08(result));
    }

    void print_to_string(word x)
    {
        give(h.newv08(print(h, x, vm.current_package())));
    }

    template<bool Many>
    void read_string(word x)
    {
        reader input{h, vm, string(x)};
        try {
            if constexpr (Many) {
                std::vector<word> forms;
                while (auto form = input.next())
                    forms.push_back(*form);
                give(list(h, forms));
            } else {
                const auto form = input.next();
                if (!form)
                    fail("END-OF-FILE");
                give(*form);
            }
        } catch (const read_error & error) {
            fail("READ-ERROR", {h.newv08(error.what())});
        }
    }

    void read_string_stream(word stream)
    {
        require(stream, tag::v32);
        const auto marker = vm.known("STRING-INPUT-STREAM");
        const auto fields = h.v32slice(stream);
        const bool named = fields.size() == 5;
        if ((!named && fields.size() != 3) || fields[0] != marker)
            fail("INVALID-STRING-INPUT-STREAM", {stream});
        const auto offset_word = fields[1], text = fields[2];
        const auto offset = number(offset_word);
        const auto bytes = string(text);
        if (offset < 0 || std::size_t(offset) > bytes.size())
            fail("BOUNDS-ERROR", {text, offset_word});
        // Own source/name before parsing can grow the byte/word pools.
        // The full source keeps diagnostics absolute across successive
        // reads.
        reader input{
            h,
            vm,
            bytes,
            named ? string(fields[3]) : "<string>",
            std::size_t(offset)};
        try {
            const auto form = input.next();
            const auto end = input.position();
            if (end > std::size_t(max_fixnum))
                fail("FIXNUM-OVERFLOW");
            h.v32set(stream, 1, fixnum(static_cast<std::int32_t>(end)));
            if (named)
                h.v32set(
                    stream,
                    4,
                    h.newv08(input.location(input.form_position())));
            give(form ? h.cons(*form, nil) : nil);
        } catch (const read_error & error) {
            // On failure leave the cursor untouched; the error identifies
            // its exact source location rather than the preceding form.
            if (named)
                h.v32set(stream, 4, h.newv08(input.location(error.offset)));
            fail("READ-ERROR", {h.newv08(error.what())});
        }
    }

    void find_package(word name)
    {
        give(vm.find_package(string(name)));
    }

    void packages()
    {
        // Return a fresh spine; guest pair mutation cannot damage the
        // evaluator's package registry.
        with_list(
            vm.packages_.get(), [&](values xs) { give(list(h, xs)); });
    }

    void define_package(word name)
    {
        const auto text = string(name);
        if (vm.find_package(text) != nil)
            fail("PACKAGE-EXISTS", {name});
        give(vm.define_package(text));
    }

    void package_uses(word pkg, word uses)
    {
        require(pkg, tag::pkg);
        with_list(uses, [&](values xs) {
            for (auto used : xs)
                require(used, tag::pkg);
        });
        h.set<tag::pkg, field::use>(pkg, uses);
        give(pkg);
    }

    void defpackage(word name, word uses)
    {
        require(name, tag::sym);
        with_list(uses, [&](values xs) {
            for (auto used : xs)
                require(used, tag::pkg);
        });
        define_package(h.get<tag::sym, field::str>(name));
        h.set<tag::pkg, field::use>(val, uses);
    }

    void in_package(word name)
    {
        require(name, tag::sym);
        const auto pkg =
            vm.find_package(string(h.get<tag::sym, field::str>(name)));
        if (pkg == nil)
            fail("UNDEFINED-PACKAGE", {name});
        vm.current_.set(pkg);
        give(pkg);
    }

    void intern(word name, word pkg)
    {
        require(pkg, tag::pkg);
        try {
            give(vm.intern(string(name), pkg));
        } catch (const std::invalid_argument &) {
            fail("INVALID-PACKAGE-USES", {pkg});
        }
    }

    void fresh_symbol()
    {
        // Unlike reference clock/random keys, these are deterministic and
        // need no host capability. Skip names explicitly interned by Lisp.
        // Keep keys.zig's epoch date and little-endian 48-bit ZB32 spelling
        // to preserve the reference key representation.
        constexpr std::string_view alphabet{
            "YBNDRFG8EJKMCPQXOT1UWISZA345H769"};
        while (vm.next_key_ < (std::uint64_t{1} << 48) - 1) {
            const auto serial = ++vm.next_key_;
            std::string name{"~20220101.YYYYYYYYYY"};
            for (unsigned i = 0; i < 10; ++i)
                name[10 + i] = alphabet[(serial >> (5 * i)) & 31];
            auto found = false;
            auto cur = h.get<tag::pkg, field::sym>(vm.keys_.get());
            (void) scan_count(cur);
            while (cur != nil) {
                const auto [x, rest] = h.read<tag::duo>(cur);
                cur = rest;
                require(x, tag::sym);
                if (string(h.get<tag::sym, field::str>(x)) == name) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            const auto key = vm.intern(name, vm.keys_.get());
            h.set<tag::sym, field::val>(key, key);
            give(key);
            return;
        }
        fail("KEY-SPACE-EXHAUSTED");
    }

    void is_key(word x)
    {
        give(
            tag_of(x) == tag::sym
                    && h.get<tag::sym, field::pkg>(x) == vm.keys_.get()
                ? t
                : nil);
    }

    void make_pin(word x)
    {
        give(h.make_pin(x));
    }

    void release_pin(word x)
    {
        require(x, tag::pin);
        h.free_pin(x);
        give(nil);
    }

    void run(word expression)
    {
        give(vm.start(expression, env));
    }

    void step_run(word target)
    {
        require(target, tag::run);
        if (std::ranges::find(active_runs, target) != active_runs.end())
            fail("ACTIVE-EVALUATOR", {target});
        step_target = target;
        give(nil);
    }

    void gc()
    {
        vm.collect_ = true;
        give(nil);
    }

    void run_expression(word run)
    {
        require(run, tag::run);
        const auto expression = h.get<tag::run, field::exp>(run);
        give(
            expression == nah
                ? h.cons(vm.known("VAL"), h.get<tag::run, field::val>(run))
                : h.cons(vm.known("EXP"), expression));
    }

    // Ordinary frames end at TOP. Only dynamic boundaries link segments;
    // their ARG is [handler-or-binding-value, suspended-outer-segment].
    struct control_context
    {
        word way = top;
        word meta = top;
    };

    void install(control_context ctx)
    {
        assert(depth == 0);
        way = ctx.way;
        meta = ctx.meta;
    }

    word boundary(word kind, word key, word value)
    {
        flush(cache_flush::boundary);
        return h.make<tag::ktx>(
            {meta, env, kind, key, h.newv32(std::array{value, way})});
    }

    control_context outer(const row<tag::ktx> & entry)
    {
        return {
            h.v32slice(entry[column_index<tag::ktx, field::arg>()])[1],
            entry[column_index<tag::ktx, field::hop>()]};
    }

    word snapshot(control_context ctx)
    {
        h.freeze_continuations();
        if (ctx.meta == top)
            return ctx.way;
        return h.make<tag::ktx>(
            {top, nil, vm.known("CONTINUATION"), ctx.way, ctx.meta});
    }

    control_context context(word ptr)
    {
        if (ptr == top)
            return {};
        require(ptr, tag::ktx);
        const auto [hop, saved_env, fun, acc, arg] = h.read<tag::ktx>(ptr);
        if (fun == vm.known("CONTINUATION"))
            return {acc, arg};
        return {ptr, top};
    }

    // Copy only the meta prefix; ordinary frames and lexical store stay
    // shared. Boundary payloads are immutable (SET! replaces them).
    word append_meta(word source, word stop, word tail)
    {
        std::vector<word> entries;
        for (auto cur = source; cur != stop;
             cur = h.get<tag::ktx, field::hop>(cur))
            entries.push_back(cur);
        auto result = tail;
        for (auto i = entries.size(); i != 0; --i) {
            auto entry = h.read<tag::ktx>(entries[i - 1]);
            entry[column_index<tag::ktx, field::hop>()] = result;
            result = h.make<tag::ktx>(entry);
        }
        return result;
    }

    control_context compose(control_context captured)
    {
        flush(cache_flush::capture);
        if (captured.way == top && captured.meta == top)
            return {way, meta};
        h.freeze_continuations();
        // Tail resumes must not accumulate empty return boundaries.
        const auto tail =
            way == top ? meta : boundary(vm.known("RESUME"), nil, nil);
        return {captured.way, append_meta(captured.meta, top, tail)};
    }

    word find_prompt(word source, word prompt_tag)
    {
        auto * p = h.profiling();
        if (p)
            ++p->continuation_searches;
        for (auto cur = source; cur != top;) {
            if (p)
                ++p->continuation_boundaries;
            const auto [hop, saved_env, fun, acc, arg] =
                h.read<tag::ktx>(cur);
            if (fun == vm.known("PROMPT") && acc == prompt_tag)
                return cur;
            cur = hop;
        }
        return top;
    }

    void send_from(
        control_context source,
        word prompt_tag,
        word value,
        word fallback,
        bool compose_outside)
    {
        const auto prompt = find_prompt(source.meta, prompt_tag);
        if (prompt == top) {
            if (fallback == nah)
                fail("UNHANDLED-ERROR", {prompt_tag, value});
            give(fallback);
            return;
        }
        if (auto * p = h.profiling())
            ++p->continuation_captures;
        h.freeze_continuations();
        const auto entry = h.read<tag::ktx>(prompt);
        const auto handler =
            h.v32slice(entry[column_index<tag::ktx, field::arg>()])[0];
        const auto inside =
            snapshot({source.way, append_meta(source.meta, prompt, top)});
        const auto outside = outer(entry);
        install(compose_outside ? compose(outside) : outside);
        const std::array args{value, inside};
        call(handler, args);
    }

    void send(word prompt_tag, word value, word fallback)
    {
        flush(cache_flush::capture);
        send_from({way, meta}, prompt_tag, value, fallback, false);
    }

    void unhandled_error(word value)
    {
        // A nonlocal raise may miss ERROR in the captured slice. Re-signal
        // at its caller, or terminate if there is no outside handler.
        send(vm.known("ERROR"), value, nah);
    }

    void
    send_to(word continuation, word prompt_tag, word value, word fallback)
    {
        flush(cache_flush::capture);
        send_from(context(continuation), prompt_tag, value, fallback, true);
    }

    void call_with_prompt(word prompt_tag, word thunk, word handler)
    {
        meta = boundary(vm.known("PROMPT"), prompt_tag, handler);
        way = top;
        call(thunk, {});
    }

    void call_with_binding(word sym, word value, word thunk)
    {
        require(sym, tag::sym);
        meta = boundary(vm.known("BINDING"), sym, value);
        way = top;
        call(thunk, {});
    }

    void get_cc()
    {
        flush(cache_flush::capture);
        give(snapshot({way, meta}));
    }

    void compose_continuation(word continuation)
    {
        give(snapshot(compose(context(continuation))));
    }

    void run_way(word run)
    {
        require(run, tag::run);
        give(snapshot(
            {h.get<tag::run, field::way>(run),
             h.get<tag::run, field::meta>(run)}));
    }

    // Preserve the flattened public view without traversing ordinary
    // frames during execution or capture. RESUME boundaries are invisible.
    row<tag::ktx> continuation_view(word ptr)
    {
        auto ctx = context(ptr);
        while (ctx.way == top && ctx.meta != top) {
            const auto entry = h.read<tag::ktx>(ctx.meta);
            const auto next = outer(entry);
            if (entry[column_index<tag::ktx, field::fun>()]
                != vm.known("RESUME"))
                return {
                    snapshot(next),
                    entry[column_index<tag::ktx, field::env>()],
                    entry[column_index<tag::ktx, field::fun>()],
                    entry[column_index<tag::ktx, field::acc>()],
                    h.v32slice(
                        entry[column_index<tag::ktx, field::arg>()])[0]};
            ctx = next;
        }
        require(ctx.way, tag::ktx);
        auto frame = h.read<tag::ktx>(ctx.way);
        auto & hop = frame[column_index<tag::ktx, field::hop>()];
        hop = snapshot({hop, ctx.meta});
        return frame;
    }

    template<field F>
    void ktx_part(word continuation)
    {
        give(continuation_view(continuation)[column_index<tag::ktx, F>()]);
    }

    void ktx_position(word continuation)
    {
        const auto acc = continuation_view(
            continuation)[column_index<tag::ktx, field::acc>()];
        const auto xs =
            tag_of(acc) == tag::v32 ? h.v32slice(acc) : values{};
        give(xs.empty() ? 0 : xs[0]);
    }

    void is_top(word continuation)
    {
        give(continuation == top ? t : nil);
    }

    void once()
    {
        if (auto * p = h.profiling())
            ++p->evaluator_steps;
        if (val != nah) {
            proceed();
            return;
        }
        switch (tag_of(exp)) {
        case tag::integer:
        case tag::v08:
        case tag::v32:
            give(exp);
            break;
        case tag::sys:
            if (exp != nil && exp != t)
                fail("INVALID-EXPRESSION", {exp});
            give(exp);
            break;
        case tag::sym:
            give(lookup(exp));
            break;
        case tag::duo:
            application(exp);
            break;
        case tag::rec:
            lowered(exp, lowered_operation(exp));
            break;
        default:
            fail("INVALID-EXPRESSION", {exp});
        }
    }
};

namespace {

std::span<const builtin> builtins()
{
    // Local identities, NOT Zig tape indices or a durable image ABI.
    static constexpr std::array table{
        builtin::bind<&eval_step::quote>("QUOTE", true),
        builtin::bind<&eval_step::function>("FUNCTION", true),
        builtin::bind<&eval_step::fn>("%FN", true),
        builtin::bind<&eval_step::macro_fn>("%MACRO-FN", true),
        builtin::bind<&eval_step::if_>("IF", true),
        builtin::bind<&eval_step::do_>("DO", true),
        builtin::bind<&eval_step::let>("LET", true),
        builtin::bind<&eval_step::add>("+"),
        builtin::bind<&eval_step::subtract>("-"),
        builtin::bind<&eval_step::multiply>("*"),
        builtin::bind<&eval_step::less>("<"),
        builtin::bind<&eval_step::greater>(">"),
        builtin::bind<&eval_step::eq>("EQ?"),
        builtin::bind<&eval_step::cons>("CONS"),
        builtin::bind<&eval_step::pair_part<field::car>>("HEAD"),
        builtin::bind<&eval_step::pair_part<field::cdr>>("TAIL"),
        builtin::bind<&eval_step::list_>("LIST"),
        builtin::bind<&eval_step::call>("CALL"),
        builtin::bind<&eval_step::apply>("APPLY"),
        builtin::bind<&eval_step::symbol_function>("SYMBOL-FUNCTION"),
        builtin::bind<&eval_step::set_symbol<field::fun>>(
            "SET-SYMBOL-FUNCTION!"),
        builtin::bind<&eval_step::set_symbol<field::val>>(
            "SET-SYMBOL-VALUE!"),
        builtin::bind<&eval_step::set>("%SET!"),
        builtin::bind<&eval_step::environment>("ENV"),
        builtin::bind<&eval_step::set_symbol<field::dyn>>(
            "SET-SYMBOL-DYNAMIC!"),
        builtin::bind<&eval_step::call_with_binding>("CALL-WITH-BINDING"),
        builtin::bind<&eval_step::call_with_prompt>("CALL-WITH-PROMPT"),
        builtin::bind<&eval_step::send>("SEND-WITH-DEFAULT!"),
        builtin::bind<&eval_step::send_to>("SEND-TO-WITH-DEFAULT!"),
        builtin::bind<&eval_step::get_cc>("GET/CC"),
        builtin::bind<&eval_step::compose_continuation>(
            "COMPOSE-CONTINUATION"),
        builtin::bind<&eval_step::ktx_part<field::hop>>(
            "KTX-HOP", false, true),
        builtin::bind<&eval_step::ktx_part<field::env>>(
            "KTX-ENV", false, true),
        builtin::bind<&eval_step::ktx_part<field::fun>>(
            "KTX-FUN", false, true),
        builtin::bind<&eval_step::ktx_part<field::acc>>(
            "KTX-ACC", false, true),
        builtin::bind<&eval_step::ktx_part<field::arg>>(
            "KTX-ARG", false, true),
        builtin::bind<&eval_step::ktx_position>("KTX-POS", false, true),
        builtin::bind<&eval_step::is_top>("TOP?", false, true),
        builtin::bind<&eval_step::eval>("EVAL"),
        builtin::bind<&eval_step::divide>("/"),
        builtin::bind<&eval_step::mod>("MOD"),
        builtin::bind<&eval_step::type_of>("TYPE-OF"),
        builtin::bind<&eval_step::prognify>("PROGNIFY"),
        builtin::bind<&eval_step::macroexpand_1>("MACROEXPAND-1"),
        builtin::bind<&eval_step::is_jet<false>>("JET?"),
        builtin::bind<&eval_step::is_jet<true>>("JET-CTL?"),
        builtin::bind<&eval_step::is_symbol>("SYMBOL?"),
        builtin::bind<&eval_step::symbol_part<field::str>>("SYMBOL-NAME"),
        builtin::bind<&eval_step::symbol_part<field::pkg>>(
            "SYMBOL-PACKAGE"),
        builtin::bind<&eval_step::closure_part<field::sym>>(
            "FUNCTION-NAME"),
        builtin::bind<&eval_step::closure_part<field::cnt>>(
            "FUNCTION-CALL-COUNT"),
        builtin::bind<&eval_step::closure_part<field::exp>>("CODE"),
        builtin::bind<&eval_step::closure_part<field::par>>(
            "FUNCTION-PARAMETERS"),
        builtin::bind<&eval_step::set_closure<field::exp>>("SET-CODE!"),
        builtin::bind<&eval_step::set_closure<field::sym>>(
            "SET-FUNCTION-NAME!"),
        builtin::bind<&eval_step::set_field<tag::duo, field::car>>(
            "SET-HEAD!"),
        builtin::bind<&eval_step::set_field<tag::duo, field::cdr>>(
            "SET-TAIL!"),
        builtin::bind<&eval_step::vector>("VECTOR"),
        builtin::bind<&eval_step::vector_get>("VECTOR-GET"),
        builtin::bind<&eval_step::vector_set>("VECTOR-SET!"),
        builtin::bind<&eval_step::length<tag::v32>>("VECTOR-LENGTH"),
        builtin::bind<&eval_step::vector_append>("VECTOR-APPEND"),
        builtin::bind<&eval_step::vector_from_list>("VECTOR-FROM-LIST"),
        builtin::bind<&eval_step::record>("RECORD"),
        builtin::bind<&eval_step::is_record>("RECORD?"),
        builtin::bind<&eval_step::record_type>("RECORD-TYPE"),
        builtin::bind<&eval_step::record_length>("RECORD-LENGTH"),
        builtin::bind<&eval_step::record_get>("RECORD-GET"),
        builtin::bind<&eval_step::record_set>("RECORD-SET!"),
        builtin::bind<&eval_step::code_operations_>("CODE-OPERATIONS"),
        builtin::bind<&eval_step::make_code>("MAKE-CODE"),
        builtin::bind<&eval_step::length<tag::v08>>("BYTE-SIZE"),
        builtin::bind<&eval_step::length<tag::v08>>("STRING-LENGTH"),
        builtin::bind<&eval_step::string_equal>("STRING-EQUAL?"),
        builtin::bind<&eval_step::string_append>("STRING-APPEND"),
        builtin::bind<&eval_step::string_search>("STRING-SEARCH"),
        builtin::bind<&eval_step::string_slice>("STRING-SLICE"),
        builtin::bind<&eval_step::string_uppercase>("STRING-TO-UPPERCASE"),
        builtin::bind<&eval_step::print_to_string>("PRINT-TO-STRING"),
        builtin::bind<&eval_step::read_string<false>>("READ-FROM-STRING"),
        builtin::bind<&eval_step::read_string<true>>(
            "READ-MANY-FROM-STRING"),
        builtin::bind<&eval_step::read_string_stream>(
            "READ-FROM-STRING-STREAM!"),
        builtin::bind<&eval_step::unhandled_error>("UNHANDLED-ERROR"),
        builtin::bind<&eval_step::fresh_symbol>("GENKEY!"),
        builtin::bind<&eval_step::fresh_symbol>("FRESH-SYMBOL!"),
        builtin::bind<&eval_step::is_key>("KEY?"),
        builtin::bind<&eval_step::make_pin>("MAKE-PINNED-VALUE"),
        builtin::bind<&eval_step::release_pin>("RELEASE-PINNED-VALUE!"),
        builtin::bind<&eval_step::find_package>("FIND-PACKAGE"),
        builtin::bind<&eval_step::packages>("PACKAGES"),
        builtin::bind<&eval_step::get_field<tag::pkg, field::nam>>(
            "PACKAGE-NAME"),
        builtin::bind<&eval_step::get_field<tag::pkg, field::sym>>(
            "PACKAGE-SYMBOLS"),
        builtin::bind<&eval_step::get_field<tag::pkg, field::use>>(
            "PACKAGE-USES"),
        builtin::bind<&eval_step::package_uses>("PACKAGE-SET-USES!"),
        builtin::bind<&eval_step::define_package>("%DEFPACKAGE"),
        builtin::bind<&eval_step::defpackage>("DEFPACKAGE", true),
        builtin::bind<&eval_step::in_package>("IN-PACKAGE", true),
        builtin::bind<&eval_step::intern>("INTERN"),
        builtin::bind<&eval_step::run>("RUN"),
        builtin::bind<&eval_step::step_run>("STEP!", false, true),
        builtin::bind<&eval_step::gc>("GC"),
        builtin::bind<&eval_step::run_expression>("RUN-EXP", false, true),
        builtin::bind<&eval_step::run_way>("RUN-WAY", false, true),
        builtin::bind<&eval_step::get_field<tag::run, field::val>>(
            "RUN-VAL", false, true),
        builtin::bind<&eval_step::get_field<tag::run, field::err>>(
            "RUN-ERR", false, true),
    };
    static_assert(
        [] {
            for (std::size_t i = 0; i < table.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (table[i].name == table[j].name)
                        return false;
            return true;
        }(),
        "builtin names are tape identities and must be unique");
    return table;
}

} // namespace

evaluation evaluator::step(word run)
{
    return execute(run, 1, false);
}

evaluation evaluator::advance(word run, std::size_t budget)
{
    return execute(run, budget, true);
}

evaluation evaluator::execute(word run, std::size_t budget, bool poll_gc)
{
    // STEP! can itself step another run. Commit each row before dispatching
    // the next, without growing the native stack or collecting scratch
    // words. Ordinary transitions fit on the stack; only unusually deep
    // STEP! chains need to grow native scratch storage.
    std::array<word, 16> local;
    std::optional<nxtrt::rack<word>> overflow;
    std::span<word> active = local;
    auto result = status(run);
    while (budget != 0 && result == evaluation::runnable
           && !(poll_gc && collect_)) {
        std::size_t depth = 0;
        auto current = run;
        while (status(current) == evaluation::runnable) {
            if (depth == active.size()) {
                nxtrt::rack<word> grown{2 * depth};
                std::uninitialized_copy_n(
                    active.data(), depth, grown.data());
                overflow = std::move(grown);
                active = {overflow->data(), overflow->size()};
            }
            std::construct_at(active.data() + depth++, current);
            const auto [exp, val, err, env, way, meta] =
                heap_.read<tag::run>(current);
            eval_step s{
                *this,
                heap_,
                exp,
                val,
                err,
                env,
                way,
                meta,
                active.first(depth)};
            if (auto * p = heap_.profiling())
                ++p->evaluator_batches;
            do {
                s.begin_transition();
                try {
                    s.once();
                } catch (const condition & c) {
                    try {
                        s.flush(cache_flush::condition);
                        s.send(known("ERROR"), c.value, nah);
                    } catch (const condition & unhandled) {
                        s.err = unhandled.value;
                    }
                }
                if (current != run)
                    break; // A nested STEP! always takes just one step.
                --budget;
            } while (budget != 0 && s.step_target == nil
                     && s.state() == evaluation::runnable
                     && !(poll_gc && collect_));
            s.commit();
            if (s.step_target == nil)
                break;
            current = s.step_target;
        }
        result = status(run);
    }
    return result;
}

} // namespace wisp
