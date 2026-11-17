// Ports of the Bitbucket tests outside the host provider: BitbucketAuthenticationTest (8 methods),
// BitbucketHelperTest (2), BitbucketRestApiRegistryTest (2), OAuth2ClientRegistryTest (2),
// BitbucketTokenEndpointResponseJsonTest (1), Cloud/BitbucketOAuth2ClientTest (4), Cloud/BitbucketRestApiTest (1),
// Cloud/UserInfoTest (2), DataCenter/BitbucketOAuth2ClientTest (2), DataCenter/BitbucketRestApiTest (3),
// DataCenter/LoginOptionsTest (1) and DataCenter/UserInfoTest (1). The UserInfo/LoginOptions POCO tests become tests of
// the JSON the REST APIs parse (the port has no such classes). Own tests: helper protocol errors and the
// git-credential-manager-ui fallback, getBaseUri, the Data Center client configuration errors.
#include "bitbucketmocks.h"

#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
#include "infra/testutils.h"
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)

#include <QUrlQuery>

using namespace gcm;
using namespace gcm::bitbucket;
using namespace gcm::bitbucket::test;
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

/// BitbucketAuthentication with TryFindHelperCommand / InvokeHelperAsync replaced (the Moq CallBase mock upstream).
class TestBitbucketAuthentication : public BitbucketAuthentication
{
public:
    using BitbucketAuthentication::BitbucketAuthentication;
    std::optional<std::pair<QString, QString>> Helper; // command, args
    bool UseRealHelperLookup = false;
    StringDictionary Result;
    QStringList Commands;
    QStringList Args;

protected:
    bool tryFindBitbucketHelper(QString &command, QString &args) override
    {
        if (UseRealHelperLookup) {
            return BitbucketAuthentication::tryFindBitbucketHelper(command, args);
        }
        if (!Helper) {
            return false;
        }
        command = Helper->first;
        args = Helper->second;
        return true;
    }
    StringDictionary invokeHelper(const QString &path, const QString &args, const OptString &, const CancellationToken &) override
    {
        Commands.append(path);
        Args.append(args);
        return Result;
    }
};

class FakeCodeGenerator : public IOAuth2CodeGenerator
{
public:
    QString createNonce() override { return Q("12345"); }
    QString createPkceCodeVerifier() override { return Q("abcde"); }
    QString createPkceCodeChallenge(OAuth2PkceChallengeMethod method, const QString &verifier) override
    {
        return method == OAuth2PkceChallengeMethod::Sha256 && verifier == Q("abcde") ? Q("xyz987") : Q("unexpected");
    }
};

/// Answers the authorization request with state + code; records the URIs it was given.
class FakeBrowser : public IOAuth2WebBrowser
{
public:
    QUrl AuthorizationUri;
    QUrl RedirectUri;
    QUrl updateRedirectUri(const QUrl &uri) override { return uri; }
    StringDictionary getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri, OAuth2ResponseMode, const CancellationToken &) override
    {
        AuthorizationUri = authorizationUri;
        RedirectUri = redirectUri;
        return dict({{"state", "12345"}, {"code", "authorization_token"}});
    }
};

/// The query parameters of a URI as a map (decoded).
QMap<QString, QString> queryOf(const QUrl &uri)
{
    QMap<QString, QString> map;
    // Form encoding: '+' stands for a space
    QString query = uri.query(QUrl::FullyEncoded);
    query.replace(QLatin1Char('+'), QStringLiteral("%20"));
    const auto items = QUrlQuery(query).queryItems(QUrl::FullyDecoded);
    for (const auto &[k, v] : items) {
        map.insert(k, v);
    }
    return map;
}

} // namespace

// =============================================================================================
// BitbucketAuthenticationTest
// =============================================================================================

class BitbucketAuthenticationTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getCredentials_Basic_SucceedsAfterUserInput()
    {
        TestCommandContext context;
        context.Console.pushText(Q("password"));
        BitbucketAuthentication auth(context);
        const CredentialsPromptResult result = auth.getCredentials(QUrl(), Q("jsquire"), AuthenticationModes::Basic);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential, (Credential{Q("jsquire"), Q("password")}));
    }

    void getCredentials_OAuth_ReturnsOAuth()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true; // Allow OAuth mode
        BitbucketAuthentication auth(context);
        const CredentialsPromptResult result = auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::OAuth);
        QCOMPARE(result.authenticationMode, AuthenticationModes::OAuth);
        QVERIFY(!result.credential);
    }

    void getCredentials_All_ShowsMenu_OAuthOption1()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true; // Allow OAuth mode
        context.Settings.IsGuiPromptsEnabled = false;   // Force text prompts
        context.Console.pushSelection(0);
        BitbucketAuthentication auth(context);
        const CredentialsPromptResult result = auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::All);
        QCOMPARE(result.authenticationMode, AuthenticationModes::OAuth);
        QVERIFY(!result.credential);
        QCOMPARE(context.Console.LastChoices, (QStringList{Q("OAuth"), Q("Username/password")}));
    }

    void getCredentials_All_ShowsMenu_BasicOption2()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true; // Allow OAuth mode
        context.Settings.IsGuiPromptsEnabled = false;   // Force text prompts
        context.Console.pushSelection(1);
        context.Console.pushText(Q("jsquire"));
        context.Console.pushText(Q("password"));
        BitbucketAuthentication auth(context);
        const CredentialsPromptResult result = auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::All);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential, (Credential{Q("jsquire"), Q("password")}));
    }

    void getCredentials_All_NoDesktopSession_BasicOnly()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = false; // Disallow OAuth mode
        context.Console.pushText(Q("jsquire"));
        context.Console.pushText(Q("password"));
        BitbucketAuthentication auth(context);
        const CredentialsPromptResult result = auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::All);
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential, (Credential{Q("jsquire"), Q("password")}));
        QCOMPARE(context.Console.Prompts, (QStringList{Q("Username"), Q("Password")}));
    }

    void getCredentials_HelperCmdLine_data()
    {
        QTest::addColumn<QString>("targetUri");
        QTest::addColumn<OptString>("userName");
        QTest::addColumn<int>("modes");
        QTest::addColumn<QString>("expectedArgs");
        QTest::newRow("AllModes_NoUser_BBCloud") << Q("https://bitbucket.org") << OptString() << int(AuthenticationModes::All) << Q("prompt --show-basic --show-oauth");
        QTest::newRow("BasicOnly_User_BBCloud") << Q("https://bitbucket.org") << OptString(Q("jsquire")) << int(AuthenticationModes::Basic) << Q("prompt --username jsquire --show-basic");
        QTest::newRow("AllModes_NoUser_BBServerDC") << Q("https://example.com/bitbucket") << OptString() << int(AuthenticationModes::All) << Q("prompt --url https://example.com/bitbucket --show-basic --show-oauth");
    }

    // Covers the three *_HelperCmdLine methods
    void getCredentials_HelperCmdLine()
    {
        QFETCH(QString, targetUri);
        QFETCH(OptString, userName);
        QFETCH(int, modes);
        QFETCH(QString, expectedArgs);
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true; // Enable OAuth and UI helper selection
        TestBitbucketAuthentication auth(context);
        auth.Helper = {Q("/usr/bin/test-helper"), QString()};
        auth.Result = dict({{"username", "jsquire"}, {"password", "password"}});
        const CredentialsPromptResult result = auth.getCredentials(QUrl(targetUri), userName, AuthenticationModes(modes));
        QCOMPARE(result.authenticationMode, AuthenticationModes::Basic);
        QCOMPARE(result.credential, (Credential{Q("jsquire"), Q("password")}));
        QCOMPARE(auth.Commands, QStringList{Q("/usr/bin/test-helper")});
        QCOMPARE(auth.Args, QStringList{expectedArgs});
    }

    // --- own tests ------------------------------------------------------------------------------

    void helper_OAuthAndErrors()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        TestBitbucketAuthentication auth(context);
        auth.Helper = {Q("/usr/bin/test-helper"), QString()};
        auth.Result = dict({{"mode", "OAuth"}});
        QCOMPARE(auth.getCredentials(QUrl(Q("https://bitbucket.org")), std::nullopt, AuthenticationModes::All).authenticationMode, AuthenticationModes::OAuth);
        const QList<std::pair<StringDictionary, QString>> cases = {{dict({}), Q("Missing username in response")}, {dict({{"username", "u"}}), Q("Missing password in response")}};
        for (const auto &[response, message] : cases) {
            auth.Result = response;
            try {
                auth.getCredentials(QUrl(Q("https://bitbucket.org")), std::nullopt, AuthenticationModes::All);
                QFAIL("no exception");
            } catch (const Exception &ex) {
                QCOMPARE(ex.message(), message);
            }
        }
    }

    void helper_UiHelperFallback()
    {
        TestCommandContext context;
        context.SessionManager.IsDesktopSession = true;
        context.FileSystem.Files[Q("/usr/local/bin/git-credential-manager-ui")] = QByteArray();
        TestBitbucketAuthentication auth(context);
        auth.UseRealHelperLookup = true;
        auth.Result = dict({{"mode", "basic"}, {"username", "u"}, {"password", "p"}});
        auth.getCredentials(QUrl(Q("https://bb.example.com/scm/p/r.git")), Q("u"), AuthenticationModes::All);
        QCOMPARE(auth.Commands, QStringList{Q("/usr/local/bin/git-credential-manager-ui")});
        QCOMPARE(auth.Args, QStringList{Q("bitbucket prompt --url https://bb.example.com/scm/p/r.git --username u --show-basic --show-oauth")});
    }

    void getCredentials_ModesNone_Throws()
    {
        TestCommandContext context;
        BitbucketAuthentication auth(context);
        // OAuth is dropped without a desktop session, leaving nothing
        QVERIFY_THROWS_EXCEPTION(ArgumentException, auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::OAuth));
        context.Settings.IsInteractionAllowed = false;
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, auth.getCredentials(QUrl(), std::nullopt, AuthenticationModes::Basic));
    }
};

