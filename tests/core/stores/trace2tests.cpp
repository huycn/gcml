// Ports of Trace2Tests and Trace2MessageTests, plus an own end-to-end test of the three TRACE2 targets.
// Not ported: TryGetPipeName_Windows (named pipes are Windows only).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/trace2.h"
#include "core/trace2message.h"
#include "core/platform.h"
#include "infra/testmain.h"
#include "infra/testutils.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

template <typename M> M baseMessage()
{
    M m;
    m.sid = Q("123");
    m.thread = Q("main");
    m.file = Q("foo.cs");
    m.line = 1;
    m.depth = 1;
    return m;
}
} // namespace

class Trace2Tests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void tryGetPipeName_posix_returnsExpectedValue_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << Q("af_unix:foo") << Q("foo");
        QTest::newRow("2") << Q("af_unix:foo/bar") << Q("foo/bar");
        QTest::newRow("3") << Q("af_unix:stream:foo/bar") << Q("foo/bar");
        QTest::newRow("4") << Q("af_unix:dgram:foo/bar/baz") << Q("foo/bar/baz");
    }
    void tryGetPipeName_posix_returnsExpectedValue()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QString actual;
        QVERIFY(Trace2::tryGetPipeName(input, actual));
        QCOMPARE(actual, expected);
    }

    void getProcessDepth_returnsCorrectDepth_data()
    {
        QTest::addColumn<QString>("sid");
        QTest::addColumn<int>("expected");
        QTest::newRow("empty") << Q("") << 0;
        QTest::newRow("abc") << Q("abc") << 0;
        QTest::newRow("abc/def") << Q("abc/def") << 1;
        QTest::newRow("abc/def/ghi") << Q("abc/def/ghi") << 2;
        QTest::newRow("abc/") << Q("abc/") << 1;
    }
    void getProcessDepth_returnsCorrectDepth()
    {
        QFETCH(QString, sid);
        QFETCH(int, expected);
        QCOMPARE(Trace2::getProcessDepth(sid), expected);
    }

    void createSid_existingParentSid_appendsToExisting()
    {
        const QByteArray original = qgetenv(Trace2::SidEnvar);
        const QString parentSid = Q("0ddfc330-30e9-49f3-86d3-6b34d99d51f4");
        qputenv(Trace2::SidEnvar, parentSid.toUtf8());
        const QString actualSid = Trace2::createSid();
        qputenv(Trace2::SidEnvar, original);
        QVERIFY(actualSid.startsWith(parentSid + Q("/")));
        QVERIFY(!actualSid.mid(parentSid.size() + 1).trimmed().isEmpty());
    }

    void createSid_noParentSid_createsNew()
    {
        const QByteArray original = qgetenv(Trace2::SidEnvar);
        qunsetenv(Trace2::SidEnvar);
        const QString actualSid = Trace2::createSid();
        if (!original.isNull()) {
            qputenv(Trace2::SidEnvar, original);
        }
        QVERIFY(!actualSid.trimmed().isEmpty());
        QVERIFY(!actualSid.contains(QLatin1Char('/')));
    }
};

// =============================================================================================

