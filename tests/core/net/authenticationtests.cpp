// Ports of AuthenticationBaseTests, BasicAuthenticationTests and WindowsIntegratedAuthenticationTests, plus own
// tests of the UI-helper protocol and OAuthAuthentication (upstream has no tests for the latter).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/auth/authentication.h"
#include "core/constants.h"
#include "core/strings.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
#include "infra/testoauth2server.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

const QString HelperPath = Q("/usr/local/bin/git-credential-manager-ui");

/// Records helper invocations instead of starting a process (Mock<BasicAuthentication> upstream).
template <typename Base> class HelperRecorder : public Base
{
public:
    using Base::Base;
    QStringList paths, args;
    StringDictionary result{Qt::CaseInsensitive};

protected:
    StringDictionary invokeHelper(const QString &path, const QString &a, const OptString & = std::nullopt,
                                  const CancellationToken & = {}) override
    {
        paths.append(path);
        args.append(a);
        return result;
    }
};

/// Exposes the protected AuthenticationBase API.
class TestAuthentication : public AuthenticationBase
{
public:
    using AuthenticationBase::AuthenticationBase;
    using AuthenticationBase::invokeHelper;
    using AuthenticationBase::tryFindHelperCommand;
    using AuthenticationBase::tryFindUiHelper;
};

void useUiHelper(TestCommandContext &context)
{
    context.SessionManager.IsDesktopSession = true;
    context.Environment.Variables.set(Constants::EnvironmentVariables::GcmUiHelper, HelperPath);
    context.FileSystem.Files.set(HelperPath, {});
}
} // namespace

// =============================================================================================

class AuthenticationBaseTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void authenticationBase_quoteCmdArg_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << Q("foo") << Q("foo");
        QTest::newRow("2") << Q("foo bar") << Q("\"foo bar\"");
        QTest::newRow("3") << Q("foo\nbar") << Q("\"foo\nbar\"");
        QTest::newRow("4") << Q("foo\rbar") << Q("\"foo\rbar\"");
        QTest::newRow("5") << Q("foo\tbar") << Q("\"foo\tbar\"");
        QTest::newRow("6") << Q("foo\" bar") << Q("\"foo\\\" bar\"");
        QTest::newRow("7") << Q("foo\"") << Q("\"foo\\\"\"");
        QTest::newRow("8") << Q("\"foo") << Q("\"\\\"foo\"");
        QTest::newRow("9") << Q("foo\\") << Q("\"foo\\\\\"");
        QTest::newRow("10") << Q("foo\\\"") << Q("\"foo\\\\\\\"\"");
    }
    void authenticationBase_quoteCmdArg()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(quoteCmdArg(input), expected);
    }

    // Own tests: helper lookup and the helper protocol.
    void tryFindHelperCommand_resolution()
    {
        TestCommandContext context;
        TestAuthentication auth(context);
        QString command, args;

        // Default name: installation directory first, then PATH; a missing default helper is not a warning
        QVERIFY(!auth.tryFindHelperCommand(Q("GCM_X"), Q("x"), Q("helper"), command, args));
        QVERIFY(context.Console.WrittenMessages.isEmpty());
        context.FileSystem.Files.set(Q("/opt/bin/helper"), {});
        context.Environment.setPath({Q("/opt/bin")});
        QVERIFY(auth.tryFindHelperCommand(Q("GCM_X"), Q("x"), Q("helper"), command, args));
        QCOMPARE(command, Q("/opt/bin/helper"));
        context.FileSystem.Files.set(Q("/usr/local/bin/helper"), {});
        QVERIFY(auth.tryFindHelperCommand(Q("GCM_X"), Q("x"), Q("helper"), command, args));
        QCOMPARE(command, Q("/usr/local/bin/helper"));

        // An empty override disables the helper
        context.Environment.Variables.set(Q("GCM_X"), Q(""));
        QVERIFY(!auth.tryFindHelperCommand(Q("GCM_X"), Q("x"), Q("helper"), command, args));

        // A configured absolute path must exist
        context.Environment.Variables.set(Q("GCM_X"), Q("/missing/helper"));
        QVERIFY(!auth.tryFindHelperCommand(Q("GCM_X"), Q("x"), Q("helper"), command, args));
        QCOMPARE(context.Console.WrittenMessages, QStringList{Q("could not find configured UI helper '/missing/helper'")});

        // No GUI without a desktop session
        context.Environment.Variables.remove(Q("GCM_X"));
        context.SessionManager.IsDesktopSession = false;
        QVERIFY(!auth.tryFindUiHelper(command, args));
    }

    void invokeHelper_protocol()
    {
        TestCommandContext context;
        TestAuthentication auth(context);
        TestChildProcess *child = nullptr;
        context.ProcessManager.CreateProcessFunc = [&](const ProcessStartInfo &psi, Trace2ProcessClass cls) {
            auto p = std::make_unique<TestChildProcess>(psi, cls);
            p->StandardOutput = "username=john\nPassword=pw\n\nignored=1\n";
            child = p.get();
            return std::unique_ptr<ChildProcess>(std::move(p));
        };
        const StringDictionary result = auth.invokeHelper(HelperPath, Q("basic --resource x"), Q("payload"));
        QCOMPARE(result.value(Q("USERNAME")), Q("john"));
        QCOMPARE(result.value(Q("password")), Q("pw"));
        QVERIFY(!result.contains(Q("ignored")));
        QCOMPARE(context.ProcessManager.CreatedProcesses[0].arguments, Q("basic --resource x"));
        QVERIFY(!context.ProcessManager.CreatedProcesses[0].redirectStandardError);
        Q_UNUSED(child);
    }

    void invokeHelper_errorAndCancel()
    {
        TestCommandContext context;
        TestAuthentication auth(context);
        context.ProcessManager.setResult("error=boom\n\n", 3);
        try {
            auth.invokeHelper(HelperPath, Q("basic"));
            QFAIL("no exception");
        } catch (const OperationCanceledException &) {
            QFAIL("wrong exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("helper error (3): boom"));
        }

        context.ProcessManager.setResult("", 1);
        try {
            auth.invokeHelper(HelperPath, Q("basic"));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("helper error (1): Unknown"));
        }

        context.ProcessManager.setResult("cancelled=1\nerror=User cancelled\n\n", 1);
        QVERIFY_THROWS_EXCEPTION(OperationCanceledException, auth.invokeHelper(HelperPath, Q("basic")));
    }
};

