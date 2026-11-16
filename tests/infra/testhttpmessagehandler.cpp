#include "infra/testhttpmessagehandler.h"

#include "core/strings.h"

#include <QTest>

namespace gcm::test {

QString TestHttpMessageHandler::createRequestKey(const QString &method, const QUrl &uri)
{
    // Trim the query and fragment
    const QUrl normalized = uri.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment);
    return method.toUpper() + QLatin1Char(' ') + absoluteUri(normalized);
}

void TestHttpMessageHandler::setup(const QString &method, const QUrl &uri, RequestHandler handler)
{
    const QString key = createRequestKey(method, uri);
    for (auto &entry : m_handlers) {
        if (entry.first == key) {
            entry.second = std::move(handler);
            return;
        }
    }
    m_handlers.append({key, std::move(handler)});
}

void TestHttpMessageHandler::setup(const QString &method, const QUrl &uri, const HttpResponse &response)
{
    setup(method, uri, [response](const HttpRequest &) { return response; });
}

void TestHttpMessageHandler::setup(const QString &method, const QUrl &uri, int statusCode, const QString &content)
{
    setup(method, uri, createResponse(statusCode, content.toUtf8()));
}

int TestHttpMessageHandler::requestCount(const QString &method, const QUrl &uri) const
{
    return m_requestCounts.value(createRequestKey(method, uri), 0);
}

int TestHttpMessageHandler::totalRequestCount() const
{
    int total = 0;
    for (const auto &[key, count] : m_requestCounts) {
        total += count;
    }
    return total;
}

void TestHttpMessageHandler::assertRequest(const QString &method, const QUrl &uri, int expectedNumberOfCalls) const
{
    const int actual = requestCount(method, uri);
    const QByteArray what = createRequestKey(method, uri).toUtf8();
    QTest::qCompare(actual, expectedNumberOfCalls, what.constData(), "expectedNumberOfCalls", __FILE__, __LINE__);
}

void TestHttpMessageHandler::assertNoRequests() const
{
    QTest::qVerify(m_requestCounts.isEmpty(), "no HTTP requests", "", __FILE__, __LINE__);
}

HttpResponse TestHttpMessageHandler::send(const HttpRequest &request, int, const CancellationToken &ct)
{
    ct.throwIfCancellationRequested();

    const QString requestKey = createRequestKey(request.method, request.url);
    m_requestCounts[requestKey]++;
    Requests.append(request);

    if (SimulateNoNetwork) {
        throw HttpRequestException(QStringLiteral("Simulated no network"));
    }

    if (SimulatePrimaryUriFailure && absoluteUri(request.url) == QLatin1String("http://example.com/")) {
        throw HttpRequestException(QStringLiteral("Simulated http failure."));
    }

    for (const auto &[key, handler] : std::as_const(m_handlers)) {
        if (key == requestKey) {
            HttpResponse response = handler(request);
            if (response.requestUri.isEmpty()) {
                response.requestUri = request.url;
            }
            return response;
        }
    }

    if (ThrowOnUnexpectedRequest) {
        throw Exception(QStringLiteral("No handler configured for the request '%1 %2'")
                            .arg(request.method, absoluteUri(request.url)));
    }

    HttpResponse response = createResponse(200);
    response.requestUri = request.url;
    return response;
}

HttpResponse TestHttpMessageHandler::createResponse(int statusCode, const QByteArray &content)
{
    HttpResponse response;
    response.statusCode = statusCode;
    response.reasonPhrase = reasonPhrase(statusCode);
    response.content = content;
    return response;
}

QString TestHttpMessageHandler::reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 201: return QStringLiteral("Created");
    case 202: return QStringLiteral("Accepted");
    case 204: return QStringLiteral("No Content");
    case 301: return QStringLiteral("Moved Permanently");
    case 302: return QStringLiteral("Found");
    case 304: return QStringLiteral("Not Modified");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 409: return QStringLiteral("Conflict");
    case 429: return QStringLiteral("Too Many Requests");
    case 500: return QStringLiteral("Internal Server Error");
    case 502: return QStringLiteral("Bad Gateway");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QString();
    }
}

} // namespace gcm::test
