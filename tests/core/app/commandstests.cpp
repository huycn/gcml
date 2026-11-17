// Ports of Commands/*Tests (GitCommandBase, Get, Store, Erase, Capability, Configure, Unconfigure, Diagnose).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "appmocks.h"
#include "core/commands.h"
#include "core/diagnostics.h"
#include "core/streams.h"
#include "infra/testcommandcontext.h"
#include "infra/testdiagnosticreporter.h"
#include "infra/testhostprovider.h"
#include "infra/testmain.h"
#include "infra/testutils.h"

#include <QDir>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

class TestGitCommand : public GitCommandBase
{
public:
    using GitCommandBase::GitCommandBase;
    std::function<void(const GitRequest &, IHostProvider &)> VerifyExecuteInternal;

protected:
    void executeInternal(const GitRequest &request, IHostProvider &provider) override
    {
        if (VerifyExecuteInternal) {
            VerifyExecuteInternal(request, provider);
        }
    }
};

std::shared_ptr<MockHostProvider> responding(const GitResponse &response)
{
    auto provider = std::make_shared<MockHostProvider>();
    provider->GetCredentialFunc = [response](const GitRequest &) { return response; };
    return provider;
}

/// Runs `get` with `stdin` against a provider answering `response`; returns stdout.
QString runGet(const QString &stdinText, const GitResponse &response, int *calls = nullptr)
{
    TestCommandContext context;
    context.Streams.In = stdinText;
    auto provider = responding(response);
    TestHostProviderRegistry registry;
    registry.Provider = provider;
    GetCommand command(context, registry);
    command.execute();
    if (calls) {
        *calls = provider->GetCalls;
    }
    return context.Streams.outText();
}

StringDictionary parseDictionary(const QString &text)
{
    StringReader reader(text);
    return readDictionary(reader);
}

const Credential Alice{Q("alice"), Q("hunter2")};
} // namespace

class GitCommandBaseTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void gitCommandBase_executeAsync_callsExecuteInternalAsyncWithCorrectArgs()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=test\nhost=example.com\npath=a/b/c\n\n");
        auto provider = std::make_shared<MockHostProvider>();
        provider->IsSupportedRequest = [](const GitRequest &) { return true; };
        TestHostProviderRegistry registry;
        registry.Provider = provider;
        TestGitCommand command(context, Q("test"), QString(), registry);
        bool called = false;
        command.VerifyExecuteInternal = [&](const GitRequest &request, IHostProvider &p) {
            called = true;
            QCOMPARE(&p, provider.get());
            QCOMPARE(request.protocol(), OptString(Q("test")));
            QCOMPARE(request.host(), OptString(Q("example.com")));
            QCOMPARE(request.path(), OptString(Q("a/b/c")));
        };
        command.execute();
        QVERIFY(called);
    }

    void gitCommandBase_executeAsync_configuresSettingsRemoteUri()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=test\nhost=example.com\npath=a/b/c\n\n");
        TestHostProviderRegistry registry;
        registry.Provider = std::make_shared<MockHostProvider>();
        TestGitCommand command(context, Q("test"), QString(), registry);
        command.execute();
        QCOMPARE(context.Settings.RemoteUri, std::optional<QUrl>(QUrl(Q("test://example.com/a/b/c"))));
    }

    // Own test: the minimum request validation messages.
    void gitCommandBase_missingArguments_throw()
    {
        TestCommandContext context;
        TestHostProviderRegistry registry;
        registry.Provider = std::make_shared<MockHostProvider>();
        TestGitCommand command(context, Q("test"), QString(), registry);
        const QList<std::pair<QString, QString>> cases{
            {Q("host=x\n\n"), Q("Missing 'protocol' request argument")},
            {Q("protocol= \nhost=x\n\n"), Q("Invalid 'protocol' request argument (cannot be empty)")},
            {Q("protocol=https\n\n"), Q("Missing 'host' request argument")},
            {Q("protocol=https\nhost=\n\n"), Q("Invalid 'host' request argument (cannot be empty)")}};
        for (const auto &[input, message] : cases) {
            context.Streams.In = input;
            try {
                command.execute();
                QFAIL("no exception");
            } catch (const InvalidOperationException &ex) {
                QCOMPARE(ex.message(), message);
            }
                 }
    }
};

// =============================================================================================

class StoreEraseCommandTests : public QObject
{
    Q_OBJECT