// =============================================================================================

class BasicAuthenticationTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void basicAuthentication_getCredentials_nullResource_throwsException()
    {
        TestCommandContext context;
        BasicAuthentication basicAuth(context);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, basicAuth.getCredentials(QString()));
    }

    void basicAuthentication_getCredentials_nonDesktopSession_resourceAndUserName_passwordPromptReturnsCredentials()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = false;
        context.Console.pushText(Q("letmein123"));
        BasicAuthentication basicAuth(context);
        const Credential credential = basicAuth.getCredentials(Q("https://example.com"), Q("john.doe"));
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("letmein123"));
        QCOMPARE(context.Console.Prompts, QStringList{Q("Password")});
        QCOMPARE(context.Console.WrittenMessages,
                 (QStringList{Q("Enter basic credentials for 'https://example.com':"), Q("Username: john.doe")}));
    }

    void basicAuthentication_getCredentials_nonDesktopSession_resource_userPassPromptReturnsCredentials()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = false;
        context.Console.pushText(Q("john.doe"));
        context.Console.pushText(Q("letmein123"));
        BasicAuthentication basicAuth(context);
        const Credential credential = basicAuth.getCredentials(Q("https://example.com"));
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("letmein123"));
        QCOMPARE(context.Console.Prompts, (QStringList{Q("Username"), Q("Password")}));
    }

    void basicAuthentication_getCredentials_nonDesktopSession_noTerminalPrompts_throwsException()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = false;
        context.Settings.IsInteractionAllowed = false;
        BasicAuthentication basicAuth(context);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, basicAuth.getCredentials(Q("https://example.com")));
    }

    void basicAuthentication_getCredentials_desktopSession_uiHelper_callsHelper()
    {
        TestCommandContext context;
        useUiHelper(context);
        HelperRecorder<BasicAuthentication> auth(context);
        auth.result = StringDictionary{{Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}};
        const Credential credential = auth.getCredentials(Q("https://example.com"));
        QCOMPARE(auth.args, QStringList{Q("basic --resource https://example.com")});
        QCOMPARE(auth.paths, QStringList{HelperPath});
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("letmein123"));
    }

    void basicAuthentication_getCredentials_desktopSession_uiHelper_userName_callsHelper()
    {
        TestCommandContext context;
        useUiHelper(context);
        HelperRecorder<BasicAuthentication> auth(context);
        auth.result = StringDictionary{{Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}};
        const Credential credential = auth.getCredentials(Q("https://example.com"), Q("john.doe"));
        QCOMPARE(auth.args, QStringList{Q("basic --resource https://example.com --username john.doe")});
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("letmein123"));
    }

    // Own tests
    void basicAuthentication_getCredentials_desktopSession_noHelper_fallsBackToTerminal()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.Console.pushText(Q("pw"));
        HelperRecorder<BasicAuthentication> auth(context);
        const Credential credential = auth.getCredentials(Q("https://example.com"), Q("john"));
        QVERIFY(auth.args.isEmpty());
        QCOMPARE(credential.password, Q("pw"));
    }

    void basicAuthentication_getCredentials_helperMissingPassword_throws()
    {
        TestCommandContext context;
        useUiHelper(context);
        HelperRecorder<BasicAuthentication> auth(context);
        auth.result = StringDictionary{{Q("username"), Q("john.doe")}};
        try {
            auth.getCredentials(Q("https://example.com"));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("Missing 'password' in response"));
        }
    }

    void basicAuthentication_getCredentials_terminalPromptsDisabled_throws()
    {
        TestCommandContext context;
        context.Settings.IsTerminalPromptsEnabled = false;
        BasicAuthentication basicAuth(context);
        try {
            basicAuth.getCredentials(Q("https://example.com"));
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Cannot prompt because terminal prompts have been disabled."));
        }
    }
};