// =============================================================================================
// BitbucketHelperTest, registries, token JSON
// =============================================================================================

class BitbucketHelperTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void isBitbucketOrg_StringHost_data()
    {
        QTest::addColumn<OptString>("host");
        QTest::addColumn<bool>("expected");
        QTest::newRow("null") << OptString() << false;
        QTest::newRow("empty") << OptString(Q("")) << false;
        QTest::newRow("spaces") << OptString(Q("    ")) << false;
        QTest::newRow("1") << OptString(Q("bitbucket.org")) << true;
        QTest::newRow("2") << OptString(Q("BITBUCKET.ORG")) << true;
        QTest::newRow("3") << OptString(Q("BiTbUcKeT.OrG")) << true;
        QTest::newRow("4") << OptString(Q("bitbucket.example.com")) << false;
        QTest::newRow("5") << OptString(Q("bitbucket.example.org")) << false;
        QTest::newRow("6") << OptString(Q("bitbucket.org.com")) << false;
        QTest::newRow("7") << OptString(Q("bitbucket.org.org")) << false;
    }

    void isBitbucketOrg_StringHost()
    {
        QFETCH(OptString, host);
        QFETCH(bool, expected);
        QCOMPARE(BitbucketHelper::isBitbucketOrg(host), expected);
    }

    void isBitbucketOrg_Uri_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<bool>("expected");
        for (const char *scheme : {"http", "https"}) {
            for (const char *host : {"bitbucket.org", "BITBUCKET.ORG", "BiTbUcKeT.OrG"}) {
                for (const char *path : {"", "/path"}) {
                    const QString uri = QStringLiteral("%1://%2%3").arg(Q(scheme), Q(host), Q(path));
                    QTest::newRow(qPrintable(uri)) << uri << true;
                }
            }
            for (const char *host : {"bitbucket.example.com", "bitbucket.example.org", "bitbucket.org.com", "bitbucket.org.org"}) {
                for (const char *path : {"", "/path"}) {
                    const QString uri = QStringLiteral("%1://%2%3").arg(Q(scheme), Q(host), Q(path));
                    QTest::newRow(qPrintable(uri)) << uri << false;
                }
            }
        }
    }

    void isBitbucketOrg_Uri()
    {
        QFETCH(QString, uri);
        QFETCH(bool, expected);
        QCOMPARE(BitbucketHelper::isBitbucketOrg(QUrl(uri)), expected);
    }

    void getBaseUri()
    {
        QCOMPARE(BitbucketHelper::getBaseUri(QUrl(Q("https://example.com"))), Q("https://example.com:443"));
        QCOMPARE(BitbucketHelper::getBaseUri(QUrl(Q("http://example.com:7990/bitbucket/scm/p/r.git"))), Q("http://example.com:7990/bitbucket"));
    }

    // BitbucketRestApiRegistryTest
    void restApiRegistry_Get_ReturnsCloudOrDataCenterApi()
    {
        TestCommandContext context;
        BitbucketRestApiRegistry registry(context);
        const std::shared_ptr<IBitbucketRestApi> cloud = registry.get(request({{"protocol", "https"}, {"host", "bitbucket.org"}}));
        QVERIFY(dynamic_cast<cloud::BitbucketRestApi *>(cloud.get()));
        const std::shared_ptr<IBitbucketRestApi> dc = registry.get(request({{"protocol", "http"}, {"host", "example.com"}}));
        QVERIFY(dynamic_cast<datacenter::BitbucketRestApi *>(dc.get()));
        // Created once
        QCOMPARE(registry.get(request({{"protocol", "https"}, {"host", "bitbucket.org"}})), cloud);
    }

    // OAuth2ClientRegistryTest
    void oauth2ClientRegistry_Get_ReturnsCloudOrDataCenterClient()
    {
        TestCommandContext context;
        context.Settings.RemoteUri = QUrl(Q("https://example.com"));
        context.Environment.Variables.set(datacenter::DataCenterConstants::EnvironmentVariables::OAuthClientId, QString());
        context.Environment.Variables.set(datacenter::DataCenterConstants::EnvironmentVariables::OAuthClientSecret, QString());
        OAuth2ClientRegistry registry(context);
        QVERIFY(dynamic_cast<cloud::BitbucketOAuth2Client *>(registry.get(request({{"protocol", "https"}, {"host", "bitbucket.org"}})).get()));
        QVERIFY(dynamic_cast<datacenter::BitbucketOAuth2Client *>(registry.get(request({{"protocol", "https"}, {"host", "example.com"}})).get()));
    }

    // BitbucketTokenEndpointResponseJsonTest
    void tokenEndpointResponseJson_Deserialize_Uses_Scopes()
    {
        OAuth2TokenResult result;
        QVERIFY(tryParseBitbucketTokenResponse(Q("{\"access_token\": \"123\", \"token_type\": \"Bearer\", \"expires_in\": 1000, \"scopes\": \"x,y,z\", \"scope\": \"a,b,c\"}"), result));
        QCOMPARE(result.accessToken, Q("123"));
        QCOMPARE(result.tokenType, Q("Bearer"));
        QCOMPARE(result.expiresInSecs, std::optional<int>(1000));
        QCOMPARE(result.scopes, QStringList{Q("x,y,z")});
        // Own: the order of the properties does not matter; bad values fail the parse
        QVERIFY(tryParseBitbucketTokenResponse(Q("{\"scope\": \"a\", \"scopes\": \"x y\", \"access_token\": \"1\", \"refresh_token\": \"r\"}"), result));
        QCOMPARE(result.scopes, (QStringList{Q("x"), Q("y")}));
        QCOMPARE(result.refreshToken, OptString(Q("r")));
        QVERIFY(!tryParseBitbucketTokenResponse(Q("{\"access_token\": 5}"), result));
        QVERIFY(!tryParseBitbucketTokenResponse(Q("[]"), result));
    }
};

