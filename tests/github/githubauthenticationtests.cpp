// Port of GitHub.Tests/GitHubAuthenticationTests.cs (all 6 methods) and GitHubOAuth2ClientTests.cs (1 method), plus
// own tests: the arguments sent to the helper for every prompt, the git-credential-manager-ui fallback with its
// `github` prefix, and the terminal prompts.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "github/github.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::github;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

/// Records helper invocations instead of starting processes (the Moq override of InvokeHelperAsync upstream).
class TestGitHubAuthentication : public GitHubAuthentication
{
public:
    using GitHubAuthentication::GitHubAuthentication;

    struct Call
    {
        QString path;
        QString args;
        OptString standardInput;
    };
    QList<Call> Calls;
    std::function<StringDictionary(const Call &)> Result;

protected:
    StringDictionary invokeHelper(const QString &path, const QString &args, const OptString &standardInput,
                                  const CancellationToken &) override
    {
        const Call call{path, args, standardInput};
        Calls.append(call);
        if (!Result) {
            throw Exception(QStringLiteral("unexpected helper call: %1").arg(args));
        }
        return Result(call);
    }
};

StringDictionary dict(std::initializer_list<std::pair<const char *, const char *>> items)
{
    StringDictionary d(Qt::CaseInsensitive);
    for (const auto &[k, v] : items) {
        d.set(Q(k), Q(v));
    }
    return d;
}

const QString UnixHelperPath = Q("/usr/local/bin/GitHub.UI");
const QString UiHelperPath = Q("/usr/local/bin/git-credential-manager-ui");

} // namespace

class GitHubAuthenticationTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getAuthentication_AuthenticationModesNone_ThrowsException()
    {
        TestCommandContext context;
        GitHubAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::None);
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QVERIFY(ex.message().contains(Q("(Parameter 'modes')")));
        }
    }

    void getAuthentication_SingleChoice_TerminalAndInteractionNotRequired_data()
    {
        QTest::addColumn<int>("modes");
        QTest::newRow("Browser") << int(AuthenticationModes::Browser);
        QTest::newRow("Device") << int(AuthenticationModes::Device);
    }

    void getAuthentication_SingleChoice_TerminalAndInteractionNotRequired()
    {
        QFETCH(int, modes);
        TestCommandContext context;
        context.Settings.IsTerminalPromptsEnabled = false;
        context.Settings.IsInteractionAllowed = false;
        context.SessionManager.IsDesktopSession = true; // necessary for browser
        context.FileSystem.Files[UnixHelperPath] = QByteArray();
        GitHubAuthentication auth(context);
        QCOMPARE(auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes(modes)).authenticationMode, AuthenticationModes(modes));
    }

    void getAuthentication_TerminalPromptsDisabled_Throws()
    {
        TestCommandContext context;
        context.Settings.IsTerminalPromptsEnabled = false;
        GitHubAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Cannot prompt because terminal prompts have been disabled."));
        }
    }

    // reproduces https://github.com/git-ecosystem/git-credential-manager/issues/453
    void getAuthentication_Terminal()
    {
        TestCommandContext context;
        context.FileSystem.Files[UnixHelperPath] = QByteArray();
        GitHubAuthentication auth(context);
        context.Console.pushSelection(0);
        QCOMPARE(auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All).authenticationMode, AuthenticationModes::Device);
        // No desktop session: no browser option
        QCOMPARE(context.Console.LastChoices, (QStringList{Q("Device code"), Q("Personal access token"), Q("Username/password")}));
    }

    void getAuthentication_Pat_DoesNotEchoToken()
    {
        const QString token = Q("secret-token");
        TestCommandContext context;
        context.Console.pushText(token);
        GitHubAuthentication auth(context);
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(Q("https://github.com")), Q("octocat"), AuthenticationModes::Pat);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Pat);
        QCOMPARE(result.credential->password, token);
        QCOMPARE(result.credential->account, Q("octocat"));
        // The token is read with a secret prompt and never written
        QCOMPARE(context.Console.Prompts, QStringList{Q("Token")});
        for (const QString &message : std::as_const(context.Console.WrittenMessages)) {
            QVERIFY(!message.contains(token));
        }
    }

    void getAuthentication_AuthenticationModesAll_RequiresInteraction()
    {
        TestCommandContext context;
        context.Settings.IsInteractionAllowed = false;
        GitHubAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(Q("https://github.com")), std::nullopt, AuthenticationModes::All);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Cannot prompt because user interactivity has been disabled."));
        }
    }

    void getAuthentication_Helper_Basic()
    {
        TestCommandContext context;
        context.FileSystem.Files[UnixHelperPath] = QByteArray();
        context.SessionManager.IsDesktopSession = true;
        context.Environment.Variables.set(GitHubConstants::EnvironmentVariables::AuthenticationHelper, UnixHelperPath);
        TestGitHubAuthentication auth(context);
        auth.Result = [](const TestGitHubAuthentication::Call &) {
            return dict({{"mode", "basic"}, {"username", "tim"}, {"password", "hunter2"}});
        };
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(Q("https://github.com")), std::nullopt, AuthenticationModes::All);
        QCOMPARE(auth.Calls.size(), 1);
        QCOMPARE(auth.Calls[0].path, UnixHelperPath);
        QCOMPARE(auth.Calls[0].args, Q("prompt --all"));
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential->account, Q("tim"));
        QCOMPARE(result.credential->password, Q("hunter2"));
    }

    // Port of GitHubOAuth2ClientTests.GitHubOAuth2Client_CommaSeparatedScopes
    void oauth2Client_CommaSeparatedScopes()
    {
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const QUrl baseUri(Q("https://github.com/"));
        handler->setup(Q("POST"), baseUri.resolved(QUrl(Q("login/oauth/access_token"))), 200,
                       Q("{ \"access_token\": \"test-token\", \"token_type\": \"bearer\", \"scope\": \"repo,gist,workflow\" }"));
        TestSettings settings;
        GitHubOAuth2Client client(HttpClient(handler), settings, baseUri);
        OAuth2AuthorizationCodeResult authCode;
        authCode.code = Q("test-auth-code");
        const OAuth2TokenResult result = client.getTokenByAuthorizationCode(authCode, CancellationToken());
        QCOMPARE(result.scopes, (QStringList{Q("repo"), Q("gist"), Q("workflow")}));
    }

    // --- own tests ------------------------------------------------------------------------------

    void helper_FallsBackToUiHelperWithGitHubPrefix()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitHubAuthentication auth(context);
        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({{"mode", "pat"}, {"pat", "ghp_x"}}); };
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(Q("https://ghe.example.com/org/repo")), Q("al ice"),
                                                                         AuthenticationModes::Pat | AuthenticationModes::Basic);
        QCOMPARE(auth.Calls.size(), 1);
        QCOMPARE(auth.Calls[0].path, UiHelperPath);
        QCOMPARE(auth.Calls[0].args, Q("github prompt --basic --pat --enterprise-url https://ghe.example.com/org/repo --username \"al ice\""));
        QCOMPARE(result.authenticationMode, AuthenticationModes::Pat);
        QCOMPARE(result.credential->account, Q("al ice"));
        QCOMPARE(result.credential->password, Q("ghp_x"));
    }

    void helper_ResponseErrors()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitHubAuthentication auth(context);
        const QList<std::pair<StringDictionary, QString>> cases = {
            {dict({}), Q("Missing 'mode' in response")},
            {dict({{"mode", "pat"}}), Q("Missing 'pat' in response")},
            {dict({{"mode", "basic"}}), Q("Missing 'username' in response")},
            {dict({{"mode", "basic"}, {"username", "u"}}), Q("Missing 'password' in response")},
            {dict({{"mode", "Bogus"}}), Q("Unknown mode value in response 'Bogus'")},
        };
        for (const auto &[response, message] : cases) {
            auth.Result = [response](const TestGitHubAuthentication::Call &) { return response; };
            try {
                auth.getAuthentication(QUrl(Q("https://github.com")), std::nullopt, AuthenticationModes::All);
                QFAIL("no exception");
            } catch (const Exception &ex) {
                QCOMPARE(ex.message(), message);
            }
        }
        // Modes from the helper are case-insensitive
        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({{"mode", "DEVICE"}}); };
        QCOMPARE(auth.getAuthentication(QUrl(Q("https://github.com")), std::nullopt, AuthenticationModes::All).authenticationMode,
                 AuthenticationModes::Device);
    }

    void helper_SelectAccount()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitHubAuthentication auth(context);
        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({{"account", "bob"}}); };
        QCOMPARE(auth.selectAccount(QUrl(Q("https://github.com/")), {Q("alice"), Q("bob")}), OptString(Q("bob")));
        QCOMPARE(auth.Calls[0].args, Q("github select-account"));
        QCOMPARE(auth.Calls[0].standardInput, OptString(Q("alice\nbob")));

        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({{"account", ""}}); };
        QCOMPARE(auth.selectAccount(QUrl(Q("https://ghe.example.com/")), {Q("alice")}), OptString());
        QCOMPARE(auth.Calls[1].args, Q("github select-account --enterprise-url https://ghe.example.com/"));

        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({}); };
        QVERIFY_THROWS_EXCEPTION(Exception, auth.selectAccount(QUrl(Q("https://github.com/")), {Q("alice")}));
    }

    void helper_TwoFactor()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitHubAuthentication auth(context);
        auth.Result = [](const TestGitHubAuthentication::Call &) { return dict({{"code", "123456"}}); };
        QCOMPARE(auth.getTwoFactorCode(QUrl(Q("https://github.com")), true), Q("123456"));
        QCOMPARE(auth.getTwoFactorCode(QUrl(Q("https://github.com")), false), Q("123456"));
        QCOMPARE(auth.Calls[0].args, Q("github 2fa --sms"));
        QCOMPARE(auth.Calls[1].args, Q("github 2fa"));
    }

    void helper_NoDesktopSession_UsesTerminal()
    {
        TestCommandContext context;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitHubAuthentication auth(context);
        context.Console.pushSelection(2);
        QCOMPARE(auth.selectAccount(QUrl(Q("https://github.com")), {Q("alice"), Q("bob")}), OptString(Q("bob")));
        QCOMPARE(context.Console.Prompts.constLast(), Q("Select an account for 'https://github.com/'"));
        QCOMPARE(context.Console.LastChoices, (QStringList{Q("Add a new account"), Q("alice"), Q("bob")}));
        context.Console.pushSelection(0);
        QCOMPARE(auth.selectAccount(QUrl(Q("https://github.com")), {Q("alice")}), OptString());
        QVERIFY(auth.Calls.isEmpty());
    }

    void terminal_BasicAndTwoFactor()
    {
        TestCommandContext context;
        GitHubAuthentication auth(context);
        context.Console.pushText(Q("pw"));
        const AuthenticationPromptResult basic = auth.getAuthentication(QUrl(Q("https://ghe.example.com")), Q("tim"), AuthenticationModes::Basic);
        QCOMPARE(basic.credential->account, Q("tim"));
        QCOMPARE(basic.credential->password, Q("pw"));
        QVERIFY(context.Console.WrittenMessages.contains(Q("Enter GitHub credentials for 'https://ghe.example.com/'...")));
        QVERIFY(context.Console.WrittenMessages.contains(Q("Username: tim")));

        context.Console.pushText(Q("654321"));
        QCOMPARE(auth.getTwoFactorCode(QUrl(Q("https://github.com")), true), Q("654321"));
        QVERIFY(context.Console.WrittenMessages.contains(Q("An SMS containing the authentication code has been sent to your registered device.")));
        QCOMPARE(context.Console.Prompts.constLast(), Q("Authentication code"));
    }

    void modes_ToStringAndParse()
    {
        QCOMPARE(modesToString(AuthenticationModes::None), Q("None"));
        QCOMPARE(modesToString(AuthenticationModes::All), Q("All"));
        QCOMPARE(modesToString(AuthenticationModes::OAuth | AuthenticationModes::Pat), Q("Pat, OAuth"));
        QCOMPARE(modesToString(AuthenticationModes::Basic | AuthenticationModes::Pat), Q("Basic, Pat"));
        QCOMPARE(modesToString(AuthenticationModes::Browser | AuthenticationModes::Pat), Q("Browser, Pat"));

        AuthenticationModes modes;
        QVERIFY(tryParseAuthenticationModes(Q("browser"), modes));
        QCOMPARE(modes, AuthenticationModes::Browser);
        QVERIFY(tryParseAuthenticationModes(Q(" pat , DEVICE"), modes));
        QCOMPARE(modes, AuthenticationModes::Pat | AuthenticationModes::Device);
        QVERIFY(tryParseAuthenticationModes(Q("5"), modes));
        QCOMPARE(modes, AuthenticationModes::Basic | AuthenticationModes::Pat);
        QVERIFY(tryParseAuthenticationModes(Q("none"), modes));
        QCOMPARE(modes, AuthenticationModes::None);
        QVERIFY(!tryParseAuthenticationModes(Q("NOT-A-REAL-VALUE"), modes));
        QVERIFY(!tryParseAuthenticationModes(Q(""), modes));
    }
};

GCM_REGISTER_TEST(GitHubAuthenticationTests)
#include "githubauthenticationtests.moc"
