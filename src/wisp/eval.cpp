// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#include "wisp/eval.hpp"
#include "wisp/printer.hpp"
#include "wisp/reader.hpp"

#include <concepts>
#include <functional>
#include <initializer_list>
#include <string>

namespace wisp {
struct eval_step;

namespace {

using values = std::span<const word>;

struct builtin
{
    std::string_view name;
    bool control;
    std::size_t minimum;
    std::size_t maximum;
    void (*invoke)(eval_step &, values);

    // A word consumes one argument; a final values parameter consumes the
    // rest. Signature, arity, and invocation cannot drift apart. No erased
    // function-pointer casts or compiler reflection are needed.
    template<auto Function>
    static consteval builtin
    bind(std::string_view name, bool control = false)
    {
        return bind<Function>(name, control, Function);
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
    static consteval builtin
    bind(std::string_view name, bool control, void (eval_step::*)(Args...))
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

// Index in known_names of each tag's TYPE-OF name, or npos when it has none.
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

// Scratch registers for exactly one transition. No guest allocation
// collects; no borrowed row or payload survives a call that can grow it.
struct eval_step
{
    evaluator & vm;
    heap & h;
    word exp, val, err, env, way, meta;
    values active_runs;
    word step_target = nil;

    [[noreturn]] void
    fail(known_name name, std::initializer_list<word> details = {})
    {
        std::vector<word> xs{vm.known(name)};
        xs.insert(xs.end(), details.begin(), details.end());
        throw condition{h.newv32(xs)};
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

    // Reject dotted and cyclic syntax instead of hanging in one host turn.
    std::vector<word> scan(word x)
    {
        std::vector<word> xs;
        auto slow = x;
        while (x != nil) {
            require(x, tag::duo);
            auto [car, cdr] = h.read<tag::duo>(x);
            xs.push_back(car);
            x = cdr;
            if (xs.size() % 2 == 0)
                slow = h.get<tag::duo, field::cdr>(slow);
            if (x != nil && x == slow)
                fail("CYCLIC-LIST");
        }
        return xs;
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

    void push(word fun, word acc, word arg)
    {
        way = h.make<tag::ktx>({way, env, fun, acc, arg});
    }

    word lookup(word sym, bool assign = false, word value = nil)
    {
        require(sym, tag::sym);
        if (!assign
            && h.get<tag::sym, field::pkg>(sym) == vm.keywords_.get())
            return sym;
        // Explicit lexical binders stay lexical even when the symbol's
        // dynamic declaration changes after a closure captures them.
        for (auto scope : scan(env)) {
            require(scope, tag::v32);
            const auto xs = h.v32slice(scope);
            if (xs.size() % 2 != 0)
                fail("INVALID-ENVIRONMENT", {scope});
            for (std::size_t i = 0; i < xs.size(); i += 2) {
                if (xs[i] == sym) {
                    if (assign)
                        h.v32set(scope, i + 1, value);
                    return assign ? value : xs[i + 1];
                }
            }
        }
        if (h.get<tag::sym, field::dyn>(sym) != nil) {
            for (auto cur = meta; cur != top;) {
                const auto [hop, saved_env, fun, name, binding] =
                    h.read<tag::ktx>(cur);
                if (fun == vm.known("BINDING") && name == sym) {
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
        (void) scan(body);
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
        const auto pars = scan(parameters);
        std::vector<word> scope;
        bool optional = false;
        std::size_t used = 0;
        for (std::size_t i = 0; i < pars.size(); ++i) {
            auto p = pars[i];
            if (p == vm.known("&OPTIONAL")) {
                optional = true;
                continue;
            }
            if (p == vm.known("&REST") || p == vm.known("&BODY")) {
                if (i + 2 != pars.size())
                    fail("INVALID-PARAMETERS", {parameters});
                require(pars[i + 1], tag::sym);
                scope.push_back(pars[i + 1]);
                scope.push_back(list(h, args.subspan(used)));
                used = args.size();
                break;
            }
            require(p, tag::sym);
            if (used == args.size() && !optional)
                fail(
                    "PROGRAM-ERROR",
                    {vm.known("INVALID-ARGUMENT-COUNT"), fun});
            scope.push_back(p);
            scope.push_back(used < args.size() ? args[used++] : nil);
        }
        if (used != args.size())
            fail(
                "PROGRAM-ERROR",
                {vm.known("INVALID-ARGUMENT-COUNT"), fun});
        env = h.cons(h.newv32(scope), captured);
        if (tag_of(fun) == tag::fun)
            h.set<tag::fun, field::cnt>(fun, count + 1);
        else
            h.set<tag::mac, field::cnt>(fun, count + 1);
        enter(body);
    }

    void call(word fun, std::span<const word> args)
    {
        if (tag_of(fun) == tag::ktx || fun == top) {
            if (args.size() != 1)
                fail(
                    "PROGRAM-ERROR",
                    {vm.known("CONTINUATION-CALL-ERROR")});
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
        const auto args = scan(arguments);
        if (tag_of(fun) == tag::mac) {
            push(vm.known("EVAL"), nil, nil);
            call(fun, args);
        } else if (
            tag_of(fun) == tag::jet && payload_of(fun) < builtins().size()
            && builtins()[payload_of(fun)].control) {
            call(fun, args);
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
        if (way == top) {
            const auto entry = h.read<tag::ktx>(meta);
            install(outer(entry));
            env = entry[column_index<tag::ktx, field::env>()];
            return;
        }
        const auto [hop, saved_env, fun, acc, arg] = h.read<tag::ktx>(way);
        env = saved_env;
        if (fun == vm.known("DO")) {
            way = hop;
            sequence(arg);
        } else if (fun == vm.known("IF")) {
            require(arg, tag::duo);
            const auto [yes, no] = h.read<tag::duo>(arg);
            way = hop;
            enter(val == nil ? no : yes);
        } else if (fun == vm.known("EVAL")) {
            way = hop;
            enter(val);
        } else if (fun == vm.known("LET")) {
            // Reverse accumulator: name, value, name, ..., body.
            auto xs = scan(acc);
            if (xs.empty() || xs.size() % 2 != 0)
                fail("INVALID-CONTINUATION", {way});
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
                require(xs[i], tag::sym);
            (void) scan(arg);
            if (arg == nil) {
                const auto body = xs.back();
                xs.pop_back();
                // The newest name takes val; earlier entries are stored
                // value/name, so rotate into name/value pairs.
                xs.insert(xs.begin(), val);
                for (std::size_t i = 0; i < xs.size(); i += 2)
                    std::swap(xs[i], xs[i + 1]);
                env = h.cons(h.newv32(xs), saved_env);
                way = hop;
                enter(body);
            } else {
                const auto [binding, rest] = h.read<tag::duo>(arg);
                const auto pair = scan(binding);
                if (pair.size() != 2)
                    fail("INVALID-BINDING", {binding});
                require(pair[0], tag::sym);
                auto next_acc = h.cons(pair[0], h.cons(val, acc));
                writable_frame();
                h.set<tag::ktx, field::acc>(way, next_acc);
                h.set<tag::ktx, field::arg>(way, rest);
                enter(pair[1]);
            }
        } else if (tag_of(fun) == tag::fun || tag_of(fun) == tag::jet) {
            if (acc == nil && arg == nil) {
                const std::array args{val};
                way = hop;
                call(fun, args);
                return;
            }
            const auto remaining = scan(arg);
            writable_frame();
            auto vector = h.get<tag::ktx, field::acc>(way);
            if (vector == nil) {
                vector = h.filledv32(2 + remaining.size(), nil);
                h.v32set(vector, 0, 0);
                h.set<tag::ktx, field::acc>(way, vector);
            }
            require(vector, tag::v32);
            const auto xs = h.v32slice(vector);
            if (xs.size() < 2 || xs[0] >= xs.size() - 1
                || remaining.size() != xs.size() - xs[0] - 2)
                fail("INVALID-CONTINUATION", {way});
            const auto pos = xs[0];
            h.v32set(vector, pos + 1, val);
            h.v32set(vector, 0, pos + 1);
            if (arg == nil) {
                // Binding and slice-taking builtins can grow the word pool.
                const auto slice = h.v32slice(vector).subspan(1);
                const std::vector<word> args(slice.begin(), slice.end());
                way = hop;
                call(fun, args);
            } else {
                const auto [first, rest] = h.read<tag::duo>(arg);
                h.set<tag::ktx, field::arg>(way, rest);
                enter(first);
            }
        } else {
            fail("INVALID-CONTINUATION", {way});
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

    void let(word clauses, values forms)
    {
        // All initializers use the caller's environment, left to right.
        const auto bindings = scan(clauses);
        for (auto binding : bindings) {
            const auto pair = scan(binding);
            if (pair.size() != 2)
                fail("INVALID-BINDING", {binding});
            require(pair[0], tag::sym);
        }
        const auto body = h.cons(vm.known("DO"), list(h, forms));
        if (bindings.empty()) {
            enter(body);
            return;
        }
        const auto first = scan(bindings[0]);
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
        const auto args = scan(arglist);
        call(fun, args);
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
        else
            give(type_symbol(tag_of(x)));
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
        const auto xs = scan(forms);
        give(
            xs.empty()       ? nil
            : xs.size() == 1 ? xs[0]
                             : h.cons(vm.known("DO"), forms));
    }

    void macroexpand_1(word form)
    {
        if (tag_of(form) == tag::duo) {
            const auto [head, tail] = h.read<tag::duo>(form);
            if (tag_of(head) == tag::sym) {
                const auto fun = h.get<tag::sym, field::fun>(head);
                if (tag_of(fun) == tag::mac) {
                    const auto args = scan(tail);
                    call(fun, args);
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
            give(list(h, scan(h.get<T, F>(x))));
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
        const auto items = scan(xs);
        give(h.newv32(items));
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
        if (fields.size() != 3 || fields[0] != marker)
            fail("INVALID-STRING-INPUT-STREAM", {stream});
        const auto offset_word = fields[1], text = fields[2];
        const auto offset = number(offset_word);
        const auto bytes = string(text);
        if (offset < 0 || std::size_t(offset) > bytes.size())
            fail("BOUNDS-ERROR", {text, offset_word});
        // Own the suffix before parsing can grow the byte/word pools.
        reader input{h, vm, bytes.substr(offset)};
        try {
            const auto form = input.next();
            const auto end = std::size_t(offset) + input.position();
            if (end > std::size_t(max_fixnum))
                fail("FIXNUM-OVERFLOW");
            h.v32set(stream, 1, fixnum(static_cast<std::int32_t>(end)));
            give(form ? h.cons(*form, nil) : nil);
        } catch (const read_error & error) {
            // On failure leave the cursor untouched. The source reader's
            // diagnostic offset is relative to this suffix.
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
        give(list(h, scan(vm.packages_.get())));
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
        for (auto used : scan(uses))
            require(used, tag::pkg);
        h.set<tag::pkg, field::use>(pkg, uses);
        give(pkg);
    }

    void defpackage(word name, word uses)
    {
        require(name, tag::sym);
        for (auto used : scan(uses))
            require(used, tag::pkg);
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
            for (auto x :
                 scan(h.get<tag::pkg, field::sym>(vm.keys_.get()))) {
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
        way = ctx.way;
        meta = ctx.meta;
    }

    word boundary(word kind, word key, word value)
    {
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
        if (captured.way == top && captured.meta == top)
            return {way, meta};
        h.freeze_continuations();
        // Tail resumes must not accumulate empty return boundaries.
        const auto tail =
            way == top ? meta : boundary(vm.known("RESUME"), nil, nil);
        return {captured.way, append_meta(captured.meta, top, tail)};
    }

    void writable_frame()
    {
        if (h.continuation_frozen(way))
            way = h.copy_continuation_frame(way);
    }

    word find_prompt(word source, word prompt_tag)
    {
        for (auto cur = source; cur != top;) {
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
        builtin::bind<&eval_step::ktx_part<field::hop>>("KTX-HOP"),
        builtin::bind<&eval_step::ktx_part<field::env>>("KTX-ENV"),
        builtin::bind<&eval_step::ktx_part<field::fun>>("KTX-FUN"),
        builtin::bind<&eval_step::ktx_part<field::acc>>("KTX-ACC"),
        builtin::bind<&eval_step::ktx_part<field::arg>>("KTX-ARG"),
        builtin::bind<&eval_step::ktx_position>("KTX-POS"),
        builtin::bind<&eval_step::is_top>("TOP?"),
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
        builtin::bind<&eval_step::step_run>("STEP!"),
        builtin::bind<&eval_step::gc>("GC"),
        builtin::bind<&eval_step::run_expression>("RUN-EXP"),
        builtin::bind<&eval_step::run_way>("RUN-WAY"),
        builtin::bind<&eval_step::get_field<tag::run, field::val>>(
            "RUN-VAL"),
        builtin::bind<&eval_step::get_field<tag::run, field::err>>(
            "RUN-ERR"),
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
    // STEP! can itself step another run. Commit each row before dispatching
    // the next, without growing the native stack or collecting scratch
    // words.
    std::vector<word> active;
    auto current = run;
    while (status(current) == evaluation::runnable) {
        active.push_back(current);
        const auto [exp, val, err, env, way, meta] =
            heap_.read<tag::run>(current);
        eval_step s{*this, heap_, exp, val, err, env, way, meta, active};
        try {
            s.once();
        } catch (const condition & c) {
            try {
                s.send(known("ERROR"), c.value, nah);
            } catch (const condition & unhandled) {
                s.err = unhandled.value;
            }
        }
        heap_.put<tag::run>(
            current, {s.exp, s.val, s.err, s.env, s.way, s.meta});
        if (s.step_target == nil)
            break;
        current = s.step_target;
    }
    return status(run);
}

evaluation evaluator::advance(word run, std::size_t budget)
{
    auto state = status(run);
    while (budget-- != 0 && state == evaluation::runnable && !collect_)
        state = step(run);
    return state;
}

} // namespace wisp
