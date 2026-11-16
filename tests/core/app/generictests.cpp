// Ports of GenericHostProviderTests and GenericOAuthConfigTests.
// Not ported: the [WindowsFact] cases (WIA and NTLM are only detected on Windows); the [PosixFact]
// NonWindows_WiaSupported case is ported.
#include "appmocks.h"
#include "core/constants.h"
#include "core/generic.h"
#include "core/strings.h"
#include "infra/nulltrace.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }
namespace CredCfg = Constants::GitConfiguration::Credential;

struct Mocks
{
    std::shared_ptr<MockBasicAuthentication> basic = std::make_shared<MockBasicAuthentication>();
    std::shared_ptr<MockWindowsAuthentication> wia = std::make_shared<MockWindowsAuthentication>();
    std::shared_ptr<MockOAuthAuthentication> oauth = std::make_shared<MockOAuthAuthentication>();
    GenericHostProvider provider(ICommandContext &context) { return GenericHostProvider(context, basic, wia, oauth); }
};

GitRequest request(std::initializer_list<std::pair<QString, QString>> args)
{
    StringDictionary dict;
    for (const auto &[k, v] : args) {
        dict.set(k, v);
    }
    return GitRequest(dict);
}

QString uri(const QUrl &u) { return absoluteUri(u); }
QString uri(const char *s) { return absoluteUri(QUrl(Q(s))); }
} // namespace

class GenericHostProviderTests : public QObject
{
    Q_OBJECT

