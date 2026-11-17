// Port of GitHub.Tests/GitHubHostProviderTests.cs (all 15 methods), plus own tests of store/erase, account
// filtering settings, the Enterprise fallback when /meta fails, and the `github` commands.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "githubmocks.h"

#include "core/cmdline.h"
#include "core/streams.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::github;
using namespace gcm::github::test;
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

const AuthenticationModes DotComAuthenticationModes = AuthenticationModes::OAuth | AuthenticationModes::Pat;
const QStringList ExpectedOAuthScopes = {Q("repo"), Q("gist"), Q("workflow")};
const QStringList ExpectedPatScopes = {Q("gist"), Q("repo")};

struct Fixture
{
    TestCommandContext context;
    std::shared_ptr<MockGitHubRestApi> api = std::make_shared<MockGitHubRestApi>();
    std::shared_ptr<MockGitHubAuthentication> auth = std::make_shared<MockGitHubAuthentication>();
    GitHubHostProvider provider{context, api, auth};

    /// Runs `github <args>` through the command-line parser.
    int runCommand(const QStringList &args)
    {
        auto root = std::make_shared<Command>(Q("git-credential-manager"), QString());
        root->addCommand(provider.createCommand());
        StringWriter out, err;
        CommandLineParser parser(root, Q("git-credential-manager"), Q("0"), out, err);
        return parser.invoke(QStringList{Q("github")} + args);
    }
};

QString uriString(const QUrl &uri) { return absoluteUri(uri); }

} // namespace

class GitHubHostProviderTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void isGitHubDotCom_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("expected");
        QTest::newRow("1") << Q("https://github.com") << true;
        QTest::newRow("2") << Q("https://gitHUB.CoM") << true;
        QTest::newRow("3") << Q("https://GITHUB.COM") << true;
        QTest::newRow("4") << Q("https://gist.github.com") << true;
        QTest::newRow("5") << Q("https://foogithub.com") << false;
        QTest::newRow("6") << Q("https://api.github.com") << false;
        QTest::newRow("7") << Q("https://api.gist.github.com") << false;
        QTest::newRow("8") << Q("https://foogist.github.com") << false;
    }

    void isGitHubDotCom()
    {
        QFETCH(QString, url);
        QFETCH(bool, expected);
        QCOMPARE(GitHubHostProvider::isGitHubDotCom(QUrl(url)), expected);
    }

    void isGitHubProxima_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("expected");
        QTest::addColumn<QString>("expectedSubdomain");
        QTest::newRow("1") << Q("https://ghe.com") << false << QString();
        QTest::newRow("2") << Q("https://ocToCoRp.gHe.CoM") << true << Q("octocorp");
        QTest::newRow("3") << Q("https://OCTOCORP.GHE.COM") << true << Q("octocorp");
        QTest::newRow("4") << Q("https://fooghe.com") << false << QString();
    }

    // Also covers GitHubHostProvider_IsGitHubProxima_ReturnsSubdomain
    void isGitHubProxima()
    {
        QFETCH(QString, url);
        QFETCH(bool, expected);
        QFETCH(QString, expectedSubdomain);
        QCOMPARE(GitHubHostProvider::isGitHubProxima(QUrl(url)), expected);
        QString subdomain;
        QCOMPARE(GitHubHostProvider::isGitHubProxima(QUrl(url), &subdomain), expected);
        QCOMPARE(subdomain, expectedSubdomain);
    }

    void isSupported_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("host");
        QTest::addColumn<bool>("expected");
        // We report that we support unencrypted HTTP here so that we can fail and show a helpful error message in
        // the call to `generateCredential` instead.
        QTest::newRow("1") << Q("http") << Q("github.com") << true;
        QTest::newRow("2") << Q("http") << Q("gist.github.com") << true;
        QTest::newRow("3") << Q("ssh") << Q("github.com") << false;
        QTest::newRow("4") << Q("https") << Q("example.com") << false;
        QTest::newRow("5") << Q("https") << Q("github.com") << true;
        QTest::newRow("6") << Q("https") << Q("github.con") << false;
        QTest::newRow("7") << Q("https") << Q("gist.github.con") << false;
        QTest::newRow("8") << Q("https") << Q("foogithub.com") << false;
        QTest::newRow("9") << Q("https") << Q("api.github.com") << false;
        QTest::newRow("10") << Q("https") << Q("gist.github.com") << true;
        QTest::newRow("11") << Q("https") << Q("GiST.GitHub.Com") << true;
        QTest::newRow("12") << Q("https") << Q("GitHub.Com") << true;
        QTest::newRow("13") << Q("https") << Q("octocorp.ghe.com") << true;
        QTest::newRow("14") << Q("https") << Q("octocorp.GhE.cOm") << true;
        QTest::newRow("15") << Q("https") << Q("octocorpghe.cOm") << false;
        QTest::newRow("16") << Q("http") << Q("github.my-company-server.com") << true;
        QTest::newRow("17") << Q("http") << Q("gist.github.my-company-server.com") << true;
        QTest::newRow("18") << Q("https") << Q("github.my-company-server.com") << true;
        QTest::newRow("19") << Q("https") << Q("gist.github.my-company-server.com") << true;
        QTest::newRow("20") << Q("https") << Q("gist.my-company-server.com") << false;
        QTest::newRow("21") << Q("https") << Q("my-company-server.com") << false;
        QTest::newRow("22") << Q("https") << Q("github.my.company.server.com") << true;
        QTest::newRow("23") << Q("https") << Q("foogithub.my-company-server.com") << false;
        QTest::newRow("24") << Q("https") << Q("api.github.my-company-server.com") << false;
        QTest::newRow("25") << Q("https") << Q("gist.github.my.company.server.com") << true;
        QTest::newRow("26") << Q("https") << Q("GitHub.My-Company-Server.Com") << true;
        QTest::newRow("27") << Q("https") << Q("GiST.GitHub.My-Company-Server.com") << true;
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
        GitHubHostProvider provider(context);
        QCOMPARE(provider.isSupported(GitRequest(d)), expected);
    }

    void getCredentialServiceUrl_data()
    {
        QTest::addColumn<QString>("host");
        QTest::addColumn<QString>("expectedService");
        QTest::newRow("1") << Q("github.com") << Q("https://github.com");
        QTest::newRow("2") << Q("GitHub.Com") << Q("https://github.com");
        QTest::newRow("3") << Q("gist.github.com") << Q("https://github.com");
        QTest::newRow("4") << Q("GiST.GitHub.Com") << Q("https://github.com");
        QTest::newRow("5") << Q("github.my-company-server.com") << Q("https://github.my-company-server.com");
        QTest::newRow("6") << Q("GitHub.My-Company-Server.Com") << Q("https://github.my-company-server.com");
        QTest::newRow("7") << Q("gist.github.my-company-server.com") << Q("https://github.my-company-server.com");
        QTest::newRow("8") << Q("GiST.GitHub.My-Company-Server.Com") << Q("https://github.my-company-server.com");
        QTest::newRow("9") << Q("github.my.company.server.com") << Q("https://github.my.company.server.com");
        QTest::newRow("10") << Q("GitHub.My.Company.Server.Com") << Q("https://github.my.company.server.com");
        QTest::newRow("11") << Q("gist.github.my.company.server.com") << Q("https://github.my.company.server.com");
        QTest::newRow("12") << Q("GiST.GitHub.My.Company.Server.Com") << Q("https://github.my.company.server.com");
    }

    void getCredentialServiceUrl()
    {
        QFETCH(QString, host);
        QFETCH(QString, expectedService);
        StringDictionary d;
        d.set(Q("protocol"), Q("https"));
        d.set(Q("host"), host);
        QCOMPARE(GitHubHostProvider::getServiceName(GitRequest(d)), expectedService);
    }

    void getSupportedAuthenticationModes_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<OptString>("gitHubAuthModes");
        QTest::addColumn<int>("expectedModes");
        QTest::newRow("1") << Q("https://example.com") << OptString(Q("browser")) << int(AuthenticationModes::Browser);
        QTest::newRow("2") << Q("https://github.com") << OptString(Q("NOT-A-REAL-VALUE")) << int(DotComAuthenticationModes);
        QTest::newRow("3") << Q("https://GitHub.Com") << OptString(Q("NOT-A-REAL-VALUE")) << int(DotComAuthenticationModes);
        QTest::newRow("4") << Q("https://github.com") << OptString(Q("none")) << int(DotComAuthenticationModes);
        QTest::newRow("5") << Q("https://GitHub.Com") << OptString(Q("none")) << int(DotComAuthenticationModes);
        QTest::newRow("6") << Q("https://github.com") << OptString() << int(DotComAuthenticationModes);
        QTest::newRow("7") << Q("https://GitHub.Com") << OptString() << int(DotComAuthenticationModes);
        QTest::newRow("8") << Q("https://gist.github.com") << OptString() << int(DotComAuthenticationModes);
        QTest::newRow("9") << Q("https://GIST.GITHUB.COM") << OptString() << int(DotComAuthenticationModes);
    }

    void getSupportedAuthenticationModes()
    {
        QFETCH(QString, url);
        QFETCH(OptString, gitHubAuthModes);
        QFETCH(int, expectedModes);
        Fixture f;
        if (gitHubAuthModes) {
            f.context.Environment.Variables.set(GitHubConstants::EnvironmentVariables::AuthenticationModes, *gitHubAuthModes);
        }
        QCOMPARE(f.provider.getSupportedAuthenticationModes(QUrl(url)), AuthenticationModes(expectedModes));
        QCOMPARE(f.api->GetMetaInfoCalls, 0);
    }

    void getSupportedAuthenticationModes_WithMetadata_data()
    {
        QTest::addColumn<OptString>("installedVersion");
        QTest::addColumn<bool>("verifiablePasswordAuthentication");
        QTest::addColumn<int>("expectedModes");
        QTest::newRow("1") << OptString(Q("0.1")) << false << int(AuthenticationModes::Pat);
        QTest::newRow("2") << OptString(Q("0.1")) << true << int(AuthenticationModes::Basic | AuthenticationModes::Pat);
        QTest::newRow("3") << OptString(Q("100.0")) << false << int(AuthenticationModes::OAuth | AuthenticationModes::Pat);
        QTest::newRow("4") << OptString(Q("100.0")) << true << int(AuthenticationModes::All);
        QTest::newRow("5") << OptString() << false << int(AuthenticationModes::OAuth | AuthenticationModes::Pat);
        QTest::newRow("6") << OptString(Q("")) << false << int(AuthenticationModes::OAuth | AuthenticationModes::Pat);
        QTest::newRow("7") << OptString(Q(" ")) << false << int(AuthenticationModes::OAuth | AuthenticationModes::Pat);
    }

    void getSupportedAuthenticationModes_WithMetadata()
    {
        QFETCH(OptString, installedVersion);
        QFETCH(bool, verifiablePasswordAuthentication);
        QFETCH(int, expectedModes);
        Fixture f;
        const QUrl targetUri(Q("https://example.com"));
        f.api->GetMetaInfo = [&](const QUrl &uri) {
            [&] { QCOMPARE(uri, targetUri); }();
            return GitHubMetaInfo{installedVersion, verifiablePasswordAuthentication};
        };
        QCOMPARE(f.provider.getSupportedAuthenticationModes(targetUri), AuthenticationModes(expectedModes));
    }

    void getCredential_NoCredentials_NoUserNoHeaders_PromptsUser()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Pat, Credential{Q("alice"), Q("password")}};
        };
        const GitResponse result = f.provider.getCredential(request({{"protocol", "https"}, {"host", "github.com"}}));
        QCOMPARE(result.credential()->account, Q("alice"));
        QCOMPARE(result.credential()->password, Q("password"));
        QCOMPARE(f.auth->GetAuthenticationCalls.size(), 1);
        QCOMPARE(uriString(f.auth->GetAuthenticationCalls[0].targetUri), Q("https://github.com/"));
        QVERIFY(!f.auth->GetAuthenticationCalls[0].userName);
    }

    void getCredential_InputUser_ReturnsCredentialForUser()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("letmein123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("bob"), Q("secret123"));
        const GitResponse result = f.provider.getCredential(request({{"protocol", "https"}, {"host", "github.com"}, {"username", "alice"}}));
        QCOMPARE(result.credential()->account, Q("alice"));
        QCOMPARE(result.credential()->password, Q("letmein123"));
    }

    void getCredential_OneDomainAccount_ReturnsCredentialForRealmAccount()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("letmein123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("bob_contoso"), Q("secret123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("test_fabrikam"), Q("hidden_value"));
        const GitResponse result = f.provider.getCredential(
            request({{"protocol", "https"}, {"host", "github.com"}, {"wwwauth", "Basic realm=\"GitHub\" domain_hint=\"contoso\""}}));
        QCOMPARE(result.credential()->account, Q("bob_contoso"));
        QCOMPARE(result.credential()->password, Q("secret123"));
    }

    void getCredential_MultipleDomainAccounts_PromptForAccountAndReturnCredentialForAccount()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("letmein123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("bob_contoso"), Q("secret123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("john_contoso"), Q("who_knows"));
        f.auth->SelectAccount = [](const QUrl &, const QStringList &) { return OptString(Q("john_contoso")); };
        const GitResponse result = f.provider.getCredential(
            request({{"protocol", "https"}, {"host", "github.com"}, {"wwwauth", "Basic realm=\"GitHub\" domain_hint=\"contoso\""}}));
        QCOMPARE(result.credential()->account, Q("john_contoso"));
        QCOMPARE(result.credential()->password, Q("who_knows"));
        QCOMPARE(f.auth->SelectAccountCalls.size(), 1);
        QCOMPARE(uriString(f.auth->SelectAccountCalls[0].first), Q("https://github.com/"));
        QCOMPARE(f.auth->SelectAccountCalls[0].second, (QStringList{Q("bob_contoso"), Q("john_contoso")}));
    }

    void getCredential_MultipleDomainAccounts_PromptForAccountNewAccount()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("letmein123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("bob_contoso"), Q("secret123"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("john_contoso"), Q("who_knows"));
        f.auth->SelectAccount = [](const QUrl &, const QStringList &) { return OptString(); };
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Pat, Credential{Q("alice"), Q("password")}};
        };
        const GitResponse result = f.provider.getCredential(
            request({{"protocol", "https"}, {"host", "github.com"}, {"wwwauth", "Basic realm=\"GitHub\" domain_hint=\"contoso\""}}));
        QCOMPARE(result.credential()->account, Q("alice"));
        QCOMPARE(result.credential()->password, Q("password"));
        QCOMPARE(f.auth->GetAuthenticationCalls.size(), 1);
        QVERIFY(!f.auth->GetAuthenticationCalls[0].userName);
        QCOMPARE(f.auth->SelectAccountCalls[0].second, (QStringList{Q("bob_contoso"), Q("john_contoso")}));
    }

    void generateCredential_UnencryptedHttp_ThrowsException()
    {
        Fixture f;
        try {
            f.provider.generateCredential(QUrl(Q("http://github.com")), std::nullopt);
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().startsWith(Q("Unencrypted HTTP is not recommended for GitHub.")));
        }
    }

    void generateCredential_Browser_ReturnsCredential()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) { return AuthenticationPromptResult{AuthenticationModes::Browser, {}}; };
        f.auth->GetOAuthTokenViaBrowser = [](const QUrl &, const QStringList &, const OptString &) { return tokenResult(Q("OAUTH-TOKEN"), Q("bearer")); };
        f.api->GetUserInfo = [](const QUrl &, const QString &token) {
            [&] { QCOMPARE(token, Q("OAUTH-TOKEN")); }();
            return GitHubUserInfo{Q("john.doe")};
        };
        const Credential credential = f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt);
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("OAUTH-TOKEN"));
        QCOMPARE(f.auth->BrowserCalls.size(), 1);
        QCOMPARE(std::get<1>(f.auth->BrowserCalls[0]), ExpectedOAuthScopes);
        QVERIFY(!std::get<2>(f.auth->BrowserCalls[0]));
    }

    void generateCredential_Browser_LoginHint_IncludesHintAndReturnsCredential()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &call) {
            [&] { QCOMPARE(call.userName, OptString(Q("john.doe"))); }();
            return AuthenticationPromptResult{AuthenticationModes::Browser, {}};
        };
        f.auth->GetOAuthTokenViaBrowser = [](const QUrl &, const QStringList &, const OptString &) { return tokenResult(Q("OAUTH-TOKEN"), Q("bearer")); };
        f.api->GetUserInfo = [](const QUrl &, const QString &) { return GitHubUserInfo{Q("john.doe")}; };
        const Credential credential = f.provider.generateCredential(QUrl(Q("https://github.com/")), Q("john.doe"));
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("OAUTH-TOKEN"));
        QCOMPARE(std::get<1>(f.auth->BrowserCalls[0]), ExpectedOAuthScopes);
        QCOMPARE(std::get<2>(f.auth->BrowserCalls[0]), OptString(Q("john.doe")));
    }

    void generateCredential_Basic_1FAOnly_ReturnsCredential()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Basic, Credential{Q("john.doe"), Q("letmein123")}};
        };
        f.api->CreatePersonalAccessToken = [](const CreatePatCall &) {
            return AuthenticationResult{GitHubAuthenticationResultType::Success, Q("PERSONAL-ACCESS-TOKEN")};
        };
        f.api->GetUserInfo = [](const QUrl &, const QString &) { return GitHubUserInfo{Q("john.doe")}; };
        const Credential credential = f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt);
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("PERSONAL-ACCESS-TOKEN"));
        QCOMPARE(f.api->CreatePatCalls.size(), 1);
        const CreatePatCall &call = f.api->CreatePatCalls[0];
        QCOMPARE(call.userName, OptString(Q("john.doe")));
        QCOMPARE(call.password, OptString(Q("letmein123")));
        QVERIFY(!call.authenticationCode);
        QCOMPARE(call.scopes, ExpectedPatScopes);
        // The PAT is stored immediately (SAML SSO workaround)
        QCOMPARE(f.context.CredentialStore.get(Q("https://github.com"), Q("john.doe"))->password, Q("PERSONAL-ACCESS-TOKEN"));
    }

    void generateCredential_Basic_2FARequired_ReturnsCredential()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Basic, Credential{Q("john.doe"), Q("letmein123")}};
        };
        f.auth->GetTwoFactorCode = [](const QUrl &, bool isSms) {
            [&] { QVERIFY(!isSms); }();
            return Q("123456");
        };
        f.api->CreatePersonalAccessToken = [](const CreatePatCall &call) {
            return call.authenticationCode ? AuthenticationResult{GitHubAuthenticationResultType::Success, Q("PERSONAL-ACCESS-TOKEN")}
                                           : AuthenticationResult{GitHubAuthenticationResultType::TwoFactorApp, std::nullopt};
        };
        f.api->GetUserInfo = [](const QUrl &, const QString &) { return GitHubUserInfo{Q("john.doe")}; };
        const Credential credential = f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt);
        QCOMPARE(credential.account, Q("john.doe"));
        QCOMPARE(credential.password, Q("PERSONAL-ACCESS-TOKEN"));
        QCOMPARE(f.api->CreatePatCalls.size(), 2);
        QVERIFY(!f.api->CreatePatCalls[0].authenticationCode);
        QCOMPARE(f.api->CreatePatCalls[1].authenticationCode, OptString(Q("123456")));
        QCOMPARE(f.api->CreatePatCalls[1].scopes, ExpectedPatScopes);
    }

    // --- own tests ------------------------------------------------------------------------------

    void generateCredential_Basic_Failure_Throws()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Basic, Credential{Q("u"), Q("p")}};
        };
        f.api->CreatePersonalAccessToken = [](const CreatePatCall &) {
            return AuthenticationResult{GitHubAuthenticationResultType::Failure, std::nullopt};
        };
        try {
            f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt);
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("Interactive logon for 'https://github.com/' failed."));
        }
    }

    void generateCredential_Pat_WithoutUserName_ResolvesLogin()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Pat, Credential{QString(), Q("ghp_1")}};
        };
        f.api->GetUserInfo = [](const QUrl &, const QString &token) {
            [&] { QCOMPARE(token, Q("ghp_1")); }();
            return GitHubUserInfo{Q("octocat")};
        };
        const Credential credential = f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt);
        QCOMPARE(credential, (Credential{Q("octocat"), Q("ghp_1")}));
    }

    void generateCredential_Device_UsesDeviceFlow()
    {
        Fixture f;
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) { return AuthenticationPromptResult{AuthenticationModes::Device, {}}; };
        QStringList scopes;
        f.auth->GetOAuthTokenViaDeviceCode = [&](const QUrl &, const QStringList &s) {
            scopes = s;
            return tokenResult(Q("dev-token"), Q("bearer"));
        };
        f.api->GetUserInfo = [](const QUrl &, const QString &) { return GitHubUserInfo{Q("octocat")}; };
        QCOMPARE(f.provider.generateCredential(QUrl(Q("https://github.com")), std::nullopt), (Credential{Q("octocat"), Q("dev-token")}));
        QCOMPARE(scopes, ExpectedOAuthScopes);
    }

    void getSupportedAuthenticationModes_MetaFails_OffersAll()
    {
        Fixture f;
        f.api->GetMetaInfo = [](const QUrl &) -> GitHubMetaInfo { throw HttpRequestException(Q("boom")); };
        QCOMPARE(f.provider.getSupportedAuthenticationModes(QUrl(Q("https://ghe.example.com"))), AuthenticationModes::All);
        QCOMPARE(f.context.Console.WrittenMessages,
                 QStringList{Q("Failed to query 'https://ghe.example.com/' for supported authentication schemes.")});
        // Proxima never asks the server
        QCOMPARE(f.provider.getSupportedAuthenticationModes(QUrl(Q("https://octocorp.ghe.com"))), DotComAuthenticationModes);
        QCOMPARE(f.api->GetMetaInfoCalls, 1);
    }

    void getCredential_AccountFilteringDisabled_PromptsForAll()
    {
        Fixture f;
        f.context.Environment.Variables.set(GitHubConstants::EnvironmentVariables::AccountFiltering, Q("false"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("a"));
        f.context.CredentialStore.add(Q("https://github.com"), Q("bob_contoso"), Q("b"));
        f.auth->SelectAccount = [](const QUrl &, const QStringList &) { return OptString(Q("alice")); };
        const GitResponse result = f.provider.getCredential(
            request({{"protocol", "https"}, {"host", "github.com"}, {"wwwauth", "Basic realm=\"GitHub\" domain_hint=\"contoso\""}}));
        QCOMPARE(result.credential()->password, Q("a"));
        QCOMPARE(f.auth->SelectAccountCalls[0].second, (QStringList{Q("alice"), Q("bob_contoso")}));
    }

    void getCredential_FilteredToNone_DoesNotUseStore()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("a"));
        f.auth->GetAuthentication = [](const GetAuthenticationCall &) {
            return AuthenticationPromptResult{AuthenticationModes::Pat, Credential{Q("bob_contoso"), Q("new")}};
        };
        const GitResponse result = f.provider.getCredential(
            request({{"protocol", "https"}, {"host", "github.com"}, {"wwwauth", "Basic realm=\"GitHub\" domain_hint=\"contoso\""}}));
        QCOMPARE(result.credential()->password, Q("new"));
    }

    void storeAndErase()
    {
        Fixture f;
        f.provider.storeCredential(request({{"protocol", "https"}, {"host", "gist.github.com"}, {"username", "alice"}, {"password", "pw"}}));
        QCOMPARE(f.context.CredentialStore.get(Q("https://github.com"), Q("alice"))->password, Q("pw"));
        // Empty credentials (WIA) are not stored
        f.provider.storeCredential(request({{"protocol", "https"}, {"host", "github.com"}, {"username", ""}, {"password", ""}}));
        QCOMPARE(f.context.CredentialStore.getAccounts(Q("https://github.com")), QStringList{Q("alice")});
        f.provider.eraseCredential(request({{"protocol", "https"}, {"host", "github.com"}, {"username", "alice"}}));
        QVERIFY(f.context.CredentialStore.getAccounts(Q("https://github.com")).isEmpty());
    }

    void isSupported_Response()
    {
        Fixture f;
        HttpResponse response;
        QVERIFY(!f.provider.isSupported(&response));
        response.headers.add(Q("X-GitHub-Request-Id"), Q("1"));
        QVERIFY(f.provider.isSupported(&response));
        QVERIFY(!f.provider.isSupported(static_cast<const HttpResponse *>(nullptr)));
    }

    void command_ListLoginLogout()
    {
        Fixture f;
        f.context.CredentialStore.add(Q("https://github.com"), Q("alice"), Q("a"));
        f.context.CredentialStore.add(Q("https://ghe.example.com"), Q("carol"), Q("c"));
        QCOMPARE(f.runCommand({Q("list")}), 0);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("alice")});
        f.context.Console.WrittenMessages.clear();
        QCOMPARE(f.runCommand({Q("list"), Q("--url"), Q("https://ghe.example.com")}), 0);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("carol")});

        // login with a token resolves the user name
        f.api->GetUserInfo = [](const QUrl &, const QString &) { return GitHubUserInfo{Q("bob")}; };
        QCOMPARE(f.runCommand({Q("login"), Q("--token"), Q("ghp_b")}), 0);
        QCOMPARE(f.context.CredentialStore.get(Q("https://github.com"), Q("bob"))->password, Q("ghp_b"));

        // an existing account is not re-authenticated without --force
        f.context.Console.WrittenMessages.clear();
        QCOMPARE(f.runCommand({Q("login"), Q("--username"), Q("ALICE")}), 0);
        QCOMPARE(f.context.Console.WrittenMessages,
                 QStringList{Q("Account 'ALICE' already has credentials for https://github.com; use --force to re-authenticate")});

        // --browser uses the OAuth browser flow
        f.auth->GetOAuthTokenViaBrowser = [](const QUrl &, const QStringList &, const OptString &) { return tokenResult(Q("oauth"), Q("bearer")); };
        QCOMPARE(f.runCommand({Q("login"), Q("--browser"), Q("--username"), Q("alice"), Q("--force")}), 0);
        QCOMPARE(std::get<2>(f.auth->BrowserCalls.constLast()), OptString(Q("alice")));
        QCOMPARE(f.context.CredentialStore.get(Q("https://github.com"), Q("bob"))->password, Q("oauth"));

        QCOMPARE(f.runCommand({Q("logout"), Q("bob")}), 0);
        QVERIFY(!f.context.CredentialStore.get(Q("https://github.com"), Q("bob")));
        f.context.Console.WrittenMessages.clear();
        QCOMPARE(f.runCommand({Q("logout"), Q("nobody")}), -1);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("no such account 'nobody' found.")});
    }
};

GCM_REGISTER_TEST(GitHubHostProviderTests)
#include "githubhostprovidertests.moc"
