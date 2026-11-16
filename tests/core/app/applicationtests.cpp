// Port of ApplicationTests, plus own tests of the command line (core/cmdline.h) and of Application::run.
// Not ported: the AggregateException cases of ContainsInterrupt (there is no aggregate exception type here).
#include "appmocks.h"
#include "core/application.h"
#include "core/cmdline.h"
#include "core/constants.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }
const QString ExecutablePath = Q("/usr/local/share/gcm-core/git-credential-manager");
const QString Key = Q("credential.helper");

/// Runs configure/unconfigure on `initial` global helper values and returns the resulting values.
std::optional<QStringList> run(bool configure, const std::optional<QStringList> &initial, int *globalCount = nullptr)
{
    TestCommandContext context;
    context.AppPath = ExecutablePath;
    Application application(context);
    if (initial) {
        context.Git.Configuration.Global[Key] = *initial;
    }
    if (configure) {
        application.configure(ConfigurationTarget::User);
    } else {
        application.unconfigure(ConfigurationTarget::User);
    }
    if (globalCount) {
        *globalCount = int(context.Git.Configuration.Global.count());
    }
    QStringList values;
    if (!context.Git.Configuration.Global.tryGetValue(Key, values)) {
        return std::nullopt;
    }
    return values;
}
} // namespace

class ApplicationTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void application_configureAsync_data()
    {
        const QString e = Q(""), g = ExecutablePath;
        QTest::addColumn<QStringList>("initial");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("NoHelpers_AddsEmptyAndGcm") << QStringList() << QStringList{e, g};
        QTest::newRow("Gcm_AddsEmptyBeforeGcm") << QStringList{g} << QStringList{e, g};
        QTest::newRow("EmptyAndGcm_DoesNothing") << QStringList{e, g} << QStringList{e, g};
        QTest::newRow("EmptyAndGcmWithOthersBefore_DoesNothing") << QStringList{Q("foo"), e, g} << QStringList{Q("foo"), e, g};
        QTest::newRow("EmptyAndGcmWithOthersAfter_DoesNothing") << QStringList{e, g, Q("foo")} << QStringList{e, g, Q("foo")};
        QTest::newRow("EmptyAndGcmWithOthersBeforeAndAfter_DoesNothing")
            << QStringList{Q("foo"), e, g, Q("bar")} << QStringList{Q("foo"), e, g, Q("bar")};
        QTest::newRow("EmptyAndGcmWithEmptyAfter_RemovesExistingGcmAndAddsEmptyAndGcm")
            << QStringList{e, g, e, Q("foo")} << QStringList{e, e, Q("foo"), e, g};
        QTest::newRow("MultiGcmWithValidEmpty_DoesNothing") << QStringList{g, e, g} << QStringList{g, e, g};
        QTest::newRow("EmptyOnly_AddsGcmOnly") << QStringList{e} << QStringList{e, g};
    }
    void application_configureAsync()
    {
        QFETCH(QStringList, initial);
        QFETCH(QStringList, expected);
        int count = 0;
        const auto actual = run(true, initial.isEmpty() ? std::nullopt : std::optional<QStringList>(initial), &count);
        QCOMPARE(count, 1);
        QVERIFY(actual);
        QCOMPARE(*actual, expected);
    }

    void application_unconfigureAsync_data()
    {
        const QString e = Q(""), g = ExecutablePath;
        QTest::addColumn<QStringList>("initial");
        QTest::addColumn<QStringList>("expected"); // empty: no entries left at all
        QTest::newRow("NoHelpers_DoesNothing") << QStringList() << QStringList();
        QTest::newRow("Gcm_RemovesGcm") << QStringList{g} << QStringList();
        QTest::newRow("EmptyAndGcm_RemovesEmptyAndGcm") << QStringList{e, g} << QStringList();
        QTest::newRow("EmptyAndGcmWithOthersBefore_RemovesEmptyAndGcm") << QStringList{Q("foo"), e, g} << QStringList{Q("foo")};
        QTest::newRow("EmptyAndGcmWithOthersAfterBefore_RemovesGcmOnly") << QStringList{e, g, Q("bar")} << QStringList{e, Q("bar")};
        QTest::newRow("EmptyAndGcmWithOthersBeforeAndAfter_RemovesGcmOnly")
            << QStringList{Q("foo"), e, g, Q("bar")} << QStringList{Q("foo"), e, Q("bar")};
    }
    void application_unconfigureAsync()
    {
        QFETCH(QStringList, initial);
        QFETCH(QStringList, expected);
        int count = 0;
        const auto actual = run(false, initial.isEmpty() ? std::nullopt : std::optional<QStringList>(initial), &count);
        if (expected.isEmpty()) {
            QCOMPARE(count, 0);
        } else {
            QCOMPARE(count, 1);
            QCOMPARE(*actual, expected);
        }
    }

    void application_containsInterrupt_direct_true()
    {
        QVERIFY(Application::containsInterrupt(InterruptedException(Q("interrupted"))));
    }

    void application_containsInterrupt_wrappedInInner_true()
    {
        const InvalidOperationException ex(
            Q("outer"), std::make_shared<Exception>(Q("mid"), std::make_shared<InterruptedException>(Q("interrupted"))));
        QVERIFY(Application::containsInterrupt(ex));
    }

    void application_containsInterrupt_notPresent_false()
    {
        const InvalidOperationException ex(Q("outer"), std::make_shared<Exception>(Q("inner")));
        QVERIFY(!Application::containsInterrupt(ex));
    }

    // Own test: a path with spaces/parentheses is escaped in credential.helper and found again.
    void application_configureUnconfigure_escapedPath()
    {
        TestCommandContext context;
        context.AppPath = Q("/opt/my apps (x)/git-credential-manager");
        Application application(context);
        application.configure(ConfigurationTarget::System);
        QCOMPARE(context.Git.Configuration.System.value(Key),
                 (QStringList{Q(""), Q("/opt/my\\ apps\\ \\(x\\)/git-credential-manager")}));
        application.unconfigure(ConfigurationTarget::System);
        QVERIFY(context.Git.Configuration.System.isEmpty());
    }
};

