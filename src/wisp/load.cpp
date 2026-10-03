// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/load.hpp"

namespace wisp {
namespace {

// #embed is intentionally used as a C++23 extension.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
constexpr unsigned char base_source[] = {
#embed "base.wisp"
};
constexpr unsigned char compiler_source[] = {
#embed "compiler.wisp"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace

std::string_view base_library() noexcept
{
    return {
        reinterpret_cast<const char *>(base_source), sizeof(base_source)};
}

std::string_view compiler_library() noexcept
{
    return {
        reinterpret_cast<const char *>(compiler_source),
        sizeof(compiler_source)};
}

loader::loader(
    heap & storage,
    evaluator & machine,
    std::string_view source,
    std::string_view path)
    : heap_(storage)
    , machine_(machine)
    , input_(storage, machine, source, path)
    , run_(storage)
{
}

evaluation loader::advance(std::size_t budget)
{
    while (budget-- != 0 && state_ == evaluation::runnable) {
        if (run_.get() == nil
            || machine_.status(run_.get()) == evaluation::done) {
            try {
                if (auto form = input_.next())
                    run_.set(machine_.start(*form));
                else
                    state_ = evaluation::done;
            } catch (const read_error & error) {
                const std::array details{
                    machine_.known("READ-ERROR"),
                    heap_.newv08(error.what())};
                run_.set(machine_.start(nil));
                heap_.set<tag::run, field::err>(
                    run_.get(), heap_.newv32(details));
                state_ = evaluation::failed;
            }
        } else if (machine_.step(run_.get()) == evaluation::failed) {
            state_ = evaluation::failed;
        }
        // Every transition has committed its registers, including nested
        // STEP! targets. input_ retains no guest words between forms.
        if (machine_.collection_requested())
            machine_.collect();
    }
    return state_;
}

} // namespace wisp
