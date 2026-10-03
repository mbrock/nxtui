#include "test.hpp"

std::string boost::ut::test_vformat(std::string_view fmt, std::format_args args)
{
    return std::vformat(fmt, args);
}

int nxt_tests_main(int argc, char ** argv)
{
    using namespace boost::ut;
    return cfg<override>.run(
        {.report_errors = true, .argc = argc, .argv = argv});
}

#if !defined(NXT_EMBEDDED_MAIN)
int main(int argc, char ** argv)
{
    return nxt_tests_main(argc, argv);
}
#endif
