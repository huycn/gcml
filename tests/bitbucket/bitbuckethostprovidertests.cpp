// Port of Atlassian.Bitbucket.Tests/BitbucketHostProviderTest.cs (27 of 28 methods; IsSupported_FailsForNullInput
// has no C++ equivalent since a request reference cannot be null), plus own tests: plain HTTP to bitbucket.org,
// cancelled prompts, refresh failure falling back to the browser flow, the Data Center fallback when its REST API
// fails, and the service names.
#include "bitbucketmocks.h"

#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)

using namespace gcm;
using namespace gcm::bitbucket;
using namespace gcm::bitbucket::test;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

const char *const MOCK_ACCESS_TOKEN = "at-0987654321";
const char *const MOCK_ACCESS_TOKEN_ALT = "at-onetwothreefour-1234";
const char *const MOCK_EXPIRED_ACCESS_TOKEN = "at-1234567890-expired";
const char *const MOCK_REFRESH_TOKEN = "rt-1234567809";
const char *const MOCK_ROTATED_REFRESH_TOKEN = "rt-01189998819991197253";
const char *const MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR = "chunks=3";
const char *const MOCK_CHUNKED_ACCESS_TOKEN_1 = "at-12";
const char *const MOCK_CHUNKED_ACCESS_TOKEN_2 = "45678";
const char *const MOCK_CHUNKED_ACCESS_TOKEN_3 = "9";
const char *const COMBINED_CHUNKED_ACCESS_TOKEN = "at-12456789";
const int MOCK_MAX_CREDENTIAL_SIZE = 5;
const char *const MOCK_CHUNKED_REFRESH_TOKEN_DESCRIPTOR = "chunks=2";
const char *const MOCK_CHUNKED_REFRESH_TOKEN_1 = "rt-98";
const char *const MOCK_CHUNKED_REFRESH_TOKEN_2 = "76543";
const char *const COMBINED_CHUNKED_REFRESH_TOKEN = "rt-9876543";
const char *const BITBUCKET_DOT_ORG_HOST = "bitbucket.org";
const char *const DC_SERVER_HOST = "example.com";

GitRequest mockInput(const QString &protocol, const OptString &host, const OptString &username, const OptString &password = std::nullopt)
{
    StringDictionary d;
    d.set(Q("protocol"), protocol);
    if (host) {
        d.set(Q("host"), *host);
    }
    if (username) {
        d.set(Q("username"), *username);
    }
    if (password) {
        d.set(Q("password"), *password);
    }
    return GitRequest(d);
}

UserInfoResult ok(const OptString &userName) { return UserInfoResult{200, userName}; }

/// The mocks, the context and the provider of one test.
struct Fixture
{
    TestCommandContext context;
    std::shared_ptr<MockBitbucketAuthentication> auth = std::make_shared<MockBitbucketAuthentication>();
    std::shared_ptr<MockBitbucketRestApi> api = std::make_shared<MockBitbucketRestApi>();

    BitbucketHostProvider provider() { return BitbucketHostProvider(context, auth, std::make_shared<MockRestApiRegistry>(api)); }

    QUrl remoteUri(const GitRequest &request) const { return request.getRemoteUri().value_or(QUrl()); }
    QString accessTokenService(const GitRequest &request) const { return BitbucketHostProvider::getServiceName(remoteUri(request)); }
    QString refreshTokenService(const GitRequest &request) const { return BitbucketHostProvider::getRefreshTokenServiceName(remoteUri(request)); }