    static void testCreateCredential_returnsBasicCredential(WindowsAuthenticationTypes supportedWiaTypes)
    {
        TestCommandContext context;
        Mocks m;
        m.basic->Func = [](const QString &, const OptString &) { return Credential{Q("basicUser"), Q("basicPass")}; };
        m.wia->Types = supportedWiaTypes;
        GenericHostProvider provider = m.provider(context);
        const GitResponse result = provider.generateCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(result.credential()->account, Q("basicUser"));
        QCOMPARE(result.credential()->password, Q("basicPass"));
        QCOMPARE(m.basic->Calls, 1);
    }

private Q_SLOTS:
    void genericHostProvider_isSupported_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<bool>("expected");
        for (const char *p : {"http", "HTTP", "hTtP", "https", "HTTPS", "hTtPs", "ssh", "SSH", "sSh", "smpt", "SmtP", "SMTP",
                              "imap", "iMAp", "IMAP", "file", "fIlE", "FILE"}) {
            QTest::newRow(p) << Q(p) << true;
                              }
        QTest::newRow("empty") << Q("") << false;
        QTest::newRow("null") << QString() << false;
    }
    void genericHostProvider_isSupported()
    {
        QFETCH(QString, protocol);
        QFETCH(bool, expected);
        StringDictionary dict{{Q("host"), Q("example.com")}, {Q("path"), Q("foo/bar")}};
        if (!protocol.isNull()) {
            dict.set(Q("protocol"), protocol);
        }
        TestCommandContext context;
        GenericHostProvider provider(context);
        QCOMPARE(provider.isSupported(GitRequest(dict)), expected);
    }

    void genericHostProvider_getCredentialServiceUrl_returnsCorrectKey()
    {
        TestCommandContext context;
        GenericHostProvider provider(context);
        QCOMPARE(provider.getServiceName(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                                  {Q("path"), Q("foo/bar")}, {Q("username"), Q("john.doe")}})),
                 Q("https://example.com/foo/bar"));
    }

    void genericHostProvider_createCredentialAsync_wiaNotAllowed_returnsBasicCredentialNoWiaCheck()
    {
        TestCommandContext context;
        context.Settings.IsWindowsIntegratedAuthenticationEnabled = false;
        Mocks m;
        m.basic->Func = [](const QString &, const OptString &) { return Credential{Q("basicUser"), Q("basicPass")}; };
        GenericHostProvider provider = m.provider(context);
        const GitResponse result = provider.generateCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(*result.credential(), (Credential{Q("basicUser"), Q("basicPass")}));
        QCOMPARE(m.wia->TypesCalls, 0);
        QCOMPARE(m.basic->Calls, 1);
    }

    void genericHostProvider_createCredentialAsync_legacyAuthorityBasic_returnsBasicCredentialNoWiaCheck()
    {
        TestCommandContext context;
        context.Settings.LegacyAuthorityOverride = Q("basic");
        Mocks m;
        m.basic->Func = [](const QString &, const OptString &) { return Credential{Q("basicUser"), Q("basicPass")}; };
        GenericHostProvider provider = m.provider(context);
        const GitResponse result = provider.generateCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(*result.credential(), (Credential{Q("basicUser"), Q("basicPass")}));
        QCOMPARE(m.wia->TypesCalls, 0);
        QCOMPARE(m.basic->Calls, 1);
    }

    void genericHostProvider_createCredentialAsync_nonHttpProtocol_returnsBasicCredentialNoWiaCheck()
    {
        TestCommandContext context;
        Mocks m;
        m.basic->Func = [](const QString &, const OptString &) { return Credential{Q("basicUser"), Q("basicPass")}; };
        GenericHostProvider provider = m.provider(context);
        const GitResponse result = provider.generateCredential(request({{Q("protocol"), Q("smtp")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(*result.credential(), (Credential{Q("basicUser"), Q("basicPass")}));
        QCOMPARE(m.wia->TypesCalls, 0);
        QCOMPARE(m.basic->Calls, 1);
    }

    void genericHostProvider_createCredentialAsync_nonWindows_wiaSupported_returnsBasicCredential()
    {
        testCreateCredential_returnsBasicCredential(WindowsAuthenticationTypes::All);
    }

    void genericHostProvider_createCredentialAsync_wiaNotSupported_returnsBasicCredential()
    {
        testCreateCredential_returnsBasicCredential(WindowsAuthenticationTypes::None);
    }

    void genericHostProvider_generateCredentialAsync_oauth_completeOAuthConfig_usesOAuth()
    {
        const QString testResource = Q("https://git.example.com/foo");
        const QStringList scopes{Q("code:write"), Q("code:read")};
        auto key = [](const QString &name) { return Q("credential.https://example.com.%1").arg(name); };
        TestCommandContext context;
        auto &global = context.Git.Configuration.Global;
        global[key(CredCfg::OAuthClientId)] = {Q("3eadfc62-9e91-45d3-8c60-20ccd6d0c7cf")};
        global[key(CredCfg::OAuthClientSecret)] = {Q("C1DA8B93CCB5F5B93DA")};
        global[key(CredCfg::OAuthRedirectUri)] = {Q("http://localhost")};
        global[key(CredCfg::OAuthScopes)] = {scopes.join(QLatin1Char(' '))};
        global[key(CredCfg::OAuthAuthzEndpoint)] = {Q("/oauth/authorize")};
        global[key(CredCfg::OAuthTokenEndpoint)] = {Q("/oauth/token")};
        global[key(CredCfg::OAuthDeviceEndpoint)] = {Q("/oauth/device")};
        global[key(CredCfg::OAuthDefaultUserName)] = {Q("TEST_OAUTH_USER")};
        context.Settings.RemoteUri = QUrl(testResource);

        Mocks m;
        m.oauth->Mode = OAuthAuthenticationModes::Browser;
        m.oauth->Token.accessToken = Q("OAUTH_TOKEN");
        m.oauth->Token.tokenType = Q("access_token");
        m.oauth->Token.scopes = scopes;
        m.oauth->Token.refreshToken = Q("OAUTH_REFRESH_TOKEN");
        GenericHostProvider provider = m.provider(context);

        const GitResponse result = provider.generateCredential(
            request({{Q("protocol"), Q("https")}, {Q("host"), Q("git.example.com")}, {Q("path"), Q("foo")}}));
        QCOMPARE(result.credential()->account, Q("TEST_OAUTH_USER"));
        QCOMPARE(result.credential()->password, Q("OAUTH_TOKEN"));

        const auto refreshToken = context.CredentialStore.tryGet(Q("https://refresh_token.git.example.com/foo"), std::nullopt);
        QVERIFY(refreshToken);
        QCOMPARE(refreshToken->account, Q("TEST_OAUTH_USER"));
        QCOMPARE(refreshToken->password, Q("OAUTH_REFRESH_TOKEN"));

        QCOMPARE(m.oauth->ModeCalls.size(), 1);
        QCOMPARE(m.oauth->ModeCalls[0].first, testResource);
        QCOMPARE(int(m.oauth->ModeCalls[0].second), int(OAuthAuthenticationModes::All));
        QCOMPARE(m.oauth->BrowserCalls, QList<QStringList>{scopes});
        QVERIFY(m.oauth->DeviceCalls.isEmpty());
        QCOMPARE(m.wia->TypesCalls, 0);
        QCOMPARE(m.basic->Calls, 0);
    }

    void genericHostProvider_generateCredentialAsync_oauth_nonHttpRemoteWithConfig_usesOAuth()
    {
        const QString testResource = Q("smtp://mail.example.com/");
        const QStringList scopes{Q("code:write"), Q("code:read")};
        auto key = [](const QString &name) { return Q("credential.smtp://example.com.%1").arg(name); };
        TestCommandContext context;
        auto &global = context.Git.Configuration.Global;
        global[key(CredCfg::OAuthClientId)] = {Q("3eadfc62-9e91-45d3-8c60-20ccd6d0c7cf")};
        global[key(CredCfg::OAuthClientSecret)] = {Q("C1DA8B93CCB5F5B93DA")};
        global[key(CredCfg::OAuthRedirectUri)] = {Q("http://localhost")};
        global[key(CredCfg::OAuthScopes)] = {scopes.join(QLatin1Char(' '))};
        global[key(CredCfg::OAuthAuthzEndpoint)] = {Q("/oauth/authorize")};
        global[key(CredCfg::OAuthTokenEndpoint)] = {Q("/oauth/token")};
        global[key(CredCfg::OAuthDeviceEndpoint)] = {Q("/oauth/device")};
        global[key(CredCfg::OAuthDefaultUserName)] = {Q("TEST_OAUTH_USER")};
        context.Settings.RemoteUri = QUrl(testResource);

        Mocks m;
        m.oauth->Token.accessToken = Q("OAUTH_TOKEN");
        m.oauth->Token.refreshToken = Q("OAUTH_REFRESH_TOKEN");
        GenericHostProvider provider = m.provider(context);

        const GitResponse result = provider.generateCredential(request({{Q("protocol"), Q("smtp")}, {Q("host"), Q("mail.example.com")}}));
        QCOMPARE(result.credential()->account, Q("TEST_OAUTH_USER"));
        QCOMPARE(result.credential()->password, Q("OAUTH_TOKEN"));
        const auto refreshToken = context.CredentialStore.tryGet(Q("smtp://refresh_token.mail.example.com"), std::nullopt);
        QVERIFY(refreshToken);
        QCOMPARE(refreshToken->password, Q("OAUTH_REFRESH_TOKEN"));
        QCOMPARE(m.oauth->ModeCalls[0].first, testResource);
        QCOMPARE(m.basic->Calls, 0);
    }

    // Own tests
    void genericHostProvider_oauth_refreshToken_usedBeforeInteractiveFlow()
    {
        TestCommandContext context;
        auto &global = context.Git.Configuration.Global;
        global[Q("credential.oauthAuthorizeEndpoint")] = {Q("/authorize")};
        global[Q("credential.oauthTokenEndpoint")] = {Q("/token")};
        global[Q("credential.oauthClientId")] = {Q("client")};
        context.Settings.RemoteUri = QUrl(Q("https://example.com/repo"));
        context.CredentialStore.add(Q("https://refresh_token.example.com/repo"), Q("OAUTH_USER"), Q("OLD_REFRESH"));

        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->setup(Q("POST"), QUrl(Q("https://example.com/token")),
                       TestHttpMessageHandler::createResponse(
                           200, "{\"access_token\":\"NEW_ACCESS\",\"token_type\":\"Bearer\",\"refresh_token\":\"NEW_REFRESH\"}"));
        context.HttpClientFactory.MessageHandler = handler;

        Mocks m;
        GenericHostProvider provider = m.provider(context);
        const GitResponse result = provider.generateCredential(
            request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}, {Q("path"), Q("repo")}}));
        QCOMPARE(*result.credential(), (Credential{Q("OAUTH_USER"), Q("NEW_ACCESS")}));
        QCOMPARE(context.CredentialStore.tryGet(Q("https://refresh_token.example.com/repo"), Q("OAUTH_USER"))->password, Q("NEW_REFRESH"));
        QVERIFY(m.oauth->ModeCalls.isEmpty());
        const StringDictionary form = parseQueryString(QString::fromUtf8(handler->Requests[0].content));
        QCOMPARE(form.value(Q("refresh_token")), Q("OLD_REFRESH"));
    }

    void genericHostProvider_httpRemote_warns()
    {
        TestCommandContext context;
        Mocks m;
        GenericHostProvider provider = m.provider(context);
        provider.generateCredential(request({{Q("protocol"), Q("http")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(context.Console.WrittenMessages,
                 QStringList{Q("use of unencrypted HTTP remote URLs is not recommended; see https://aka.ms/gcm/unsaferemotes for more information.")});
        QCOMPARE(m.basic->Resources, QStringList{Q("http://example.com/")});
    }

    void genericHostProvider_getStoreErase_useCredentialStore()
    {
        TestCommandContext context;
        Mocks m;
        m.basic->Func = [](const QString &, const OptString &) { return Credential{Q("new"), Q("pw")}; };
        GenericHostProvider provider = m.provider(context);
        const GitRequest req = request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}, {Q("username"), Q("john")},
                                        {Q("password"), Q("letmein")}});
        provider.storeCredential(req);
        QCOMPARE(provider.getCredential(req).credential()->password, Q("letmein"));
        QCOMPARE(m.basic->Calls, 0);
        provider.eraseCredential(req);
        QCOMPARE(context.CredentialStore.count(), 0);
        QCOMPARE(provider.getCredential(req).credential()->account, Q("new"));
    }
};

// =============================================================================================

class GenericOAuthConfigTests : public QObject
{
    Q_OBJECT

    static bool tryGetWithResponseMode(const OptString &responseMode, GenericOAuthConfig &config)
    {
        auto key = [](const QString &name) { return Q("credential.https://example.com.%1").arg(name); };
        NullTrace trace;
        TestGitConfiguration gitConfig;
        gitConfig.Global[key(CredCfg::OAuthClientId)] = {Q("client-id")};
        gitConfig.Global[key(CredCfg::OAuthAuthzEndpoint)] = {Q("/oauth/authorize")};
        gitConfig.Global[key(CredCfg::OAuthTokenEndpoint)] = {Q("/oauth/token")};
        if (responseMode) {
            gitConfig.Global[key(CredCfg::OAuthResponseMode)] = {*responseMode};
        }
        TestSettings settings;
        settings.GitConfiguration = &gitConfig;
        settings.RemoteUri = QUrl(Q("https://example.com"));
        return GenericOAuthConfig::tryGet(trace, settings, request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}), config);
    }

