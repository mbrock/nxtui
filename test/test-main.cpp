#include <boost/ut.hpp>

int main(int argc, char ** argv)
{
    using namespace boost::ut;
    return cfg<override>.run(
        {.report_errors = true, .argc = argc, .argv = argv});
}