    static void assertRequest(const GitRequest &actual)
    {
        QCOMPARE(actual.protocol(), OptString(Q("http")));
        QCOMPARE(actual.host(), OptString(Q("example.com")));
        QCOMPARE(actual.path(), OptString());
        QCOMPARE(actual.userName(), OptString(Q("john.doe")));
        QCOMPARE(actual.password(), OptString(Q("letmein123")));
    }

private Q_SLOTS:
    void storeCommand_executeAsync_callsHostProvider()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=http\nhost=example.com\nusername=john.doe\npassword=letmein123\n\n");
        auto provider = std::make_shared<MockHostProvider>();
        TestHostProviderRegistry registry;
        registry.Provider = provider;
        StoreCommand(context, registry).execute();
        QCOMPARE(provider->StoreCalls.size(), 1);
        assertRequest(provider->StoreCalls[0]);
    }

    void eraseCommand_executeAsync_callsHostProvider()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=http\nhost=example.com\nusername=john.doe\npassword=letmein123\n\n");
        auto provider = std::make_shared<MockHostProvider>();
        TestHostProviderRegistry registry;
        registry.Provider = provider;
        EraseCommand(context, registry).execute();
        QCOMPARE(provider->EraseCalls.size(), 1);
        assertRequest(provider->EraseCalls[0]);
    }

    // Own test: store requires the secret (credential with authtype, otherwise username and password).
    void storeCommand_missingSecret_throws()
    {
        TestCommandContext context;
        TestHostProviderRegistry registry;
        registry.Provider = std::make_shared<MockHostProvider>();
        StoreCommand command(context, registry);
        context.Streams.In = Q("protocol=http\nhost=example.com\nauthtype=Bearer\n\n");
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, command.execute());
        context.Streams.In = Q("protocol=http\nhost=example.com\nusername=\n\n");
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, command.execute());
        context.Streams.In = Q("protocol=http\nhost=example.com\nusername=\npassword=\n\n");
        command.execute();
    }
};

// =============================================================================================

class GetCommandTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void getCommand_executeAsync_callsHostProviderAndWritesCredential()
    {
        int calls = 0;
        const QString out = runGet(Q("protocol=http\nhost=example.com\n\n"), GitResponse(Credential{Q("john.doe"), Q("letmein123")}), &calls);
        QCOMPARE(calls, 1);
        QCOMPARE(parseDictionary(out), (StringDictionary{{Q("protocol"), Q("http")}, {Q("host"), Q("example.com")},
                                                         {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
    }

    void getCommand_executeAsync_emptyCredential_preservesEmptyUsernameAndPassword()
    {
        const QString out = runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse(Credential{}));
        QVERIFY(out.contains(Q("username=\n")));
        QVERIFY(out.contains(Q("password=\n")));
    }

    void getCommand_executeAsync_additionalProperties_areEmitted()
    {
        GitResponse response(Credential{});
        response.additionalProperties.set(Q("ntlm"), Q("allow"));
        const QString out = runGet(Q("protocol=https\nhost=example.com\n\n"), response);
        QCOMPARE(parseDictionary(out).value(Q("ntlm")), Q("allow"));
    }

    void getCommand_executeAsync_negotiatedCapability_echoesIntersection()
    {
        const QString out = runGet(Q("protocol=https\nhost=example.com\ncapability[]=authtype\ncapability[]=state\n\n"), GitResponse(Alice));
        QVERIFY(out.contains(Q("capability[]=state\n")));
        QVERIFY(out.contains(Q("capability[]=authtype\n")));
    }

    void getCommand_executeAsync_noCapabilityFromGit_emitsNoCapabilityLines()
    {
        QVERIFY(!runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse(Alice)).contains(Q("capability")));
    }

    void getCommand_executeAsync_cancelledResponse_emitsQuitAndNoCredential()
    {
        QCOMPARE(runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse::cancel()), Q("quit=1\n\n"));
    }

    void getCommand_executeAsync_yieldedResponse_emitsEmptyResponse()
    {
        QCOMPARE(runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse::yield()), Q("\n"));
    }

    void getCommand_executeAsync_stateNegotiated_emitsStateLinesWithGcmPrefix()
    {
        GitResponse response = GitResponse::ok(Alice);
        response.setState(Q("github.account"), Q("alice"));
        response.setState(Q("azure.tenant"), Q("contoso"));
        const QString out = runGet(Q("protocol=https\nhost=example.com\ncapability[]=state\n\n"), response);
        QVERIFY(out.contains(Q("state[]=gcm.github.account=alice\n")));
        QVERIFY(out.contains(Q("state[]=gcm.azure.tenant=contoso\n")));
    }

    void getCommand_executeAsync_stateNotNegotiated_dropsStateLines()
    {
        GitResponse response = GitResponse::ok(Alice);
        response.setState(Q("github.account"), Q("alice"));
        QVERIFY(!runGet(Q("protocol=https\nhost=example.com\n\n"), response).contains(Q("state[]")));
    }

    void getCommand_executeAsync_continueNegotiated_emitsContinue1AlongsideCredential()
    {
        const QString out = runGet(Q("protocol=https\nhost=example.com\ncapability[]=state\n\n"), GitResponse::continue_(Alice));
        QVERIFY(out.contains(Q("username=alice\n")));
        QVERIFY(out.contains(Q("password=hunter2\n")));
        QVERIFY(out.contains(Q("continue=1\n")));
    }

    void getCommand_executeAsync_continueNotNegotiated_dropsContinueLine()
    {
        const QString out = runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse::continue_(Alice));
        QVERIFY(out.contains(Q("username=alice")));
        QVERIFY(!out.contains(Q("continue=")));
    }

    void getCommand_executeAsync_outputOrdering_capabilitiesFirstThenScalarsThenContinueThenState()
    {
        GitResponse response = GitResponse::continue_(Alice);
        response.setState(Q("k"), Q("v"));
        const QString out = runGet(Q("protocol=https\nhost=example.com\ncapability[]=state\n\n"), response);
        const qsizetype posCapability = out.indexOf(Q("capability[]=state"));
        const qsizetype posUsername = out.indexOf(Q("username="));
        const qsizetype posContinue = out.indexOf(Q("continue=1"));
        const qsizetype posState = out.indexOf(Q("state[]="));
        QVERIFY2(posCapability >= 0 && posUsername > posCapability, "capability[] must precede scalar fields");
        QVERIFY2(posContinue > posUsername, "continue=1 must follow scalar fields");
        QVERIFY2(posState > posContinue, "state[] must follow continue=1");
    }

    void getCommand_executeAsync_capabilityAuthType_missingRevertsToPlainPassword()
    {
        const QStringList out = runGet(Q("protocol=https\nhost=example.com\n\n"), GitResponse::ok(Alice, true, Q("token"))).split(QLatin1Char('\n'));
        QVERIFY(out.contains(Q("username=alice")));
        QVERIFY(out.contains(Q("password=hunter2")));
        QVERIFY(!out.contains(Q("authtype=token")));
        QVERIFY(!out.contains(Q("credential=hunter2")));
        QVERIFY(!out.contains(Q("ephemeral=1")));
    }

    void getCommand_executeAsync_capabilityAuthType_usesAuthTypeFeatures()
    {
        const QStringList out = runGet(Q("protocol=https\nhost=example.com\ncapability[]=authtype\n\n"),
                                       GitResponse::ok(Alice, true, Q("token"))).split(QLatin1Char('\n'));
        QVERIFY(!out.contains(Q("username=alice")));
        QVERIFY(!out.contains(Q("password=hunter2")));
        QVERIFY(out.contains(Q("authtype=token")));
        QVERIFY(out.contains(Q("credential=hunter2")));
        QVERIFY(out.contains(Q("ephemeral=1")));
    }

    // Own test: a cancellation thrown by the provider (the user closed a dialog) becomes quit=1.
    void getCommand_executeAsync_operationCanceled_emitsQuit()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\n\n");
        auto provider = std::make_shared<MockHostProvider>();
        provider->GetCredentialFunc = [](const GitRequest &) -> GitResponse { throw OperationCanceledException(Q("cancelled")); };
        TestHostProviderRegistry registry;
        registry.Provider = provider;
        GetCommand(context, registry).execute();
        QCOMPARE(context.Streams.outText(), Q("quit=1\n\n"));
        QCOMPARE(context.Console.WrittenMessages, QStringList{Q("user cancelled the credential request.")});
    }
};

// =============================================================================================

class CapabilityCommandTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void capabilityCommand_execute_writesVersionAndAdvertisedCapabilities()
    {
        TestCommandContext context;
        CapabilityCommand(context).execute();
        QCOMPARE(context.Streams.outText(), Q("version 0\ncapability state\ncapability authtype\n"));
    }

    void capabilityCommand_executeAsync_doesNotReadStandardInput()
    {
        TestCommandContext context;
        context.Streams.In = Q("protocol=https\nhost=example.com\n\n");
        CapabilityCommand(context).execute();
        QVERIFY(context.Streams.outText().startsWith(Q("version 0\n")));
        QCOMPARE(context.Streams.in().readLine(), OptString(Q("protocol=https")));
    }
};

// =============================================================================================

class ConfigurationCommandsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void configureCommand_executeAsync_user_invokesConfigurationServiceConfigureUser()
    {
        TestCommandContext context;
        RecordingConfigurationService service;
        ConfigureCommand(context, service).execute(false);
        QCOMPARE(service.Configured, QList<ConfigurationTarget>{ConfigurationTarget::User});
    }

    void configureCommand_executeAsync_system_invokesConfigurationServiceConfigureSystem()
    {
        TestCommandContext context;
        RecordingConfigurationService service;
        ConfigureCommand(context, service).execute(true);
        QCOMPARE(service.Configured, QList<ConfigurationTarget>{ConfigurationTarget::System});
    }

    void unconfigureCommand_executeAsync_user_invokesConfigurationServiceUnconfigureUser()
    {
        TestCommandContext context;
        RecordingConfigurationService service;
        UnconfigureCommand(context, service).execute(false);
        QCOMPARE(service.Unconfigured, QList<ConfigurationTarget>{ConfigurationTarget::User});
    }

    void unconfigureCommand_executeAsync_system_invokesConfigurationServiceUnconfigureSystem()
    {
        TestCommandContext context;
        RecordingConfigurationService service;
        UnconfigureCommand(context, service).execute(true);
        QCOMPARE(service.Unconfigured, QList<ConfigurationTarget>{ConfigurationTarget::System});
    }
};

// =============================================================================================

class DiagnoseCommandTests : public QObject
{
    Q_OBJECT
    QString m_dir;

    int runDiagnose(const QList<std::shared_ptr<MockDiagnostic>> &diagnostics, const QStringList &extraArgs, QString *output = nullptr)
    {
        TestCommandContext context;
        StringWriter console;
        DiagnoseCommand command(context, console);
        for (const auto &d : diagnostics) {
            command.addDiagnostic(d);
        }
        const int result = command.invoke(QStringList{Q("--output"), m_dir} + extraArgs);
        if (output) {
            *output = console.toString();
        }
        return result;
    }

private Q_SLOTS:
    void init() { m_dir = TestUtils::getTempDirectory(); }
    void cleanup() { QDir(m_dir).removeRecursively(); }

    void diagnoseCommand_allSuccessful_returnsZero()
    {
        auto d = std::make_shared<MockDiagnostic>(Q("TestDiagnostic"));
        QString output;
        QCOMPARE(runDiagnose({d}, {}, &output), 0);
        QCOMPARE(d->RunCalls, 1);
        QVERIFY(output.contains(Q("[ OK ] TestDiagnostic")));
        QVERIFY(output.contains(Q("1 passed    0 skipped    0 warned    0 failed")));
        QFile log(QDir(m_dir).filePath(Q("gcm-diagnose.log")));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(log.readAll());
        QVERIFY(text.contains(Q("Diagnostic: TestDiagnostic\nOutcome: Success\nLog:\n")));
    }

    void diagnoseCommand_atLeastOneFailure_returnsNonZero()
    {
        auto d1 = std::make_shared<MockDiagnostic>(Q("TestDiagnostic1"), QList<DiagnosticReport>{{DiagnosticReportKind::Error, Q("Failure"), std::nullopt}});
        auto d2 = std::make_shared<MockDiagnostic>(Q("TestDiagnostic2"));
        QString output;
        QVERIFY(runDiagnose({d1, d2}, {}, &output) != 0);
        QCOMPARE(d1->RunCalls, 1);
        QCOMPARE(d2->RunCalls, 1);
        QVERIFY(output.contains(Q("[FAIL] TestDiagnostic1")));
        QVERIFY(output.contains(Q("Failure")));
    }

    void diagnoseCommand_warnings_nonStrict_returnsZero()
    {
        auto d = std::make_shared<MockDiagnostic>(Q("TestDiagnostic"), QList<DiagnosticReport>{{DiagnosticReportKind::Warning, Q("Caution"), std::nullopt}});
        QCOMPARE(runDiagnose({d}, {}), 0);
        QCOMPARE(d->RunCalls, 1);
    }

    void diagnoseCommand_warnings_strict_returnsNonZero()
    {
        auto d = std::make_shared<MockDiagnostic>(Q("TestDiagnostic"), QList<DiagnosticReport>{{DiagnosticReportKind::Warning, Q("Caution"), std::nullopt}});
        QVERIFY(runDiagnose({d}, {Q("--strict")}) != 0);
        QCOMPARE(d->RunCalls, 1);
    }

