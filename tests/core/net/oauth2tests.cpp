// Ports of OAuth2ClientTests, OAuth2CryptographicCodeGeneratorTests, OAuth2ResponseModeTests,
// OAuth2SystemWebBrowserTests and TokenEndpointResponseJsonTest, plus own tests of the loopback listener.
#include "core/auth/oauth2.h"
#include "core/strings.h"
#include "infra/testmain.h"
#include "infra/testoauth2server.h"
#include "infra/testsessionmanager.h"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QTimer>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

const QString TestClientId = Q("9ffe7f11c8");
const QString TestClientSecret = Q("62adac63a4614d93833470942a38454f");
const QUrl TestRedirectUri(Q("http://localhost/oauth-callback"));
const QStringList ExpectedScopes{Q("read"), Q("write"), Q("delete")};

std::shared_ptr<OAuth2Application> createTestApplication()
{
    auto app = std::make_shared<OAuth2Application>(TestClientId);
    app->Secret = TestClientSecret;
    app->RedirectUris = {TestRedirectUri.toString()};
    return app;
}

OAuth2ServerEndpoints createEndpoints()
{
    OAuth2ServerEndpoints endpoints(QUrl(Q("https://example.com/oauth/v2.0/authorize")),
                                    QUrl(Q("https://example.com/oauth/v2.0/access_token")));
    endpoints.deviceAuthorizationEndpoint = QUrl(Q("https://example.com/oauth/v2.0/authorize_device"));
    return endpoints;
}

std::unique_ptr<OAuth2Client> createClient(const std::shared_ptr<TestHttpMessageHandler> &handler,
                                           const OAuth2ServerEndpoints &endpoints,
                                           OAuth2ResponseMode responseMode = OAuth2ResponseMode::Default)
{
    return std::make_unique<OAuth2Client>(HttpClient(handler), endpoints, TestClientId, TestRedirectUri, TestClientSecret,
                                          true, responseMode);
}

/// Server + handler wired together, as each upstream test does.
struct ServerFixture
{
    ServerFixture()
    {
        handler->ThrowOnUnexpectedRequest = true;
        server.registerApplication(app);
        server.bind(*handler);
    }
    OAuth2ServerEndpoints endpoints = createEndpoints();
    std::shared_ptr<TestHttpMessageHandler> handler = std::make_shared<TestHttpMessageHandler>();
    std::shared_ptr<OAuth2Application> app = createTestApplication();
    TestOAuth2Server server{endpoints};
};
} // namespace