// =============================================================================================

class WindowsIntegratedAuthenticationTests : public QObject
{
    Q_OBJECT
    const QString NegotiateHeader = Q("Negotiate [test-challenge-string]");
    const QString NtlmHeader = Q("NTLM [test-challenge-string]");

    static void testGetIsSupported(const QStringList &wwwAuthHeaders, WindowsAuthenticationTypes expected)
    {
        TestCommandContext context;
        const QUrl uri(Q("https://example.com"));
        HttpResponse wiaResponse = TestHttpMessageHandler::createResponse(401);
        for (const QString &header : wwwAuthHeaders) {
            wiaResponse.headers.add(Q("WWW-Authenticate"), header);
        }
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->ThrowOnUnexpectedRequest = true;
        httpHandler->setup(Q("HEAD"), uri, wiaResponse);
        context.HttpClientFactory.MessageHandler = httpHandler;

        WindowsIntegratedAuthentication wiaAuth(context);
        QCOMPARE(int(wiaAuth.getAuthenticationTypes(uri)), int(expected));
        httpHandler->assertRequest(Q("HEAD"), uri, 1);
    }

private Q_SLOTS:
    void windowsIntegratedAuthentication_getIsSupportedAsync_nullUri_throwsException()
    {
        TestCommandContext context;
        WindowsIntegratedAuthentication wiaAuth(context);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, wiaAuth.getAuthenticationTypes(QUrl()));
    }

    void windowsIntegratedAuthentication_getIsSupportedAsync_negotiateAndNtlm_returnsTrue()
    {
        testGetIsSupported({NegotiateHeader, NtlmHeader}, WindowsAuthenticationTypes::All);
        // Also check with the headers in the other order
        testGetIsSupported({NtlmHeader, NegotiateHeader}, WindowsAuthenticationTypes::All);
    }

    void windowsIntegratedAuthentication_getIsSupportedAsync_ntlm_returnsTrue()
    {
        testGetIsSupported({NtlmHeader}, WindowsAuthenticationTypes::Ntlm);
    }

    void windowsIntegratedAuthentication_getIsSupportedAsync_negotiate_returnsTrue()
    {
        testGetIsSupported({NegotiateHeader}, WindowsAuthenticationTypes::Negotiate);
    }

    void windowsIntegratedAuthentication_getIsSupportedAsync_noHeaders_returnsFalse()
    {
        testGetIsSupported({}, WindowsAuthenticationTypes::None);
    }

    void windowsIntegratedAuthentication_getIsSupportedAsync_noWiaHeaders_returnsFalse()
    {
        testGetIsSupported({Q("Bearer"), Q("Bearer foo"), Q("NotNTLM test test NTLM test"),
                            Q("NotNegotiate test test Negotiate test"), Q("NotKerberos test test Negotiate test")},
                           WindowsAuthenticationTypes::None);
    }

    // Own test
    void windowsIntegratedAuthentication_askEnableNtlm_terminalSelection()
    {
        TestCommandContext context;
        context.Console.pushSelection(1);
        WindowsIntegratedAuthentication wiaAuth(context);
        QCOMPARE(int(wiaAuth.askEnableNtlm(QUrl(Q("https://example.com/repo")))), int(NtlmSupport::Always));
        QCOMPARE(context.Console.LastChoices[1], Q("Yes - always for https://example.com/repo"));
    }
};