    void mockDCSSOEnabled()
    {
        api->AuthenticationMethods = QList<AuthenticationMethod>{AuthenticationMethod::BasicAuth, AuthenticationMethod::Sso};
        api->OAuthInstalled = true;
    }
    /// MockStoredAccount: the remote URL without its trailing slash
    void mockStoredAccount(const GitRequest &request, const QString &password)
    {
        QString remoteUrl = absoluteUri(remoteUri(request));
        remoteUrl.chop(1);
        context.CredentialStore.add(remoteUrl, valueOr(request.userName()), password);
    }
    void mockStoredRefreshToken(const GitRequest &request, const QString &token)
    {
        context.CredentialStore.add(refreshTokenService(request), valueOr(request.userName()), token);
    }
    void mockStoredToken(const QString &service, const QString &account, const QString &token) { context.CredentialStore.add(service, account, token); }
    void mockRemoteBasicValid(const GitRequest &request, const QString &password) { api->setupUserInfo(request.userName(), password, false, ok(request.userName())); }
    void mockRemoteAccessTokenValid(const QString &username, const QString &token) { api->setupUserInfo(std::nullopt, token, true, ok(username)); }
    void mockRemoteAccessTokenExpired(const QString &token) { api->setupUserInfo(std::nullopt, token, true, UserInfoResult{401, std::nullopt}); }
    void mockRemoteRefreshTokenValid(const QString &refreshToken, const QString &accessToken, const OptString &rotatedRefreshToken = std::nullopt)
    {
        auth->RefreshResults.append({refreshToken, tokenResult(accessToken, rotatedRefreshToken)});
    }
    void mockPromptBasic(const GitRequest &request, const QString &password)
    {
        auth->PromptResult = CredentialsPromptResult{AuthenticationModes::Basic, Credential{valueOr(request.userName()), password}};
    }
    void mockPromptOAuth() { auth->PromptResult = CredentialsPromptResult{AuthenticationModes::OAuth, std::nullopt}; }
    void mockRemoteOAuthTokenCreate(const QString &accessToken, const QString &refreshToken) { auth->CreateResult = tokenResult(accessToken, refreshToken); }

    // --- verifications --------------------------------------------------------------------------
    bool interactiveAuthRan(const GitRequest &request) const
    {
        return auth->GetCredentialsCalls.size() == 1 && auth->GetCredentialsCalls[0].targetUri == remoteUri(request) &&
               auth->GetCredentialsCalls[0].userName == request.userName();
    }
    bool interactiveAuthNeverRan() const { return auth->GetCredentialsCalls.isEmpty(); }
    bool oauthFlowRan(const QString &token) const { return auth->CreateCalls == 1 && api->userInfoCalls(std::nullopt, token, true) >= 1; }
    bool validateAccessTokenRan(const QString &token) const { return api->userInfoCalls(std::nullopt, token, true) == 1; }
    bool validateBasicAuthRan(const GitRequest &request, const QString &password) const { return api->userInfoCalls(request.userName(), password, false) == 1; }
    bool oauthRefreshRan(const QString &refreshToken) const { return auth->RefreshCalls == QStringList{refreshToken}; }
    void verifyTokenStored(const QString &service, const QString &account, const QString &token) const
    {
        const std::optional<TestCredential> c = context.CredentialStore.tryGet(service, account);
        QVERIFY2(c.has_value(), qPrintable(QStringLiteral("nothing stored for %1 / %2").arg(service, account)));
        QCOMPARE(c->account, account);
        QCOMPARE(c->password, token);
    }
};

} // namespace

class BitbucketHostProviderTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void isSupported_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<OptString>("host");
        QTest::addColumn<bool>("expected");
        QTest::newRow("no host") << Q("https") << OptString() << false;
        // We report that we support unencrypted HTTP here so that we can fail and show a helpful error message instead.
        QTest::newRow("http org") << Q("http") << OptString(Q("bitbucket.org")) << true;
        QTest::newRow("ssh org") << Q("ssh") << OptString(Q("bitbucket.org")) << false;
        QTest::newRow("https org") << Q("https") << OptString(Q("bitbucket.org")) << true;
        QTest::newRow("api.bitbucket.org") << Q("https") << OptString(Q("api.bitbucket.org")) << true; // Currently does support sub domains.
        QTest::newRow("bitbucket.ogg") << Q("https") << OptString(Q("bitbucket.ogg")) << false;
        QTest::newRow("bitbucket.com") << Q("https") << OptString(Q("bitbucket.com")) << false;
        QTest::newRow("dc") << Q("https") << OptString(Q("example.com")) << false;
        QTest::newRow("named http") << Q("http") << OptString(Q("bitbucket.my-company-server.com")) << false; // Currently no support for named on-premise instances
        QTest::newRow("my-company") << Q("https") << OptString(Q("my-company-server.com")) << false;
        QTest::newRow("dotted") << Q("https") << OptString(Q("bitbucket.my.company.server.com")) << false;
        QTest::newRow("api named") << Q("https") << OptString(Q("api.bitbucket.my-company-server.com")) << false;
        QTest::newRow("case") << Q("https") << OptString(Q("BITBUCKET.My-Company-Server.Com")) << false;
    }

    void isSupported()
    {
        QFETCH(QString, protocol);
        QFETCH(OptString, host);
        QFETCH(bool, expected);
        TestCommandContext context;
        BitbucketHostProvider provider(context);
        QCOMPARE(provider.isSupported(mockInput(protocol, host, std::nullopt)), expected);
    }

    void isSupported_WWWAuth_data()
    {
        QTest::addColumn<QString>("wwwauth");
        QTest::addColumn<bool>("expected");
        QTest::newRow("bitbucket") << Q("Basic realm=\"Atlassian Bitbucket\"") << true;
        QTest::newRow("other") << Q("Basic realm=\"GitSponge\"") << false;
    }

    void isSupported_WWWAuth()
    {
        QFETCH(QString, wwwauth);
        QFETCH(bool, expected);
        StringDictionary d;
        d.set(Q("wwwauth"), wwwauth);
        TestCommandContext context;
        BitbucketHostProvider provider(context);
        QCOMPARE(provider.isSupported(GitRequest(d)), expected);
    }

    void isSupported_FailsForNullHttpResponseMessage()
    {
        TestCommandContext context;
        BitbucketHostProvider provider(context);
        QVERIFY(!provider.isSupported(static_cast<const HttpResponse *>(nullptr)));
    }

    void isSupported_HttpResponseMessage_data()
    {
        QTest::addColumn<OptString>("header");
        QTest::addColumn<bool>("expected");
        QTest::newRow("X-AREQUESTID") << OptString(Q("X-AREQUESTID")) << true; // only the specific header is acceptable
        QTest::newRow("X-REQUESTID") << OptString(Q("X-REQUESTID")) << false;
        QTest::newRow("none") << OptString() << false;
    }

    void isSupported_HttpResponseMessage()
    {
        QFETCH(OptString, header);
        QFETCH(bool, expected);
        HttpResponse response;
        if (header) {
            response.headers.add(*header, Q("123456789"));
        }
        TestCommandContext context;
        BitbucketHostProvider provider(context);
        QCOMPARE(provider.isSupported(&response), expected);
    }

    void getCredential_Valid_Stored_Basic_data()
    {
        QTest::addColumn<QString>("host");
        QTest::newRow("dc") << Q(DC_SERVER_HOST);
        QTest::newRow("cloud") << Q(BITBUCKET_DOT_ORG_HOST);
    }

    void getCredential_Valid_Stored_Basic()
    {
        QFETCH(QString, host);
        Fixture f;
        const GitRequest request = mockInput(Q("https"), host, Q("jsquire"));
        if (host == Q(DC_SERVER_HOST)) {
            f.mockDCSSOEnabled();
        }
        f.mockStoredAccount(request, Q("password"));
        f.mockRemoteBasicValid(request, Q("password"));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q("password")}));
        // The OAuth validation fails (strict mock), the basic validation runs once
        QVERIFY(f.validateBasicAuthRan(request, Q("password")));
        // Stored credentials so don't ask for more
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_Valid_Stored_OAuth()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q("password"));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q("password"));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q("password")}));
        QVERIFY(f.validateAccessTokenRan(Q("password")));
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_Valid_Stored_ChunkedOAuth()
    {
        Fixture f;
        // The MaxCredentialSize is not used outside of being a gate to enforce whether to support de-chunking a token.
        f.context.CredentialStore.MaxCredentialSize = 100;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR));
        const QString chunkService = BitbucketHostProvider::getChunkServiceName(f.accessTokenService(request));
        f.mockStoredToken(chunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 0), Q(MOCK_CHUNKED_ACCESS_TOKEN_1));
        f.mockStoredToken(chunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 1), Q(MOCK_CHUNKED_ACCESS_TOKEN_2));
        f.mockStoredToken(chunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 2), Q(MOCK_CHUNKED_ACCESS_TOKEN_3));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(COMBINED_CHUNKED_ACCESS_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_Valid_IgnoresChunkedOAuthForUnsupportedCredentialStore()
    {
        // A contrived test: a password that looks chunked is passed on as is when the store does not chunk
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = 0;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR)));
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_Valid_New_Basic_data()
    {
        QTest::addColumn<QString>("host");
        QTest::newRow("dc") << Q(DC_SERVER_HOST);
        QTest::newRow("cloud") << Q(BITBUCKET_DOT_ORG_HOST);
    }

    void getCredential_Valid_New_Basic()
    {
        QFETCH(QString, host);
        Fixture f;
        const GitRequest request = mockInput(Q("https"), host, Q("jsquire"));
        f.mockPromptBasic(request, Q("password"));
        f.mockRemoteBasicValid(request, Q("password"));
        if (host == Q(DC_SERVER_HOST)) {
            // (Moq returns default values for the strict mock's unset async members only when awaited; the DC
            // detection of upstream's test fails and falls back to all modes, as here.)
            f.api->AuthenticationMethods = QList<AuthenticationMethod>{AuthenticationMethod::BasicAuth};
            f.api->OAuthInstalled = false;
        }
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q("password")}));
        QVERIFY(f.interactiveAuthRan(request));
    }

    // Also covers _Valid_New_OAuth_With_Null_Request_Username (upstream's two tests are identical)
    void getCredential_Valid_New_OAuth()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), std::nullopt);
        f.mockPromptOAuth();
        f.mockRemoteOAuthTokenCreate(Q(MOCK_ACCESS_TOKEN), Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.interactiveAuthRan(request));
        QVERIFY(f.oauthFlowRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_REFRESH_TOKEN));
    }

    void getCredential_MissingAT_OAuth_Refresh()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // AT does not exist, but RT is still valid
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_Invalid_MalformedChunkedAT_OAuth_Refresh()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // A chunked AT record exists, but we're missing a part of it. This is treated as if we have no AT
        const QString chunkService = BitbucketHostProvider::getChunkServiceName(f.accessTokenService(request));
        f.mockStoredToken(chunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 0), Q(MOCK_CHUNKED_ACCESS_TOKEN_1));
        f.mockStoredToken(chunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 1), Q(MOCK_CHUNKED_ACCESS_TOKEN_2));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN), Q(MOCK_ROTATED_REFRESH_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_ROTATED_REFRESH_TOKEN));
    }

    void getCredential_ExpiredAT_OAuth_Refresh()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // AT exists but has expired, but RT is still valid
        f.mockStoredAccount(request, Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockRemoteAccessTokenExpired(Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
    }

    void getCredential_ExpiredAT_OAuth_Refresh_WithChunkedRT()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = 100;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q(MOCK_EXPIRED_ACCESS_TOKEN));
        const QString rtChunkService = BitbucketHostProvider::getChunkServiceName(f.refreshTokenService(request));
        f.mockStoredToken(rtChunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 0), Q(MOCK_CHUNKED_REFRESH_TOKEN_1));
        f.mockStoredToken(rtChunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 1), Q(MOCK_CHUNKED_REFRESH_TOKEN_2));
        f.mockStoredRefreshToken(request, Q(MOCK_CHUNKED_REFRESH_TOKEN_DESCRIPTOR));
        f.mockRemoteAccessTokenExpired(Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(COMBINED_CHUNKED_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN), Q(MOCK_ROTATED_REFRESH_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.oauthRefreshRan(Q(COMBINED_CHUNKED_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_ROTATED_REFRESH_TOKEN));
    }

    void getCredential_ExpiredAT_OAuth_Refresh_StoresChunkedRT()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = MOCK_MAX_CREDENTIAL_SIZE;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenExpired(Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        // After refreshing we're given back a wide rotated refresh token which needs chunking
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN), Q(COMBINED_CHUNKED_REFRESH_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
        // Stored RT as chunked
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_CHUNKED_REFRESH_TOKEN_DESCRIPTOR));
        const QString rtChunkService = BitbucketHostProvider::getChunkServiceName(f.refreshTokenService(request));
        f.verifyTokenStored(rtChunkService, Q("jsquire_0"), Q(MOCK_CHUNKED_REFRESH_TOKEN_1));
        f.verifyTokenStored(rtChunkService, Q("jsquire_1"), Q(MOCK_CHUNKED_REFRESH_TOKEN_2));
    }

    void getCredential_ExpiredAT_OAuth_Refresh_DoesNotChunkLargeRTForUnsupportedCredentialStore()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = 0;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenExpired(Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN), Q(COMBINED_CHUNKED_REFRESH_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
        QVERIFY(f.interactiveAuthNeverRan());
        // Stored RT without chunking
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(COMBINED_CHUNKED_REFRESH_TOKEN));
    }

    void getCredential_ExpiredAT_OAuth_InvalidChunkedRefresh_PromptsConsent()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = 100;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // AT exists but has expired; the RT is missing a chunk, so it is ignored
        f.mockStoredAccount(request, Q(MOCK_EXPIRED_ACCESS_TOKEN));
        const QString rtChunkService = BitbucketHostProvider::getChunkServiceName(f.refreshTokenService(request));
        f.mockStoredToken(rtChunkService, BitbucketHostProvider::getChunkAccount(Q("jsquire"), 0), Q(MOCK_CHUNKED_REFRESH_TOKEN_1));
        f.mockStoredRefreshToken(request, Q(MOCK_CHUNKED_REFRESH_TOKEN_DESCRIPTOR));
        f.mockRemoteAccessTokenExpired(Q(MOCK_EXPIRED_ACCESS_TOKEN));
        f.mockPromptOAuth();
        // On the OAuth prompt we create and store the rotated refresh token
        f.mockRemoteOAuthTokenCreate(Q(MOCK_ACCESS_TOKEN), Q(MOCK_ROTATED_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QVERIFY(f.interactiveAuthRan(request));
        QVERIFY(f.oauthFlowRan(Q(MOCK_ACCESS_TOKEN)));
        QVERIFY(f.validateAccessTokenRan(Q(MOCK_ACCESS_TOKEN)));
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_ROTATED_REFRESH_TOKEN));
    }

    void getCredential_PreconfiguredMode_OAuth_ValidRT_IsRespected()
    {
        Fixture f;
        f.context.Environment.Variables.set(BitbucketConstants::EnvironmentVariables::AuthenticationModes, Q("oauth"));
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // We have a stored RT so we can just use that without any prompts
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN));
        const GitResponse result = f.provider().getCredential(request);
        QVERIFY(result.credential().has_value());
        QVERIFY(f.interactiveAuthNeverRan());
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
    }

    void getCredential_AlwaysRefreshCredentials_OAuth_IsRespected()
    {
        Fixture f;
        f.context.Environment.Variables.set(BitbucketConstants::EnvironmentVariables::AlwaysRefreshCredentials, Q("True"));
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        // User has stored access token that we shouldn't use - RT should be used to mint new AT
        f.mockStoredAccount(request, Q(MOCK_ACCESS_TOKEN));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN_ALT));
        f.mockRemoteRefreshTokenValid(Q(MOCK_REFRESH_TOKEN), Q(MOCK_ACCESS_TOKEN_ALT));
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN_ALT)}));
        QVERIFY(f.interactiveAuthNeverRan());
        QVERIFY(f.oauthRefreshRan(Q(MOCK_REFRESH_TOKEN)));
    }

    void getCredential_AlwaysRefreshCredentials_Basic_IsRespected_data()
    {
        QTest::addColumn<QString>("host");
        QTest::newRow("cloud") << Q(BITBUCKET_DOT_ORG_HOST);
        QTest::newRow("dc") << Q(DC_SERVER_HOST);
    }

    void getCredential_AlwaysRefreshCredentials_Basic_IsRespected()
    {
        QFETCH(QString, host);
        Fixture f;
        f.context.Environment.Variables.set(BitbucketConstants::EnvironmentVariables::AlwaysRefreshCredentials, Q("True"));
        const GitRequest request = mockInput(Q("https"), host, Q("jsquire"));
        // User has stored password that we shouldn't use
        f.mockStoredAccount(request, Q("old-password"));
        f.mockPromptBasic(request, Q("new-password"));
        if (host == Q(DC_SERVER_HOST)) {
            f.api->AuthenticationMethods = QList<AuthenticationMethod>{AuthenticationMethod::BasicAuth};
            f.api->OAuthInstalled = false;
        }
        const GitResponse result = f.provider().getCredential(request);
        QCOMPARE(*result.credential(), (Credential{Q("jsquire"), Q("new-password")}));
        QVERIFY(f.interactiveAuthRan(request));
    }

    void getSupportedAuthenticationModes_data()
    {
        QTest::addColumn<QString>("host");
        QTest::addColumn<OptString>("bitbucketAuthModes");
        QTest::addColumn<int>("expectedModes");
        const int server = int(datacenter::DataCenterConstants::ServerAuthenticationModes);
        const int dotOrg = int(cloud::CloudConstants::DotOrgAuthenticationModes);
        // DC - supports Basic, OAuth
        QTest::newRow("dc basic") << Q("example.com") << OptString(Q("basic")) << int(AuthenticationModes::Basic);
        QTest::newRow("dc oauth") << Q("example.com") << OptString(Q("oauth")) << int(AuthenticationModes::OAuth);
        QTest::newRow("dc bad") << Q("example.com") << OptString(Q("NOT-A-REAL-VALUE")) << server;
        QTest::newRow("dc none") << Q("example.com") << OptString(Q("none")) << server;
        QTest::newRow("dc unset") << Q("example.com") << OptString() << server;
        // Cloud - supports Basic, OAuth
        QTest::newRow("cloud oauth") << Q("bitbucket.org") << OptString(Q("oauth")) << int(AuthenticationModes::OAuth);
        QTest::newRow("cloud basic") << Q("bitbucket.org") << OptString(Q("basic")) << int(AuthenticationModes::Basic);
        QTest::newRow("cloud bad") << Q("bitbucket.org") << OptString(Q("NOT-A-REAL-VALUE")) << dotOrg;
        QTest::newRow("cloud none") << Q("bitbucket.org") << OptString(Q("none")) << dotOrg;
        QTest::newRow("cloud unset") << Q("bitbucket.org") << OptString() << dotOrg;
    }

    void getSupportedAuthenticationModes()
    {
        QFETCH(QString, host);
        QFETCH(OptString, bitbucketAuthModes);
        QFETCH(int, expectedModes);
        Fixture f;
        if (bitbucketAuthModes) {
            f.context.Environment.Variables.set(BitbucketConstants::EnvironmentVariables::AuthenticationModes, *bitbucketAuthModes);
        }
        // DC without overrides: the strict API mock fails, which falls back to all modes (= ServerAuthenticationModes)
        QCOMPARE(f.provider().getSupportedAuthenticationModes(mockInput(Q("https"), host, std::nullopt)), AuthenticationModes(expectedModes));
    }

    void storeCredential()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(DC_SERVER_HOST), Q("jsquire"));
        QCOMPARE(f.context.CredentialStore.count(), 0);
        f.provider().storeCredential(request);
        QCOMPARE(f.context.CredentialStore.count(), 1);
    }

    void storeCredential_SplitsIntoChunksForSupportedCredentialStores()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = MOCK_MAX_CREDENTIAL_SIZE;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
        f.provider().storeCredential(request);
        // The chunk descriptor and 3 chunks
        QCOMPARE(f.context.CredentialStore.count(), 4);
        f.verifyTokenStored(f.accessTokenService(request), Q("jsquire"), Q(MOCK_CHUNKED_ACCESS_TOKEN_DESCRIPTOR));
        const QString chunkService = BitbucketHostProvider::getChunkServiceName(f.accessTokenService(request));
        f.verifyTokenStored(chunkService, Q("jsquire_0"), Q(MOCK_CHUNKED_ACCESS_TOKEN_1));
        f.verifyTokenStored(chunkService, Q("jsquire_1"), Q(MOCK_CHUNKED_ACCESS_TOKEN_2));
        f.verifyTokenStored(chunkService, Q("jsquire_2"), Q(MOCK_CHUNKED_ACCESS_TOKEN_3));
    }

    void storeCredential_DoesNotChunk()
    {
        Fixture f;
        f.context.CredentialStore.MaxCredentialSize = 0;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
        f.provider().storeCredential(request);
        QCOMPARE(f.context.CredentialStore.count(), 1);
        f.verifyTokenStored(f.accessTokenService(request), Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
    }

    void storeCredential_DoesNotChunkForSecretsSmallerThanMaximumCredentialSize()
    {
        Fixture f;
        // The exact size of the password: it fits, so no chunking
        f.context.CredentialStore.MaxCredentialSize = int(qstrlen(COMBINED_CHUNKED_ACCESS_TOKEN));
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
        f.provider().storeCredential(request);
        QCOMPARE(f.context.CredentialStore.count(), 1);
        f.verifyTokenStored(f.accessTokenService(request), Q("jsquire"), Q(COMBINED_CHUNKED_ACCESS_TOKEN));
    }

    void eraseCredential()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(DC_SERVER_HOST), Q("jsquire"));
        f.mockStoredAccount(request, Q("password"));
        QCOMPARE(f.context.CredentialStore.count(), 1);
        f.provider().eraseCredential(request);
        QCOMPARE(f.context.CredentialStore.count(), 0);
    }

    // --- own tests ------------------------------------------------------------------------------

    void getCredential_UnencryptedHttpToBitbucketOrg_Throws()
    {
        Fixture f;
        try {
            f.provider().getCredential(mockInput(Q("http"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire")));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().startsWith(Q("Unencrypted HTTP is not recommended for Bitbucket.org.")));
        }
        // Data Center over HTTP is allowed
        f.api->AuthenticationMethods = QList<AuthenticationMethod>{AuthenticationMethod::BasicAuth};
        f.api->OAuthInstalled = false;
        f.auth->PromptResult = CredentialsPromptResult{AuthenticationModes::Basic, Credential{Q("u"), Q("p")}};
        QCOMPARE(*f.provider().getCredential(mockInput(Q("http"), Q(DC_SERVER_HOST), Q("u"))).credential(), (Credential{Q("u"), Q("p")}));
    }

    void getCredential_PromptReturnsNone_Throws()
    {
        Fixture f;
        f.auth->PromptResult = CredentialsPromptResult{AuthenticationModes::None, std::nullopt};
        try {
            f.provider().getCredential(mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire")));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("User cancelled credential prompt"));
        }
    }

    void getCredential_RefreshFails_RestartsBrowserFlow()
    {
        Fixture f;
        const GitRequest request = mockInput(Q("https"), Q(BITBUCKET_DOT_ORG_HOST), Q("jsquire"));
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        // No refresh result set up: the mock throws -- but only an OAuth2Exception restarts the flow
        f.auth->RefreshResults.clear();
        auto throwing = std::make_shared<MockBitbucketAuthentication>();
        f.auth = throwing;
        f.mockStoredRefreshToken(request, Q(MOCK_REFRESH_TOKEN));
        QVERIFY_THROWS_EXCEPTION(Exception, f.provider().getCredential(request));

        // With an OAuth2Exception the browser flow starts and the new refresh token is stored
        class OAuthFailingAuth : public MockBitbucketAuthentication
        {
        public:
            OAuth2TokenResult refreshOAuthCredentials(const GitRequest &, const QString &) override { throw OAuth2Exception(QStringLiteral("invalid_grant")); }
        };
        auto failing = std::make_shared<OAuthFailingAuth>();
        failing->CreateResult = tokenResult(Q(MOCK_ACCESS_TOKEN), Q(MOCK_ROTATED_REFRESH_TOKEN));
        f.auth = failing;
        f.mockRemoteAccessTokenValid(Q("jsquire"), Q(MOCK_ACCESS_TOKEN));
        QCOMPARE(*f.provider().getCredential(request).credential(), (Credential{Q("jsquire"), Q(MOCK_ACCESS_TOKEN)}));
        QCOMPARE(failing->CreateCalls, 1);
        QVERIFY(failing->GetCredentialsCalls.isEmpty());
        f.verifyTokenStored(f.refreshTokenService(request), Q("jsquire"), Q(MOCK_ROTATED_REFRESH_TOKEN));
    }

    void getSupportedAuthenticationModes_DataCenterDetection()
    {
        Fixture f;
        f.api->AuthenticationMethods = QList<AuthenticationMethod>{AuthenticationMethod::Sso};
        f.api->OAuthInstalled = true;
        QCOMPARE(f.provider().getSupportedAuthenticationModes(mockInput(Q("https"), Q(DC_SERVER_HOST), std::nullopt)), AuthenticationModes::OAuth);
        f.api->AuthenticationMethods.reset();
        QCOMPARE(f.provider().getSupportedAuthenticationModes(mockInput(Q("https"), Q(DC_SERVER_HOST), std::nullopt)), AuthenticationModes::All);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("Failed to query 'https://example.com/' for supported authentication schemes.")});
    }

    void serviceNames()
    {
        QCOMPARE(BitbucketHostProvider::getServiceName(QUrl(Q("https://jsquire@bitbucket.org/org/repo.git"))), Q("https://bitbucket.org/org/repo.git"));
        QCOMPARE(BitbucketHostProvider::getRefreshTokenServiceName(QUrl(Q("https://jsquire@bitbucket.org/org/repo.git"))), Q("https://bitbucket.org/refresh_token"));
        QCOMPARE(BitbucketHostProvider::getChunkServiceName(Q("https://bitbucket.org")), Q("https://bitbucket.org/chunks"));
        QCOMPARE(BitbucketHostProvider::getChunkAccount(Q("jsquire"), 2), Q("jsquire_2"));
        QCOMPARE(BitbucketHostProvider::getChunkSecret(3), Q("chunks=3"));
    }
};

GCM_REGISTER_TEST(BitbucketHostProviderTests)
#include "bitbuckethostprovidertests.moc"