class OAuth2ClientTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void oauth2Client_getAuthorizationCodeAsync()
    {
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("68c39cbd8d"));
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints);
        const OAuth2AuthorizationCodeResult result = client->getAuthorizationCode(ExpectedScopes, browser, {}, {});
        QCOMPARE(result.code, Q("68c39cbd8d"));
    }

    void oauth2Client_getAuthorizationCodeAsync_redirectUrlOriginalStringPreserved_data()
    {
        QTest::addColumn<QString>("expectedRedirectUrl");
        for (const char *url : {"http://localhost", "http://localhost/", "http://localhost/oauth-callback",
                                "http://localhost/oauth-callback/", "http://127.0.0.1", "http://127.0.0.1/",
                                "http://127.0.0.1/oauth-callback", "http://127.0.0.1/oauth-callback/"}) {
            QTest::newRow(url) << Q(url);
                                }
    }
    void oauth2Client_getAuthorizationCodeAsync_redirectUrlOriginalStringPreserved()
    {
        QFETCH(QString, expectedRedirectUrl);
        ServerFixture f;
        f.app->RedirectUris = {expectedRedirectUrl};
        f.server.TokenGenerator.AuthCodes.append(Q("unused"));
        OptString actualRedirectUri;
        f.server.AuthorizationEndpointInvoked = [&](const HttpRequest &request) {
            actualRedirectUri = getQueryParameters(request.url).tryGet(OAuth2Constants::RedirectUriParameter);
        };
        TestOAuth2WebBrowser browser(f.handler);
        OAuth2Client client(HttpClient(f.handler), f.endpoints, TestClientId, QUrl(expectedRedirectUrl), TestClientSecret);
        client.getAuthorizationCode({Q("unused")}, browser, {}, {});
        QCOMPARE(actualRedirectUri, OptString(expectedRedirectUrl));
    }

    void oauth2Client_getAuthorizationCodeAsync_extraQueryParams()
    {
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("68c39cbd8d"));
        const StringDictionary extraParams{{Q("param1"), Q("value1")}, {Q("param2"), Q("value2")}, {Q("param3"), Q("value3")}};
        StringDictionary actualParams;
        f.server.AuthorizationEndpointInvoked = [&](const HttpRequest &request) { actualParams = getQueryParameters(request.url); };
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints);
        const OAuth2AuthorizationCodeResult result = client->getAuthorizationCode(ExpectedScopes, browser, extraParams, {});
        QCOMPARE(result.code, Q("68c39cbd8d"));
        for (const auto &[key, value] : extraParams) {
            QCOMPARE(actualParams.tryGet(key), OptString(value));
        }
    }

    void oauth2Client_getAuthorizationCodeAsync_extraQueryParams_overrideStandardArgs_throwsException()
    {
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("68c39cbd8d"));
        const StringDictionary extraParams{
            {Q("param1"), Q("value1")}, {OAuth2Constants::ClientIdParameter, Q("value2")}, {Q("param3"), Q("value3")}};
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, client->getAuthorizationCode(ExpectedScopes, browser, extraParams, {}));
    }

    void oauth2Client_getAuthorizationCodeAsync_nonDefaultResponseMode_sendsResponseModeParameter_data()
    {
        QTest::addColumn<int>("responseMode");
        QTest::addColumn<QString>("expectedValue");
        QTest::newRow("query") << int(OAuth2ResponseMode::Query) << Q("query");
        QTest::newRow("fragment") << int(OAuth2ResponseMode::Fragment) << Q("fragment");
        QTest::newRow("form_post") << int(OAuth2ResponseMode::FormPost) << Q("form_post");
    }
    void oauth2Client_getAuthorizationCodeAsync_nonDefaultResponseMode_sendsResponseModeParameter()
    {
        QFETCH(int, responseMode);
        QFETCH(QString, expectedValue);
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("68c39cbd8d"));
        OptString actualMode;
        f.server.AuthorizationEndpointInvoked = [&](const HttpRequest &request) {
            actualMode = getQueryParameters(request.url).tryGet(OAuth2Constants::AuthorizationEndpoint::ResponseModeParameter);
        };
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints, OAuth2ResponseMode(responseMode));
        QCOMPARE(client->getAuthorizationCode(ExpectedScopes, browser, {}, {}).code, Q("68c39cbd8d"));
        QCOMPARE(actualMode, OptString(expectedValue));
    }

    void oauth2Client_getAuthorizationCodeAsync_defaultResponseMode_omitsResponseModeParameter()
    {
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("68c39cbd8d"));
        bool hasMode = true;
        f.server.AuthorizationEndpointInvoked = [&](const HttpRequest &request) {
            hasMode = getQueryParameters(request.url).contains(OAuth2Constants::AuthorizationEndpoint::ResponseModeParameter);
        };
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints, OAuth2ResponseMode::Default);
        QCOMPARE(client->getAuthorizationCode(ExpectedScopes, browser, {}, {}).code, Q("68c39cbd8d"));
        QVERIFY(!hasMode);
    }

    void oauth2Client_getDeviceCodeAsync()
    {
        ServerFixture f;
        f.server.TokenGenerator.UserCodes.append(Q("254583"));
        f.server.TokenGenerator.DeviceCodes.append(Q("6d1e34151aff4f41b9f186e177a0b15d"));
        auto client = createClient(f.handler, f.endpoints);
        const OAuth2DeviceCodeResult result = client->getDeviceCode(ExpectedScopes, {});
        QCOMPARE(result.userCode, Q("254583"));
        QCOMPARE(result.deviceCode, Q("6d1e34151aff4f41b9f186e177a0b15d"));
    }

    void oauth2Client_getTokenByAuthorizationCodeAsync()
    {
        ServerFixture f;
        f.app->AuthGrants.append({Q("a63ef59691"), ExpectedScopes, std::nullopt, std::nullopt, OAuth2PkceChallengeMethod::Plain});
        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME"));
        auto client = createClient(f.handler, f.endpoints);
        const OAuth2TokenResult result = client->getTokenByAuthorizationCode({Q("a63ef59691"), TestRedirectUri, std::nullopt}, {});
        QCOMPARE(result.scopes, ExpectedScopes);
        QCOMPARE(result.accessToken, Q("LET_ME_IN"));
        QCOMPARE(result.refreshToken, OptString(Q("REFRESH_ME")));
    }

    void oauth2Client_getTokenByRefreshTokenAsync()
    {
        ServerFixture f;
        // Setup an existing access and refresh token
        f.app->AccessTokens.set(Q("OLD_LET_ME_IN"), Q("OLD_REFRESH_ME"));
        f.app->RefreshTokens.set(Q("OLD_REFRESH_ME"), ExpectedScopes);
        f.server.TokenGenerator.AccessTokens.append(Q("NEW_LET_ME_IN"));
        f.server.TokenGenerator.RefreshTokens.append(Q("NEW_REFRESH_ME"));
        auto client = createClient(f.handler, f.endpoints);
        const OAuth2TokenResult result = client->getTokenByRefreshToken(Q("OLD_REFRESH_ME"), {});
        QCOMPARE(result.scopes, ExpectedScopes);
        QCOMPARE(result.accessToken, Q("NEW_LET_ME_IN"));
        QCOMPARE(result.refreshToken, OptString(Q("NEW_REFRESH_ME")));
    }

    void oauth2Client_getTokenByDeviceCodeAsync()
    {
        ServerFixture f;
        f.app->DeviceGrants.append({Q("342728"), Q("ad6498533bf54f4db53e49612a4acfb0"), ExpectedScopes, false});
        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME"));
        auto client = createClient(f.handler, f.endpoints);

        OAuth2DeviceCodeResult deviceCodeResult;
        deviceCodeResult.deviceCode = Q("ad6498533bf54f4db53e49612a4acfb0");
        deviceCodeResult.userCode = Q("342728");
        // Upstream polls every 5 s (the default); 1 s keeps the test short with the same sequence of events.
        deviceCodeResult.pollingIntervalSecs = 1;

        // Simulate the user taking some time to sign in with the user code (runs while the client waits to poll)
        QTimer::singleShot(500, [&] { f.server.signInDeviceWithUserCode(Q("342728")); });
        const OAuth2TokenResult result = client->getTokenByDeviceCode(deviceCodeResult, {});
        QCOMPARE(result.scopes, ExpectedScopes);
        QCOMPARE(result.accessToken, Q("LET_ME_IN"));
        QCOMPARE(result.refreshToken, OptString(Q("REFRESH_ME")));
        QCOMPARE(f.handler->requestCount(Q("POST"), f.endpoints.tokenEndpoint), 2);
    }

    void oauth2Client_e2e_interactiveWebFlowAndRefresh()
    {
        ServerFixture f;
        f.server.TokenGenerator.AuthCodes.append(Q("e78a711d11"));
        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN-1"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME-1"));
        TestOAuth2WebBrowser browser(f.handler);
        auto client = createClient(f.handler, f.endpoints);

        const OAuth2AuthorizationCodeResult authCodeResult = client->getAuthorizationCode(ExpectedScopes, browser, {}, {});
        const OAuth2TokenResult result1 = client->getTokenByAuthorizationCode(authCodeResult, {});
        QCOMPARE(result1.scopes, ExpectedScopes);
        QCOMPARE(result1.accessToken, Q("LET_ME_IN-1"));
        QCOMPARE(result1.refreshToken, OptString(Q("REFRESH_ME-1")));

        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN-2"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME-2"));
        const OAuth2TokenResult result2 = client->getTokenByRefreshToken(*result1.refreshToken, {});
        QCOMPARE(result2.scopes, ExpectedScopes);
        QCOMPARE(result2.accessToken, Q("LET_ME_IN-2"));
        QCOMPARE(result2.refreshToken, OptString(Q("REFRESH_ME-2")));
    }

    void oauth2Client_e2e_deviceFlowAndRefresh()
    {
        ServerFixture f;
        f.server.TokenGenerator.UserCodes.append(Q("736998"));
        f.server.TokenGenerator.DeviceCodes.append(Q("db6558b2a1d649758394ac3c2d9e00b1"));
        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN-1"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME-1"));
        auto client = createClient(f.handler, f.endpoints);

        const OAuth2DeviceCodeResult deviceResult = client->getDeviceCode(ExpectedScopes, {});
        f.server.signInDeviceWithUserCode(deviceResult.userCode);
        const OAuth2TokenResult result1 = client->getTokenByDeviceCode(deviceResult, {});
        QCOMPARE(result1.scopes, ExpectedScopes);
        QCOMPARE(result1.accessToken, Q("LET_ME_IN-1"));
        QCOMPARE(result1.refreshToken, OptString(Q("REFRESH_ME-1")));

        f.server.TokenGenerator.AccessTokens.append(Q("LET_ME_IN-2"));
        f.server.TokenGenerator.RefreshTokens.append(Q("REFRESH_ME-2"));
        const OAuth2TokenResult result2 = client->getTokenByRefreshToken(*result1.refreshToken, {});
        QCOMPARE(result2.scopes, ExpectedScopes);
        QCOMPARE(result2.accessToken, Q("LET_ME_IN-2"));
        QCOMPARE(result2.refreshToken, OptString(Q("REFRESH_ME-2")));
    }

    // Own tests
    void oauth2Client_errorResponse_throwsOAuth2ExceptionWithMessage()
    {
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const OAuth2ServerEndpoints endpoints = createEndpoints();
        HttpResponse error = TestHttpMessageHandler::createResponse(
            400, "{\"error\":\"invalid_grant\",\"error_description\":\"Bad token\",\"error_uri\":\"https://example.com/help\"}");
        handler->setup(Q("POST"), endpoints.tokenEndpoint, error);
        auto client = createClient(handler, endpoints);
        try {
            client->getTokenByRefreshToken(Q("x"), {});
            QFAIL("no exception");
        } catch (const OAuth2Exception &ex) {
            QCOMPARE(ex.message(), Q("invalid_grant: Bad token [https://example.com/help]"));
        }

        handler->setup(Q("POST"), endpoints.tokenEndpoint, TestHttpMessageHandler::createResponse(500, "oops"));
        try {
            client->getTokenByRefreshToken(Q("x"), {});
            QFAIL("no exception");
        } catch (const OAuth2Exception &ex) {
            QCOMPARE(ex.message(), Q("Unknown OAuth error: oops"));
        }
    }

    void oauth2Client_clientSecret_sentAsBasicAuthOrInBody()
    {
        auto handler = std::make_shared<TestHttpMessageHandler>();
        const OAuth2ServerEndpoints endpoints = createEndpoints();
        handler->setup(Q("POST"), endpoints.tokenEndpoint,
                       TestHttpMessageHandler::createResponse(200, "{\"access_token\":\"a\",\"token_type\":\"Bearer\"}"));

        OAuth2Client withHeader(HttpClient(handler), endpoints, TestClientId, std::nullopt, TestClientSecret, true);
        withHeader.getTokenByRefreshToken(Q("r"), {});
        const HttpRequest first = handler->Requests.last();
        QVERIFY(first.headers.value(Q("Authorization"))->startsWith(Q("Basic ")));
        QVERIFY(!parseQueryString(QString::fromUtf8(first.content)).contains(OAuth2Constants::ClientSecretParameter));
        QCOMPARE(first.headers.value(Q("Accept")), OptString(Constants::Http::MimeTypeJson));

        OAuth2Client inBody(HttpClient(handler), endpoints, TestClientId, std::nullopt, TestClientSecret, false);
        inBody.getTokenByRefreshToken(Q("r"), {});
        const HttpRequest second = handler->Requests.last();
        QVERIFY(!second.headers.contains(Q("Authorization")));
        QCOMPARE(parseQueryString(QString::fromUtf8(second.content)).value(OAuth2Constants::ClientSecretParameter), TestClientSecret);
    }

    void oauth2Client_getTokenByDeviceCode_cancelled_throws()
    {
        ServerFixture f;
        f.app->DeviceGrants.append({Q("1"), Q("2"), ExpectedScopes, false});
        auto client = createClient(f.handler, f.endpoints);
        OAuth2DeviceCodeResult dcr;
        dcr.deviceCode = Q("2");
        dcr.userCode = Q("1");
        dcr.pollingIntervalSecs = 60;
        CancellationToken ct;
        QTimer::singleShot(100, [ct] { ct.cancel(); });
        QVERIFY_THROWS_EXCEPTION(OperationCanceledException, client->getTokenByDeviceCode(dcr, ct));
    }
};

