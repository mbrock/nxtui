// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#include "wisp/eval.hpp"

#include <initializer_list>

namespace wisp {
namespace {

// Local builtin identities, NOT Zig tape indices or a durable image ABI.
enum class op : word {
    quote,
    function,
    fn,
    macro_fn,
    if_,
    do_,
    let,
    add,
    subtract,
    multiply,
    less,
    greater,
    eq,
    cons,
    head,
    tail,
    list,
    call,
    apply,
    symbol_function,
    set_function,
    set_value,
    set,
    env,
};

struct builtin
{
    std::string_view name;
    bool control;
    std::size_t minimum;
    std::size_t maximum;
};

constexpr auto many = std::size_t(-1);
constexpr std::array builtins{
    builtin{"QUOTE", true, 1, 1},
    builtin{"FUNCTION", true, 1, 1},
    builtin{"%FN", true, 3, 3},
    builtin{"%MACRO-FN", true, 2, 2},
    builtin{"IF", true, 3, 3},
    builtin{"DO", true, 0, many},
    builtin{"LET", true, 1, many},
    builtin{"+", false, 0, many},
    builtin{"-", false, 1, many},
    builtin{"*", false, 0, many},
    builtin{"<", false, 2, 2},
    builtin{">", false, 2, 2},
    builtin{"EQ?", false, 2, 2},
    builtin{"CONS", false, 2, 2},
    builtin{"HEAD", false, 1, 1},
    builtin{"TAIL", false, 1, 1},
    builtin{"LIST", false, 0, many},
    builtin{"CALL", false, 1, many},
    builtin{"APPLY", false, 2, 2},
    builtin{"SYMBOL-FUNCTION", false, 1, 1},
    builtin{"SET-SYMBOL-FUNCTION!", false, 2, 2},
    builtin{"SET-SYMBOL-VALUE!", false, 2, 2},
    builtin{"%SET!", false, 2, 2},
    builtin{"ENV", false, 0, 0},
};
static_assert(builtins.size() == word(op::env) + 1);

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

evaluator::evaluator(heap & storage)
    : heap_(storage)
    , base_(
          storage,
          storage.make<tag::pkg>({storage.newv08("WISP"), nil, nil}))
    , keywords_(
          storage,
          storage.make<tag::pkg>({storage.newv08("KEYWORD"), nil, nil}))
{
    for (word i = 0; i < builtins.size(); ++i)
        heap_.set<tag::sym, field::fun>(
            intern(builtins[i].name), immediate(tag::jet, i));
}

word evaluator::intern(std::string_view name, word package)
{
    const auto symbols = heap_.get<tag::pkg, field::sym>(package);
    for (auto cur = symbols; cur != nil;) {
        const auto [sym, next] = heap_.read<tag::duo>(cur);
        if (heap_.v08slice(heap_.get<tag::sym, field::str>(sym)) == name)
            return sym;
        cur = next;
    }
    const auto sym =
        heap_.make<tag::sym>({heap_.newv08(name), package, nah, nil, nil});
    heap_.set<tag::pkg, field::sym>(package, heap_.cons(sym, symbols));
    return sym;
}

word evaluator::intern(std::string_view name)
{
    if (name == "NIL")
        return nil;
    if (name == "T")
        return t;
    return intern(name, base_.get());
}

word evaluator::keyword(std::string_view name)
{
    return intern(name, keywords_.get());
}

word evaluator::start(word expression, word environment)
{
    return heap_.make<tag::run>({expression, nah, nil, environment, top});
}

evaluation evaluator::status(word run) const noexcept
{
    if (heap_.get<tag::run, field::err>(run) != nil)
        return evaluation::failed;
    if (heap_.get<tag::run, field::way>(run) == top
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
    word exp, val, err, env, way;

    [[noreturn]] void
    fail(std::string_view name, std::initializer_list<word> details = {})
    {
        std::vector<word> xs{vm.intern(name)};
        xs.insert(xs.end(), details.begin(), details.end());
        throw condition{h.newv32(xs)};
    }

    void require(word x, tag type, std::string_view name)
    {
        if (tag_of(x) != type)
            fail("TYPE-MISMATCH", {vm.intern(name), x});
    }

    // Reject dotted and cyclic syntax instead of hanging in one host turn.
    std::vector<word> scan(word x)
    {
        std::vector<word> xs;
        auto slow = x;
        while (x != nil) {
            require(x, tag::duo, "CONS");
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

    void push(word fun, word acc, word arg)
    {
        way = h.make<tag::ktx>({way, env, fun, acc, arg});
    }

    word lookup(word sym, bool assign = false, word value = nil)
    {
        require(sym, tag::sym, "SYMBOL");
        if (!assign
            && h.get<tag::sym, field::pkg>(sym) == vm.keywords_.get())
            return sym;
        for (auto cur = env; cur != nil;) {
            const auto [scope, next] = h.read<tag::duo>(cur);
            const auto xs = h.v32slice(scope);
            for (std::size_t i = 0; i < xs.size(); i += 2) {
                if (xs[i] == sym) {
                    if (assign)
                        h.v32set(scope, i + 1, value);
                    return assign ? value : xs[i + 1];
                }
            }
            cur = next;
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
        if (body == nil) {
            give(nil);
            return;
        }
        const auto [first, rest] = h.read<tag::duo>(body);
        // The last form is in tail position, including a singleton DO.
        if (rest != nil)
            push(vm.intern("DO"), nil, rest);
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
            if (p == vm.intern("&OPTIONAL")) {
                optional = true;
                continue;
            }
            if (p == vm.intern("&REST") || p == vm.intern("&BODY")) {
                if (i + 2 != pars.size())
                    fail("INVALID-PARAMETERS", {parameters});
                require(pars[i + 1], tag::sym, "SYMBOL");
                scope.push_back(pars[i + 1]);
                scope.push_back(list(h, args.subspan(used)));
                used = args.size();
                break;
            }
            require(p, tag::sym, "SYMBOL");
            if (used == args.size() && !optional)
                fail(
                    "PROGRAM-ERROR",
                    {vm.intern("INVALID-ARGUMENT-COUNT"), fun});
            scope.push_back(p);
            scope.push_back(used < args.size() ? args[used++] : nil);
        }
        if (used != args.size())
            fail(
                "PROGRAM-ERROR",
                {vm.intern("INVALID-ARGUMENT-COUNT"), fun});
        env = h.cons(h.newv32(scope), captured);
        if (tag_of(fun) == tag::fun)
            h.set<tag::fun, field::cnt>(fun, count + 1);
        else
            h.set<tag::mac, field::cnt>(fun, count + 1);
        enter(body);
    }

    void call(word fun, std::span<const word> args)
    {
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
            push(vm.intern("EVAL"), nil, nil);
            call(fun, args);
        } else if (
            tag_of(fun) == tag::jet && payload_of(fun) < builtins.size()
            && builtins[payload_of(fun)].control) {
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
        const auto [hop, saved_env, fun, acc, arg] = h.read<tag::ktx>(way);
        env = saved_env;
        if (fun == vm.intern("DO")) {
            way = hop;
            sequence(arg);
        } else if (fun == vm.intern("IF")) {
            const auto [yes, no] = h.read<tag::duo>(arg);
            way = hop;
            enter(val == nil ? no : yes);
        } else if (fun == vm.intern("EVAL")) {
            way = hop;
            enter(val);
        } else if (fun == vm.intern("LET")) {
            // Reverse accumulator: name, value, name, ..., body.
            if (arg == nil) {
                auto xs = scan(acc);
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
                auto next_acc = h.cons(pair[0], h.cons(val, acc));
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
            auto vector = acc;
            if (vector == nil) {
                vector = h.filledv32(2 + scan(arg).size(), nil);
                h.v32set(vector, 0, 0);
                h.set<tag::ktx, field::acc>(way, vector);
            }
            const auto pos = h.v32slice(vector)[0];
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
        if (id >= builtins.size())
            fail("INVALID-FUNCTION", {jet});
        try {
            const auto & def = builtins[id];
            if (args.size() < def.minimum || args.size() > def.maximum)
                fail(
                    "PROGRAM-ERROR",
                    {vm.intern("INVALID-ARGUMENT-COUNT"), jet});
            invoke(op(id), args);
        } catch (const condition & c) {
            fail("BUILTIN-FAILURE", {jet, c.value});
        }
    }

    std::int64_t number(word x)
    {
        require(x, tag::integer, "INTEGER");
        return integer(x);
    }

    void invoke(op code, std::span<const word> a)
    {
        switch (code) {
        case op::quote:
            give(a[0]);
            break;
        case op::function:
            require(a[0], tag::sym, "SYMBOL");
            give(h.get<tag::sym, field::fun>(a[0]));
            break;
        case op::fn:
            give(h.make<tag::fun>({env, a[1], a[2], a[0], 0}));
            break;
        case op::macro_fn:
            give(h.make<tag::mac>({env, a[0], a[1], nil, 0}));
            break;
        case op::if_:
            push(vm.intern("IF"), nil, h.cons(a[1], a[2]));
            enter(a[0]);
            break;
        case op::do_:
            sequence(list(h, a));
            break;
        case op::let: {
            // All initializers use the caller's environment, left to right.
            const auto bindings = scan(a[0]);
            for (auto binding : bindings) {
                const auto pair = scan(binding);
                if (pair.size() != 2)
                    fail("INVALID-BINDING", {binding});
                require(pair[0], tag::sym, "SYMBOL");
            }
            const auto body =
                h.cons(vm.intern("DO"), list(h, a.subspan(1)));
            if (bindings.empty()) {
                enter(body);
                break;
            }
            const auto first = scan(bindings[0]);
            push(
                vm.intern("LET"),
                h.cons(first[0], h.cons(body, nil)),
                h.get<tag::duo, field::cdr>(a[0]));
            enter(first[1]);
            break;
        }
        case op::add:
        case op::subtract:
        case op::multiply: {
            std::int64_t result = code == op::multiply ? 1 : 0;
            std::size_t i = 0;
            if (code == op::subtract)
                result = number(a[i++]);
            for (; i < a.size(); ++i) {
                auto x = number(a[i]);
                if (code == op::add)
                    result += x;
                else if (code == op::subtract)
                    result -= x;
                else
                    result *= x;
                if (result < min_fixnum || result > max_fixnum)
                    fail("FIXNUM-OVERFLOW");
            }
            // Like Zig Wisp, unary subtraction is the identity, not
            // negation.
            give(fixnum(static_cast<std::int32_t>(result)));
            break;
        }
        case op::less:
            give(number(a[0]) < number(a[1]) ? t : nil);
            break;
        case op::greater:
            give(number(a[0]) > number(a[1]) ? t : nil);
            break;
        case op::eq:
            give(a[0] == a[1] ? t : nil);
            break;
        case op::cons:
            give(h.cons(a[0], a[1]));
            break;
        case op::head:
        case op::tail:
            if (a[0] == nil)
                give(nil);
            else {
                require(a[0], tag::duo, "CONS");
                give(h.read<tag::duo>(a[0])[code == op::head ? 0 : 1]);
            }
            break;
        case op::list:
            give(list(h, a));
            break;
        case op::call:
            call(a[0], a.subspan(1));
            break;
        case op::apply: {
            const auto args = scan(a[1]);
            call(a[0], args);
            break;
        }
        case op::symbol_function:
            if (tag_of(a[0]) == tag::sys)
                give(nil);
            else {
                require(a[0], tag::sym, "SYMBOL");
                give(h.get<tag::sym, field::fun>(a[0]));
            }
            break;
        case op::set_function:
            require(a[0], tag::sym, "SYMBOL");
            h.set<tag::sym, field::fun>(a[0], a[1]);
            if (tag_of(a[1]) == tag::fun)
                h.set<tag::fun, field::sym>(a[1], a[0]);
            if (tag_of(a[1]) == tag::mac)
                h.set<tag::mac, field::sym>(a[1], a[0]);
            give(a[1]);
            break;
        case op::set_value:
            require(a[0], tag::sym, "SYMBOL");
            h.set<tag::sym, field::val>(a[0], a[1]);
            give(a[1]);
            break;
        case op::set:
            give(lookup(a[0], true, a[1]));
            break;
        case op::env:
            give(env);
            break;
        }
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

evaluation evaluator::step(word run)
{
    if (status(run) != evaluation::runnable)
        return status(run);
    const auto [exp, val, err, env, way] = heap_.read<tag::run>(run);
    eval_step s{*this, heap_, exp, val, err, env, way};
    try {
        s.once();
    } catch (const condition & c) {
        s.err = c.value;
    }
    heap_.put<tag::run>(run, {s.exp, s.val, s.err, s.env, s.way});
    return status(run);
}

evaluation evaluator::advance(word run, std::size_t budget)
{
    auto state = status(run);
    while (budget-- != 0 && state == evaluation::runnable)
        state = step(run);
    return state;
}

} // namespace wisp
