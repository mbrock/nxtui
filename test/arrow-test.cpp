#include <nxtio/arrow.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace nxt::test {

using namespace boost::ut;

suite arrow_tests = [] {
    "ipc trace rows include wall-clock unix milliseconds"_test = [] {
        auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        auto path = std::filesystem::temp_directory_path()
                  / "nxt-arrow-unix-ms-test.arrow";

        {
            auto trace = nxt::io::arrow::ipc_trace{
                std::string{path.string()}, "TESTTIME"};
            trace.add("phase", "event", "data", "{}");
            trace.write();
        }

        auto rows = nxt::io::arrow::read_trace_ipc(path.string());
        std::filesystem::remove(path);

        expect(rows.size() == 1_ul);
        expect(rows[0].run_id == "TESTTIME");
        expect(rows[0].elapsed_ms >= 0_i);
        expect(rows[0].unix_ms >= before.count());
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