// =============================================================================================

class OAuth2CryptographicCodeGeneratorTests : public QObject
{
    Q_OBJECT
    const QString ValidBase64UrlCharsNoPad = Q("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");

private Q_SLOTS:
    void oauth2CryptographicCodeGenerator_createNonce_isUnique()
    {
        OAuth2CryptographicCodeGenerator generator;
        QSet<QString> nonces;
        for (int i = 0; i < 32; ++i) {
            nonces.insert(generator.createNonce());
        }
        QCOMPARE(nonces.size(), 32);
    }

    void oauth2CryptographicCodeGenerator_createPkceCodeVerifier_isUniqueBase64UrlStringWithoutPaddingAndLengthBetween43And128()
    {
        OAuth2CryptographicCodeGenerator generator;
        QSet<QString> verifiers;
        for (int i = 0; i < 32; ++i) {
            const QString v = generator.createPkceCodeVerifier();
            for (QChar c : v) {
                QVERIFY(ValidBase64UrlCharsNoPad.contains(c));
            }
            QVERIFY(v.size() >= 43 && v.size() <= 128);
            verifiers.insert(v);
        }
        QCOMPARE(verifiers.size(), 32);
    }

    void oauth2CryptographicCodeGenerator_createPkceCodeChallenge_plain_returnsVerifierUnchanged()
    {
        OAuth2CryptographicCodeGenerator generator;
        const QString verifier = generator.createPkceCodeVerifier();
        QCOMPARE(generator.createPkceCodeChallenge(OAuth2PkceChallengeMethod::Plain, verifier), verifier);
    }

