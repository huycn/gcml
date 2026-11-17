// Ports of Core.Tests/Authentication/Entra: EntraAccountTests (2 methods), EntraAuthenticationTests (3),
// ManagedIdentityTests (4), ServicePrincipalTests (1) and WorkloadFederationOptionsTests (1). MsalParentWindowAdapterTests
// (3) are not ported: they test the MSAL parent window handle adapter, which has no equivalent without MSAL.
// Own tests of our Entra client (it replaces MSAL) against a fake identity platform: the device code, system browser,
// silent and refresh flows, MSA passthrough, error mapping, the token cache and its fallback, service principals
// (secret, certificate assertion, PKCS#12 store lookup), managed identity sources, workload federation, the
// interaction mode selection and the diagnostic.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "azrepos/azrepos.h"
#include "core/strings.h"
#include "infra/testcommandcontext.h"
#include "infra/testdiagnosticreporter.h"
#include "infra/testmain.h"
#include "infra/testutils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace gcm;
using namespace gcm::entra;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

const QString ClientId = Q("c9e8fda6-1d46-484c-917c-3dbd518f27c3");
const QStringList Scopes = {Q("499b84ac-1321-427f-aa17-267ca6975798/.default")};
const QString Login = Q("https://login.microsoftonline.com");

QUrl endpoint(const QString &tenant, const QString &name) { return QUrl(Q("%1/%2/oauth2/v2.0/%3").arg(Login, tenant, name)); }

QString b64url(const QByteArray &data) { return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)); }
QByteArray unb64url(const QString &s) { return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }

QString json(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

/// An unsigned JWT with the given payload
QString jwt(const QJsonObject &payload)
{
    return b64url(json(QJsonObject{{Q("alg"), Q("none")}}).toUtf8()) + Q(".") + b64url(json(payload).toUtf8()) + Q(".sig");
}

/// A token endpoint success response for user `upn` (object id `uid` in tenant `tid`)
HttpResponse userTokenResponse(const QString &accessToken, const QString &upn, const QString &uid, const QString &tid, int expiresIn = 3600,
                               const QString &refreshToken = Q("REFRESH-TOKEN"))
{
    const QJsonObject body{
        {Q("token_type"), Q("Bearer")},
        {Q("access_token"), accessToken},
        {Q("refresh_token"), refreshToken},
        {Q("expires_in"), expiresIn},
        {Q("id_token"), jwt(QJsonObject{{Q("oid"), uid}, {Q("tid"), tid}, {Q("preferred_username"), upn}})},
        {Q("client_info"), b64url(json(QJsonObject{{Q("uid"), uid}, {Q("utid"), tid}}).toUtf8())},
    };
    return TestHttpMessageHandler::createResponse(200, json(body).toUtf8());
}

HttpResponse appTokenResponse(const QString &accessToken, int expiresIn = 3600)
{
    return TestHttpMessageHandler::createResponse(200, json(QJsonObject{{Q("access_token"), accessToken}, {Q("expires_in"), expiresIn}}).toUtf8());
}

HttpResponse errorResponse(int status, const QJsonObject &body) { return TestHttpMessageHandler::createResponse(status, json(body).toUtf8()); }

/// application/x-www-form-urlencoded content as a dictionary
StringDictionary form(const HttpRequest &request)
{
    StringDictionary d;
    const QStringList pairs = QString::fromUtf8(request.content).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const qsizetype eq = pair.indexOf(QLatin1Char('='));
        auto decode = [](QString s) { return QUrl::fromPercentEncoding(s.replace(QLatin1Char('+'), QLatin1Char(' ')).toUtf8()); };
        d.set(decode(pair.left(eq)), decode(pair.mid(eq + 1)));
    }
    return d;
}

StringDictionary query(const QUrl &url)
{
    HttpRequest r;
    r.content = url.query(QUrl::FullyEncoded).toUtf8();
    return form(r);
}

/// A token cache storage in memory (the same bytes shared by every storage of the factory)
class MemoryStorage : public ITokenCacheStorage
{
public:
    MemoryStorage(std::shared_ptr<QByteArray> data, bool failing) : m_data(std::move(data)), m_failing(failing) {}
    QByteArray load() override
    {
        if (m_failing) {
            throw IOException(Q("The Secret Service is not available"));
        }
        return *m_data;
    }
    void save(const QByteArray &data) override { *m_data = data; }
    QString description() const override { return m_failing ? Q("failing memory") : Q("memory"); }

private:
    std::shared_ptr<QByteArray> m_data;
    bool m_failing;
};

struct MemoryCaches
{
    std::shared_ptr<QByteArray> user = std::make_shared<QByteArray>();
    std::shared_ptr<QByteArray> app = std::make_shared<QByteArray>();
    bool primaryFails = false;
    bool fallbackFails = false;

    TokenCacheStorageFactory factory()
    {
        return [this](TokenCacheKind kind, bool fallback) {
            return std::make_unique<MemoryStorage>(kind == TokenCacheKind::User ? user : app, fallback ? fallbackFails : primaryFails);
        };
    }
    TokenCache userCache() const { return TokenCache::fromJson(*user); }
    TokenCache appCache() const { return TokenCache::fromJson(*app); }
};

class FakeCodeGenerator : public IOAuth2CodeGenerator
{
public:
    QString createNonce() override { return Q("STATE-12345"); }
    QString createPkceCodeVerifier() override { return Q("VERIFIER-abcde"); }
    QString createPkceCodeChallenge(OAuth2PkceChallengeMethod, const QString &verifier) override { return Q("CHALLENGE-") + verifier; }
};

class FakeBrowser : public IOAuth2WebBrowser
{
public:
    StringDictionary Response;
    QUrl AuthorizationUri;
    QUrl RedirectUri;
    QUrl updateRedirectUri(const QUrl &) override { return QUrl(Q("http://localhost:54321/")); }
    StringDictionary getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri, OAuth2ResponseMode, const CancellationToken &) override
    {
        AuthorizationUri = authorizationUri;
        RedirectUri = redirectUri;
        return Response;
    }
};

struct Fixture
{
    TestCommandContext context;
    std::shared_ptr<TestHttpMessageHandler> handler = std::make_shared<TestHttpMessageHandler>();
    MemoryCaches caches;
    std::shared_ptr<FakeBrowser> browser = std::make_shared<FakeBrowser>();
    qint64 now = 1700000000;

    Fixture()
    {
        handler->ThrowOnUnexpectedRequest = true;
        context.HttpClientFactory.MessageHandler = handler;
    }

    std::unique_ptr<EntraAuthentication> create(std::optional<PublicClientConfig> config = PublicClientConfig{ClientId})
    {
        auto auth = std::make_unique<EntraAuthentication>(context, std::move(config), caches.factory());
        auth->setCodeGenerator(std::make_shared<FakeCodeGenerator>());
        auth->setWebBrowser(browser);
        auth->setClock([this] { return now; });
        return auth;
    }
};

