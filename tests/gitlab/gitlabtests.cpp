// Ports of GitLab.Tests/GitLabAuthenticationTests.cs (all 7 methods) and GitLabHostProviderTests.cs (all 4 methods),
// plus own tests: the helper protocol (GitLab.UI and the git-credential-manager-ui fallback), the OAuth token storage
// with refresh and expiry, erase, detection by WWW-Authenticate and probe header, and the modes formatting.
#include "gitlab/gitlab.h"

#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)

using namespace gcm;
using namespace gcm::gitlab;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

GitRequest request(std::initializer_list<std::pair<const char *, const char *>> items)
{
    StringDictionary d;
    for (const auto &[k, v] : items) {
        d.set(Q(k), Q(v));
    }
    return GitRequest(d);
}

StringDictionary dict(std::initializer_list<std::pair<const char *, const char *>> items)
{
    StringDictionary d(Qt::CaseInsensitive);
    for (const auto &[k, v] : items) {
        d.set(Q(k), Q(v));
    }
    return d;
}

/// Records helper invocations instead of starting processes.
class TestGitLabAuthentication : public GitLabAuthentication
{
public:
    using GitLabAuthentication::GitLabAuthentication;
    QStringList Args;
    QStringList Paths;
    StringDictionary Result;

protected:
    StringDictionary invokeHelper(const QString &path, const QString &args, const OptString &, const CancellationToken &) override
    {
        Paths.append(path);
        Args.append(args);
        return Result;
    }
};

/// Stand-in for the Moq mock of IGitLabAuthentication.
class MockGitLabAuthentication : public IGitLabAuthentication
{
public:
    std::function<AuthenticationPromptResult(const QUrl &, const OptString &, AuthenticationModes)> GetAuthentication;
    std::function<OAuth2TokenResult(const QUrl &, const QStringList &)> GetOAuthTokenViaBrowser;
    std::function<OAuth2TokenResult(const QUrl &, const QString &)> GetOAuthTokenViaRefresh;
    int Prompts = 0;
    QStringList RefreshTokensUsed;

    AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override
    {
        ++Prompts;
        if (!GetAuthentication) {
            throw Exception(Q("unexpected call: getAuthentication"));
        }
        return GetAuthentication(targetUri, userName, modes);
    }
    OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes) override
    {
        if (!GetOAuthTokenViaBrowser) {
            throw Exception(Q("unexpected call: getOAuthTokenViaBrowser"));
        }
        return GetOAuthTokenViaBrowser(targetUri, scopes);
    }
    OAuth2TokenResult getOAuthTokenViaRefresh(const QUrl &targetUri, const QString &refreshToken) override
    {
        RefreshTokensUsed.append(refreshToken);
        if (!GetOAuthTokenViaRefresh) {
            throw Exception(Q("unexpected call: getOAuthTokenViaRefresh"));
        }
        return GetOAuthTokenViaRefresh(targetUri, refreshToken);
    }
};

OAuth2TokenResult tokens(const char *access, const char *refresh)
{
    OAuth2TokenResult r;
    r.accessToken = Q(access);
    r.tokenType = Q("bearer");
    r.refreshToken = Q(refresh);
    return r;
}

const QString UiHelperPath = Q("/usr/local/bin/git-credential-manager-ui");
const QString GitLabUiPath = Q("/usr/local/bin/GitLab.UI");

} // namespace

class GitLabAuthenticationTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getAuthentication_AuthenticationModesNone_ThrowsException()
    {
        TestCommandContext context;
        GitLabAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::None);
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QVERIFY(ex.message().contains(Q("(Parameter 'modes')")));
        }
    }

    void getAuthentication_SingleChoice_InteractionStillRequired()
    {
        TestCommandContext context;
        context.Settings.IsInteractionAllowed = true;
        context.SessionManager.IsDesktopSession = true; // necessary for browser
        context.Settings.IsGuiPromptsEnabled = false;
        GitLabAuthentication auth(context);
        QCOMPARE(auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::Browser).authenticationMode, AuthenticationModes::Browser);
    }

    void getAuthentication_TerminalPromptsDisabled_Throws()
    {
        TestCommandContext context;
        context.Settings.IsTerminalPromptsEnabled = false;
        GitLabAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Cannot prompt because terminal prompts have been disabled."));
        }
    }

    void getAuthentication_Terminal()
    {
        TestCommandContext context;
        GitLabAuthentication auth(context);
        context.SessionManager.IsDesktopSession = true;
        context.Settings.IsGuiPromptsEnabled = false;
        context.Console.pushSelection(0);
        QCOMPARE(auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All).authenticationMode, AuthenticationModes::Browser);
        QCOMPARE(context.Console.LastChoices, (QStringList{Q("Web browser"), Q("Personal access token"), Q("Username/password")}));
    }

    void getAuthentication_ChoosePat()
    {
        TestCommandContext context;
        GitLabAuthentication auth(context);
        context.Console.pushSelection(0);
        context.Console.pushText(Q("username"));
        context.Console.pushText(Q("token"));
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Pat);
        QCOMPARE(result.credential->account, Q("username"));
        QCOMPARE(result.credential->password, Q("token"));
        QCOMPARE(context.Console.Prompts, (QStringList{Q("Select an authentication method for ''"), Q("Username"), Q("Personal access token")}));
    }

    void getAuthentication_ChooseBasic()
    {
        TestCommandContext context;
        GitLabAuthentication auth(context);
        context.Console.pushSelection(1);
        context.Console.pushText(Q("username"));
        context.Console.pushText(Q("password"));
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(), std::nullopt, AuthenticationModes::All);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential->account, Q("username"));
        QCOMPARE(result.credential->password, Q("password"));
    }

    void getAuthentication_AuthenticationModesAll_RequiresInteraction()
    {
        TestCommandContext context;
        context.Settings.IsInteractionAllowed = false;
        GitLabAuthentication auth(context);
        try {
            auth.getAuthentication(QUrl(Q("https://GitLab.com")), std::nullopt, AuthenticationModes::All);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Cannot prompt because user interactivity has been disabled."));
        }
    }

    // --- own tests ------------------------------------------------------------------------------

    void helper_UiHelperFallback_Pat()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[UiHelperPath] = QByteArray();
        TestGitLabAuthentication auth(context);
        auth.Result = dict({{"mode", "pat"}, {"pat", "glpat-1"}, {"username", "jo"}});
        const AuthenticationPromptResult result = auth.getAuthentication(QUrl(Q("https://gitlab.example.com/g/r")), Q("jo"), AuthenticationModes::Basic | AuthenticationModes::Pat);
        QCOMPARE(auth.Paths, QStringList{UiHelperPath});
        QCOMPARE(auth.Args, QStringList{Q("gitlab prompt --username jo --url https://gitlab.example.com/g/r --basic --pat")});
        QCOMPARE(result.authenticationMode, AuthenticationModes::Pat);
        QCOMPARE(result.credential, (Credential{Q("jo"), Q("glpat-1")}));
    }

    void helper_GitLabUi_BasicAndErrors()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[GitLabUiPath] = QByteArray();
        TestGitLabAuthentication auth(context);
        auth.Result = dict({{"mode", "Basic"}, {"username", "u"}, {"password", "p"}});
        QCOMPARE(auth.getAuthentication(QUrl(Q("https://gitlab.com")), std::nullopt, AuthenticationModes::All).credential, (Credential{Q("u"), Q("p")}));
        QCOMPARE(auth.Paths, QStringList{GitLabUiPath});
        // The GitLab.UI protocol has no command prefix
        QCOMPARE(auth.Args, QStringList{Q("prompt --url https://gitlab.com/ --basic --browser --pat")});

        const QList<std::pair<StringDictionary, QString>> cases = {
            {dict({}), Q("Missing 'mode' in response")},
            {dict({{"mode", "pat"}}), Q("Missing 'pat' in response")},
            {dict({{"mode", "basic"}}), Q("Missing 'username' in response")},
            {dict({{"mode", "basic"}, {"username", "u"}}), Q("Missing 'password' in response")},
            {dict({{"mode", "device"}}), Q("Unknown mode value in response 'device'")},
        };
        for (const auto &[response, message] : cases) {
            auth.Result = response;
            try {
                auth.getAuthentication(QUrl(Q("https://gitlab.com")), std::nullopt, AuthenticationModes::All);
                QFAIL("no exception");
            } catch (const Exception &ex) {
                QCOMPARE(ex.message(), message);
            }
        }
        // A token without user name
        auth.Result = dict({{"mode", "pat"}, {"pat", "t"}});
        QCOMPARE(auth.getAuthentication(QUrl(Q("https://gitlab.com")), std::nullopt, AuthenticationModes::All).credential, (Credential{QString(), Q("t")}));
    }

    void terminal_SingleModes()
    {
        TestCommandContext context;
        GitLabAuthentication auth(context);
        context.Console.pushText(Q("pw"));
        const AuthenticationPromptResult basic = auth.getAuthentication(QUrl(Q("https://gitlab.example.com")), Q("jo"), AuthenticationModes::Basic);
        QCOMPARE(basic.credential, (Credential{Q("jo"), Q("pw")}));
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("Enter GitLab credentials for 'https://gitlab.example.com/'..."), Q("Username: jo")}));
        QCOMPARE(context.Console.Prompts, QStringList{Q("Password")});
    }

    void modes_ToStringAndParse()
    {
        QCOMPARE(modesToString(AuthenticationModes::All), Q("All"));
        QCOMPARE(modesToString(AuthenticationModes::Basic | AuthenticationModes::Pat), Q("Basic, Pat"));
        QCOMPARE(modesToString(AuthenticationModes::None), Q("None"));
        AuthenticationModes modes;
        QVERIFY(tryParseAuthenticationModes(Q("browser, PAT"), modes));
        QCOMPARE(modes, AuthenticationModes::Browser | AuthenticationModes::Pat);
        QVERIFY(!tryParseAuthenticationModes(Q("device"), modes));
    }
};

class GitLabHostProviderTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void isSupported_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("host");
        QTest::addColumn<bool>("expected");
        QTest::newRow("1") << Q("https") << Q("gitlab.com") << true;
        QTest::newRow("2") << Q("http") << Q("gitlab.com") << true;
        QTest::newRow("3") << Q("https") << Q("gitlab.example.com") << true;
        QTest::newRow("4") << Q("https") << Q("github.com") << false;
        QTest::newRow("5") << Q("https") << Q("github.example.com") << false;
    }

    void isSupported()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, host);
        QFETCH(bool, expected);
        StringDictionary d;
        d.set(Q("protocol"), protocol);
        d.set(Q("host"), host);
        TestCommandContext context;
        GitLabHostProvider provider(context);
        QCOMPARE(provider.isSupported(GitRequest(d)), expected);
    }

    void getSupportedAuthenticationModes_DotCom_ReturnsDotComModes()
    {
        TestCommandContext context;
        GitLabHostProvider provider(context);
        QCOMPARE(provider.getSupportedAuthenticationModes(QUrl(GitLabConstants::GitLabDotCom)), DotComAuthenticationModes);
    }

    void getSupportedAuthenticationModes_Custom_NoOAuthConfig_ReturnsBasicPat()
    {
        TestCommandContext context;
        GitLabHostProvider provider(context);
        QCOMPARE(provider.getSupportedAuthenticationModes(QUrl(Q("https://gitlab.example.com"))), AuthenticationModes::Basic | AuthenticationModes::Pat);
        QCOMPARE(context.Console.WrittenMessages, QStringList{Q("missing OAuth configuration for gitlab.example.com - see https://aka.ms/gcm/gitlab for more information")});
    }

    void getSupportedAuthenticationModes_Custom_WithOAuthConfig_ReturnsBasicPatBrowser()
    {
        TestCommandContext context;
        context.Environment.Variables.set(GitLabConstants::EnvironmentVariables::DevOAuthClientId, Q("abcdefg1234567"));
        GitLabHostProvider provider(context);
        QCOMPARE(provider.getSupportedAuthenticationModes(QUrl(Q("https://gitlab.example.com"))), AuthenticationModes::Basic | AuthenticationModes::Pat | AuthenticationModes::Browser);
    }

    // --- own tests ------------------------------------------------------------------------------

    void getSupportedAuthenticationModes_Override()
    {
        TestCommandContext context;
        context.Environment.Variables.set(GitLabConstants::EnvironmentVariables::AuthenticationModes, Q("pat"));
        GitLabHostProvider provider(context);
        QCOMPARE(provider.getSupportedAuthenticationModes(QUrl(Q("https://gitlab.com"))), AuthenticationModes::Pat);
        context.Environment.Variables.set(GitLabConstants::EnvironmentVariables::AuthenticationModes, Q("none"));
        QCOMPARE(provider.getSupportedAuthenticationModes(QUrl(Q("https://gitlab.com"))), AuthenticationModes::All);
    }

    void isSupported_WwwAuthAndResponse()
    {
        TestCommandContext context;
        GitLabHostProvider provider(context);
        QVERIFY(provider.isSupported(request({{"protocol", "https"}, {"host", "git.example.com"}, {"wwwauth", "Basic realm=\"GitLab\""}})));
        QVERIFY(!provider.isSupported(request({{"protocol", "ssh"}, {"host", "gitlab.com"}})));
        HttpResponse response;
        QVERIFY(!provider.isSupported(&response));
        response.headers.add(Q("X-Gitlab-Feature-Category"), Q("source_code_management"));
        QVERIFY(provider.isSupported(&response));
    }

    void generateCredential_UnencryptedHttp_Throws()
    {
        TestCommandContext context;
        GitLabHostProvider provider(context, std::make_shared<MockGitLabAuthentication>());
        try {
            provider.generateCredential(request({{"protocol", "http"}, {"host", "gitlab.com"}}));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().startsWith(Q("Unencrypted HTTP is not recommended for GitLab.")));
        }
    }

    void getCredential_Pat_NotStored()
    {
        TestCommandContext context;
        auto auth = std::make_shared<MockGitLabAuthentication>();
        auth->GetAuthentication = [](const QUrl &, const OptString &, AuthenticationModes) {
            return AuthenticationPromptResult{AuthenticationModes::Pat, Credential{Q("jo"), Q("glpat")}};
        };
        GitLabHostProvider provider(context, auth);
        const GitResponse response = provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}}));
        QCOMPARE(*response.credential(), (Credential{Q("jo"), Q("glpat")}));
        // Git stores it after a successful operation
        QVERIFY(context.CredentialStore.getAccounts(std::nullopt).isEmpty());
    }

    void getCredential_Browser_StoresAccessAndRefreshTokens()
    {
        TestCommandContext context;
        auto auth = std::make_shared<MockGitLabAuthentication>();
        auth->GetAuthentication = [](const QUrl &, const OptString &, AuthenticationModes) { return AuthenticationPromptResult{AuthenticationModes::Browser, {}}; };
        QStringList scopes;
        auth->GetOAuthTokenViaBrowser = [&](const QUrl &, const QStringList &s) {
            scopes = s;
            return tokens("access-1", "refresh-1");
        };
        GitLabHostProvider provider(context, auth);
        const GitResponse response = provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}}));
        QCOMPARE(*response.credential(), (Credential{Q("oauth2"), Q("access-1")}));
        QCOMPARE(scopes, (QStringList{Q("write_repository"), Q("read_repository")}));
        QCOMPARE(context.CredentialStore.get(Q("https://gitlab.com"), Q("oauth2"))->password, Q("access-1"));
        QCOMPARE(context.CredentialStore.get(Q("https://oauth-refresh-token.gitlab.com/"), Q("oauth2"))->password, Q("refresh-1"));
    }

    void getCredential_StoredOAuthToken_ValidOrExpired()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        context.HttpClientFactory.MessageHandler = handler;
        int infoStatus = 200;
        handler->setup(Q("GET"), QUrl(Q("https://gitlab.com/oauth/token/info")), [&](const HttpRequest &r) {
            [&] { QCOMPARE(r.headers.value(Q("Authorization")), OptString(Q("Bearer access-1"))); }();
            return TestHttpMessageHandler::createResponse(infoStatus);
        });
        context.CredentialStore.add(Q("https://gitlab.com"), Q("oauth2"), Q("access-1"));
        context.CredentialStore.add(Q("https://oauth-refresh-token.gitlab.com/"), Q("oauth2"), Q("refresh-1"));
        auto auth = std::make_shared<MockGitLabAuthentication>();
        auth->GetOAuthTokenViaRefresh = [](const QUrl &, const QString &) { return tokens("access-2", "refresh-2"); };
        GitLabHostProvider provider(context, auth);

        // Valid: used as is
        QCOMPARE(provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}})).credential()->password, Q("access-1"));
        QVERIFY(auth->RefreshTokensUsed.isEmpty());

        // Expired: removed, refreshed with the stored refresh token, both new tokens stored
        infoStatus = 401;
        QCOMPARE(provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}})).credential()->password, Q("access-2"));
        QCOMPARE(auth->RefreshTokensUsed, QStringList{Q("refresh-1")});
        QCOMPARE(context.CredentialStore.get(Q("https://gitlab.com"), Q("oauth2"))->password, Q("access-2"));
        QCOMPARE(context.CredentialStore.get(Q("https://oauth-refresh-token.gitlab.com/"), Q("oauth2"))->password, Q("refresh-2"));
        QCOMPARE(auth->Prompts, 0);
    }

    void getCredential_RefreshFails_Prompts()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://oauth-refresh-token.gitlab.com/"), Q("oauth2"), Q("refresh-1"));
        auto auth = std::make_shared<MockGitLabAuthentication>();
        auth->GetOAuthTokenViaRefresh = [](const QUrl &, const QString &) -> OAuth2TokenResult { throw OAuth2Exception(Q("invalid_grant")); };
        auth->GetAuthentication = [](const QUrl &, const OptString &, AuthenticationModes) {
            return AuthenticationPromptResult{AuthenticationModes::Basic, Credential{Q("u"), Q("p")}};
        };
        GitLabHostProvider provider(context, auth);
        QCOMPARE(*provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}})).credential(), (Credential{Q("u"), Q("p")}));
        QCOMPARE(context.Console.WrittenMessages, QStringList{Q("OAuth token refresh failed: invalid_grant")});
    }

    void getCredential_TokenInfoFails_KeepsToken()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulateNoNetwork = true;
        context.HttpClientFactory.MessageHandler = handler;
        context.CredentialStore.add(Q("https://gitlab.com"), Q("oauth2"), Q("access-1"));
        GitLabHostProvider provider(context, std::make_shared<MockGitLabAuthentication>());
        QCOMPARE(provider.getCredential(request({{"protocol", "https"}, {"host", "gitlab.com"}})).credential()->password, Q("access-1"));
        QCOMPARE(context.Console.WrittenMessages.size(), 1);
        QVERIFY(context.Console.WrittenMessages[0].startsWith(Q("OAuth token info request failed: ")));
    }

    void erase_RemovesRefreshToken()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://gitlab.example.com/group/repo"), Q("oauth2"), Q("a"));
        context.CredentialStore.add(Q("https://oauth-refresh-token.gitlab.example.com/group/repo"), Q("oauth2"), Q("r"));
        GitLabHostProvider provider(context);
        const GitRequest req = request({{"protocol", "https"}, {"host", "gitlab.example.com"}, {"path", "group/repo"}, {"username", "oauth2"}});
        QCOMPARE(provider.getRefreshTokenServiceName(req), Q("https://oauth-refresh-token.gitlab.example.com/group/repo"));
        provider.eraseCredential(req);
        QVERIFY(context.CredentialStore.getAccounts(std::nullopt).isEmpty());
    }
};

GCM_REGISTER_TEST(GitLabAuthenticationTests)
GCM_REGISTER_TEST(GitLabHostProviderTests)
#include "gitlabtests.moc"
