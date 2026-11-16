// Ports of HttpRequestExtensionsTests, HttpClientExtensionsTests, HttpClientFactoryTests and CurlCookieTests, plus
// own tests of the real QNetworkAccessManager handler against a local server.
// Adapted: the factory tests inspect the handler options instead of a .NET WebProxy; the TLS-backend cases of
// HttpClientFactory_GetClient_ChecksCertBundleOnlyIfEnabled that need Schannel are Windows-only and not ported.
#include "core/constants.h"
#include "core/http.h"
#include "core/strings.h"
#include "infra/nulltrace.h"
#include "infra/testconsoleservice.h"
#include "infra/testfilesystem.h"
#include "infra/testhttpmessagehandler.h"
#include "infra/testmain.h"
#include "infra/testsettings.h"
#include "localhttpserver.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

/// A file system that answers fileExists with a fixed value and counts the calls (Mock<IFileSystem> upstream).
class CountingFileSystem : public TestFileSystem
{
public:
    mutable int fileExistsCalls = 0;
    bool fileExists(const QString &) const override
    {
        ++fileExistsCalls;
        return true;
    }
};
} // namespace

// =============================================================================================

class HttpRequestExtensionsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void httpRequestExtensions_addBasicAuthenticationHeader_data()
    {
        QTest::addColumn<QString>("userName");
        QTest::addColumn<QString>("password");
        QTest::addColumn<QString>("expected");
        QTest::newRow("ComplexUserPass") << Q("hello-my_name is:john.doe") << Q("this!isAP4SSW0RD: with? _lots of/ chars")
                                         << Q("aGVsbG8tbXlfbmFtZSBpczpqb2huLmRvZTp0aGlzIWlzQVA0U1NXMFJEOiB3aXRoPyBfbG90cyBvZi8gY2hhcnM=");
        QTest::newRow("EmptyUserName") << Q("") << Q("letmein123") << Q("OmxldG1laW4xMjM=");
        QTest::newRow("EmptyPassword") << Q("john.doe") << Q("") << Q("am9obi5kb2U6");
        QTest::newRow("EmptyCredential") << Q("") << Q("") << Q("Og==");
    }
    void httpRequestExtensions_addBasicAuthenticationHeader()
    {
        QFETCH(QString, userName);
        QFETCH(QString, password);
        QFETCH(QString, expected);
        HttpRequest message;
        message.setBasicAuthorization(userName, password);
        const OptString header = message.headers.value(Q("Authorization"));
        QVERIFY(header.has_value());
        QCOMPARE(*header, Constants::Http::WwwAuthenticateBasicScheme + QLatin1Char(' ') + expected);
    }

    // Own tests
    void httpRequest_setFormContent_encodesLikeFormUrlEncodedContent()
    {
        HttpRequest request;
        request.setFormContent({{Q("a b"), Q("c&d=e")}, {Q("scope"), Q("read write")}});
        QCOMPARE(request.content, QByteArray("a+b=c%26d%3De&scope=read+write"));
        QCOMPARE(request.contentType, OptString(Constants::Http::MimeTypeFormUrlEncoded));
    }

    void httpResponse_ensureSuccessStatusCode()
    {
        HttpResponse ok = TestHttpMessageHandler::createResponse(204);
        ok.ensureSuccessStatusCode();
        HttpResponse notFound = TestHttpMessageHandler::createResponse(404);
        try {
            notFound.ensureSuccessStatusCode();
            QFAIL("no exception");
        } catch (const HttpRequestException &ex) {
            QCOMPARE(ex.message(), Q("Response status code does not indicate success: 404 (Not Found)."));
        }
    }
};

// =============================================================================================

class HttpClientExtensionsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void httpClientExtensions_sendAsync_sendsRequestMessage()
    {
        const QUrl uri(Q("http://example.com"));
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->ThrowOnUnexpectedRequest = true;
        bool called = false;
        httpHandler->setup(Q("GET"), uri, [&](const HttpRequest &request) {
            called = true;
            [&] {
                QCOMPARE(request.method, Q("GET"));
                QCOMPARE(request.url, uri);
            }();
            return TestHttpMessageHandler::createResponse(200);
        });
        HttpClient(httpHandler).send(Q("GET"), uri, MultiDictionary());
        QVERIFY(called);
    }

    void httpClientExtensions_sendAsync_content_setsContent()
    {
        const QUrl uri(Q("http://example.com"));
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->ThrowOnUnexpectedRequest = true;
        QByteArray actualContent;
        httpHandler->setup(Q("GET"), uri, [&](const HttpRequest &request) {
            actualContent = request.content;
            return TestHttpMessageHandler::createResponse(200);
        });
        HttpClient(httpHandler).send(Q("GET"), uri, MultiDictionary(), "foobar", Q("text/plain"));
        QCOMPARE(actualContent, QByteArray("foobar"));
    }

    void httpClientExtensions_sendAsync_headers_setsHeaders()
    {
        const QUrl uri(Q("http://example.com"));
        const MultiDictionary customHeaders{
            {Q("header0"), {}},
            {Q("header1"), {Q("first-value")}},
            {Q("header2"), {Q("first-value"), Q("second-value")}},
            {Q("header3"), {Q("first-value"), Q("second-value"), Q("third-value")}},
        };
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->ThrowOnUnexpectedRequest = true;
        HttpHeaders actual;
        httpHandler->setup(Q("GET"), uri, [&](const HttpRequest &request) {
            actual = request.headers;
            return TestHttpMessageHandler::createResponse(200);
        });
        HttpClient(httpHandler).send(Q("GET"), uri, customHeaders);
        QVERIFY(!actual.contains(Q("header0")));
        for (const char *name : {"header1", "header2", "header3"}) {
            QCOMPARE(actual.values(Q(name)), customHeaders.value(Q(name)));
        }
    }

    // Own test: default headers are added unless the request carries the header itself.
    void httpClient_defaultRequestHeaders_addedUnlessPresent()
    {
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        HttpClient client(httpHandler);
        client.defaultRequestHeaders().set(Q("User-Agent"), Q("default-agent"));
        client.defaultRequestHeaders().set(Q("Cache-Control"), Q("no-cache"));
        HttpRequest request;
        request.url = QUrl(Q("http://example.com"));
        request.headers.set(Q("user-agent"), Q("mine"));
        client.send(request);
        QCOMPARE(httpHandler->Requests.size(), 1);
        QCOMPARE(httpHandler->Requests[0].headers.values(Q("User-Agent")), QStringList{Q("mine")});
        QCOMPARE(httpHandler->Requests[0].headers.values(Q("Cache-Control")), QStringList{Q("no-cache")});
    }
};

// =============================================================================================

class HttpClientFactoryTests : public QObject
{
    Q_OBJECT

    static std::optional<HttpProxySettings> createProxy(TestSettings &settings, bool &result)
    {
        TestFileSystem fs;
        NullTrace trace;
        TestConsoleService console;
        HttpClientFactory factory(fs, trace, settings, console);
        HttpProxySettings proxy;
        result = factory.tryCreateProxy(proxy);
        return result ? std::optional<HttpProxySettings>(proxy) : std::nullopt;
    }

    static TestSettings proxySettings(const OptString &userName, const OptString &password, const OptString &noProxy = std::nullopt)
    {
        TestSettings settings;
        settings.RemoteUri = QUrl(Q("https://remote.example.com/foo.git"));
        ProxyConfiguration config;
        config.address = QUrl(Q("https://proxy.example.com/git"));
        config.userName = userName;
        config.password = password;
        config.noProxyRaw = noProxy;
        settings.ProxyConfiguration = config;
        return settings;
    }

private Q_SLOTS:
    void httpClientFactory_getClient_setsDefaultHeaders()
    {
        TestFileSystem fs;
        NullTrace trace;
        TestSettings settings;
        TestConsoleService console;
        HttpClientFactory factory(fs, trace, settings, console);
        HttpClient client = factory.createClient();
        QCOMPARE(client.defaultRequestHeaders().value(Q("User-Agent")), OptString(Constants::getHttpUserAgent()));
        QCOMPARE(client.defaultRequestHeaders().value(Q("Cache-Control")), OptString(Q("no-cache")));
        QVERIFY(dynamic_cast<QtHttpMessageHandler *>(client.handler().get()));
    }