// =============================================================================================
// Own tests of OAuthAuthentication

class OAuthAuthenticationTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void getAuthenticationMode_singleModeOrNoDesktop_noPrompt()
    {
        TestCommandContext context;
        OAuthAuthentication auth(context);
        QCOMPARE(int(auth.getAuthenticationMode(Q("https://example.com"), OAuthAuthenticationModes::All)),
                 int(OAuthAuthenticationModes::DeviceCode)); // no desktop: browser removed
        QVERIFY_THROWS_EXCEPTION(ArgumentException,
                                 auth.getAuthenticationMode(Q("https://example.com"), OAuthAuthenticationModes::Browser));
        QVERIFY(context.Console.Prompts.isEmpty());
    }

    void getAuthenticationMode_terminalMenu()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.Console.pushSelection(0);
        OAuthAuthentication auth(context);
        QCOMPARE(int(auth.getAuthenticationMode(Q("https://example.com"), OAuthAuthenticationModes::All)),
                 int(OAuthAuthenticationModes::Browser));
        QCOMPARE(context.Console.Prompts, QStringList{Q("Select an authentication method for 'https://example.com'")});
        QCOMPARE(context.Console.LastChoices, (QStringList{Q("Web browser"), Q("Device code")}));
    }

    void getAuthenticationMode_helper()
    {
        TestCommandContext context;
        useUiHelper(context);
        HelperRecorder<OAuthAuthentication> auth(context);
        auth.result = StringDictionary{{Q("mode"), Q("DeviceCode")}};
        QCOMPARE(int(auth.getAuthenticationMode(Q("https://example.com"), OAuthAuthenticationModes::All)),
                 int(OAuthAuthenticationModes::DeviceCode));
        QCOMPARE(auth.args, QStringList{Q("oauth --resource https://example.com --browser --device-code")});
    }

    void getTokenByDeviceCode_terminal()
    {
        TestCommandContext context;
        OAuth2ServerEndpoints endpoints(QUrl(Q("https://example.com/authorize")), QUrl(Q("https://example.com/token")));
        endpoints.deviceAuthorizationEndpoint = QUrl(Q("https://example.com/device"));
        auto handler = std::make_shared<TestHttpMessageHandler>();
        TestOAuth2Server server(endpoints);
        auto app = std::make_shared<OAuth2Application>(Q("id"));
        app->RedirectUris = {Q("http://localhost/")};
        server.registerApplication(app);
        server.bind(*handler);
        server.TokenGenerator.UserCodes.append(Q("USER-CODE"));
        server.TokenGenerator.AccessTokens.append(Q("TOKEN"));
        // The user signs in as soon as the device code has been issued
        server.DeviceAuthorizationEndpointInvoked = [&](const HttpRequest &) {};
        OAuth2Client client(HttpClient(handler), endpoints, Q("id"));

        // Sign in when the first poll arrives
        server.TokenEndpointInvoked = [&](const HttpRequest &) {
            if (!app->DeviceGrants.isEmpty() && !app->DeviceGrants.first().approved) {
                server.signInDeviceWithUserCode(Q("USER-CODE"));
            }
        };
        OAuthAuthentication auth(context);
        const OAuth2TokenResult result = auth.getTokenByDeviceCode(client, {Q("read")});
        QCOMPARE(result.accessToken, Q("TOKEN"));
        QCOMPARE(context.Console.WrittenMessages,
                 QStringList{Q("To complete authentication please visit https://example.com/devicelogin and enter the following code:\nUSER-CODE")});
    }

    void getTokenByBrowser_noDesktop_throws()
    {
        TestCommandContext context;
        OAuthAuthentication auth(context);
        OAuth2Client client(HttpClient(std::make_shared<TestHttpMessageHandler>()), OAuth2ServerEndpoints(), Q("id"));
        try {
            auth.getTokenByBrowser(client, {});
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Browser authentication requires a desktop session"));
        }
    }
};

GCM_REGISTER_TEST(AuthenticationBaseTests)
GCM_REGISTER_TEST(BasicAuthenticationTests)
GCM_REGISTER_TEST(WindowsIntegratedAuthenticationTests)
GCM_REGISTER_TEST(OAuthAuthenticationTests)
#include "authenticationtests.moc"