// =============================================================================================
// Cloud and Data Center OAuth2 clients and REST APIs
// =============================================================================================

class BitbucketCloudTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void oauth2Client_GetAuthorizationCode_ReturnsCode_data()
    {
        QTest::addColumn<OptString>("clientIdOverride");
        QTest::newRow("default") << OptString();
        QTest::newRow("override") << OptString(Q("i234"));
    }

    // Covers GetAuthorizationCodeAsync_ReturnsCode and _RespectsClientIdOverride_ReturnsCode
    void oauth2Client_GetAuthorizationCode_ReturnsCode()
    {
        QFETCH(OptString, clientIdOverride);
        TestCommandContext context;
        if (clientIdOverride) {
            context.Environment.Variables.set(cloud::CloudConstants::EnvironmentVariables::OAuthClientId, *clientIdOverride);
        }
        cloud::BitbucketOAuth2Client client(HttpClient(std::make_shared<TestHttpMessageHandler>()), context.Settings);
        client.setCodeGenerator(std::make_shared<FakeCodeGenerator>());
        FakeBrowser browser;
        const OAuth2AuthorizationCodeResult result = client.getAuthorizationCode(browser, CancellationToken());
        QCOMPARE(result.code, Q("authorization_token"));
        QCOMPARE(result.redirectUri, std::optional<QUrl>(QUrl(Q("http://localhost:34106/"))));
        QCOMPARE(result.codeVerifier, OptString(Q("abcde")));

        QCOMPARE(browser.AuthorizationUri.adjusted(QUrl::RemoveQuery), QUrl(Q("https://bitbucket.org/site/oauth2/authorize")));
        const QMap<QString, QString> query = queryOf(browser.AuthorizationUri);
        QCOMPARE(query.value(Q("response_type")), Q("code"));
        QCOMPARE(query.value(Q("client_id")), clientIdOverride.value_or(Q("HJdmKXV87DsmC9zSWB")));
        QCOMPARE(query.value(Q("state")), Q("12345"));
        QCOMPARE(query.value(Q("code_challenge_method")), Q("S256"));
        QCOMPARE(query.value(Q("code_challenge")), Q("xyz987"));
        QCOMPARE(query.value(Q("redirect_uri")), Q("http://localhost:34106/"));
        QCOMPARE(query.value(Q("scope")), Q("repository:write account"));
    }

    void oauth2Client_GetDeviceCode_Throws()
    {
        TestCommandContext context;
        cloud::BitbucketOAuth2Client client(HttpClient(std::make_shared<TestHttpMessageHandler>()), context.Settings);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, client.getDeviceCode({}, CancellationToken()));
    }

    void oauth2Client_GetRefreshTokenServiceName_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("host");
        QTest::addColumn<QString>("username");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << Q("https") << Q("example.com") << Q("john") << Q("https://example.com/refresh_token");
        QTest::newRow("2") << Q("http") << Q("example.com") << Q("john") << Q("http://example.com/refresh_token");
        QTest::newRow("3") << Q("https") << Q("example.com") << Q("dave") << Q("https://example.com/refresh_token");
        QTest::newRow("4") << Q("https") << Q("example.com/") << Q("john") << Q("https://example.com/refresh_token");
    }

    void oauth2Client_GetRefreshTokenServiceName()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, host);
        QFETCH(QString, username);
        QFETCH(QString, expected);
        TestCommandContext context;
        cloud::BitbucketOAuth2Client client(HttpClient(std::make_shared<TestHttpMessageHandler>()), context.Settings);
        StringDictionary d;
        d.set(Q("protocol"), protocol);
        d.set(Q("host"), host);
        d.set(Q("username"), username);
        QCOMPARE(client.getRefreshTokenServiceName(GitRequest(d)), expected);
    }

    void restApi_GetUserInformation_ReturnsUserInfo_ForSuccessfulRequest_data()
    {
        QTest::addColumn<QString>("password");
        QTest::addColumn<bool>("isBearerToken");
        QTest::newRow("token") << Q("token") << true;
        QTest::newRow("password") << Q("password") << false;
    }

    void restApi_GetUserInformation_ReturnsUserInfo_ForSuccessfulRequest()
    {
        QFETCH(QString, password);
        QFETCH(bool, isBearerToken);
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const QUrl expectedUri(Q("https://api.bitbucket.org/2.0/user"));
        handler->setup(Q("GET"), expectedUri, [&](const HttpRequest &r) {
            if (isBearerToken) {
                RestTestUtilities::assertBearerAuth(r, password);
            } else {
                RestTestUtilities::assertBasicAuth(r, Q("jsquire"), password);
            }
            return TestHttpMessageHandler::createResponse(200, "{\"username\":\"jsquire\",\"has_2fa_enabled\":false,\"account_id\":\"1234\",\"uuid\":\"{bef4bd75}\"}");
        });
        context.HttpClientFactory.MessageHandler = handler;
        cloud::BitbucketRestApi api(context);
        const UserInfoResult result = api.getUserInformation(Q("jsquire"), password, isBearerToken);
        QVERIFY(result.succeeded());
        QCOMPARE(result.userName, OptString(Q("jsquire")));
        handler->assertRequest(Q("GET"), expectedUri, 1);
    }

    // Cloud/UserInfoTest.Deserialize_UserInfo (case-insensitive property, null values elsewhere) and UserInfo_Set
    void restApi_UserInfoJson()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->setup(Q("GET"), QUrl(Q("https://api.bitbucket.org/2.0/user")), 200, Q("{\"uuid\": \"{bef4bd75-03fe-4f19-9c6c-ed57b05ab6f6}\", \"has_2fa_enabled\": null, \"UserName\": \"bob\", \"account_id\": \"123abc\"}"));
        context.HttpClientFactory.MessageHandler = handler;
        cloud::BitbucketRestApi api(context);
        QCOMPARE(api.getUserInformation(std::nullopt, Q("t"), true).userName, OptString(Q("bob")));
        QVERIFY(api.isOAuthInstalled());
        QVERIFY(api.getAuthenticationMethods().isEmpty());
        QCOMPARE((UserInfoResult{200, Q("123")}).userName, OptString(Q("123")));
    }
};

