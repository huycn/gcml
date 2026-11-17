#include "infra/testmain.h"

#include <QCoreApplication>
#include <QList>
#include <QStringList>

#include <cstdio>
#include <memory>

namespace gcm::test {

namespace {
struct Entry
{
    QString name;
    std::function<QObject *()> factory;
};
QList<Entry> &registry()
{
    static QList<Entry> entries;
    return entries;
}
TestApplicationFactory::Factory &applicationFactory()
{
    static TestApplicationFactory::Factory factory;
    return factory;
}
} // namespace

TestApplicationFactory::TestApplicationFactory(Factory factory)
{
    applicationFactory() = std::move(factory);
}

TestRegistration::TestRegistration(const char *name, std::function<QObject *()> factory)
{
    registry().append({QString::fromLatin1(name), std::move(factory)});
}

} // namespace gcm::test

int main(int argc, char **argv)
{
    using namespace gcm::test;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    std::unique_ptr<QCoreApplication> appHolder =
        applicationFactory() ? applicationFactory()(argc, argv) : std::make_unique<QCoreApplication>(argc, argv);
    QCoreApplication &app = *appHolder;

    QStringList classes;
    QStringList qtArgs{app.arguments().constFirst()};
    const QStringList args = app.arguments();
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--class") && i + 1 < args.size()) {
            classes.append(args[++i]);
        } else if (args[i] == QLatin1String("--list-classes")) {
            for (const Entry &e : std::as_const(registry())) {
                std::printf("%s\n", qPrintable(e.name));
            }
            return 0;
        } else {
            qtArgs.append(args[i]);
        }
    }

    int failed = 0;
    int ran = 0;
    for (const Entry &e : std::as_const(registry())) {
        if (!classes.isEmpty() && !classes.contains(e.name)) {
            continue;
        }
        std::unique_ptr<QObject> test(e.factory());
        failed += QTest::qExec(test.get(), qtArgs) != 0 ? 1 : 0;
        ++ran;
    }
    std::printf("\n==== %d test class(es) run, %d failed ====\n", ran, failed);
    return failed == 0 ? 0 : 1;
}