    void httpClientFactory_getClient_multipleCalls_returnsNewInstance()
    {
        TestFileSystem fs;
        NullTrace trace;
        TestSettings settings;
        TestConsoleService console;
        HttpClientFactory factory(fs, trace, settings, console);
        QVERIFY(factory.createClient().handler() != factory.createClient().handler());
    }

    void httpClientFactory_tryCreateProxy_noProxy_returnsFalseOutNull()
    {
        TestSettings settings;
        settings.RemoteUri = QUrl(Q("https://remote.example.com/foo.git"));
        bool result = true;
        QVERIFY(!createProxy(settings, result).has_value());
        QVERIFY(!result);
    }

    void httpClientFactory_tryCreateProxy_proxyNoCredentials_returnsTrueOutProxyWithUrlDefaultCredentials()
    {
        TestSettings settings = proxySettings(std::nullopt, std::nullopt);
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QCOMPARE(absoluteUri(proxy->address), Q("https://proxy.example.com/git"));
        QVERIFY(proxy->useDefaultCredentials);
        QVERIFY(!proxy->userName && !proxy->password);
    }

    void httpClientFactory_tryCreateProxy_proxyWithBypass_returnsTrueOutProxyWithBypassedHosts()
    {
        TestSettings settings = proxySettings(std::nullopt, std::nullopt, Q("contoso.com,fabrikam.com"));
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QCOMPARE(absoluteUri(proxy->address), Q("https://proxy.example.com/git"));
        QVERIFY(proxy->isBypassed(QUrl(Q("https://contoso.com"))));
        QVERIFY(proxy->isBypassed(QUrl(Q("http://fabrikam.com"))));
        QVERIFY(proxy->isBypassed(QUrl(Q("https://subdomain.fabrikam.com"))));
        QVERIFY(!proxy->isBypassed(*settings.RemoteUri));
    }

    void httpClientFactory_tryCreateProxy_proxyWithWildcardBypass_returnsFalse()
    {
        TestSettings settings = proxySettings(std::nullopt, std::nullopt, Q("*"));
        bool result = true;
        QVERIFY(!createProxy(settings, result).has_value());
        QVERIFY(!result);
    }

    void httpClientFactory_tryCreateProxy_proxyWithCredentials_returnsTrueOutProxyWithUrlConfiguredCredentials()
    {
        TestSettings settings = proxySettings(Q("john.doe"), Q("letmein"));
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QCOMPARE(absoluteUri(proxy->address), Q("https://proxy.example.com/git"));
        QVERIFY(!proxy->useDefaultCredentials);
        QCOMPARE(proxy->userName, OptString(Q("john.doe")));
        QCOMPARE(proxy->password, OptString(Q("letmein")));
    }

    void httpClientFactory_tryCreateProxy_proxyWithNonEmptyUserAndEmptyPass_returnsTrueOutProxyWithUrlConfiguredCredentials()
    {
        TestSettings settings = proxySettings(Q("john.doe"), std::nullopt);
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QVERIFY(!proxy->useDefaultCredentials);
        QCOMPARE(proxy->userName, OptString(Q("john.doe")));
        QVERIFY(isNullOrWhiteSpace(proxy->password));
    }

    void httpClientFactory_tryCreateProxy_proxyWithEmptyUserAndNonEmptyPass_returnsTrueOutProxyWithUrlConfiguredCredentials()
    {
        TestSettings settings = proxySettings(std::nullopt, Q("letmein"));
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QVERIFY(!proxy->useDefaultCredentials);
        QVERIFY(isNullOrWhiteSpace(proxy->userName));
        QCOMPARE(proxy->password, OptString(Q("letmein")));
    }

    void httpClientFactory_tryCreateProxy_proxyEmptyUserAndEmptyPass_returnsTrueOutProxyWithUrlDefaultCredentials()
    {
        TestSettings settings = proxySettings(Q(""), Q(""));
        bool result = false;
        const auto proxy = createProxy(settings, result);
        QVERIFY(result);
        QCOMPARE(absoluteUri(proxy->address), Q("https://proxy.example.com/git"));
        QVERIFY(proxy->useDefaultCredentials);
    }