// =============================================================================================
// Own tests of Application::run end to end with fakes

class ApplicationRunTests : public QObject
{
    Q_OBJECT

    static std::shared_ptr<MockHostProvider> credentialProvider()
    {
        auto provider = std::make_shared<MockHostProvider>();
        provider->Id = Q("mock");
        provider->Name = Q("Mock");
        provider->IsSupportedRequest = [](const GitRequest &) { return true; };
        provider->GetCredentialFunc = [](const GitRequest &) { return GitResponse(Credential{Q("john"), Q("pw")}); };
        return provider;
    }

private Q_SLOTS:
    void run_get_writesCredential()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\n\n");
        Application application(context);
        application.registerProvider(credentialProvider(), HostProviderPriority::Normal);
        QCOMPARE(application.run({Q("get")}), 0);
        QCOMPARE(context.Streams.outText(), Q("protocol=https\nhost=example.com\nusername=john\npassword=pw\n\n"));
    }

    void run_noUi_disablesGuiPrompts()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\n\n");
        Application application(context);
        application.registerProvider(credentialProvider(), HostProviderPriority::Normal);
        QVERIFY(context.Settings.IsGuiPromptsEnabled);
        QCOMPARE(application.run({Q("get"), Q("--no-ui")}), 0);
        QVERIFY(!context.Settings.IsGuiPromptsEnabled);
    }

    void run_exception_writesFatalAndReturnsMinusOne()
    {
        TestCommandContext context;
        context.Streams.In = Q("host=example.com\n\n");
        Application application(context);
        QCOMPARE(application.run({Q("get")}), -1);
        QCOMPARE(context.Console.WrittenMessages, QStringList{Q("Missing 'protocol' request argument")});
    }

    void run_interrupted_returns130()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\n\n");
        auto provider = credentialProvider();
        provider->StoreCalls.clear();
        Application application(context);
        auto interrupting = std::make_shared<MockHostProvider>(*provider);
        interrupting->GetCredentialFunc = [](const GitRequest &) -> GitResponse {
            throw Exception(Q("outer"), std::make_shared<InterruptedException>(Q("Ctrl+C")));
        };
        application.registerProvider(interrupting, HostProviderPriority::Normal);
        QCOMPARE(application.run({Q("get")}), 130);
        QVERIFY(context.Console.WrittenMessages.isEmpty());
    }

    void run_gitException_writesExitCodeAndGitMessage()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\nusername=a\npassword=b\n\n");
        auto provider = credentialProvider();
        Application application(context);
        auto failing = std::make_shared<MockHostProvider>(*provider);
        failing->GetCredentialFunc = [](const GitRequest &) -> GitResponse {
            throw GitException(Q("Failed to do it"), Q("fatal: no"), 128);
        };
        application.registerProvider(failing, HostProviderPriority::Normal);
        QCOMPARE(application.run({Q("get")}), -1);
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("Failed to do it [128]"), Q("fatal: no")}));
    }

    void run_capability_and_version_and_help()
    {
        {
            TestCommandContext context;
            Application application(context);
            QCOMPARE(application.run({Q("capability")}), 0);
            QCOMPARE(context.Streams.outText(), Q("version 0\ncapability state\ncapability authtype\n"));
        }
        {
            TestCommandContext context;
            Application application(context);
            QCOMPARE(application.run({Q("--version")}), 0);
            QCOMPARE(context.Streams.outText(), Constants::gcmInformationalVersion() + QLatin1Char('\n'));
        }
        {
            TestCommandContext context;
            Application application(context);
            QCOMPARE(application.run({Q("--help")}), 0);
            const QString help = context.Streams.outText();
            QVERIFY(help.contains(Q("Usage:\n  git-credential-manager [command] [options]")));
            QVERIFY(help.contains(Q("configure    Configure Git Credential Manager as the Git credential helper")));
            QVERIFY(help.contains(Q("--no-ui")));
            QVERIFY(!help.contains(Q("capability"))); // hidden
        }
        {
            TestCommandContext context;
            Application application(context);
            QCOMPARE(application.run({}), 1);
            QCOMPARE(context.Streams.errorText(), Q("Required command was not provided.\n\n"));
        }
        {
            TestCommandContext context;
            Application application(context);
            QCOMPARE(application.run({Q("bogus")}), 1);
            QCOMPARE(context.Streams.errorText(), Q("Required command was not provided.\nUnrecognized command or argument 'bogus'.\n\n"));
        }
    }

    void run_trace_toStderr()
    {
        TestCommandContext context;
        context.Settings.Trace = Q("1");
        context.Trace = std::make_shared<Trace>();
        Application application(context);
        QCOMPARE(application.run({Q("capability")}), 0);
        QVERIFY(context.Streams.errorText().contains(Q("Start 'capability' command...")));
    }

    void run_configure_usesConfigurationService()
    {
        TestCommandContext context;
        auto service = std::make_shared<RecordingConfigurationService>();
        Application application(context, std::make_shared<HostProviderRegistry>(context), service);
        QCOMPARE(application.run({Q("configure"), Q("--system")}), 0);
        QCOMPARE(service->Configured, QList<ConfigurationTarget>{ConfigurationTarget::System});
        QCOMPARE(application.run({Q("unconfigure")}), 0);
        QCOMPARE(service->Unconfigured, QList<ConfigurationTarget>{ConfigurationTarget::User});
    }
};

