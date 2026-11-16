// Multi-class Qt Test runner.
//
// Each test class is a QObject with private test slots (plain Qt Test). Instead of QTEST_MAIN, end the .cpp with
//
//     GCM_REGISTER_TEST(MyClassTests)
//     #include "myclasstests.moc"
//
// and all registered classes of the executable run from the shared main() in testmain.cpp.
// Run a subset with `--class <Name>` (repeatable); remaining arguments go to Qt Test (e.g. a test function name).
#pragma once

#include <QObject>
#include <QTest>

#include <functional>

namespace gcm::test {

struct TestRegistration
{
    TestRegistration(const char *name, std::function<QObject *()> factory);
};

} // namespace gcm::test

#define GCM_REGISTER_TEST(Class) \
    static const ::gcm::test::TestRegistration gcmTestRegistration_##Class(#Class, [] { return static_cast<QObject *>(new Class); });