class Trace2MessageTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void buildTimeSpan_match_returnsExpectedString_data()
    {
        QTest::addColumn<double>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << 0.013772 << Q("  0.013772 ");
        QTest::newRow("2") << 26.316083 << Q(" 26.316083 ");
        QTest::newRow("3") << 100.316083 << Q("100.316083 ");
        QTest::newRow("4") << 1000.316083 << Q("1000.316083");
        QTest::newRow("5") << 10000.316083 << Q("10000.316083");
        QTest::newRow("6") << 100000.31608 << Q("100000.316080");
    }
    void buildTimeSpan_match_returnsExpectedString()
    {
        QFETCH(double, input);
        QFETCH(QString, expected);
        QCOMPARE(PerformanceFormatFields::getTimeSpan(input), expected);
    }

    void buildRepoSpan_match_returnsExpectedString() { QCOMPARE(PerformanceFormatFields::getRepoSpan(1), Q(" r1  ")); }

    void buildCategorySpan_match_returnsExpectedString_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << Q("foo") << Q(" foo         ");
        QTest::newRow("2") << Q("foobar") << Q(" foobar      ");
        QTest::newRow("3") << Q("foo_bar_baz") << Q(" foo_bar_baz ");
        QTest::newRow("4") << Q("foobarbazfoo") << Q(" foobarbazfo ");
    }
    void buildCategorySpan_match_returnsExpectedString()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(PerformanceFormatFields::getCategorySpan(input), expected);
    }

    void event_message_without_snake_case_toJson_createsExpectedJson()
    {
        auto m = baseMessage<ErrorMessage>();
        m.message = Q("bar");
        m.parameterizedMessage = Q("baz");
        QCOMPARE(m.toJson(), Q("{\"event\":\"error\",\"sid\":\"123\",\"thread\":\"main\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1,\"msg\":\"bar\",\"fmt\":\"baz\"}"));
    }

    void event_message_with_snake_case_toJson_createsExpectedJson()
    {
        auto m = baseMessage<ChildStartMessage>();
        m.id = 1;
        m.classification = Trace2ProcessClass::UiHelper;
        m.useShell = false;
        m.argv = {Q("bar"), Q("baz")};
        m.elapsedTime = 0.05;
        QCOMPARE(m.toJson(), Q("{\"event\":\"child_start\",\"sid\":\"123\",\"thread\":\"main\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1,\"t_abs\":0.05,\"argv\":[\"bar\",\"baz\"],\"child_id\":1,\"child_class\":\"ui_helper\",\"use_shell\":false}"));
    }

    void thread_events_toJson_createExpectedJson()
    {
        auto start = baseMessage<ThreadStartMessage>();
        start.thread = Q("AppMain");
        auto exit = baseMessage<ThreadExitMessage>();
        exit.thread = Q("AppMain");
        exit.line = 2;
        exit.relativeTime = 0.05;
        QCOMPARE(start.toJson(), Q("{\"event\":\"thread_start\",\"sid\":\"123\",\"thread\":\"AppMain\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1}"));
        QCOMPARE(exit.toJson(), Q("{\"event\":\"thread_exit\",\"sid\":\"123\",\"thread\":\"AppMain\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":2,\"depth\":1,\"t_rel\":0.05}"));
    }

    void data_event_toJson_createsExpectedJson()
    {
        auto m = baseMessage<DataMessage>();
        m.elapsedTime = 0.05;
        m.relativeTime = 0.01;
        m.repo = 1;
        m.nesting = 2;
        m.category = Q("index");
        m.key = Q("read/cache_nr");
        m.value = Q("3552");
        QCOMPARE(m.toJson(), Q("{\"event\":\"data\",\"sid\":\"123\",\"thread\":\"main\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1,\"t_abs\":0.05,\"t_rel\":0.01,\"repo\":1,\"nesting\":2,\"category\":\"index\",\"key\":\"read/cache_nr\",\"value\":\"3552\"}"));
    }

    void commandName_event_toJson_createsExpectedJson()
    {
        auto m = baseMessage<CommandNameMessage>();
        m.name = Q("get");
        m.hierarchy = Q("git/get");
        QCOMPARE(m.toJson(), Q("{\"event\":\"cmd_name\",\"sid\":\"123\",\"thread\":\"main\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1,\"name\":\"get\",\"hierarchy\":\"git/get\"}"));
    }

    void dataJson_event_toJson_createsExpectedJson()
    {
        auto m = baseMessage<DataJsonMessage>();
        m.elapsedTime = 0.05;
        m.relativeTime = 0.01;
        m.repo = 1;
        m.nesting = 2;
        m.category = Q("index");
        m.key = Q("read/statistics");
        m.value = Q("{\"count\":2,\"items\":[\"one\",\"two\"]}");
        QCOMPARE(m.toJson(), Q("{\"event\":\"data_json\",\"sid\":\"123\",\"thread\":\"main\",\"time\":\"0001-01-01T00:00:00+00:00\",\"file\":\"foo.cs\",\"line\":1,\"depth\":1,\"t_abs\":0.05,\"t_rel\":0.01,\"repo\":1,\"nesting\":2,\"category\":\"index\",\"key\":\"read/statistics\",\"value\":{\"count\":2,\"items\":[\"one\",\"two\"]}}"));
    }

    // Own tests
    void jsonEscaping_and_numbers()
    {
        QCOMPARE(jsonString(Q("a\"b\\c\n<é>+")), Q("\"a\\\"b\\\\c\\n\\u003C\\u00E9\\u003E\\u002B\""));
        QCOMPARE(formatDouble(0.05), Q("0.05"));
        QCOMPARE(formatDouble(3), Q("3"));
        QCOMPARE(formatDouble(1e-05), Q("1E-05"));
        QCOMPARE(Trace2Time::toJson(Trace2Time::MinValue + 1500), Q("0001-01-01T00:00:00.0015+00:00"));
    }

    void textFormats()
    {
        auto m = baseMessage<ExitMessage>();
        m.elapsedTime = 0.25;
        m.code = 0;
        const QString normal = m.toNormalString();
        QVERIFY2(normal.endsWith(Q(" ") + Q("foo.cs:1").leftJustified(33) + Q(" exit elapsed:0.25 code:0")), qPrintable(normal));
        const QString perf = m.toPerformanceString();
        const QString expectedPerf = Q(" ") + Q("foo.cs:1").leftJustified(29) +
                                     Q("| d1 | main                     | exit         |     |  0.250000 |           |              | elapsed:0.25 code:0");
        QVERIFY2(perf.endsWith(expectedPerf), qPrintable(perf));
    }
};