private Q_SLOTS:
    void genericOAuthConfig_tryGet_valid_returnsTrue()
    {
        auto key = [](const QString &name) { return Q("credential.https://example.com.%1").arg(name); };
        NullTrace trace;
        TestGitConfiguration gitConfig;
        gitConfig.Global[key(CredCfg::OAuthClientId)] = {Q("115845b0-77f8-4c06-a3dc-7d277381fad1")};
        gitConfig.Global[key(CredCfg::OAuthClientSecret)] = {Q("4D35385D9F24")};
        gitConfig.Global[key(CredCfg::OAuthRedirectUri)] = {Q("http://localhost:12345/")};
        gitConfig.Global[key(CredCfg::OAuthScopes)] = {Q("scope1 scope2")};
        gitConfig.Global[key(CredCfg::OAuthAuthzEndpoint)] = {Q("/oauth/authorize")};
        gitConfig.Global[key(CredCfg::OAuthTokenEndpoint)] = {Q("/oauth/token")};
        gitConfig.Global[key(CredCfg::OAuthDeviceEndpoint)] = {Q("/oauth/device")};
        gitConfig.Global[key(CredCfg::OAuthDefaultUserName)] = {Q("TEST_USER")};
        TestSettings settings;
        settings.GitConfiguration = &gitConfig;
        settings.RemoteUri = QUrl(Q("https://example.com"));

        GenericOAuthConfig config;
        QVERIFY(GenericOAuthConfig::tryGet(trace, settings, request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}), config));
        QCOMPARE(config.clientId, OptString(Q("115845b0-77f8-4c06-a3dc-7d277381fad1")));
        QCOMPARE(config.clientSecret, OptString(Q("4D35385D9F24")));
        QCOMPARE(uri(config.redirectUri), uri("http://localhost:12345"));
        QCOMPARE(config.scopes, (QStringList{Q("scope1"), Q("scope2")}));
        QCOMPARE(uri(config.endpoints.authorizationEndpoint), uri("https://example.com/oauth/authorize"));
        QCOMPARE(uri(config.endpoints.tokenEndpoint), uri("https://example.com/oauth/token"));
        QCOMPARE(uri(*config.endpoints.deviceAuthorizationEndpoint), uri("https://example.com/oauth/device"));
        QCOMPARE(config.defaultUserName, Q("TEST_USER"));
        QVERIFY(config.useAuthHeader);
    }

    void genericOAuthConfig_tryGet_gitea()
    {
        NullTrace trace;
        TestSettings settings;
        settings.RemoteUri = QUrl(Q("https://example.com"));
        GenericOAuthConfig config;
        QVERIFY(GenericOAuthConfig::tryGet(trace, settings,
                                           request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                                    {Q("wwwauth"), Q("Basic realm=\"Gitea\"")}}),
                                           config));
        QCOMPARE(config.clientId, OptString(GenericOAuthConfig::WellKnown::GiteaClientId));
        QCOMPARE(config.redirectUri, GenericOAuthConfig::WellKnown::LocalIPv4RedirectUri);
        QVERIFY(config.scopes.isEmpty());
        QCOMPARE(uri(config.endpoints.authorizationEndpoint), uri("https://example.com/login/oauth/authorize"));
        QCOMPARE(uri(config.endpoints.tokenEndpoint), uri("https://example.com/login/oauth/access_token"));
    }

    void genericOAuthConfig_tryGet_gitea_preservesDeploymentPrefix_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expectedAuthzPath");
        QTest::addColumn<QString>("expectedTokenPath");
        QTest::newRow("none") << Q("") << Q("/login/oauth/authorize") << Q("/login/oauth/access_token");
        QTest::newRow("repo") << Q("/test/test.git") << Q("/login/oauth/authorize") << Q("/login/oauth/access_token");
        QTest::newRow("prefix") << Q("/git/test/test.git") << Q("/git/login/oauth/authorize") << Q("/git/login/oauth/access_token");
        QTest::newRow("deep") << Q("/sub/git/test/test.git") << Q("/sub/git/login/oauth/authorize") << Q("/sub/git/login/oauth/access_token");
    }
    void genericOAuthConfig_tryGet_gitea_preservesDeploymentPrefix()
    {
        QFETCH(QString, path);
        QFETCH(QString, expectedAuthzPath);
        QFETCH(QString, expectedTokenPath);
        NullTrace trace;
        TestSettings settings;
        settings.RemoteUri = QUrl(Q("https://example.com") + path);
        GenericOAuthConfig config;
        QVERIFY(GenericOAuthConfig::tryGet(trace, settings,
                                           request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}, {Q("path"), path},
                                                    {Q("wwwauth"), Q("Basic realm=\"Gitea\"")}}),
                                           config));
        QCOMPARE(uri(config.endpoints.authorizationEndpoint), absoluteUri(QUrl(Q("https://example.com") + expectedAuthzPath)));
        QCOMPARE(uri(config.endpoints.tokenEndpoint), absoluteUri(QUrl(Q("https://example.com") + expectedTokenPath)));
    }

    void genericOAuthConfig_tryGet_parsesResponseMode_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<int>("expected");
        QTest::newRow("query") << Q("query") << int(OAuth2ResponseMode::Query);
        QTest::newRow("fragment") << Q("fragment") << int(OAuth2ResponseMode::Fragment);
        QTest::newRow("form_post") << Q("form_post") << int(OAuth2ResponseMode::FormPost);
        QTest::newRow("FORM_POST") << Q("FORM_POST") << int(OAuth2ResponseMode::FormPost);
    }
    void genericOAuthConfig_tryGet_parsesResponseMode()
    {
        QFETCH(QString, value);
        QFETCH(int, expected);
        GenericOAuthConfig config;
        QVERIFY(tryGetWithResponseMode(value, config));
        QCOMPARE(int(config.responseMode), expected);
    }

    void genericOAuthConfig_tryGet_invalidResponseMode_fallsBackToDefault()
    {
        GenericOAuthConfig config;
        QVERIFY(tryGetWithResponseMode(Q("bogus"), config));
        QCOMPARE(config.responseMode, OAuth2ResponseMode::Default);
    }

    void genericOAuthConfig_tryGet_responseModeUnset_usesDefault()
    {
        GenericOAuthConfig config;
        QVERIFY(tryGetWithResponseMode(std::nullopt, config));
        QCOMPARE(config.responseMode, OAuth2ResponseMode::Default);
    }

    // Own test: without endpoints there is no OAuth configuration.
    void genericOAuthConfig_tryGet_noEndpoints_returnsFalse()
    {
        NullTrace trace;
        TestSettings settings;
        GenericOAuthConfig config;
        QVERIFY(!GenericOAuthConfig::tryGet(trace, settings, request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}), config));
    }
};

GCM_REGISTER_TEST(GenericHostProviderTests)
GCM_REGISTER_TEST(GenericOAuthConfigTests)
#include "generictests.moc"
