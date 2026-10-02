// SPDX-License-Identifier: AGPL-3.0-or-later
// Adapted from mbrock/wisp core/sexp-dump.zig at
// 223535633179cdf2a49391820bdab16a5db5bf4e (2026-10-01).
// Wisp is free software under the GNU Affero General Public License,
// version 3 or later, without any warranty, including merchantability or
// fitness for a particular purpose. See <https://www.gnu.org/licenses/>.
#include "wisp/printer.hpp"

#include <unordered_set>

namespace wisp {
namespace {

class compact_printer
{
    enum class action { value, tail, elements, leave, close_list };

    struct command
    {
        action kind;
        word value = nil;
        std::size_t index = 0;
    };

    const heap & h_;
    word current_;
    std::string out_;
    std::vector<command> pending_;
    std::unordered_set<word> active_;

    // Only path membership counts as a cycle. Keeping cdr-chain conses
    // active until their tails finish also catches cycles through cars.
    bool enter(word x)
    {
        if (!active_.insert(x).second) {
            out_ += "#<CYCLE>";
            return false;
        }
        pending_.push_back({action::leave, x});
        return true;
    }

    void cons(word x)
    {
        const auto [car, cdr] = h_.read<tag::duo>(x);
        pending_.push_back({action::tail, cdr});
        pending_.push_back({action::value, car});
    }

    void string(std::string_view bytes)
    {
        out_ += '"';
        for (char c : bytes) {
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            default:
                out_ += c;
                break;
            }
        }
        out_ += '"';
    }

    void symbol(word x)
    {
        const auto package = h_.get<tag::sym, field::pkg>(x);
        if (package == nil) {
            out_ += "#:";
        } else {
            const auto name =
                h_.v08slice(h_.get<tag::pkg, field::nam>(package));
            if (name == "KEYWORD") {
                out_ += ':';
            } else if (
                name != "KEY"
                && (current_ == nil ? name != "WISP"
                                    : package != current_)) {
                out_ += name;
                out_ += ':';
            }
        }
        out_ += h_.v08slice(h_.get<tag::sym, field::str>(x));
    }

    template<tag T>
    void function(word x)
    {
        const auto sym = h_.get<T, field::sym>(x);
        if (sym == nil) {
            out_ += T == tag::fun ? "#<ANONYMOUS-FUNCTION>"
                                  : "#<ANONYMOUS-MACRO>";
        } else {
            // The reference deliberately omits the symbol's package.
            out_ += "#'";
            out_ += h_.v08slice(h_.get<tag::sym, field::str>(sym));
        }
    }

    void diagnostic(std::string_view name, word payload)
    {
        out_ += "#<";
        out_ += name;
        out_ += ' ';
        out_ += std::to_string(payload);
        out_ += '>';
    }

    // The type word itself, or a type record's first slot (its name).
    word record_name(word r) const noexcept
    {
        const auto type = h_.words<tag::rec>(r)[0];
        if (tag_of(type) == tag::rec && h_.words<tag::rec>(type).size() > 1)
            return h_.words<tag::rec>(type)[1];
        return type;
    }

    // A type record's second slot may list one symbol per slot, as a
    // DEFSTRUCT descriptor does; then slots print as :NAME value pairs.
    word slot_name(word r, std::size_t slot) const noexcept
    {
        const auto xs = h_.words<tag::rec>(r);
        const auto type = xs[0];
        if (tag_of(type) != tag::rec || h_.words<tag::rec>(type).size() < 3)
            return nil;
        auto names = h_.words<tag::rec>(type)[2];
        std::size_t count = 0;
        word found = nil;
        while (tag_of(names) == tag::duo && count < xs.size()) {
            const auto [name, rest] = h_.read<tag::duo>(names);
            if (tag_of(name) != tag::sym)
                return nil;
            if (++count == slot)
                found = name;
            names = rest;
        }
        return names == nil && count + 1 == xs.size() ? found : nil;
    }

    template<tag T>
    void fields(word x, std::size_t i)
    {
        if (i == schema<T>::columns.size())
            return;
        out_ += ' ';
        out_ += schema<T>::columns[i].name;
        out_ += '=';
        pending_.push_back({action::elements, x, i + 1});
        pending_.push_back({action::value, h_.read<T>(x)[i]});
    }