/// Runs `openssl` (for the PKCS#12 test fixtures)
void openssl(const QStringList &args)
{
    QProcess p;
    p.start(Q("openssl"), args);
    QVERIFY(p.waitForFinished(30000));
    QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
}

/// A new RSA private key, PEM
QByteArray newRsaKeyPem()
{
    EVP_PKEY *key = EVP_RSA_gen(2048);
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    QByteArray pem(data, len);
    BIO_free(bio);
    EVP_PKEY_free(key);
    return pem;
}

/// Verifies an RS256 signature with the public part of a PEM private key
bool verifyRs256(const QByteArray &privateKeyPem, const QByteArray &data, const QByteArray &signature)
{
    BIO *bio = BIO_new_mem_buf(privateKeyPem.constData(), int(privateKeyPem.size()));
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                    EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.constData()), size_t(signature.size()),
                                     reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

} // namespace

// =============================================================================================
// Ported tests
// =============================================================================================

class EntraTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- EntraAccountTests ----

    // (EntraAccount has no hash code here: the GetHashCode assertion has no counterpart)
    void entraAccount_Equals_DifferentCase_ReturnsTrue()
    {
        QVERIFY((EntraAccount{Q("HOME-ID"), Q("User@Example.com")} == EntraAccount{Q("home-id"), Q("user@example.com")}));
    }

    void entraAccount_Equals_DifferentHomeId_ReturnsFalse()
    {
        QVERIFY(!(EntraAccount{Q("home-id-1"), Q("user@example.com")} == EntraAccount{Q("home-id-2"), Q("user@example.com")}));
    }

    // ---- EntraAuthenticationTests ----

    void isBrokerEnabled_data()
    {
        QTest::addColumn<OptString>("environmentValue");
        QTest::addColumn<OptString>("gitConfigValue");
        QTest::addColumn<bool>("expected");
        const OptString none;
        auto row = [](const OptString &env, const OptString &cfg, bool expected) {
            QTest::addRow("env=%s cfg=%s", env ? qPrintable(Q("'%1'").arg(*env)) : "null", cfg ? qPrintable(Q("'%1'").arg(*cfg)) : "null")
                << env << cfg << expected;
        };
        row(none, none, true);
        const QList<std::pair<QString, bool>> values = {{Q("true"), true}, {Q("false"), false}, {Q("FALSE"), false}, {Q("0"), false}, {Q("no"), false},
                                                         {Q("off"), false}, {Q("invalid"), true}, {Q(""), true}, {Q(" "), true}};
        for (const auto &[value, expected] : values) {
            row(none, value, expected);
        }
        for (const auto &[value, expected] : values) {
            row(value, none, expected);
        }
        row(Q("false"), Q("true"), false);
        row(Q("true"), Q("false"), true);
        row(Q("invalid"), Q("false"), true);
        row(Q(""), Q("false"), true);
    }

    void isBrokerEnabled()
    {
        QFETCH(OptString, environmentValue);
        QFETCH(OptString, gitConfigValue);
        QFETCH(bool, expected);
        TestCommandContext context;
        if (gitConfigValue) {
            context.Git.Configuration.Global[Q("credential.msauthUseBroker")] = QStringList{*gitConfigValue};
        }
        if (environmentValue) {
            context.Environment.Variables.set(Constants::EnvironmentVariables::MsAuthUseBroker, *environmentValue);
        }
        EntraAuthentication entraAuth(context);
        QCOMPARE(entraAuth.isBrokerEnabled(), expected);
    }

    void getTokenForUser_NoInteraction_ThrowsException()
    {
        Fixture f;
        f.context.Settings.IsInteractionAllowed = false;
        auto entraAuth = f.create(PublicClientConfig{Q("C9E8FDA6-1D46-484C-917C-3DBD518F27C3")});
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, entraAuth->getTokenForUser({Q("user.read")}, Q("https://login.microsoftonline.com/common")));
        f.handler->assertNoRequests();
    }

    void getUserAccounts_NoPublicClientConfig_ThrowsException()
    {
        TestCommandContext context;
        EntraAuthentication entraAuth(context);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, entraAuth.getUserAccounts());
    }

    // ---- ManagedIdentityTests ----

    void managedIdentity_TryCreate_data()
    {
        QTest::addColumn<OptString>("value");
        QTest::addColumn<OptString>("expectedId");
        QTest::addColumn<ManagedIdentity::Kind>("expectedKind");
        const OptString clientIdMi(Q("id://8b49dca0-1298-4a0d-ad6d-934e40230839"));
        const OptString resourceIdMi(Q("resource://8b49dca0-1298-4a0d-ad6d-934e40230839"));
        // TryCreate_SystemId_ReturnsSystemIdentity
        for (const char *v : {"system", "SYSTEM", "sYsTeM"}) {
            QTest::newRow(v) << OptString(Q(v)) << OptString(Q("system")) << ManagedIdentity::Kind::System;
        }
        // TryCreate_ClientId_ReturnsUserIdentity
        for (const char *v : {"8B49DCA0-1298-4A0D-AD6D-934E40230839", "id://8B49DCA0-1298-4A0D-AD6D-934E40230839", "ID://8B49DCA0-1298-4A0D-AD6D-934E40230839",
                              "Id://8B49DCA0-1298-4A0D-AD6D-934E40230839"}) {
            QTest::newRow(v) << OptString(Q(v)) << clientIdMi << ManagedIdentity::Kind::ClientId;
        }
        // TryCreate_ResourceId_ReturnsUserIdentity
        for (const char *v : {"resource://8B49DCA0-1298-4A0D-AD6D-934E40230839", "RESOURCE://8B49DCA0-1298-4A0D-AD6D-934E40230839",
                              "rEsOuRcE://8B49DCA0-1298-4A0D-AD6D-934E40230839"}) {
            QTest::newRow(v) << OptString(Q(v)) << resourceIdMi << ManagedIdentity::Kind::ResourceId;
        }
        // TryCreate_InvalidId_ReturnsFalse
        QTest::newRow("null") << OptString() << OptString() << ManagedIdentity::Kind::System;
        for (const char *v : {"", "   ", "unknown://8B49DCA0-1298-4A0D-AD6D-934E40230839", "this is a string"}) {
            QTest::newRow(v) << OptString(Q(v)) << OptString() << ManagedIdentity::Kind::System;
        }
    }

    void managedIdentity_TryCreate()
    {
        QFETCH(OptString, value);
        QFETCH(OptString, expectedId);
        QFETCH(ManagedIdentity::Kind, expectedKind);
        ManagedIdentity identity = ManagedIdentity::system();
        QCOMPARE(ManagedIdentity::tryCreate(value, identity), expectedId.has_value());
        if (expectedId) {
            QCOMPARE(identity.id(), *expectedId);
            QCOMPARE(identity.kind(), expectedKind);
            if (expectedKind == ManagedIdentity::Kind::System) {
                QVERIFY(&ManagedIdentity::system() != &identity && identity == ManagedIdentity::system());
            } else {
                QCOMPARE(identity.value(), Q("8b49dca0-1298-4a0d-ad6d-934e40230839"));
            }
        }
    }

    // Own test: Create throws ArgumentException with the message upstream prints
    void managedIdentity_Create_Invalid_Throws()
    {
        try {
            ManagedIdentity::create(Q("bogus"));
            QFAIL("no exception");
        } catch (const ArgumentException &ex) {
            QCOMPARE(ex.message(), Q("Invalid managed identity id 'bogus' (Parameter 'id')"));
        }
    }

    // ---- ServicePrincipalTests ----

    void servicePrincipal_GetToken_NoCredential_ThrowsException()
    {
        Fixture f;
        auto entraAuth = f.create(std::nullopt);
        ServicePrincipalIdentity sp;
        sp.id = Q("11111111-1111-1111-1111-111111111111");
        sp.tenantId = Q("22222222-2222-2222-2222-222222222222");
        QVERIFY_THROWS_EXCEPTION(ArgumentException, entraAuth->getTokenForServicePrincipal({Q("scope")}, sp));
        f.handler->assertNoRequests();
    }

    // ---- WorkloadFederationOptionsTests ----

    void workloadFederationOptions_NullAudience_UsesDefault()
    {
        WorkloadFederationOptions options;
        options.setAudience(std::nullopt);
        QCOMPARE(options.audience(), WorkloadFederationOptions::DefaultAudience);
        QCOMPARE(options.audience(), Q("api://AzureADTokenExchange"));
    }
};
GCM_REGISTER_TEST(EntraTests)

