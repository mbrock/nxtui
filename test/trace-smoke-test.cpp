// trace-smoke-test — exercise the span/trace plumbing without needing
// a TTY. Spawns two children that work briefly, then exits. With
// `NXT_TRACE=auto` set, this leaves an Arrow IPC trace under `traces/`
// that downstream tooling (cassette renderer, duckdb) can inspect.

#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/process.hpp>

#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;

nxt::task<> child(nxt::ui::yard & self, const char * tag)
{
    for (int i = 0; i < 3 && !self.cancelled(); ++i) {
        {
            auto _ = self.span(std::string{"tick:"} + tag);
            co_await self.sleep(10ms);
        }
    }
}

nxt::task<> root(nxt::ui::yard & self)
{
    auto a = self.spawn("alpha",
                        [](nxt::ui::yard & y) { return child(y, "a"); });
    auto b = self.spawn("beta",
                        [](nxt::ui::yard & y) { return child(y, "b"); });
    co_await a.join();
    co_await b.join();
}

int main()
{
    auto rc = nxt::ui::main(
        [](nxt::ui::UIRuntime & runtime) { nxt::ui::run2(runtime, root); });
    std::printf("rc=%d\n", rc);
    return rc;
}