class BitbucketDataCenterTests : public QObject
{
    Q_OBJECT

private:
    static void configureClient(TestCommandContext &context, const OptString &redirectUri = std::nullopt)
    {
        namespace DC = datacenter::DataCenterConstants;
        context.Settings.RemoteUri = QUrl(Q("http://example.com"));
        context.Environment.Variables.set(DC::EnvironmentVariables::OAuthClientId, Q("dc-client-id"));
        context.Environment.Variables.set(DC::EnvironmentVariables::OAuthClientSecret, Q("dc-client-seccret"));
        if (redirectUri) {
            context.Environment.Variables.set(DC::EnvironmentVariables::OAuthRedirectUri, *redirectUri);
        }
    }

private Q_SLOTS:
    void oauth2Client_GetAuthorizationCode_ReturnsCode_data()
    {
        QTest::addColumn<OptString>("redirectOverride");
        QTest::addColumn<QString>("expectedRedirect");
        QTest::newRow("default") << OptString() << Q("http://localhost:34106/");
        QTest::newRow("override") << OptString(Q("http://localhost:12345/")) << Q("http://localhost:12345/");
    }

    // Covers GetAuthorizationCodeAsync_ReturnsCode and _ReturnsCode_WhileRespectingRedirectUriOverride
    void oauth2Client_GetAuthorizationCode_ReturnsCode()
    {
        QFETCH(OptString, redirectOverride);
        QFETCH(QString, expectedRedirect);
        TestCommandContext context;
        configureClient(context, redirectOverride);
        datacenter::BitbucketOAuth2Client client(HttpClient(std::make_shared<TestHttpMessageHandler>()), context.Settings);
        client.setCodeGenerator(std::make_shared<FakeCodeGenerator>());
        FakeBrowser browser;
        const OAuth2AuthorizationCodeResult result = client.getAuthorizationCode(browser, CancellationToken());
        QCOMPARE(result.code, Q("authorization_token"));
        QCOMPARE(result.redirectUri, std::optional<QUrl>(QUrl(expectedRedirect)));
        QCOMPARE(result.codeVerifier, OptString(Q("abcde")));
        QCOMPARE(browser.AuthorizationUri.adjusted(QUrl::RemoveQuery), QUrl(Q("http://example.com/rest/oauth2/latest/authorize")));
        const QMap<QString, QString> query = queryOf(browser.AuthorizationUri);
        QCOMPARE(query.value(Q("client_id")), Q("dc-client-id"));
        QCOMPARE(query.value(Q("redirect_uri")), expectedRedirect);
        QCOMPARE(query.value(Q("scope")), Q("PUBLIC_REPOS REPO_READ REPO_WRITE"));
    }