    void httpClientFactory_getClient_checksCertBundleOnlyIfEnabled_data()
    {
        QTest::addColumn<QString>("customCertBundle");
        QTest::addColumn<bool>("expectBundleChecked");
        QTest::newRow("unset") << QString() << false;
        QTest::newRow("set") << Q("ca-bundle.crt") << true;
    }
    void httpClientFactory_getClient_checksCertBundleOnlyIfEnabled()
    {
        QFETCH(QString, customCertBundle);
        QFETCH(bool, expectBundleChecked);
        CountingFileSystem fs;
        NullTrace trace;
        TestConsoleService console;
        TestSettings settings;
        if (!customCertBundle.isNull()) {
            settings.CustomCertificateBundlePath = customCertBundle;
        }
        HttpClientFactory factory(fs, trace, settings, console);
        HttpClient client = factory.createClient();
        QCOMPARE(fs.fileExistsCalls, expectBundleChecked ? 1 : 0);
        const auto *handler = dynamic_cast<QtHttpMessageHandler *>(client.handler().get());
        QVERIFY(handler);
        QCOMPARE(handler->options().customCaBundlePath, customCertBundle.isNull() ? OptString() : OptString(customCertBundle));
    }

    void httpClientFactory_getClient_setCookieOnlyIfEnabled_data()
    {
        QTest::addColumn<QString>("cookieFilePath");
        QTest::addColumn<bool>("expectCookieChecked");
        QTest::addColumn<QString>("cookieFileContent");
        QTest::newRow("unset") << QString() << false << QString();
        QTest::newRow("set") << Q("~/.git-cookie") << true
                             << Q("# Netscape HTTP Cookie File\n"
                                  "# https://curl.haxx.se/rfc/cookie_spec.html\n"
                                  "# This is a generated file! Do not edit.\n"
                                  "\n"
                                  ".example.com\tTRUE\t/\tTRUE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\t/\tTRUE\t0\tcookie2\tvalue2\n"
                                  "#HttpOnly_.example.com\tTRUE\t/\tTRUE\t0\tcookie3\tvalue3\n");
    }
    void httpClientFactory_getClient_setCookieOnlyIfEnabled()
    {
        QFETCH(QString, cookieFilePath);
        QFETCH(bool, expectCookieChecked);
        QFETCH(QString, cookieFileContent);
        CountingFileSystem fs;
        if (!cookieFileContent.isEmpty()) {
            fs.Files.set(cookieFilePath, cookieFileContent.toUtf8());
        }
        NullTrace trace;
        TestConsoleService console;
        TestSettings settings;
        if (!cookieFilePath.isNull()) {
            settings.CustomCookieFilePath = cookieFilePath;
        }
        HttpClientFactory factory(fs, trace, settings, console);
        HttpClient client = factory.createClient();
        QCOMPARE(fs.fileExistsCalls > 0, expectCookieChecked);
        const auto *handler = dynamic_cast<QtHttpMessageHandler *>(client.handler().get());
        QCOMPARE(handler->options().cookies.size(), expectCookieChecked ? 3 : 0);
    }

    // Own tests
    void httpClientFactory_getClient_certificateVerificationDisabled_warns()
    {
        TestFileSystem fs;
        NullTrace trace;
        TestConsoleService console;
        TestSettings settings;
        settings.IsCertificateVerificationEnabled = false;
        settings.CustomCertificateBundlePath = Q("/does/not/matter.crt");
        HttpClientFactory factory(fs, trace, settings, console);
        HttpClient client = factory.createClient();
        const auto *handler = dynamic_cast<QtHttpMessageHandler *>(client.handler().get());
        QVERIFY(!handler->options().verifyCertificates);
        QVERIFY(!handler->options().customCaBundlePath);
        QCOMPARE(console.WrittenMessages.size(), 4);
        QCOMPARE(console.WrittenMessages[1], Q("| TLS certificate verification has been disabled! |"));
    }