    void oauth2CryptographicCodeGenerator_createPkceCodeChallenge_sha256_returnsBase64UrlEncodedSha256HashOfAsciiVerifier()
    {
        OAuth2CryptographicCodeGenerator generator;
        const QString verifier = generator.createPkceCodeVerifier();
        const QString expected = QString::fromLatin1(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                                                         .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        QCOMPARE(generator.createPkceCodeChallenge(OAuth2PkceChallengeMethod::Sha256, verifier), expected);
    }
};

// =============================================================================================

class OAuth2ResponseModeTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void oauth2ResponseMode_getParameterValue()
    {
        QCOMPARE(OAuth2ResponseModes::getParameterValue(OAuth2ResponseMode::Default), OptString());
        QCOMPARE(OAuth2ResponseModes::getParameterValue(OAuth2ResponseMode::Query), OptString(Q("query")));
        QCOMPARE(OAuth2ResponseModes::getParameterValue(OAuth2ResponseMode::Fragment), OptString(Q("fragment")));
        QCOMPARE(OAuth2ResponseModes::getParameterValue(OAuth2ResponseMode::FormPost), OptString(Q("form_post")));
    }

    void oauth2ResponseMode_tryParse_valid_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<int>("expected");
        QTest::newRow("query") << Q("query") << int(OAuth2ResponseMode::Query);
        QTest::newRow("Query") << Q("Query") << int(OAuth2ResponseMode::Query);
        QTest::newRow("fragment") << Q("fragment") << int(OAuth2ResponseMode::Fragment);
        QTest::newRow("FRAGMENT") << Q("FRAGMENT") << int(OAuth2ResponseMode::Fragment);
        QTest::newRow("form_post") << Q("form_post") << int(OAuth2ResponseMode::FormPost);
        QTest::newRow("FORM_POST") << Q("FORM_POST") << int(OAuth2ResponseMode::FormPost);
        QTest::newRow("formpost") << Q("formpost") << int(OAuth2ResponseMode::FormPost);
    }
    void oauth2ResponseMode_tryParse_valid()
    {
        QFETCH(QString, value);
        QFETCH(int, expected);
        OAuth2ResponseMode actual;
        QVERIFY(OAuth2ResponseModes::tryParse(value, actual));
        QCOMPARE(int(actual), expected);
    }

    void oauth2ResponseMode_tryParse_invalid_returnsFalse_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("null") << QString();
        QTest::newRow("empty") << Q("");
        QTest::newRow("spaces") << Q("   ");
        QTest::newRow("web_message") << Q("web_message");
        QTest::newRow("unknown") << Q("unknown");
    }
    void oauth2ResponseMode_tryParse_invalid_returnsFalse()
    {
        QFETCH(QString, value);
        OAuth2ResponseMode actual;
        QVERIFY(!OAuth2ResponseModes::tryParse(value.isNull() ? OptString() : OptString(value), actual));
    }
};