    void oauth2Client_MissingConfiguration_Throws()
    {
        TestCommandContext context;
        try {
            datacenter::BitbucketOAuth2Client client(HttpClient(), context.Settings);
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QCOMPARE(ex.message(), Q("RemoteUri must be defined to generate Bitbucket DC OAuth2 endpoint Urls"));
        }
        context.Settings.RemoteUri = QUrl(Q("https://example.com"));
        try {
            datacenter::BitbucketOAuth2Client client(HttpClient(), context.Settings);
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QCOMPARE(ex.message(), Q("Bitbucket DC OAuth Client ID must be defined"));
        }
        context.Environment.Variables.set(datacenter::DataCenterConstants::EnvironmentVariables::OAuthClientId, Q("id"));
        try {
            datacenter::BitbucketOAuth2Client client(HttpClient(), context.Settings);
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QCOMPARE(ex.message(), Q("Bitbucket DC OAuth Client Secret must be defined"));
        }
    }

    // Also DataCenter/UserInfoTest.UserInfo_Set: the placeholder user name
    void restApi_GetUserInformation_ReturnsUserInfo_ForSuccessfulRequest_DoesNothing()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const QUrl expectedUri(Q("http://example.com/rest/api/1.0/users"));
        handler->setup(Q("GET"), expectedUri, 200);
        context.HttpClientFactory.MessageHandler = handler;
        context.Settings.RemoteUri = QUrl(Q("http://example.com"));
        datacenter::BitbucketRestApi api(context);
        const UserInfoResult result = api.getUserInformation(Q("never used"), Q("never used"), false);
        QCOMPARE(result.userName, OptString(Q("OAUTH_USERNAME")));
        handler->assertRequest(Q("GET"), expectedUri, 1);
    }

    void restApi_IsOAuthInstalled_ReflectsBitbucketAuthenticationResponse_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("impliedSupport");
        QTest::newRow("401") << 401 << true;
        QTest::newRow("404") << 404 << false;
    }

    void restApi_IsOAuthInstalled_ReflectsBitbucketAuthenticationResponse()
    {
        QFETCH(int, status);
        QFETCH(bool, impliedSupport);
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const QUrl expectedUri(Q("http://example.com/rest/oauth2/1.0/client"));
        handler->setup(Q("GET"), expectedUri, status);
        context.HttpClientFactory.MessageHandler = handler;
        context.Settings.RemoteUri = QUrl(Q("http://example.com"));
        datacenter::BitbucketRestApi api(context);
        QCOMPARE(api.isOAuthInstalled(), impliedSupport);
        handler->assertRequest(Q("GET"), expectedUri, 1);
    }

    // Also DataCenter/LoginOptionsTest: the login options JSON
    void restApi_GetAuthenticationMethods_ReflectRestApiResponse_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<QList<int>>("expected");
        QTest::newRow("form") << Q("{ \"results\":[ { \"type\":\"LOGIN_FORM\"}]}") << QList<int>{int(AuthenticationMethod::BasicAuth)};
        QTest::newRow("idp") << Q("{ \"results\":[{\"type\":\"IDP\"}]}") << QList<int>{int(AuthenticationMethod::Sso)};
        QTest::newRow("both") << Q("{ \"results\":[{\"type\":\"IDP\", \"id\": 1}, { \"type\":\"LOGIN_FORM\"},  { \"type\":\"UNDEFINED\"}]}")
                              << QList<int>{int(AuthenticationMethod::BasicAuth), int(AuthenticationMethod::Sso)};
    }

    void restApi_GetAuthenticationMethods_ReflectRestApiResponse()
    {
        QFETCH(QString, json);
        QFETCH(QList<int>, expected);
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const QUrl expectedUri(Q("http://example.com/rest/authconfig/1.0/login-options"));
        handler->setup(Q("GET"), expectedUri, 200, json);
        context.HttpClientFactory.MessageHandler = handler;
        context.Settings.RemoteUri = QUrl(Q("http://example.com"));
        datacenter::BitbucketRestApi api(context);
        QList<int> actual;
        const QList<AuthenticationMethod> methods = api.getAuthenticationMethods();
        for (const AuthenticationMethod m : methods) {
            actual.append(int(m));
        }
        QCOMPARE(actual, expected);
        handler->assertRequest(Q("GET"), expectedUri, 1);
    }

    void restApi_ContextPathAndValidationSetting()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->setup(Q("GET"), QUrl(Q("https://example.com:8443/bitbucket/rest/api/1.0/users")), 401);
        context.HttpClientFactory.MessageHandler = handler;
        context.Settings.RemoteUri = QUrl(Q("https://example.com:8443/bitbucket/scm/proj/repo.git"));
        datacenter::BitbucketRestApi api(context);
        const UserInfoResult result = api.getUserInformation(Q("u"), Q("p"), false);
        QVERIFY(!result.succeeded());
        QCOMPARE(result.statusCode, 401);
        // Validation disabled: no request, placeholder user
        context.Environment.Variables.set(BitbucketConstants::EnvironmentVariables::ValidateStoredCredentials, Q("false"));
        QCOMPARE(api.getUserInformation(Q("u"), Q("p"), false).userName, OptString(Q("OAUTH_USERNAME")));
        QCOMPARE(handler->totalRequestCount(), 1);
    }
};

GCM_REGISTER_TEST(BitbucketAuthenticationTests)
GCM_REGISTER_TEST(BitbucketHelperTests)
GCM_REGISTER_TEST(BitbucketCloudTests)
GCM_REGISTER_TEST(BitbucketDataCenterTests)
#include "bitbucketapitests.moc"