    void httpClientFactory_getClient_missingCertBundle_throws()
    {
        TestFileSystem fs;
        NullTrace trace;
        TestConsoleService console;
        TestSettings settings;
        settings.CustomCertificateBundlePath = Q("/missing/ca.crt");
        HttpClientFactory factory(fs, trace, settings, console);
        try {
            factory.createClient();
            QFAIL("no exception");
        } catch (const IOException &ex) {
            QCOMPARE(ex.message(), Q("Custom certificate bundle not found at path: /missing/ca.crt"));
        }
    }

    void httpClientFactory_tryCreateProxy_deprecatedSource_warns()
    {
        TestSettings settings = proxySettings(std::nullopt, std::nullopt);
        settings.ProxyConfiguration->isDeprecatedSource = true;
        TestFileSystem fs;
        NullTrace trace;
        TestConsoleService console;
        HttpClientFactory factory(fs, trace, settings, console);
        HttpProxySettings proxy;
        QVERIFY(factory.tryCreateProxy(proxy));
        QCOMPARE(console.WrittenMessages,
                 QStringList{Q("Using a deprecated proxy configuration. See https://aka.ms/gcm/httpproxy for more information.")});
    }
};

// =============================================================================================

class CurlCookieParserTests : public QObject
{
    Q_OBJECT

    static void assertCookie(const CurlCookie &cookie, const char *domain, const char *path, bool secureOnly, qint64 expires,
                             const char *name, const char *value)
    {
        QCOMPARE(cookie.name, Q(name));
        QCOMPARE(cookie.value, Q(value));
        QCOMPARE(cookie.domain, Q(domain));
        QCOMPARE(cookie.path, Q(path));
        QCOMPARE(cookie.secureOnly, secureOnly);
        QCOMPARE(cookie.expires, expires);
    }

    static QList<CurlCookie> parse(const char *content)
    {
        NullTrace trace;
        return CurlCookieParser(trace).parse(Q(content));
    }

private Q_SLOTS:
    void curlCookieParser_emptyFile_returnsNoCookies() { QVERIFY(parse("").isEmpty()); }

    void curlCookieParser_parse_missingFields_skipsInvalidLines()
    {
        const auto actual = parse(".example.com\tTRUE\t/path/here\tTRUE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\tTRUE\tcookie1\tvalue1\n");
        QCOMPARE(actual.size(), 1);
        assertCookie(actual[0], ".example.com", "/path/here", true, 0, "cookie1", "value1");
    }

    void curlCookieParser_parse_missingFields_returnsValidCookiesWithDefaults()
    {
        const auto actual = parse(".example.com\tTRUE\t\tTRUE\t852852\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\t/path/here\tTRUE\t\tcookie1\tvalue1");
        QCOMPARE(actual.size(), 2);
        assertCookie(actual[0], ".example.com", "/", true, 852852, "cookie1", "value1");
        assertCookie(actual[1], ".example.com", "/path/here", true, 0, "cookie1", "value1");
    }

    void curlCookieParser_parse_validFields_returnsValidCookies()
    {
        const auto actual = parse(".example.com\tTRUE\t/path\tTRUE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tfAlSe\t/path\ttRuE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\t/path\tTRUE\t0\tcookie1 with spaces\tvalue1 with spaces\n"
                                  ".example.com\tFALSE\t/path\tTRUE\t0\tcookie1\tvalue1\n"
                                  "example.com\tFALSE\t/path\tTRUE\t0\tcookie1\tvalue1\n"
                                  "example.com\tTRUE\t/path\tTRUE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\t/path\tFALSE\t0\tcookie1\tvalue1\n"
                                  ".example.com\tTRUE\t/path\tFALSE\t401654\tcookie1\tvalue1\n");
        QCOMPARE(actual.size(), 8);
        assertCookie(actual[0], ".example.com", "/path", true, 0, "cookie1", "value1");
        assertCookie(actual[1], "example.com", "/path", true, 0, "cookie1", "value1");
        assertCookie(actual[2], ".example.com", "/path", true, 0, "cookie1 with spaces", "value1 with spaces");
        assertCookie(actual[3], "example.com", "/path", true, 0, "cookie1", "value1");
        assertCookie(actual[4], "example.com", "/path", true, 0, "cookie1", "value1");
        assertCookie(actual[5], "example.com", "/path", true, 0, "cookie1", "value1");
        assertCookie(actual[6], ".example.com", "/path", false, 0, "cookie1", "value1");
        assertCookie(actual[7], ".example.com", "/path", false, 401654, "cookie1", "value1");
    }