// =============================================================================================

class OAuth2SystemWebBrowserTests : public QObject
{
    Q_OBJECT

    /// GET (or POST) `url` with a real QNetworkAccessManager from inside the browser's wait loop.
    static void requestLater(QNetworkAccessManager &nam, const QUrl &url, QByteArray *responseBody,
                             const QByteArray &postBody = {}, int *status = nullptr)
    {
        QTimer::singleShot(0, &nam, [&nam, url, responseBody, postBody, status] {
            QNetworkRequest request(url);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            QNetworkReply *reply;
            if (postBody.isNull()) {
                reply = nam.get(request);
            } else {
                request.setHeader(QNetworkRequest::ContentTypeHeader, Q("application/x-www-form-urlencoded"));
                reply = nam.post(request, postBody);
            }
            QObject::connect(reply, &QNetworkReply::finished, reply, [reply, responseBody, status] {
                if (responseBody) {
                    *responseBody = reply->readAll();
                }
                if (status) {
                    *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                }
                reply->deleteLater();
            });
        });
    }

private Q_SLOTS:
    void oauth2SystemWebBrowser_updateRedirectUri_nonLoopback_throwsError()
    {
        TestSessionManager sm;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        QVERIFY_THROWS_EXCEPTION(ArgumentException, browser.updateRedirectUri(QUrl(Q("http://example.com"))));
    }

