// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/load.hpp"

namespace wisp {
namespace {

constexpr unsigned char base_source[] = {
#embed "base.wisp"
};

} // namespace

std::string_view base_library() noexcept
{
    return {
        reinterpret_cast<const char *>(base_source), sizeof(base_source)};
}

loader::loader(heap & storage, evaluator & machine, std::string_view source)
    : heap_(storage)
    , machine_(machine)
    , input_(storage, machine, source)
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
