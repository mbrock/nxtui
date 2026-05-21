#include <boost/ut.hpp>

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