    void oauth2SystemWebBrowser_updateRedirectUri_specificPort_data()
    {
        QTest::addColumn<QString>("input");
        for (const char *url : {"http://localhost:1234", "http://localhost:1234/", "http://localhost:1234/oauth-callback",
                                "http://localhost:1234/oauth-callback/", "http://127.0.0.7:1234", "http://127.0.0.7:1234/",
                                "http://127.0.0.7:1234/oauth-callback", "http://127.0.0.7:1234/oauth-callback/"}) {
            QTest::newRow(url) << Q(url);
                                }
    }
    void oauth2SystemWebBrowser_updateRedirectUri_specificPort()
    {
        QFETCH(QString, input);
        TestSessionManager sm;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        QCOMPARE(browser.updateRedirectUri(QUrl(input)).toString(QUrl::FullyEncoded), input);
    }

    void oauth2SystemWebBrowser_updateRedirectUri_anyPort_data()
    {
        QTest::addColumn<QString>("input");
        for (const char *url : {"http://localhost", "http://localhost/", "http://localhost/oauth-callback",
                                "http://localhost/oauth-callback/", "http://127.0.0.7", "http://127.0.0.7/",
                                "http://127.0.0.7/oauth-callback", "http://127.0.0.7/oauth-callback/"}) {
            QTest::newRow(url) << Q(url);
                                }
    }
    void oauth2SystemWebBrowser_updateRedirectUri_anyPort()
    {
        QFETCH(QString, input);
        TestSessionManager sm;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        const QUrl inputUri(input);
        const QUrl actualUri = browser.updateRedirectUri(inputUri);
        QCOMPARE(actualUri.scheme(), inputUri.scheme());
        QCOMPARE(actualUri.host(), inputUri.host());
        // System.Uri reports "/" for an empty path
        auto path = [](const QUrl &u) { return u.path().isEmpty() ? Q("/") : u.path(); };
        QCOMPARE(path(actualUri), path(inputUri));
        QVERIFY(!isDefaultPort(actualUri));
    }

    void oauth2SystemWebBrowser_isFormUrlEncoded_data()
    {
        QTest::addColumn<QString>("contentType");
        QTest::addColumn<bool>("expected");
        QTest::newRow("1") << Q("application/x-www-form-urlencoded") << true;
        QTest::newRow("2") << Q("application/x-www-form-urlencoded; charset=utf-8") << true;
        QTest::newRow("3") << Q("application/x-www-form-urlencoded;charset=UTF-8") << true;
        QTest::newRow("4") << Q("APPLICATION/X-WWW-FORM-URLENCODED") << true;
        QTest::newRow("5") << Q("  application/x-www-form-urlencoded ; charset=utf-8 ") << true;
        QTest::newRow("6") << Q("application/json") << false;
        QTest::newRow("7") << Q("text/plain; charset=utf-8") << false;
        QTest::newRow("8") << Q("multipart/form-data; boundary=----abc") << false;
        QTest::newRow("9") << Q("") << false;
        QTest::newRow("null") << QString() << false;
    }
    void oauth2SystemWebBrowser_isFormUrlEncoded()
    {
        QFETCH(QString, contentType);
        QFETCH(bool, expected);
        QCOMPARE(OAuth2SystemWebBrowser::isFormUrlEncoded(contentType.isNull() ? OptString() : OptString(contentType)), expected);
    }

