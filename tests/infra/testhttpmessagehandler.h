// Port of TestInfrastructure/Objects/TestHttpMessageHandler.cs and TestHttpClientFactory.cs
#pragma once

#include "core/http.h"

namespace gcm::test {

class TestHttpMessageHandler : public IHttpMessageHandler
{
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest &)>;

    bool ThrowOnUnexpectedRequest = false;
    bool SimulateNoNetwork = false;
    bool SimulatePrimaryUriFailure = false;

    /// Handlers match on method and URI without query and fragment.
    void setup(const QString &method, const QUrl &uri, RequestHandler handler);
    void setup(const QString &method, const QUrl &uri, const HttpResponse &response);
    void setup(const QString &method, const QUrl &uri, int statusCode, const QString &content = QString());

    /// Number of requests made for method + URI (query and fragment ignored).
    int requestCount(const QString &method, const QUrl &uri) const;
    /// Total number of requests made.
    int totalRequestCount() const;
    /// Qt Test assertions (record a failure in the running test function).
    void assertRequest(const QString &method, const QUrl &uri, int expectedNumberOfCalls) const;
    void assertNoRequests() const;

    /// Every request received, in order.
    QList<HttpRequest> Requests;

    HttpResponse send(const HttpRequest &request, int timeoutMs, const CancellationToken &ct) override;

    /// A response with the standard reason phrase for the status.
    static HttpResponse createResponse(int statusCode, const QByteArray &content = {});
    static QString reasonPhrase(int statusCode);

private:
    static QString createRequestKey(const QString &method, const QUrl &uri);
    QList<std::pair<QString, RequestHandler>> m_handlers;
    Dictionary<int> m_requestCounts;
};

class TestHttpClientFactory : public IHttpClientFactory
{
public:
    std::shared_ptr<IHttpMessageHandler> MessageHandler = std::make_shared<TestHttpMessageHandler>();

    HttpClient createClient() override { return HttpClient(MessageHandler); }
};

} // namespace gcm::test