// =============================================================================================
// Own tests of the command-line parser

class CommandLineParserTests : public QObject
{
    Q_OBJECT

    struct Fixture
    {
        Fixture()
        {
            root->addGlobalOption(Option::flag(Q("--global-flag"), Q("A global flag")));
            auto sub = std::make_shared<Command>(Q("sub"), Q("A sub-command"));
            sub->addOption(Option{{Q("--output"), Q("-o")}, Q("Output"), true});
            sub->addOption(Option{{Q("--multi")}, Q("Many"), true, false, true});
            sub->addArgument(Argument{Q("name"), Q("A name"), true});
            sub->setHandler([this](const ParseResult &r) {
                result = r;
                return 7;
            });
            root->addCommand(sub);
        }
        int invoke(const QStringList &args)
        {
            CommandLineParser parser(root, Q("tool"), Q("1.2.3"), out, err);
            return parser.invoke(args);
        }
        std::shared_ptr<Command> root = std::make_shared<Command>(Q("tool"), Q("The tool"));
        ParseResult result;
        StringWriter out, err;
    };

private Q_SLOTS:
    void parse_optionsArgumentsAndGlobals()
    {
        Fixture f;
        QCOMPARE(f.invoke({Q("sub"), Q("-o"), Q("dir"), Q("--multi=a"), Q("--multi"), Q("b"), Q("the-name"), Q("--global-flag")}), 7);
        QCOMPARE(f.result.value(Q("--output")), OptString(Q("dir")));
        QCOMPARE(f.result.values(Q("--multi")), (QStringList{Q("a"), Q("b")}));
        QCOMPARE(f.result.argument(Q("name")), OptString(Q("the-name")));
        QVERIFY(f.result.has(Q("--global-flag")));
        QCOMPARE(f.result.command->name(), Q("sub"));
    }

