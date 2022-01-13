/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <testMain.h>

#include <epicsUnitTest.h>

#include <pvxs/unittest.h>
#include <pvxs/nt.h>

#include <epicsTime.h>

#if EPICS_VERSION_INT < VERSION_INT(7,0,6,1)
static
std::ostream& operator<<(std::ostream& strm, const epicsTime& ts)
{
    char temp[64];

    (void)ts.strftime(temp, sizeof(temp), "%Y-%m-%d %H:%M:%S.%09f");
    temp[sizeof(temp)-1u] = '\0';
    return strm<<temp;
}
#endif

namespace {

using namespace pvxs;

void testNTScalar()
{
    testDiag("In %s", __func__);

    // plain, without display meta
    auto top = nt::NTScalar{TypeCode::Int32}.create();

    testTrue(top.idStartsWith("epics:nt/NTScalar:"))<<"\n"<<top;

    testEq(top["value"].type(), TypeCode::Int32);
    testEq(top["display.limitLow"].type(), TypeCode::Null);
    testEq(top["display.description"].type(), TypeCode::Null);
    testEq(top["control.limitLow"].type(), TypeCode::Null);

    // with display, but not control
    top = nt::NTScalar{TypeCode::Float64,true}.create();

    testEq(top["value"].type(), TypeCode::Float64);
    testEq(top["display.limitLow"].type(), TypeCode::Float64);
    testEq(top["display.description"].type(), TypeCode::String);
    testEq(top["control.limitLow"].type(), TypeCode::Null);

    // with everything
    top = nt::NTScalar{TypeCode::Float64,true,true,true}.create();

    testEq(top["value"].type(), TypeCode::Float64);
    testEq(top["display.limitLow"].type(), TypeCode::Float64);
    testEq(top["display.description"].type(), TypeCode::String);
    testEq(top["control.limitLow"].type(), TypeCode::Float64);
}

void testNTNDArray()
{
    testDiag("In %s", __func__);

    auto top = nt::NTNDArray{}.create();

    testTrue(top.idStartsWith("epics:nt/NTNDArray:"))<<"\n"<<top;
}

void testNTURI()
{
    testDiag("In %s", __func__);

    using namespace pvxs::members;

    auto def = nt::NTURI({
                             UInt32("arg1"),
                             String("arg2"),
                         });

    auto top = def.call(42, "hello");

    testTrue(top.idStartsWith("epics:nt/NTURI:"))<<"\n"<<top;
    testEq(top["query.arg1"].as<uint32_t>(), 42u);
    testEq(top["query.arg2"].as<std::string>(), "hello");
}

void testNTEnum()
{
    testDiag("In %s", __func__);

    auto top = nt::NTEnum{}.create();

    testTrue(top.idStartsWith("epics:nt/NTEnum:"))<<"\n"<<top;
}

void test_time_t()
{
    testDiag("In %s", __func__);

    auto top(nt::TimeStamp{}.build().create());
    const auto now(epicsTime::getCurrent());

    top = now;
    auto ets(top.as<epicsTimeStamp>());

    testEq(now, ets);
    testEq(ets.secPastEpoch, top["secondsPastEpoch"].as<uint32_t>() - POSIX_TIME_AT_EPICS_EPOCH);
}

} // namespace

MAIN(testnt) {
    testPlan(20);
    testNTScalar();
    testNTNDArray();
    testNTURI();
    testNTEnum();
    test_time_t();
    return testDone();
}
