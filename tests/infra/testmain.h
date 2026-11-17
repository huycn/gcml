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

#include <QCoreApplication>
#include <QObject>
#include <QTest>

#include <functional>
#include <memory>

namespace gcm::test {

struct TestRegistration
{
    TestRegistration(const char *name, std::function<QObject *()> factory);
};

/// Replaces the QCoreApplication of the runner, e.g. by a QApplication for widget tests. Use as a static object.
struct TestApplicationFactory
{
    using Factory = std::function<std::unique_ptr<QCoreApplication>(int &argc, char **argv)>;
    explicit TestApplicationFactory(Factory factory);
};

} // namespace gcm::test

#define GCM_REGISTER_TEST(Class) \
    static const ::gcm::test::TestRegistration gcmTestRegistration_##Class(#Class, [] { return static_cast<QObject *>(new Class); });