    void networkingDiagnostic_sendHttpRequest_primary_ok()
    {
        TestDiagnosticReporter reporter;
        TestCommandContext context;
        NetworkingDiagnostic diagnostic(context);
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->setup(Q("HEAD"), QUrl(Q("http://example.com")), 200);
        diagnostic.sendHttpRequest(reporter, HttpClient(handler));
        handler->assertRequest(Q("HEAD"), QUrl(Q("http://example.com")), 1);
        QCOMPARE(reporter.Progress, QStringList{Q("Sending HEAD request to http://example.com")});
    }

    void networkingDiagnostic_sendHttpRequest_backup_ok()
    {
        TestDiagnosticReporter reporter;
        TestCommandContext context;
        NetworkingDiagnostic diagnostic(context);
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulatePrimaryUriFailure = true;
        handler->setup(Q("HEAD"), QUrl(Q("http://example.com")), 200);
        handler->setup(Q("HEAD"), QUrl(Q("http://httpforever.com")), 200);
        diagnostic.sendHttpRequest(reporter, HttpClient(handler));
        handler->assertRequest(Q("HEAD"), QUrl(Q("http://example.com")), 1);
        handler->assertRequest(Q("HEAD"), QUrl(Q("http://httpforever.com")), 1);
        QCOMPARE(reporter.Progress, (QStringList{Q("Sending HEAD request to http://example.com"), Q("Sending HEAD request to http://httpforever.com")}));
        QCOMPARE(reporter.Warnings, QStringList{Q("HEAD request failed")});
    }

    void networkingDiagnostic_sendHttpRequest_no_network()
    {
        TestDiagnosticReporter reporter;
        TestCommandContext context;
        NetworkingDiagnostic diagnostic(context);
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulateNoNetwork = true;
        diagnostic.sendHttpRequest(reporter, HttpClient(handler));
        handler->assertRequest(Q("HEAD"), QUrl(Q("http://example.com")), 1);
        handler->assertRequest(Q("HEAD"), QUrl(Q("http://httpforever.com")), 1);
        QCOMPARE(reporter.Progress.size(), 2);
        QCOMPARE(reporter.Warnings, (QStringList{Q("HEAD request failed"), Q("HEAD request failed")}));
    }

    // Own tests of the standard diagnostics against the fakes.
    void credentialStoreDiagnostic_roundTrips()
    {
        TestCommandContext context;
        CredentialStoreDiagnostic diagnostic(context);
        const DiagnosticResult result = diagnostic.run();
        QCOMPARE(result.outcome, DiagnosticOutcome::Success);
        QCOMPARE(result.reports.first().message, Q("Credential store is: Test Credential Store"));
        QCOMPARE(context.CredentialStore.count(), 0);
    }

    void fileSystemDiagnostic_runs()
    {
        TestCommandContext context;
        const DiagnosticResult result = FileSystemDiagnostic(context).run();
        QCOMPARE(result.outcome, DiagnosticOutcome::Success);
        QCOMPARE(result.reports.last().message, Q("GetCurrentDirectory(): %1").arg(context.FileSystem.CurrentDirectory));
    }

    void networkingDiagnostic_unhandledException_isError()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulateNoNetwork = true;
        context.HttpClientFactory.MessageHandler = handler;
        const DiagnosticResult result = NetworkingDiagnostic(context).run();
        QCOMPARE(result.outcome, DiagnosticOutcome::Error);
        QCOMPARE(result.reports.last().message, Q("Unhandled exception: Simulated no network"));
        QCOMPARE(result.exceptions(), QStringList{Q("System.Net.Http.HttpRequestException: Simulated no network")});
    }

    void exceptionToString_likeDotnet()
    {
        QCOMPARE(exceptionToString(Exception(Q("plain"))), Q("System.Exception: plain"));
        QCOMPARE(exceptionToString(IOException(Q("io"))), Q("System.IO.IOException: io"));
        QCOMPARE(exceptionToString(InteropException(Q("native"), 5)), Q("GitCredentialManager.Interop.InteropException: native"));
        QCOMPARE(exceptionToString(std::runtime_error("std")), Q("std"));
        const InvalidOperationException outer(Q("outer"), std::make_shared<KeyNotFoundException>(Q("inner")));
        QCOMPARE(exceptionToString(outer), Q("System.InvalidOperationException: outer ---> System.Collections.Generic.KeyNotFoundException: inner\n   --- End of inner exception stack trace ---"));
    }
};

GCM_REGISTER_TEST(GitCommandBaseTests)
GCM_REGISTER_TEST(StoreEraseCommandTests)
GCM_REGISTER_TEST(GetCommandTests)
GCM_REGISTER_TEST(CapabilityCommandTests)
GCM_REGISTER_TEST(ConfigurationCommandsTests)
GCM_REGISTER_TEST(DiagnoseCommandTests)
#include "commandstests.moc"
