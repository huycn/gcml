// Port of GitHub.Tests/GitHubRestApiTests.cs (all 11 methods), plus own tests of getUserInfo / getMetaInfo and of
// the request body.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "github/github.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
#include "infra/testutils.h"

using namespace gcm;
using namespace gcm::github;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

const QString TestUserName = Q("john.doe");
const QString TestPassword = Q("letmein123"); // fake credential
const QStringList TestScopes = {Q("scope1"), Q("scope2")};
const QUrl ExpectedRequestUri(Q("https://api.github.com/authorizations"));

void assertAuthCode(const HttpRequest &request, const OptString &authCode)
{
    if (!authCode) {
        QVERIFY(!request.headers.contains(GitHubConstants::GitHubOptHeader));
    } else {
        QCOMPARE(request.headers.values(GitHubConstants::GitHubOptHeader), QStringList{*authCode});
    }
}

struct Fixture
{
    TestCommandContext context;
    std::shared_ptr<TestHttpMessageHandler> handler = std::make_shared<TestHttpMessageHandler>();

    Fixture()
    {
        handler->ThrowOnUnexpectedRequest = true;
        context.HttpClientFactory.MessageHandler = handler;
    }

    /// Sets up POST /authorizations answering `response` after checking the basic auth and OTP headers.
    void setupAuthorizations(const HttpResponse &response, const QString &password, const OptString &authCode)
    {
        handler->setup(Q("POST"), ExpectedRequestUri, [=](const HttpRequest &request) {
            RestTestUtilities::assertBasicAuth(request, TestUserName, password);
            assertAuthCode(request, authCode);
            return response;
        });
    }
};

HttpResponse response(int status, const QByteArray &content = {})
{
    return TestHttpMessageHandler::createResponse(status, content);
}

} // namespace

class GitHubRestApiTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getApiRequestUri_data()
    {
        QTest::addColumn<QString>("targetUrl");
        QTest::addColumn<QString>("apiUrl");
        QTest::addColumn<QString>("expected");
        QTest::newRow("dotcom") << Q("https://github.com") << Q("user") << Q("https://api.github.com/user");
        QTest::newRow("dotcom path") << Q("https://github.com") << Q("users/123") << Q("https://api.github.com/users/123");
        QTest::newRow("dotcom case") << Q("https://gItHuB.cOm") << Q("uSeRs/123") << Q("https://api.github.com/uSeRs/123");
        QTest::newRow("gist") << Q("https://gist.github.com") << Q("user") << Q("https://api.github.com/user");
        QTest::newRow("ghes") << Q("https://github.example.com") << Q("user") << Q("https://github.example.com/api/v3/user");
        QTest::newRow("ghes raw") << Q("https://raw.github.example.com") << Q("user") << Q("https://github.example.com/api/v3/user");
        QTest::newRow("ghes gist") << Q("https://gist.github.example.com") << Q("user") << Q("https://github.example.com/api/v3/user");
        QTest::newRow("ghes port") << Q("https://github.example.com:8080") << Q("user") << Q("https://github.example.com:8080/api/v3/user");
        QTest::newRow("proxima") << Q("https://octocorp.ghe.com") << Q("user") << Q("https://api.octocorp.ghe.com/user");
    }

    void getApiRequestUri()
    {
        QFETCH(QString, targetUrl);
        QFETCH(QString, apiUrl);
        QFETCH(QString, expected);
        QCOMPARE(GitHubRestApi::getApiRequestUri(QUrl(targetUrl), apiUrl).toString(), expected);
    }

    void acquireToken_NullUri_ThrowsException()
    {
        TestCommandContext context;
        GitHubRestApi api(context);
        // A null Uri upstream (ArgumentNullException); an empty QUrl here, rejected as not absolute
        QVERIFY_THROWS_EXCEPTION(ArgumentException, api.createPersonalAccessToken(QUrl(), TestUserName, TestPassword, Q("1234"), TestScopes));
    }

    void acquireToken_NoNetwork_ThrowsException()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulateNoNetwork = true;
        context.HttpClientFactory.MessageHandler = handler;
        GitHubRestApi api(context);
        QVERIFY_THROWS_EXCEPTION(HttpRequestException, api.createPersonalAccessToken(QUrl(Q("https://example.com")), TestUserName,
                                                                                     TestPassword, Q("1234"), TestScopes));
    }

    void acquireToken_ValidRequestOK_ReturnsToken()
    {
        Fixture f;
        f.setupAuthorizations(response(200, "{ \"token\": \"GITHUB_TOKEN_VALUE\" }"), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        const AuthenticationResult result = api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes);
        QCOMPARE(result.type, GitHubAuthenticationResultType::Success);
        QCOMPARE(result.token, OptString(Q("GITHUB_TOKEN_VALUE")));
    }

    void acquireToken_ValidRequestOKBadJson_ReturnsFailure()
    {
        Fixture f;
        f.setupAuthorizations(response(200, "ThisIsBadJSON"), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        const AuthenticationResult result = api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes);
        QCOMPARE(result.type, GitHubAuthenticationResultType::Failure);
    }

    void acquireToken_ValidRequestCreated_ReturnsToken()
    {
        Fixture f;
        f.setupAuthorizations(response(201, "{ \"token\": \"GITHUB_TOKEN_VALUE\" }"), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        const AuthenticationResult result = api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes);
        QCOMPARE(result.type, GitHubAuthenticationResultType::Success);
        QCOMPARE(result.token, OptString(Q("GITHUB_TOKEN_VALUE")));
    }

    void acquireToken_Valid1FANoAppAuthCode_ReturnsApp2FARequired()
    {
        Fixture f;
        HttpResponse r = response(401);
        r.headers.add(GitHubConstants::GitHubOptHeader, Q("app"));
        f.setupAuthorizations(r, TestPassword, std::nullopt);
        GitHubRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, std::nullopt, TestScopes).type,
                 GitHubAuthenticationResultType::TwoFactorApp);
    }

    void acquireToken_Valid1FANoSmsAuthCode_ReturnsSms2FARequired()
    {
        Fixture f;
        HttpResponse r = response(401);
        r.headers.add(GitHubConstants::GitHubOptHeader, Q("sms"));
        f.setupAuthorizations(r, TestPassword, std::nullopt);
        GitHubRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, std::nullopt, TestScopes).type,
                 GitHubAuthenticationResultType::TwoFactorSms);
    }

    void acquireToken_ValidOAuthToken_ReturnsOAuthToken()
    {
        Fixture f;
        const QString oauthToken = Q("TestOAuthToken");
        f.setupAuthorizations(response(403, "This API can only be accessed with username and password Basic Auth"), oauthToken, std::nullopt);
        GitHubRestApi api(f.context);
        const AuthenticationResult result =
            api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, oauthToken, std::nullopt, TestScopes);
        QCOMPARE(result.type, GitHubAuthenticationResultType::Success);
        QCOMPARE(result.token, OptString(oauthToken));
    }

    void acquireToken_Unauthorized_ReturnsFailure()
    {
        Fixture f;
        f.setupAuthorizations(response(401, ""), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes).type,
                 GitHubAuthenticationResultType::Failure);
    }

    void acquireToken_Forbidden_ReturnsFailure()
    {
        Fixture f;
        f.setupAuthorizations(response(403, ""), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes).type,
                 GitHubAuthenticationResultType::Failure);
    }

    void acquireToken_UnknownResponse_ReturnsFailure()
    {
        Fixture f;
        // https://www.rfc-editor.org/rfc/rfc2324#section-2.3.2
        f.setupAuthorizations(response(418, "I am a tea pot (short and stout)."), TestPassword, Q("1234"));
        GitHubRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, Q("1234"), TestScopes).type,
                 GitHubAuthenticationResultType::Failure);
    }

    // --- own tests ------------------------------------------------------------------------------

    void acquireToken_RequestBodyAndHeaders()
    {
        Fixture f;
        f.setupAuthorizations(response(201, "{\"token\":\"t\"}"), TestPassword, std::nullopt);
        GitHubRestApi api(f.context);
        api.createPersonalAccessToken(QUrl(Q("https://github.com")), TestUserName, TestPassword, std::nullopt, TestScopes);
        QCOMPARE(f.handler->Requests.size(), 1);
        const HttpRequest &request = f.handler->Requests.constFirst();
        const QString body = QString::fromUtf8(request.content);
        QVERIFY2(body.startsWith(Q("{ \"scopes\": [\"scope1\", \"scope2\"], \"note\": \"git: https://github.com/ on ")), qPrintable(body));
        QCOMPARE(request.headers.value(Q("Accept")), OptString(Q("application/vnd.github.v3+json")));
    }

    void getUserInfo_ReturnsLogin()
    {
        Fixture f;
        f.handler->setup(Q("GET"), QUrl(Q("https://github.example.com/api/v3/user")), [](const HttpRequest &request) {
            RestTestUtilities::assertBearerAuth(request, Q("tok"));
            return response(200, "{\"login\":\"octocat\",\"id\":1}");
        });
        GitHubRestApi api(f.context);
        QCOMPARE(api.getUserInfo(QUrl(Q("https://github.example.com")), Q("tok")).login, Q("octocat"));
    }

    void getUserInfo_Error_Throws()
    {
        Fixture f;
        f.handler->setup(Q("GET"), QUrl(Q("https://api.github.com/user")), 401);
        GitHubRestApi api(f.context);
        QVERIFY_THROWS_EXCEPTION(HttpRequestException, api.getUserInfo(QUrl(Q("https://github.com")), Q("tok")));
    }

    void getMetaInfo_ParsesFields()
    {
        Fixture f;
        f.handler->setup(Q("GET"), QUrl(Q("https://github.example.com/api/v3/meta")), 200,
                         Q("{\"installed_version\":\"3.9.1\",\"verifiable_password_authentication\":true}"));
        f.handler->setup(Q("GET"), QUrl(Q("https://api.github.com/meta")), 200, Q("{\"verifiable_password_authentication\":false}"));
        GitHubRestApi api(f.context);
        const GitHubMetaInfo ghes = api.getMetaInfo(QUrl(Q("https://github.example.com")));
        QCOMPARE(ghes.installedVersion, OptString(Q("3.9.1")));
        QVERIFY(ghes.verifiablePasswordAuthentication);
        const GitHubMetaInfo dotcom = api.getMetaInfo(QUrl(Q("https://github.com")));
        QVERIFY(!dotcom.installedVersion);
        QVERIFY(!dotcom.verifiablePasswordAuthentication);
    }
};

GCM_REGISTER_TEST(GitHubRestApiTests)
#include "githubrestapitests.moc"