    void parse_errors()
    {
        Fixture f;
        QCOMPARE(f.invoke({Q("sub")}), 1);
        QCOMPARE(f.err.toString(), Q("Required argument missing for command: 'sub'.\n\n"));
        Fixture g;
        QCOMPARE(g.invoke({Q("sub"), Q("x"), Q("--output")}), 1);
        QCOMPARE(g.err.toString(), Q("Required argument missing for option: '--output'.\n\n"));
        Fixture h;
        QCOMPARE(h.invoke({Q("sub"), Q("x"), Q("--nope")}), 1);
        QCOMPARE(h.err.toString(), Q("Unrecognized command or argument '--nope'.\n\n"));
    }

    void help_subCommand()
    {
        Fixture f;
        QCOMPARE(f.invoke({Q("sub"), Q("-h")}), 0);
        QCOMPARE(f.out.toString(), Q("Description:\n  A sub-command\n\n"
                                     "Usage:\n  tool sub <name> [options]\n\n"
                                     "Arguments:\n  <name>  A name\n\n"
                                     "Options:\n"
                                     "  -o, --output <output>  Output\n"
                                     "  --multi <multi>        Many\n"
                                     "  --global-flag          A global flag\n"
                                     "  -?, -h, --help         Show help and usage information\n"
                                     "\n\n"));
    }

    void exceptionHandler_mapsExceptions()
    {
        Fixture f;
        f.root->findCommand(Q("sub"))->setHandler([](const ParseResult &) -> int { throw Exception(Q("boom")); });
        CommandLineParser parser(f.root, Q("tool"), Q("1"), f.out, f.err);
        QString seen;
        parser.setExceptionHandler([&](const std::exception &ex) {
            seen = QString::fromUtf8(ex.what());
            return 42;
        });
        QCOMPARE(parser.invoke({Q("sub"), Q("x")}), 42);
        QCOMPARE(seen, Q("boom"));
    }
};

GCM_REGISTER_TEST(ApplicationTests)
GCM_REGISTER_TEST(ApplicationRunTests)
GCM_REGISTER_TEST(CommandLineParserTests)
#include "applicationtests.moc"