// =============================================================================================
// Own tests: public client flows
// =============================================================================================

class EntraPublicClientTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void deviceCode_SignsInAndCachesAccount()
    {
        Fixture f; // no desktop session: no browser, so Auto resolves to the device code flow
        int tokenPolls = 0;
        f.handler->setup(Q("POST"), endpoint(Q("organizations"), Q("devicecode")), [](const HttpRequest &request) {
            const StringDictionary d = form(request);
            if (d.tryGet(Q("client_id")) != OptString(ClientId) || d.tryGet(Q("scope")) != OptString(Scopes.first() + Q(" openid profile offline_access"))) {
                return TestHttpMessageHandler::createResponse(400);
            }
            return TestHttpMessageHandler::createResponse(
                200, json(QJsonObject{{Q("device_code"), Q("DEVICE-CODE")}, {Q("user_code"), Q("ABCD-EFGH")}, {Q("verification_uri"), Q("https://microsoft.com/devicelogin")},
                                      {Q("message"), Q("To sign in, enter the code ABCD-EFGH")}, {Q("interval"), 0}, {Q("expires_in"), 900}})
                         .toUtf8());
        });
        f.handler->setup(Q("POST"), endpoint(Q("organizations"), Q("token")), [&](const HttpRequest &request) {
            const StringDictionary d = form(request);
            if (d.tryGet(Q("grant_type")) != OptString(Q("urn:ietf:params:oauth:grant-type:device_code")) || d.tryGet(Q("device_code")) != OptString(Q("DEVICE-CODE"))) {
                return TestHttpMessageHandler::createResponse(400);
            }
            if (++tokenPolls == 1) {
                return errorResponse(400, QJsonObject{{Q("error"), Q("authorization_pending")}});
            }
            return userTokenResponse(Q("ACCESS-TOKEN-1"), Q("alice@contoso.com"), Q("uid1"), Q("tid1"));
        });
        auto entraAuth = f.create();

        const EntraAuthResult result = entraAuth->getTokenForUser(Scopes, Q("https://login.microsoftonline.com/organizations"));
        QCOMPARE(result.accessToken, Q("ACCESS-TOKEN-1"));
        QVERIFY(result.account);
        QCOMPARE(result.account->homeAccountId, OptString(Q("uid1.tid1")));
        QCOMPARE(result.account->userName, OptString(Q("alice@contoso.com")));
        QCOMPARE(tokenPolls, 2);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("To sign in, enter the code ABCD-EFGH")});

        // The account and its tokens are cached
        const TokenCache cache = f.caches.userCache();
        QCOMPARE(cache.accounts.size(), 1);
        QCOMPARE(cache.accounts[0].tenantId, Q("tid1"));
        QCOMPARE(cache.refreshTokens.size(), 1);
        QCOMPARE(cache.refreshTokens[0].secret, Q("REFRESH-TOKEN"));
        QCOMPARE(cache.accessTokens.size(), 1);
        QCOMPARE(cache.accessTokens[0].tenantId, Q("tid1"));
        QCOMPARE(cache.accessTokens[0].expiresOn, f.now + 3600);
        QCOMPARE(entraAuth->getUserAccounts(), (QList<EntraAccount>{EntraAccount{Q("uid1.tid1"), Q("alice@contoso.com")}}));
    }

    void silent_CachedAccessToken_ThenRefreshToken()
    {
        Fixture f;
        int refreshes = 0;
        // A tenant-less authority uses the account's home tenant for silent requests
        f.handler->setup(Q("POST"), endpoint(Q("tid1"), Q("token")), [&](const HttpRequest &request) {
            const StringDictionary d = form(request);
            if (d.tryGet(Q("grant_type")) != OptString(Q("refresh_token")) || d.tryGet(Q("refresh_token")) != OptString(Q("REFRESH-TOKEN"))) {
                return TestHttpMessageHandler::createResponse(400);
            }
            ++refreshes;
            return userTokenResponse(Q("ACCESS-TOKEN-2"), Q("alice@contoso.com"), Q("uid1"), Q("tid1"), 3600, Q("REFRESH-TOKEN-2"));
        });
        seedUserCache(f, Q("tid1"), Q("ACCESS-TOKEN-1"), f.now + 3600);
        auto entraAuth = f.create();
        const EntraAccount account{std::nullopt, Q("ALICE@contoso.com")}; // matched by UPN, case-insensitively

        // A valid cached access token: no request
        QCOMPARE(entraAuth->getTokenForUser(Scopes, CommonAuthority(), account).accessToken, Q("ACCESS-TOKEN-1"));
        QCOMPARE(f.handler->totalRequestCount(), 0);

        // Expiring within five minutes: the refresh token is redeemed, and the new refresh token replaces the old one
        f.now += 3600 - 200;
        const EntraAuthResult refreshed = entraAuth->getTokenForUser(Scopes, CommonAuthority(), account);
        QCOMPARE(refreshed.accessToken, Q("ACCESS-TOKEN-2"));
        QCOMPARE(refreshed.account->homeAccountId, OptString(Q("uid1.tid1")));
        QCOMPARE(refreshes, 1);
        QCOMPARE(f.caches.userCache().refreshTokens.size(), 1);
        QCOMPARE(f.caches.userCache().refreshTokens[0].secret, Q("REFRESH-TOKEN-2"));
        QCOMPARE(f.caches.userCache().accessTokens.size(), 1); // the expired one was dropped
    }

    void silent_InvalidGrant_FallsBackToInteractive()
    {
        Fixture f;
        f.context.Settings.IsInteractionAllowed = false;
        f.handler->setup(Q("POST"), endpoint(Q("tid1"), Q("token")), errorResponse(400, QJsonObject{{Q("error"), Q("invalid_grant")}, {Q("error_description"), Q("AADSTS70008: expired")}}));
        seedUserCache(f, Q("tid1"), Q("ACCESS-TOKEN-1"), f.now - 1);
        auto entraAuth = f.create();
        // Silent fails with "UI required", then interaction is not allowed
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, entraAuth->getTokenForUser(Scopes, CommonAuthority(), EntraAccount{Q("uid1.tid1"), std::nullopt}));
        QCOMPARE(f.handler->requestCount(Q("POST"), endpoint(Q("tid1"), Q("token"))), 1);
    }

    void silent_OtherServiceError_Throws()
    {
        Fixture f;
        f.handler->setup(Q("POST"), endpoint(Q("tid1"), Q("token")),
                         errorResponse(400, QJsonObject{{Q("error"), Q("invalid_client")}, {Q("error_description"), Q("AADSTS7000218: bad client")}, {Q("claims"), Q("{}")}}));
        seedUserCache(f, Q("tid1"), Q("ACCESS-TOKEN-1"), f.now - 1);
        auto entraAuth = f.create();
        try {
            entraAuth->getTokenForUser(Scopes, CommonAuthority(), EntraAccount{Q("uid1.tid1"), std::nullopt});
            QFAIL("no exception");
        } catch (const EntraServiceException &ex) {
            QCOMPARE(QLatin1String(ex.typeName()), QLatin1String("MsalServiceException"));
            QCOMPARE(ex.errorCode(), Q("invalid_client"));
            QCOMPARE(ex.message(), Q("AADSTS7000218: bad client"));
            QCOMPARE(ex.statusCode(), 400);
            QCOMPARE(ex.claims(), OptString(Q("{}")));
            QVERIFY(!ex.isRetryable());
        }
    }

    void silent_MsaAccount_UsesTransferTenant()
    {
        Fixture f;
        const QString msaTenant = Constants::MsaHomeTenantId;
        PublicClientConfig config{ClientId};
        config.isMsaPassthroughEnabled = true;
        f.handler->setup(Q("POST"), endpoint(Constants::MsaTransferTenantId, Q("token")),
                         userTokenResponse(Q("MSA-TOKEN"), Q("bob@outlook.com"), Q("uid2"), msaTenant));
        TokenCache cache;
        cache.accounts.append({Q("uid2.") + msaTenant, Q("login.microsoftonline.com"), msaTenant, Q("bob@outlook.com")});
        cache.refreshTokens.append({Q("uid2.") + msaTenant, Q("login.microsoftonline.com"), ClientId, Q("REFRESH-TOKEN")});
        *f.caches.user = cache.toJson();
        auto entraAuth = f.create(config);
        QCOMPARE(entraAuth->getTokenForUser(Scopes, Q("https://login.microsoftonline.com/organizations"), EntraAccount{std::nullopt, Q("bob@outlook.com")}).accessToken,
                 Q("MSA-TOKEN"));
    }

    void systemWebView_AuthorizationCodeWithPkce()
    {
        Fixture f;
        f.context.SessionManager.IsDesktopSession = true;
        f.browser->Response = StringDictionary{{Q("code"), Q("AUTH-CODE")}, {Q("state"), Q("STATE-12345")}};
        StringDictionary tokenForm;
        f.handler->setup(Q("POST"), endpoint(Q("common"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return userTokenResponse(Q("ACCESS-TOKEN"), Q("alice@contoso.com"), Q("uid1"), Q("tid1"));
        });
        auto entraAuth = f.create();
        const EntraAuthResult result = entraAuth->getTokenForUser(Scopes);
        QCOMPARE(result.accessToken, Q("ACCESS-TOKEN"));
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("opening browser to complete authentication...")});

        QCOMPARE(absoluteUri(f.browser->AuthorizationUri.adjusted(QUrl::RemoveQuery)), Q("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
        const StringDictionary q = query(f.browser->AuthorizationUri);
        QCOMPARE(q.tryGet(Q("client_id")), OptString(ClientId));
        QCOMPARE(q.tryGet(Q("response_type")), OptString(Q("code")));
        QCOMPARE(q.tryGet(Q("redirect_uri")), OptString(Q("http://localhost:54321/")));
        QCOMPARE(q.tryGet(Q("scope")), OptString(Scopes.first() + Q(" openid profile offline_access")));
        QCOMPARE(q.tryGet(Q("state")), OptString(Q("STATE-12345")));
        QCOMPARE(q.tryGet(Q("code_challenge")), OptString(Q("CHALLENGE-VERIFIER-abcde")));
        QCOMPARE(q.tryGet(Q("code_challenge_method")), OptString(Q("S256")));
        QCOMPARE(q.tryGet(Q("prompt")), OptString(Q("select_account")));

        QCOMPARE(tokenForm.tryGet(Q("grant_type")), OptString(Q("authorization_code")));
        QCOMPARE(tokenForm.tryGet(Q("code")), OptString(Q("AUTH-CODE")));
        QCOMPARE(tokenForm.tryGet(Q("code_verifier")), OptString(Q("VERIFIER-abcde")));
        QCOMPARE(tokenForm.tryGet(Q("redirect_uri")), OptString(Q("http://localhost:54321/")));
        QCOMPARE(tokenForm.tryGet(Q("client_info")), OptString(Q("1")));
    }

    void systemWebView_Errors_data()
    {
        QTest::addColumn<StringDictionary>("response");
        QTest::addColumn<QString>("expectedType");
        QTest::addColumn<QString>("expectedCode");
        QTest::newRow("access denied") << StringDictionary{{Q("error"), Q("access_denied")}, {Q("error_description"), Q("The user cancelled")}}
                                       << Q("MsalClientException") << Q("authentication_canceled");
        QTest::newRow("server error") << StringDictionary{{Q("error"), Q("server_error")}} << Q("MsalServiceException") << Q("server_error");
        QTest::newRow("state mismatch") << StringDictionary{{Q("code"), Q("AUTH-CODE")}, {Q("state"), Q("OTHER")}} << Q("MsalClientException") << Q("state_mismatch");
        QTest::newRow("no code") << StringDictionary{{Q("state"), Q("STATE-12345")}} << Q("MsalClientException") << Q("authentication_ui_failed");
    }

    void systemWebView_Errors()
    {
        QFETCH(StringDictionary, response);
        QFETCH(QString, expectedType);
        QFETCH(QString, expectedCode);
        Fixture f;
        f.context.SessionManager.IsDesktopSession = true;
        f.browser->Response = response;
        auto entraAuth = f.create();
        try {
            entraAuth->getTokenForUser(Scopes);
            QFAIL("no exception");
        } catch (const EntraException &ex) {
            QCOMPARE(QString::fromLatin1(ex.typeName()), expectedType);
            QCOMPARE(ex.errorCode(), expectedCode);
        }
        f.handler->assertNoRequests();
    }

    void interactionMode_Preference_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expectDeviceCode");
        QTest::addColumn<QStringList>("expectedMessages");
        QTest::newRow("devicecode") << Q("devicecode") << true << QStringList{};
        QTest::newRow("number") << Q("3") << true << QStringList{};
        QTest::newRow("system") << Q("System") << false << QStringList{};
        QTest::newRow("unknown") << Q("bogus") << false << QStringList{Q("unknown interaction mode 'bogus'; using 'auto'")};
    }

    // GCM_MSAUTH_FLOW overrides Auto (here with a browser available)
    void interactionMode_Preference()
    {
        QFETCH(QString, value);
        QFETCH(bool, expectDeviceCode);
        QFETCH(QStringList, expectedMessages);
        Fixture f;
        f.context.SessionManager.IsDesktopSession = true;
        f.context.Environment.Variables.set(Constants::EnvironmentVariables::MsAuthFlow, value);
        f.browser->Response = StringDictionary{{Q("error"), Q("access_denied")}};
        f.handler->setup(Q("POST"), endpoint(Q("common"), Q("devicecode")), errorResponse(400, QJsonObject{{Q("error"), Q("invalid_scope")}}));
        auto entraAuth = f.create();
        try {
            entraAuth->getTokenForUser(Scopes);
            QFAIL("no exception");
        } catch (const EntraException &ex) {
            // device code: the error of the device code endpoint; browser: the cancelled sign-in
            QCOMPARE(ex.errorCode(), expectDeviceCode ? Q("invalid_scope") : Q("authentication_canceled"));
        }
        expectedMessages.append(expectDeviceCode ? QStringList{} : QStringList{Q("opening browser to complete authentication...")});
        QCOMPARE(f.context.Console.WrittenMessages, expectedMessages);
    }

    void interactive_NoModeAvailable_Throws()
    {
        Fixture f;
        f.context.Settings.IsTerminalPromptsEnabled = false;
        auto entraAuth = f.create();
        try {
            entraAuth->getTokenForUser(Scopes);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("No available interaction modes."));
        }
    }

    void getInteractionMode_TerminalPrompt()
    {
        Fixture f;
        f.context.SessionManager.IsDesktopSession = true;
        f.context.Settings.IsGuiPromptsEnabled = false;
        f.context.Console.pushSelection(2);
        auto entraAuth = f.create();
        QCOMPARE(entraAuth->getInteractionMode(), InteractionMode::DeviceCode);
        QCOMPARE(f.context.Console.Prompts, QStringList{Q("Select an authentication flow")});
        QCOMPARE(f.context.Console.LastChoices, (QStringList{Q("Auto"), Q("System web view"), Q("Device code")}));
    }

    void removeUserAccount()
    {
        Fixture f;
        seedUserCache(f, Q("tid1"), Q("ACCESS-TOKEN-1"), f.now + 3600);
        auto entraAuth = f.create();
        QVERIFY(!entraAuth->removeUserAccount(EntraAccount{Q("other.tid1"), std::nullopt}));
        QVERIFY(entraAuth->removeUserAccount(EntraAccount{Q("UID1.TID1"), std::nullopt}));
        const TokenCache cache = f.caches.userCache();
        QVERIFY(cache.accounts.isEmpty() && cache.refreshTokens.isEmpty() && cache.accessTokens.isEmpty());
        QVERIFY(entraAuth->getUserAccounts().isEmpty());
    }

    void tokenCache_JsonRoundTrip()
    {
        TokenCache cache;
        cache.accounts.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), Q("tid1"), Q("alice@contoso.com")});
        cache.refreshTokens.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), ClientId, Q("RT")});
        cache.accessTokens.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), ClientId, Q("tid1"), Q("scope"), Q("AT"), 1234});
        const QByteArray bytes = cache.toJson();
        const QJsonObject obj = QJsonDocument::fromJson(bytes).object();
        QVERIFY(obj.contains(Q("version")) && obj.contains(Q("accounts")) && obj.contains(Q("refresh_tokens")) && obj.contains(Q("access_tokens")));
        const TokenCache back = TokenCache::fromJson(bytes);
        QCOMPARE(back.toJson(), bytes);
        QCOMPARE(back.accessTokens[0].expiresOn, 1234);
        // Garbage and empty input give an empty cache
        QVERIFY(TokenCache::fromJson(QByteArray()).accounts.isEmpty());
    }

    void cache_SecureStorageFails_UsesFallback()
    {
        Fixture f;
        f.caches.primaryFails = true;
        seedUserCache(f, Q("tid1"), Q("ACCESS-TOKEN-1"), f.now + 3600);
        auto entraAuth = f.create();
        QCOMPARE(entraAuth->getTokenForUser(Scopes, CommonAuthority(), EntraAccount{Q("uid1.tid1"), std::nullopt}).accessToken, Q("ACCESS-TOKEN-1"));
        QCOMPARE(f.context.Console.WrittenMessages,
                 (QStringList{Q("cannot persist Entra authentication token cache securely!"), Q("using plain-text fallback token cache")}));
    }

    void cache_DefaultStorage_FallbackFile()
    {
        TestCommandContext context;
        const QString dir = TestUtils::getTempDirectory();
        context.FileSystem.UserDataDirectoryPath = dir;
        std::unique_ptr<ITokenCacheStorage> storage = createDefaultTokenCacheStorage(context, TokenCacheKind::App, true);
        QCOMPARE(storage->description(), Q("file '%1/entra/app.cache'").arg(dir));
        QCOMPARE(storage->load(), QByteArray());
        storage->lock();
        storage->save("{\"version\":1}");
        storage->unlock();
        QCOMPARE(storage->load(), QByteArray("{\"version\":1}"));
        QVERIFY(!(QFile::permissions(dir + Q("/entra/app.cache")) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
        QCOMPARE(createDefaultTokenCacheStorage(context, TokenCacheKind::User, false)->description(),
                 Q("Secret Service item 'gcm-entra-token-cache://user'"));
        QDir(dir).removeRecursively();
    }

    void diagnostic_ReportsStorages()
    {
        TestCommandContext context;
        MemoryCaches caches;
        caches.primaryFails = true;
        azrepos::EntraAuthenticationDiagnostic diagnostic(context, caches.factory());
        const DiagnosticResult result = diagnostic.run();
        QStringList messages;
        for (const DiagnosticReport &r : result.reports) {
            messages.append(r.message);
        }
        QVERIFY(messages.contains(Q("Storage: failing memory")));
        QVERIFY(messages.contains(Q("Fallback storage: memory")));
        // The fallback works, so the cache works: no warning (as upstream, which only checks the fallback file)
        QCOMPARE(messages.count(Q("Secure storage unavailable: The Secret Service is not available")), 2);
        QCOMPARE(result.warningCount, 0);
        QCOMPARE(result.outcome, DiagnosticOutcome::Success);
    }

    void diagnostic_BothStoragesFail_Warns()
    {
        TestCommandContext context;
        MemoryCaches caches;
        caches.primaryFails = true;
        caches.fallbackFails = true;
        azrepos::EntraAuthenticationDiagnostic diagnostic(context, caches.factory());
        const DiagnosticResult result = diagnostic.run();
        QCOMPARE(result.warningCount, 2);
        QCOMPARE(result.reports.last().message, Q("Failed cache persistence test: The Secret Service is not available"));
    }

private:
    static QString CommonAuthority() { return Q("https://login.microsoftonline.com/common"); }

    /// alice@contoso.com (uid1.tid1) with a refresh token and an access token for `Scopes`
    static void seedUserCache(Fixture &f, const QString &tenant, const QString &accessToken, qint64 expiresOn)
    {
        TokenCache cache;
        cache.accounts.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), tenant, Q("alice@contoso.com")});
        cache.refreshTokens.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), ClientId, Q("REFRESH-TOKEN")});
        cache.accessTokens.append({Q("uid1.tid1"), Q("login.microsoftonline.com"), ClientId, tenant, Scopes.first().toLower(), accessToken, expiresOn});
        *f.caches.user = cache.toJson();
    }
};
GCM_REGISTER_TEST(EntraPublicClientTests)