    void curlCookieParser_parse_comments_returnsCookies()
    {
        const auto actual = parse("# This is a cookie file with various comments!\n"
                                  "# Lines starting with # are comments, except those that\n"
                                  "# start with exactly '#HttpOnly_'.. two #s is a comment still!\n"
                                  "##HttpOnly_ <-- this is a comment still!\n"
                                  ".example.com\tTRUE\t/\tTRUE\t0\tcookie1\tvalue1\n"
                                  "#.example.com\tTRUE\t/\tTRUE\t0\tcookie1\tvalue1\n"
                                  "#HttpOnly_.example.com\tTRUE\t/\tTRUE\t0\tcookie1\tvalue1\n");
        QCOMPARE(actual.size(), 2);
        assertCookie(actual[0], ".example.com", "/", true, 0, "cookie1", "value1");
        assertCookie(actual[1], ".example.com", "/", true, 0, "cookie1", "value1");
    }
};

// =============================================================================================
// Own tests of QtHttpMessageHandler against a local server

class QtHttpMessageHandlerTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void send_get_returnsStatusHeadersAndBody()
    {
        LocalHttpServer server([](const LocalHttpRequest &) {
            return LocalHttpServer::response(200, "OK", {"X-Test: one", "Content-Type: application/json"}, "{\"a\":1}");
        });
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.url = QUrl(server.url(Q("/path?x=1")));
        request.headers.set(Q("Accept"), Q("application/json"));
        const HttpResponse response = handler.send(request, 5000, {});
        QCOMPARE(response.statusCode, 200);
        QCOMPARE(response.reasonPhrase, Q("OK"));
        QCOMPARE(response.headers.value(Q("x-test")), OptString(Q("one")));
        QCOMPARE(response.content, QByteArray("{\"a\":1}"));
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests[0].method, Q("GET"));
        QCOMPARE(server.requests[0].target, Q("/path?x=1"));
        QCOMPARE(server.requests[0].headers.value(Q("Accept")), Q("application/json"));
    }

    void send_post_sendsBodyAndContentType()
    {
        LocalHttpServer server([](const LocalHttpRequest &) { return LocalHttpServer::response(201, "Created"); });
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.method = Q("POST");
        request.url = QUrl(server.url(Q("/token")));
        request.setFormContent({{Q("grant_type"), Q("refresh_token")}});
        const HttpResponse response = handler.send(request, 5000, {});
        QCOMPARE(response.statusCode, 201);
        QCOMPARE(server.requests[0].method, Q("POST"));
        QCOMPARE(server.requests[0].body, QByteArray("grant_type=refresh_token"));
        QCOMPARE(server.requests[0].headers.value(Q("Content-Type")), Constants::Http::MimeTypeFormUrlEncoded);
    }

    void send_unauthorized_returnsResponseWithAllChallenges()
    {
        LocalHttpServer server([](const LocalHttpRequest &) {
            return LocalHttpServer::response(401, "Unauthorized",
                                             {"WWW-Authenticate: Negotiate", "WWW-Authenticate: NTLM",
                                              "WWW-Authenticate: Basic realm=\"x, y\", charset=\"UTF-8\""});
        });
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.method = Q("HEAD");
        request.url = QUrl(server.url(Q("/")));
        const HttpResponse response = handler.send(request, 5000, {});
        QCOMPARE(response.statusCode, 401);
        QCOMPARE(response.wwwAuthenticate(),
                 (QStringList{Q("Negotiate"), Q("NTLM"), Q("Basic realm=\"x, y\", charset=\"UTF-8\"")}));
    }

    void send_redirect_isFollowed()
    {
        LocalHttpServer *self = nullptr;
        LocalHttpServer server([&self](const LocalHttpRequest &request) {
            if (request.target == QLatin1String("/old")) {
                return LocalHttpServer::response(302, "Found", {"Location: " + self->url(Q("/new")).toUtf8()});
            }
            return LocalHttpServer::response(200, "OK", {}, "moved");
        });
        self = &server;
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.url = QUrl(server.url(Q("/old")));
        const HttpResponse response = handler.send(request, 5000, {});
        QCOMPARE(response.statusCode, 200);
        QCOMPARE(response.content, QByteArray("moved"));
        QCOMPARE(response.requestUri, QUrl(server.url(Q("/new"))));
    }

    void send_connectionRefused_throwsHttpRequestException()
    {
        quint16 port = 0;
        {
            LocalHttpServer server([](const LocalHttpRequest &) { return QByteArray(); });
            port = server.port();
        }
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.url = QUrl(Q("http://127.0.0.1:%1/").arg(port));
        QVERIFY_THROWS_EXCEPTION(HttpRequestException, handler.send(request, 5000, {}));
    }

    void send_noResponse_timesOut()
    {
        QTcpServer silent;
        silent.listen(QHostAddress::LocalHost, 0);
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.url = QUrl(Q("http://127.0.0.1:%1/").arg(silent.serverPort()));
        QElapsedTimer timer;
        timer.start();
        QVERIFY_THROWS_EXCEPTION(TimeoutException, handler.send(request, 200, {}));
        QVERIFY(timer.elapsed() < 5000);
    }

    void send_cancelled_throwsOperationCanceled()
    {
        QTcpServer silent;
        silent.listen(QHostAddress::LocalHost, 0);
        QtHttpMessageHandler handler;
        HttpRequest request;
        request.url = QUrl(Q("http://127.0.0.1:%1/").arg(silent.serverPort()));
        CancellationToken ct;
        QTimer::singleShot(100, [ct] { ct.cancel(); });
        QVERIFY_THROWS_EXCEPTION(OperationCanceledException, handler.send(request, 0, ct));
    }

    void send_proxy_isUsedUnlessBypassed()
    {
        // The "proxy" answers every request itself; a proxied request carries the absolute URI as its target.
        LocalHttpServer proxyServer([](const LocalHttpRequest &) { return LocalHttpServer::response(200, "OK", {}, "via proxy"); });
        HttpHandlerOptions options;
        HttpProxySettings proxy;
        proxy.address = QUrl(proxyServer.url(Q("/")));
        proxy.bypassList = ProxyConfiguration::convertToBypassRegexArray(Q("bypassed.example"));
        options.proxy = proxy;
        QtHttpMessageHandler handler(options);

        HttpRequest request;
        request.url = QUrl(Q("http://remote.example/repo.git"));
        const HttpResponse response = handler.send(request, 5000, {});
        QCOMPARE(response.content, QByteArray("via proxy"));
        QCOMPARE(proxyServer.requests[0].target, Q("http://remote.example/repo.git"));
        QVERIFY(proxy.isBypassed(QUrl(Q("http://www.bypassed.example/x"))));
    }

    void send_cookies_areSentToTheirHost()
    {
        LocalHttpServer server([](const LocalHttpRequest &) { return LocalHttpServer::response(200, "OK"); });
        HttpHandlerOptions options;
        CurlCookie cookie;
        cookie.domain = Q("127.0.0.1");
        cookie.name = Q("session");
        cookie.value = Q("abc");
        options.cookies = {cookie};
        QtHttpMessageHandler handler(options);
        HttpRequest request;
        request.url = QUrl(server.url(Q("/")));
        handler.send(request, 5000, {});
        QCOMPARE(server.requests[0].headers.value(Q("Cookie")), Q("session=abc"));
    }
};

GCM_REGISTER_TEST(HttpRequestExtensionsTests)
GCM_REGISTER_TEST(HttpClientExtensionsTests)
GCM_REGISTER_TEST(HttpClientFactoryTests)
GCM_REGISTER_TEST(CurlCookieParserTests)
GCM_REGISTER_TEST(QtHttpMessageHandlerTests)
#include "httptests.moc"