    void value(word x)
    {
        switch (tag_of(x)) {
        case tag::integer:
            out_ += std::to_string(integer(x));
            break;
        case tag::sys:
            switch (x) {
            case nil:
                out_ += "NIL";
                break;
            case t:
                out_ += 'T';
                break;
            case nah:
                out_ += "#<NAH>";
                break;
            case top:
                out_ += "#<TOP>";
                break;
            case zap:
                out_ += "#<ZAP>";
                break;
            default:
                diagnostic("sys", payload_of(x));
                break;
            }
            break;
        case tag::sym:
            symbol(x);
            break;
        case tag::v08:
            string(h_.v08slice(x));
            break;
        case tag::duo:
            if (enter(x)) {
                out_ += '(';
                pending_.push_back({action::close_list});
                cons(x);
            }
            break;
        case tag::rec:
            if (enter(x)) {
                out_ += "#S(";
                pending_.push_back({action::elements, x, 1});
                pending_.push_back({action::value, record_name(x)});
            }
            break;
        case tag::v32:
        case tag::ktx:
        case tag::run:
            if (enter(x)) {
                out_ += tag_of(x) == tag::v32   ? "#<"
                        : tag_of(x) == tag::ktx ? "<%ktx"
                                                : "<run";
                pending_.push_back({action::elements, x});
            }
            break;
        case tag::pkg:
            out_ += "<package>";
            break;
        case tag::fun:
            function<tag::fun>(x);
            break;
        case tag::mac:
            function<tag::mac>(x);
            break;
        case tag::ext:
            diagnostic("ext", h_.get<tag::ext, field::idx>(x));
            break;
        case tag::jet:
            diagnostic("jet", payload_of(x));
            break;
        case tag::chr:
            diagnostic("chr", payload_of(x));
            break;
        case tag::pin:
            diagnostic("pin", payload_of(x));
            break;
        default:
            diagnostic("word", x);
            break;
        }
    }

public:
    compact_printer(const heap & h, word current)
        : h_(h)
        , current_(current)
    {
    }

    std::string run(word x)
    {
        pending_.push_back({action::value, x});
        while (!pending_.empty()) {
            const auto next = pending_.back();
            pending_.pop_back();
            const auto y = next.value;
            switch (next.kind) {
            case action::value:
                value(y);
                break;
            case action::tail:
                if (tag_of(y) == tag::duo) {
                    if (active_.contains(y)) {
                        out_ += " . #<CYCLE>";
                    } else {
                        out_ += ' ';
                        enter(y);
                        cons(y);
                    }
                } else if (y != nil) {
                    out_ += " . ";
                    pending_.push_back({action::value, y});
                }
                break;
            case action::elements:
                if (tag_of(y) == tag::rec) {
                    const auto xs = h_.words<tag::rec>(y);
                    if (next.index != xs.size()) {
                        out_ += ' ';
                        if (const auto name = slot_name(y, next.index);
                            name != nil) {
                            out_ += ':';
                            out_ += h_.v08slice(
                                h_.get<tag::sym, field::str>(name));
                            out_ += ' ';
                        }
                        pending_.push_back(
                            {action::elements, y, next.index + 1});
                        pending_.push_back({action::value, xs[next.index]});
                    }
                } else if (tag_of(y) == tag::v32) {
                    const auto xs = h_.v32slice(y);
                    if (next.index != xs.size()) {
                        if (next.index != 0)
                            out_ += ' ';
                        pending_.push_back(
                            {action::elements, y, next.index + 1});
                        pending_.push_back({action::value, xs[next.index]});
                    }
                } else if (tag_of(y) == tag::ktx) {
                    fields<tag::ktx>(y, next.index);
                } else {
                    fields<tag::run>(y, next.index);
                }
                break;
            case action::leave:
                active_.erase(y);
                if (tag_of(y) == tag::rec)
                    out_ += ')';
                else if (tag_of(y) != tag::duo)
                    out_ += '>';
                break;
            case action::close_list:
                out_ += ')';
                break;
            }
        }
        return std::move(out_);
    }
};

} // namespace

std::string print(const heap & h, word x, word current)
{
    return compact_printer{h, current}.run(x);
}

} // namespace wisp