// =============================================================================================
// Own tests: confidential clients and managed identities
// =============================================================================================

class EntraConfidentialClientTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void servicePrincipal_Secret_ClientCredentialsAndAppCache()
    {
        Fixture f;
        StringDictionary tokenForm;
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return appTokenResponse(Q("SP-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        ServicePrincipalIdentity sp;
        sp.id = Q("client1");
        sp.tenantId = Q("tenant1");
        sp.clientSecret = Q("SECRET");
        QCOMPARE(entraAuth->getTokenForServicePrincipal(Scopes, sp).accessToken, Q("SP-TOKEN"));
        QCOMPARE(tokenForm.tryGet(Q("grant_type")), OptString(Q("client_credentials")));
        QCOMPARE(tokenForm.tryGet(Q("client_id")), OptString(Q("client1")));
        QCOMPARE(tokenForm.tryGet(Q("client_secret")), OptString(Q("SECRET")));
        QCOMPARE(tokenForm.tryGet(Q("scope")), OptString(Scopes.first()));
        // The second request is answered from the application cache
        QCOMPARE(entraAuth->getTokenForServicePrincipal(Scopes, sp).accessToken, Q("SP-TOKEN"));
        QCOMPARE(f.handler->totalRequestCount(), 1);
        QCOMPARE(f.caches.appCache().accessTokens.size(), 1);
    }

    void servicePrincipal_Certificate_SignedAssertion_data()
    {
        QTest::addColumn<bool>("sendX5C");
        QTest::newRow("x5t only") << false;
        QTest::newRow("with x5c") << true;
    }

    void servicePrincipal_Certificate_SignedAssertion()
    {
        QFETCH(bool, sendX5C);
        Fixture f;
        StringDictionary tokenForm;
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return appTokenResponse(Q("SP-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        ServicePrincipalIdentity sp;
        sp.id = Q("client1");
        sp.tenantId = Q("tenant1");
        sp.certificate = ServicePrincipalIdentity::Certificate{Q("THUMB"), QByteArray("certificate DER bytes"), newRsaKeyPem()};
        sp.sendX5C = sendX5C;
        QCOMPARE(entraAuth->getTokenForServicePrincipal(Scopes, sp).accessToken, Q("SP-TOKEN"));

        QCOMPARE(tokenForm.tryGet(Q("client_assertion_type")), OptString(Q("urn:ietf:params:oauth:client-assertion-type:jwt-bearer")));
        QVERIFY(!tokenForm.contains(Q("client_secret")));
        const QStringList parts = valueOr(tokenForm.tryGet(Q("client_assertion"))).split(QLatin1Char('.'));
        QCOMPARE(parts.size(), 3);
        const QJsonObject header = QJsonDocument::fromJson(unb64url(parts[0])).object();
        const QJsonObject payload = QJsonDocument::fromJson(unb64url(parts[1])).object();
        QCOMPARE(header.value(Q("alg")).toString(), Q("RS256"));
        QCOMPARE(header.value(Q("x5t")).toString(), b64url(QCryptographicHash::hash("certificate DER bytes", QCryptographicHash::Sha1)));
        QCOMPARE(header.contains(Q("x5c")), sendX5C);
        if (sendX5C) {
            QCOMPARE(header.value(Q("x5c")).toArray().first().toString(), QString::fromLatin1(QByteArray("certificate DER bytes").toBase64()));
        }
        QCOMPARE(payload.value(Q("aud")).toString(), absoluteUri(endpoint(Q("tenant1"), Q("token"))));
        QCOMPARE(payload.value(Q("iss")).toString(), Q("client1"));
        QCOMPARE(payload.value(Q("sub")).toString(), Q("client1"));
        QCOMPARE(qint64(payload.value(Q("exp")).toDouble()), f.now + 600);
        QVERIFY(verifyRs256(sp.certificate->privateKeyPem, (parts[0] + Q(".") + parts[1]).toLatin1(), unb64url(parts[2])));
    }

    void getCertificateByThumbprint_Pkcs12Store()
    {
        const QString home = TestUtils::getTempDirectory();
        const QString store = home + Q("/.dotnet/corefx/cryptography/x509stores/my");
        QVERIFY(QDir().mkpath(store));
        openssl({Q("req"), Q("-x509"), Q("-newkey"), Q("rsa:2048"), Q("-nodes"), Q("-keyout"), home + Q("/key.pem"), Q("-out"), home + Q("/cert.pem"),
                 Q("-subj"), Q("/CN=gcm-test"), Q("-days"), Q("1")});
        openssl({Q("pkcs12"), Q("-export"), Q("-inkey"), home + Q("/key.pem"), Q("-in"), home + Q("/cert.pem"), Q("-out"), store + Q("/some-name.pfx"), Q("-passout"), Q("pass:")});
        QFile certFile(home + Q("/cert.pem"));
        QVERIFY(certFile.open(QIODevice::ReadOnly));
        const QByteArray pem = certFile.readAll();
        const QByteArray der = QByteArray::fromBase64(pem.mid(pem.indexOf('\n') + 1, pem.indexOf("-----END") - pem.indexOf('\n') - 1));
        const QString thumbprint = QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha1).toHex());

        // Found whatever the case and separators of the thumbprint
        const std::optional<ServicePrincipalIdentity::Certificate> cert = getCertificateByThumbprint(home, thumbprint.toLower());
        QVERIFY(cert);
        QCOMPARE(cert->thumbprint, thumbprint.toUpper());
        QCOMPARE(cert->der, der);
        QVERIFY(cert->privateKeyPem.startsWith("-----BEGIN PRIVATE KEY-----"));

        // Not found: the exception .NET throws on Unix when it goes on to the LocalMachine store
        try {
            getCertificateByThumbprint(home, Q("00"));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(QLatin1String(ex.typeName()), QLatin1String("CryptographicException"));
            QCOMPARE(ex.message(), Q("Unix LocalMachine X509Store is limited to the Root and CertificateAuthority stores."));
            QVERIFY(ex.inner());
        }
        QDir(home).removeRecursively();
    }

    void managedIdentity_Sources_data()
    {
        QTest::addColumn<StringDictionary>("variables");
        QTest::addColumn<QString>("identity");
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("url");
        QTest::addColumn<StringDictionary>("expectedQuery");
        QTest::addColumn<QString>("expectedHeader");
        const QString res = Q("499b84ac-1321-427f-aa17-267ca6975798");
        const QString guid = Q("8b49dca0-1298-4a0d-ad6d-934e40230839");
        QTest::newRow("imds system") << StringDictionary{} << Q("system") << Q("GET") << Q("http://169.254.169.254/metadata/identity/oauth2/token")
                                     << StringDictionary{{Q("api-version"), Q("2018-02-01")}, {Q("resource"), res}} << Q("Metadata: true");
        QTest::newRow("imds client id") << StringDictionary{} << guid << Q("GET") << Q("http://169.254.169.254/metadata/identity/oauth2/token")
                                        << StringDictionary{{Q("api-version"), Q("2018-02-01")}, {Q("resource"), res}, {Q("client_id"), guid}} << Q("Metadata: true");
        QTest::newRow("imds resource id") << StringDictionary{{Q("AZURE_POD_IDENTITY_AUTHORITY_HOST"), Q("http://pod-identity:2020")}} << Q("resource://") + guid << Q("GET")
                                          << Q("http://pod-identity:2020/metadata/identity/oauth2/token")
                                          << StringDictionary{{Q("api-version"), Q("2018-02-01")}, {Q("resource"), res}, {Q("msi_res_id"), guid}} << Q("Metadata: true");
        QTest::newRow("app service") << StringDictionary{{Q("IDENTITY_ENDPOINT"), Q("http://localhost:42356/msi/token")}, {Q("IDENTITY_HEADER"), Q("SECRET-HEADER")}}
                                     << Q("id://") + guid << Q("GET") << Q("http://localhost:42356/msi/token")
                                     << StringDictionary{{Q("api-version"), Q("2019-08-01")}, {Q("resource"), res}, {Q("client_id"), guid}}
                                     << Q("X-IDENTITY-HEADER: SECRET-HEADER");
        QTest::newRow("cloud shell") << StringDictionary{{Q("MSI_ENDPOINT"), Q("http://localhost:50342/oauth2/token")}} << Q("system") << Q("POST")
                                     << Q("http://localhost:50342/oauth2/token") << StringDictionary{} << Q("Metadata: true");
    }

    void managedIdentity_Sources()
    {
        QFETCH(StringDictionary, variables);
        QFETCH(QString, identity);
        QFETCH(QString, method);
        QFETCH(QString, url);
        QFETCH(StringDictionary, expectedQuery);
        QFETCH(QString, expectedHeader);
        Fixture f;
        for (const auto &[k, v] : variables) {
            f.context.Environment.Variables.set(k, v);
        }
        HttpRequest seen;
        f.handler->setup(method, QUrl(url), [&](const HttpRequest &request) {
            seen = request;
            return appTokenResponse(Q("MI-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        QCOMPARE(entraAuth->getTokenForManagedIdentity(Q("499b84ac-1321-427f-aa17-267ca6975798"), ManagedIdentity::create(identity)).accessToken, Q("MI-TOKEN"));
        const StringDictionary q = query(seen.url);
        QCOMPARE(q.count(), expectedQuery.count());
        for (const auto &[k, v] : expectedQuery) {
            QCOMPARE(q.tryGet(k), OptString(v));
        }
        const qsizetype colon = expectedHeader.indexOf(QLatin1Char(':'));
        QCOMPARE(seen.headers.value(expectedHeader.left(colon)), OptString(expectedHeader.mid(colon + 2)));
        if (method == QLatin1String("POST")) {
            QCOMPARE(form(seen).tryGet(Q("resource")), OptString(Q("499b84ac-1321-427f-aa17-267ca6975798")));
        }
    }

    void managedIdentity_AzureArc_KeyChallenge()
    {
        Fixture f;
        f.context.Environment.Variables.set(Q("IDENTITY_ENDPOINT"), Q("http://localhost:40342/metadata/identity/oauth2/token"));
        f.context.Environment.Variables.set(Q("IMDS_ENDPOINT"), Q("http://localhost:40342"));
        f.context.FileSystem.Files[Q("/var/opt/azcmagent/tokens/abc.key")] = "ARC-KEY\n";
        int calls = 0;
        f.handler->setup(Q("GET"), QUrl(Q("http://localhost:40342/metadata/identity/oauth2/token")), [&](const HttpRequest &request) {
            if (++calls == 1) {
                HttpResponse challenge = TestHttpMessageHandler::createResponse(401);
                challenge.headers.add(Q("WWW-Authenticate"), Q("Basic realm=/var/opt/azcmagent/tokens/abc.key"));
                return challenge;
            }
            return request.headers.value(Q("Authorization")) == OptString(Q("Basic ARC-KEY")) ? appTokenResponse(Q("ARC-TOKEN"))
                                                                                                 : TestHttpMessageHandler::createResponse(401);
        });
        auto entraAuth = f.create(std::nullopt);
        QCOMPARE(entraAuth->getTokenForManagedIdentity(Q("resource"), ManagedIdentity::system()).accessToken, Q("ARC-TOKEN"));
        QCOMPARE(calls, 2);
    }

    void managedIdentity_Errors()
    {
        // A user-assigned identity is not supported by Cloud Shell
        {
            Fixture f;
            f.context.Environment.Variables.set(Q("MSI_ENDPOINT"), Q("http://localhost:50342/oauth2/token"));
            auto entraAuth = f.create(std::nullopt);
            try {
                entraAuth->getTokenForManagedIdentity(Q("resource"), ManagedIdentity::create(Q("8b49dca0-1298-4a0d-ad6d-934e40230839")));
                QFAIL("no exception");
            } catch (const EntraClientException &ex) {
                QCOMPARE(ex.errorCode(), Q("user_assigned_managed_identity_not_supported"));
            }
        }
        // An error answer of IMDS
        {
            Fixture f;
            f.handler->setup(Q("GET"), QUrl(Q("http://169.254.169.254/metadata/identity/oauth2/token")),
                             errorResponse(400, QJsonObject{{Q("error"), Q("invalid_request")}, {Q("error_description"), Q("Identity not found")}}));
            auto entraAuth = f.create(std::nullopt);
            try {
                entraAuth->getTokenForManagedIdentity(Q("resource"), ManagedIdentity::system());
                QFAIL("no exception");
            } catch (const EntraServiceException &ex) {
                QCOMPARE(ex.errorCode(), Q("managed_identity_request_failed"));
                QCOMPARE(ex.message(), Q("[Managed Identity] Identity not found"));
                QCOMPARE(ex.statusCode(), 400);
            }
        }
        // No network
        {
            Fixture f;
            f.handler->SimulateNoNetwork = true;
            auto entraAuth = f.create(std::nullopt);
            try {
                entraAuth->getTokenForManagedIdentity(Q("resource"), ManagedIdentity::system());
                QFAIL("no exception");
            } catch (const EntraServiceException &ex) {
                QCOMPARE(ex.errorCode(), Q("managed_identity_unreachable_network"));
            }
        }
    }

    void workloadFederation_Generic()
    {
        Fixture f;
        StringDictionary tokenForm;
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return appTokenResponse(Q("WIF-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        WorkloadFederationOptions opts;
        opts.scenario = WorkloadFederationScenario::Generic;
        opts.clientId = Q("client1");
        opts.tenantId = Q("tenant1");
        opts.genericClientAssertion = Q("CLIENT-ASSERTION");
        QCOMPARE(entraAuth->getTokenUsingWorkloadFederation(Scopes, opts).accessToken, Q("WIF-TOKEN"));
        QCOMPARE(tokenForm.tryGet(Q("client_assertion")), OptString(Q("CLIENT-ASSERTION")));
        QCOMPARE(tokenForm.tryGet(Q("client_assertion_type")), OptString(Q("urn:ietf:params:oauth:client-assertion-type:jwt-bearer")));
        QCOMPARE(tokenForm.tryGet(Q("grant_type")), OptString(Q("client_credentials")));
    }

    void workloadFederation_ManagedIdentity()
    {
        Fixture f;
        StringDictionary imdsQuery, tokenForm;
        f.handler->setup(Q("GET"), QUrl(Q("http://169.254.169.254/metadata/identity/oauth2/token")), [&](const HttpRequest &request) {
            imdsQuery = query(request.url);
            return appTokenResponse(Q("MI-ASSERTION"));
        });
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return appTokenResponse(Q("WIF-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        WorkloadFederationOptions opts;
        opts.scenario = WorkloadFederationScenario::ManagedIdentity;
        opts.clientId = Q("client1");
        opts.tenantId = Q("tenant1");
        opts.managedIdentityId = Q("8b49dca0-1298-4a0d-ad6d-934e40230839");
        QCOMPARE(entraAuth->getTokenUsingWorkloadFederation(Scopes, opts).accessToken, Q("WIF-TOKEN"));
        // The managed identity token is requested for the federation audience
        QCOMPARE(imdsQuery.tryGet(Q("resource")), OptString(Q("api://AzureADTokenExchange")));
        QCOMPARE(imdsQuery.tryGet(Q("client_id")), OptString(Q("8b49dca0-1298-4a0d-ad6d-934e40230839")));
        QCOMPARE(tokenForm.tryGet(Q("client_assertion")), OptString(Q("MI-ASSERTION")));
    }

    void workloadFederation_GitHubActions()
    {
        Fixture f;
        HttpRequest oidcRequest;
        StringDictionary tokenForm;
        f.handler->setup(Q("GET"), QUrl(Q("https://token.actions.example.com/oidc/example")), [&](const HttpRequest &request) {
            oidcRequest = request;
            return TestHttpMessageHandler::createResponse(200, json(QJsonObject{{Q("value"), Q("GITHUB-OIDC-TOKEN")}}).toUtf8());
        });
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")), [&](const HttpRequest &request) {
            tokenForm = form(request);
            return appTokenResponse(Q("WIF-TOKEN"));
        });
        auto entraAuth = f.create(std::nullopt);
        WorkloadFederationOptions opts;
        opts.scenario = WorkloadFederationScenario::GitHubActions;
        opts.clientId = Q("client1");
        opts.tenantId = Q("tenant1");
        opts.setAudience(Q("api://custom audience"));
        opts.gitHubTokenRequestUrl = QUrl(Q("https://token.actions.example.com/oidc/example?param=value"));
        opts.gitHubTokenRequestToken = Q("REQUEST-TOKEN");
        QCOMPARE(entraAuth->getTokenUsingWorkloadFederation(Scopes, opts).accessToken, Q("WIF-TOKEN"));
        QCOMPARE(oidcRequest.url.query(QUrl::FullyEncoded), Q("param=value&audience=api%3A%2F%2Fcustom%20audience"));
        RestTestUtilities::assertBearerAuth(oidcRequest, Q("REQUEST-TOKEN"));
        QCOMPARE(tokenForm.tryGet(Q("client_assertion")), OptString(Q("GITHUB-OIDC-TOKEN")));
    }

    void workloadFederation_GitHubActions_BadResponse()
    {
        Fixture f;
        f.handler->setup(Q("GET"), QUrl(Q("https://token.actions.example.com/oidc")),
                         TestHttpMessageHandler::createResponse(200, json(QJsonObject{{Q("other"), 1}}).toUtf8()));
        auto entraAuth = f.create(std::nullopt);
        WorkloadFederationOptions opts;
        opts.scenario = WorkloadFederationScenario::GitHubActions;
        opts.clientId = Q("client1");
        opts.tenantId = Q("tenant1");
        opts.gitHubTokenRequestUrl = QUrl(Q("https://token.actions.example.com/oidc"));
        opts.gitHubTokenRequestToken = Q("REQUEST-TOKEN");
        try {
            entraAuth->getTokenUsingWorkloadFederation(Scopes, opts);
            QFAIL("no exception");
        } catch (const InvalidOperationException &ex) {
            QCOMPARE(ex.message(), Q("Invalid response from GitHub OIDC token endpoint: 'value' property not found."));
        }
    }

    void tokenEndpoint_ProtectionPolicyRequired_ThrowsIntuneException()
    {
        Fixture f;
        f.handler->setup(Q("POST"), endpoint(Q("tenant1"), Q("token")),
                         errorResponse(400, QJsonObject{{Q("error"), Q("unauthorized_client")}, {Q("suberror"), Q("protection_policy_required")}}));
        auto entraAuth = f.create(std::nullopt);
        ServicePrincipalIdentity sp;
        sp.id = Q("client1");
        sp.tenantId = Q("tenant1");
        sp.clientSecret = Q("SECRET");
        QVERIFY_THROWS_EXCEPTION(IntuneAppProtectionPolicyRequiredException, entraAuth->getTokenForServicePrincipal(Scopes, sp));
    }
};
GCM_REGISTER_TEST(EntraConfidentialClientTests)

#include "entratests.moc"