    // Own tests of the loopback listener: the "browser" is a QNetworkAccessManager request to the redirect URI.
    void oauth2SystemWebBrowser_getAuthenticationResponse_query()
    {
        TestSessionManager sm;
        QNetworkAccessManager nam;
        QByteArray body;
        QUrl opened;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        const QUrl redirect = browser.updateRedirectUri(QUrl(Q("http://127.0.0.1/oauth-callback")));
        sm.OpenBrowserFunc = [&](const QUrl &uri) {
            opened = uri;
            QUrl target = redirect;
            target.setQuery(Q("code=abc&state=s+1%21"));
            requestLater(nam, target, &body);
        };
        const StringDictionary result =
            browser.getAuthenticationResponse(QUrl(Q("https://example.com/authorize")), redirect, OAuth2ResponseMode::Query, {});
        QCOMPARE(opened, QUrl(Q("https://example.com/authorize")));
        QCOMPARE(result.value(Q("code")), Q("abc"));
        QCOMPARE(result.value(Q("state")), Q("s 1!"));
        QTRY_VERIFY(body.contains("Authentication successful"));
    }

    void oauth2SystemWebBrowser_getAuthenticationResponse_formPostAndError()
    {
        TestSessionManager sm;
        QNetworkAccessManager nam;
        QByteArray body;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        const QUrl redirect = browser.updateRedirectUri(QUrl(Q("http://127.0.0.1/cb")));
        sm.OpenBrowserFunc = [&](const QUrl &) {
            requestLater(nam, redirect, &body, "error=access_denied&error_description=No+way");
        };
        const StringDictionary result =
            browser.getAuthenticationResponse(QUrl(Q("https://example.com/a")), redirect, OAuth2ResponseMode::FormPost, {});
        QCOMPARE(result.value(Q("error")), Q("access_denied"));
        QTRY_VERIFY(body.contains("<dt>Error:</dt><dd>access_denied</dd>"));
        QVERIFY(body.contains("<dt>Description:</dt><dd>No way</dd>"));
        QVERIFY(body.contains("<dt>URL:</dt><dd>none</dd>"));
        QVERIFY(body.contains("body{font-family:sans-serif;}"));
    }

    void oauth2SystemWebBrowser_getAuthenticationResponse_fragmentFirstServesScript()
    {
        TestSessionManager sm;
        QNetworkAccessManager nam;
        QByteArray scriptPage, finalPage;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        const QUrl redirect = browser.updateRedirectUri(QUrl(Q("http://127.0.0.1/cb")));
        sm.OpenBrowserFunc = [&](const QUrl &) {
            requestLater(nam, redirect, &scriptPage); // fragment never reaches the server
            QTimer::singleShot(200, &nam, [&] { requestLater(nam, redirect, &finalPage, "code=xyz&state=1"); });
        };
        const StringDictionary result =
            browser.getAuthenticationResponse(QUrl(Q("https://example.com/a")), redirect, OAuth2ResponseMode::Fragment, {});
        QCOMPARE(result.value(Q("code")), Q("xyz"));
        QVERIFY(scriptPage.contains("gcm-fragment-form"));
    }

    void oauth2SystemWebBrowser_getAuthenticationResponse_otherPathGets404AndSuccessRedirect()
    {
        TestSessionManager sm;
        QNetworkAccessManager nam;
        int otherStatus = 0, finalStatus = 0;
        OAuth2WebBrowserOptions options;
        options.successRedirect = QUrl(Q("https://example.com/done"));
        OAuth2SystemWebBrowser browser(sm, options);
        const QUrl redirect = browser.updateRedirectUri(QUrl(Q("http://127.0.0.1/cb")));
        sm.OpenBrowserFunc = [&](const QUrl &) {
            QUrl favicon = redirect;
            favicon.setPath(Q("/favicon.ico"));
            requestLater(nam, favicon, nullptr, {}, &otherStatus);
            QTimer::singleShot(200, &nam, [&] {
                QUrl target = redirect;
                target.setQuery(Q("code=1&state=2"));
                requestLater(nam, target, nullptr, {}, &finalStatus);
            });
        };
        const StringDictionary result =
            browser.getAuthenticationResponse(QUrl(Q("https://example.com/a")), redirect, OAuth2ResponseMode::Query, {});
        QCOMPARE(result.value(Q("code")), Q("1"));
        QCOMPARE(otherStatus, 404);
        QTRY_COMPARE(finalStatus, 302);
    }

