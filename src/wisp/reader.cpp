// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/sexp-read.zig and core/sexp.zig,
// revision 223535633179cdf2a49391820bdab16a5db5bf4e.
#include "wisp/reader.hpp"

namespace wisp {
namespace {

bool digit(char32_t c)
{
    return c >= '0' && c <= '9';
}

bool constituent(char32_t c)
{
    if (c < 128) {
        if (c == ' ' || (c >= '\t' && c <= '\r'))
            return false;
        return std::string_view{"()[]{}\";#`',"}.find(char(c))
               == std::string_view::npos;
    }

    // Wisp's explicit letter/symbol/emoji ranges, not Unicode categories.
    static constexpr std::pair<char32_t, char32_t> ranges[]{
        {0x00c0, 0x02af},   {0x0370, 0x03ff},   {0x1f00, 0x1fff},
        {0x0400, 0x052f},   {0x0530, 0x077f},   {0x0900, 0x0d7f},
        {0x1e00, 0x1eff},   {0x1d00, 0x1dbf},   {0x1400, 0x167f},
        {0x1200, 0x137f},   {0x13a0, 0x13ff},   {0x16a0, 0x16ff},
        {0x10a0, 0x10ff},   {0x1100, 0x11ff},   {0x3040, 0x30ff},
        {0x31a0, 0x31ff},   {0xac00, 0xd7a3},   {0x4e00, 0x9fff},
        {0x3400, 0x4dbf},   {0x2000, 0x206f},   {0x20a0, 0x20cf},
        {0x2100, 0x218f},   {0x2190, 0x21ff},   {0x2200, 0x22ff},
        {0x2300, 0x23ff},   {0x2460, 0x24ff},   {0x25a0, 0x2bff},
        {0x1f300, 0x1f5ff}, {0x1f600, 0x1f64f}, {0x1f680, 0x1f6ff},
        {0x1f900, 0x1fad0}, {0x1fa70, 0x1faff},
    };
    for (auto [first, last] : ranges)
        if (c >= first && c <= last)
            return true;
    return false;
}

} // namespace

read_error::read_error(std::size_t at, std::string_view message)
    : std::runtime_error(
          "Wisp read at byte " + std::to_string(at) + ": "
          + std::string(message))
    , offset(at)
{
}

reader::reader(
    heap & storage, evaluator & language, std::string_view source)
    : heap_(storage)
    , evaluator_(language)
    , source_(source)
{
}

char32_t reader::peek() const
{
    if (position_ == source_.size())
        return eof;
    const auto lead = static_cast<unsigned char>(source_[position_]);
    if (lead < 0x80)
        return lead;

    const auto width = lead >= 0xc2 && lead <= 0xdf   ? 2u
                       : lead >= 0xe0 && lead <= 0xef ? 3u
                       : lead >= 0xf0 && lead <= 0xf4 ? 4u
                                                      : 0u;
    if (width == 0 || source_.size() - position_ < width)
        throw read_error(position_, "invalid or truncated UTF-8");
    char32_t c = lead & (0x7f >> width);
    for (unsigned i = 1; i < width; ++i) {
        const auto byte =
            static_cast<unsigned char>(source_[position_ + i]);
        if ((byte & 0xc0) != 0x80)
            throw read_error(position_, "invalid UTF-8 continuation");
        c = (c << 6) | (byte & 0x3f);
    }
    if ((width == 3 && c < 0x800) || (width == 4 && c < 0x10000)
        || (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff)
        throw read_error(position_, "invalid UTF-8 scalar");
    return c;
}

char32_t reader::take()
{
    const auto c = peek();
    if (c == eof)
        throw read_error(position_, "unexpected EOF");
    position_ += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    return c;
}

void reader::space()
{
    while (true) {
        switch (peek()) {
        case ' ':
        case '\n':
        case '\f':
            take();
            break;
        case ';':
            // Unlike the reference's unchecked EOF unwrap, a final comment
            // may end at EOF. CR and tab remain invalid between forms.
            while (peek() != eof && take() != '\n') {
            }
            break;
        default:
            return;
        }
    }
}

std::string reader::name()
{
    const auto start = position_;
    while (constituent(peek()))
        take();
    auto text = source_.substr(start, position_ - start);
    for (char & c : text)
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
    return text;
}

word reader::symbol()
{
    const auto start = position_;
    const auto text = name();
    const auto colon = text.find(':');
    try {
        if (colon == std::string::npos)
            return evaluator_.intern(text, evaluator_.current_package());
        const auto extra = text.find(':', colon + 1);
        if (extra != std::string::npos)
            throw read_error(start + extra, "colon in symbol name");
        const auto package = evaluator_.find_package(text.substr(0, colon));
        if (package == nil)
            throw read_error(start, "no such package");
        return evaluator_.intern(
            std::string_view{text}.substr(colon + 1), package);
    } catch (const std::invalid_argument & error) {
        throw read_error(start, error.what());
    }
}

word reader::number()
{
    word result = 0;
    unsigned digits = 0;
    while (digit(peek())) {
        const auto d = peek() - '0';
        // The reference computes 10^(length-1) in i31, so even an eleven-
        // digit spelling of zero overflows. Keep that lexical restriction.
        if (++digits > 10 || result > (word(max_fixnum) - d) / 10)
            throw read_error(position_, "integer overflow");
        result = result * 10 + d;
        take();
    }
    return result;
}

word reader::string()
{
    take(); // opening quote
    std::string text;
    while (true) {
        const auto start = position_;
        const auto c = take();
        if (c == '"')
            return heap_.newv08(text);
        if (c != '\\') {
            text.append(source_, start, position_ - start);
            continue;
        }
        const auto escape = position_;
        switch (take()) {
        case 'n':
            text += '\n';
            break;
        case '"':
            text += '"';
            break;
        case '\\':
            text += '\\';
            break;
        default:
            throw read_error(escape, "bad string escape");
        }
    }
}

std::optional<word> reader::next()
{
    enum class kind { list, vector, quote, tail, close };

    struct frame
    {
        kind type;
        word suffix = nil; // quote operator or dotted tail
        std::vector<word> items{};
    };

    // Explicit frames make flat and deeply nested input independent of the
    // native stack. No collection occurs while these unrooted words live.
    std::vector<frame> stack;
    while (true) {
        space();
        const auto c = peek();
        if (c == eof) {
            if (stack.empty())
                return std::nullopt;
            throw read_error(position_, "unexpected EOF");
        }

        word value;
        if (!stack.empty()
            && (stack.back().type == kind::close
                || (stack.back().type == kind::list && c == ')')
                || (stack.back().type == kind::vector && c == ']'))) {
            auto & f = stack.back();
            if (f.type == kind::close && c != ')')
                throw read_error(
                    position_, "expected ')' after dotted tail");
            take();
            if (f.type == kind::vector) {
                value = heap_.newv32(f.items);
            } else {
                value = f.suffix;
                for (auto i = f.items.size(); i != 0; --i)
                    value = heap_.cons(f.items[i - 1], value);
            }
            stack.pop_back();
        } else if (
            !stack.empty() && stack.back().type == kind::list && c == '.') {
            // No token boundary or preceding element required in Wisp:
            // (. x) is x, and (a .b) is (a . b).
            take();
            stack.back().type = kind::tail;
            continue;
        } else {
            switch (c) {
            case '(':
            case '[':
                take();
                stack.push_back({c == '(' ? kind::list : kind::vector});
                continue;
            case '\'':
            case '`':
            case ',': {
                take();
                auto op = c == '\''  ? evaluator_.known("QUOTE")
                          : c == '`' ? evaluator_.known("BACKQUOTE")
                                     : evaluator_.known("UNQUOTE");
                if (c == ',' && peek() == '@') {
                    take();
                    op = evaluator_.known("UNQUOTE-SPLICING");
                }
                stack.push_back({kind::quote, op});
                continue;
            }
            case '#': {
                take();
                const auto dispatch = position_;
                switch (take()) {
                case ':': {
                    const auto text = heap_.newv08(name());
                    value =
                        heap_.make<tag::sym>({text, nil, nah, nil, nil});
                    break;
                }
                case '\\':
                    value = immediate(tag::chr, take());
                    break;
                case '\'':
                    stack.push_back(
                        {kind::quote, evaluator_.known("FUNCTION")});
                    continue;
                default:
                    throw read_error(dispatch, "unknown '#' dispatch");
                }
                break;
            }
            case ':':
                take();
                value = evaluator_.keyword(name());
                heap_.set<tag::sym, field::val>(value, value);
                break;
            case '"':
                value = string();
                break;
            default:
                if (digit(c))
                    value = number();
                else if (constituent(c))
                    value = symbol();
                else
                    throw read_error(position_, "unexpected character");
            }
        }

        while (true) {
            if (stack.empty())
                return value;
            auto & f = stack.back();
            if (f.type == kind::quote) {
                value = heap_.cons(f.suffix, heap_.cons(value, nil));
                stack.pop_back();
            } else {
                if (f.type == kind::tail) {
                    f.suffix = value;
                    f.type = kind::close;
                } else {
                    f.items.push_back(value);
                }
                break;
            }
        }
    }
}

} // namespace wisp