// =============================================================================================
// Own end-to-end test: initialize with the three targets as files, emit events, stop.
// It runs last; the process-wide TRACE2 state stays initialized (without writers) afterwards.

class Trace2EndToEndTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initialize_writesAllTargets()
    {
        const QString dir = TestUtils::getTempDirectory();
        const QString eventFile = dir + Q("/event.json"), normalFile = dir + Q("/normal.txt"), perfFile = dir + Q("/perf.txt");
        qputenv("GIT_TRACE2_EVENT", eventFile.toUtf8());
        qputenv("GIT_TRACE2", normalFile.toUtf8());
        qputenv("GIT_TRACE2_PERF", perfFile.toUtf8());
        const QByteArray oldSid = qgetenv(Trace2::SidEnvar);
        qputenv(Trace2::SidEnvar, "parent-sid");

        Trace2::initialize({Q("get")});
        QVERIFY(Trace2::isEnabled());
        QVERIFY(Trace2::sessionId().startsWith(Q("parent-sid/")));
        Trace2::writeCommandName(Q("get"));
        {
            Trace2::Scope region = Trace2::startRegion(Q("cat"), Q("label"));
            Trace2::writeData(Q("cat"), Q("key"), OptString(Q("value")));
        }
        {
            ProcessStartInfo psi;
            psi.fileName = Q("/bin/true");
            ChildProcess child(psi, Trace2ProcessClass::Other);
            QVERIFY(child.start());
            child.waitForExit();
        }
        Trace2::writeError(Q("boom"));
        Trace2::stop(3);

        qunsetenv("GIT_TRACE2_EVENT");
        qunsetenv("GIT_TRACE2");
        qunsetenv("GIT_TRACE2_PERF");
        qputenv(Trace2::SidEnvar, oldSid);

        QFile events(eventFile);
        QVERIFY(events.open(QIODevice::ReadOnly));
        QStringList names;
        for (const QList<QByteArray> lines = events.readAll().split('\n'); const QByteArray &line : lines) {
            if (line.isEmpty()) {
                continue;
            }
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            const QString name = obj.value(Q("event")).toString();
            names.append(name);
            // version, start and exit carry no depth upstream
            const bool noDepth = name == QLatin1String("version") || name == QLatin1String("start") || name == QLatin1String("exit");
            QCOMPARE(obj.value(Q("depth")).toInt(), noDepth ? 0 : 1);
            if (name == QLatin1String("child_exit")) {
                QVERIFY(obj.value(Q("pid")).toInt() > 0);
            }
        }
        QCOMPARE(names, (QStringList{Q("version"), Q("start"), Q("cmd_name"), Q("region_enter"), Q("data"), Q("region_leave"),
                                     Q("child_start"), Q("child_exit"), Q("error"), Q("exit")}));

        QFile normal(normalFile), perf(perfFile);
        QVERIFY(normal.open(QIODevice::ReadOnly) && perf.open(QIODevice::ReadOnly));
        const QString normalText = QString::fromUtf8(normal.readAll());
        QVERIFY(normalText.contains(Q(" command_name get (get)")));
        QVERIFY(normalText.contains(Q(" exit elapsed:")));
        QVERIFY(QString::fromUtf8(perf.readAll()).contains(Q("| child_start  |")));
        QDir(dir).removeRecursively();
    }
};

GCM_REGISTER_TEST(Trace2Tests)
GCM_REGISTER_TEST(Trace2MessageTests)
GCM_REGISTER_TEST(Trace2EndToEndTests)
#include "trace2tests.moc"