    void oauth2SystemWebBrowser_getAuthenticationResponse_cancelled_throws()
    {
        TestSessionManager sm;
        OAuth2SystemWebBrowser browser(sm, OAuth2WebBrowserOptions());
        const QUrl redirect = browser.updateRedirectUri(QUrl(Q("http://127.0.0.1/cb")));
        CancellationToken ct;
        QTimer::singleShot(100, [ct] { ct.cancel(); });
        QVERIFY_THROWS_EXCEPTION(OperationCanceledException,
                                 browser.getAuthenticationResponse(QUrl(Q("https://example.com/a")), redirect,
                                                                   OAuth2ResponseMode::Query, ct));
    }
};

// =============================================================================================

class TokenEndpointResponseJsonTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void tokenEndpointResponseJson_deserialize_uses_scope()
    {
        const QString json = Q("{\"access_token\": \"123\", \"token_type\": \"Bearer\", \"expires_in\": 1000, "
                               "\"scopes\": \"x,y,z\", \"scope\": \"a,b,c\"}");
        OAuth2TokenResult result;
        QVERIFY(OAuth2Json::tryParseTokenResponse(json, result));
        QCOMPARE(result.accessToken, Q("123"));
        QCOMPARE(result.tokenType, Q("Bearer"));
        QCOMPARE(result.expiresInSecs, std::optional<int>(1000));
        QCOMPARE(result.scopes, QStringList{Q("a,b,c")});
    }

    // Own tests
    void tokenEndpointResponseJson_requiredAndTypes()
    {
        OAuth2TokenResult result;
        QVERIFY(!OAuth2Json::tryParseTokenResponse(Q("{\"token_type\":\"Bearer\"}"), result));
        QVERIFY(!OAuth2Json::tryParseTokenResponse(Q("{\"access_token\":\"a\",\"token_type\":\"b\",\"expires_in\":\"10\"}"), result));
        QVERIFY(!OAuth2Json::tryParseTokenResponse(Q("not json"), result));
        QVERIFY(OAuth2Json::tryParseTokenResponse(Q("{\"ACCESS_TOKEN\":\"a\",\"Token_Type\":\"b\"}"), result));
        QCOMPARE(result.accessToken, Q("a"));
        QVERIFY(result.scopes.isEmpty());
    }

    void deviceCodeResponseJson_parses()
    {
        OAuth2DeviceCodeResult result;
        QVERIFY(OAuth2Json::tryParseDeviceCodeResponse(
            Q("{\"device_code\":\"d\",\"user_code\":\"u\",\"verification_uri\":\"https://example.com/device\",\"interval\":7}"), result));
        QCOMPARE(result.deviceCode, Q("d"));
        QCOMPARE(result.userCode, Q("u"));
        QCOMPARE(result.verificationUri, QUrl(Q("https://example.com/device")));
        QCOMPARE(result.pollingIntervalSecs, 7);
        QVERIFY(OAuth2Json::tryParseDeviceCodeResponse(
            Q("{\"device_code\":\"d\",\"user_code\":\"u\",\"verification_uri\":\"https://example.com/device\"}"), result));
        QCOMPARE(result.pollingIntervalSecs, 5);
        QVERIFY(!OAuth2Json::tryParseDeviceCodeResponse(Q("{\"device_code\":\"d\",\"user_code\":\"u\"}"), result));
    }
};

GCM_REGISTER_TEST(OAuth2ClientTests)
GCM_REGISTER_TEST(OAuth2CryptographicCodeGeneratorTests)
GCM_REGISTER_TEST(OAuth2ResponseModeTests)
GCM_REGISTER_TEST(OAuth2SystemWebBrowserTests)
GCM_REGISTER_TEST(TokenEndpointResponseJsonTest)
#include "oauth2tests.moc"
