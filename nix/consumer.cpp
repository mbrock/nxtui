// Built by the flake's `consumer` check against the installed package
// through pkg-config, to keep the install layout honest. Mirrors the
// README example.
#include <nxtrt/app.hpp>

using namespace std::chrono_literals;

nxtrt::task<void> main_task()
{
    co_await nxtrt::op::timeout::after(30ms);
}

int main()
{
    auto rt = nxtrt::runtime{};
    rt.run(main_task);
}
