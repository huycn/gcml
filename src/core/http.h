// HTTP client abstraction over QtNetwork with a synchronous API (a nested event loop runs while waiting).
// Ports HttpClientFactory.cs, HttpClientExtensions.cs, HttpRequestExtensions.cs, CurlCookie.cs.
// System.Net.Http's HttpClient / HttpMessageHandler / HttpResponseMessage map to HttpClient /
// IHttpMessageHandler / HttpResponse below so tests can substitute the handler.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"

namespace gcm {

class IFileSystem;
class ITrace;
class ISettings;
class IConsoleService;

/// Case-insensitive multi-valued header collection.
class HttpHeaders
{
public:
    void add(const QString &name, const QString &value) { m_values[name].append(value); }
    void set(const QString &name, const QString &value) { m_values.set(name, {value}); }
    bool contains(const QString &name) const { return m_values.contains(name); }
    QStringList values(const QString &name) const { return m_values.value(name); }
    /// First value, or nullopt.
    OptString value(const QString &name) const
    {
        const QStringList v = m_values.value(name);
        return v.isEmpty() ? OptString() : OptString(v.first());
    }
    void remove(const QString &name) { m_values.remove(name); }
    const MultiDictionary &all() const { return m_values; }

private:
    MultiDictionary m_values{Qt::CaseInsensitive};
};

struct HttpRequest
{
    QString method = QStringLiteral("GET");
    QUrl url;
    HttpHeaders headers;
    QByteArray content;
    /// Content-Type of `content` (e.g. Constants::Http::MimeTypeJson); ignored when content is empty.
    OptString contentType;

    /// Sets `Authorization: Basic base64(user:password)`.
    void setBasicAuthorization(const QString &userName, const QString &password);
    void setBearerAuthorization(const QString &token);
    /// URL-encodes the pairs as an application/x-www-form-urlencoded body (FormUrlEncodedContent).
    void setFormContent(const StringDictionary &form);
    void setJsonContent(const QByteArray &json);
};

/// The name of the System.Net.HttpStatusCode enum value ("OK", "NotFound", "Unauthorized"), or the number for codes
/// the enum has no name for -- what upstream prints with $"{response.StatusCode}".
QString httpStatusCodeName(int statusCode);

struct HttpResponse
{
    int statusCode = 0;
    QString reasonPhrase;
    HttpHeaders headers;
    QByteArray content;
    QUrl requestUri;

    bool isSuccessStatusCode() const { return statusCode >= 200 && statusCode <= 299; }
    QString contentString() const { return QString::fromUtf8(content); }
    /// Raw `WWW-Authenticate` header values, one entry per challenge header line.
    QStringList wwwAuthenticate() const { return headers.values(QStringLiteral("WWW-Authenticate")); }
    /// Throws HttpRequestException("Response status code does not indicate success: 404 (Not Found).").
    void ensureSuccessStatusCode() const;
};

class IHttpMessageHandler
{
public:
    virtual ~IHttpMessageHandler() = default;
    /// Sends and blocks until the complete response arrived. Throws HttpRequestException on transport errors,
    /// TimeoutException after `timeoutMs` (<= 0: no timeout), OperationCanceledException when `ct` is cancelled.
    /// Redirects are followed.
    virtual HttpResponse send(const HttpRequest &request, int timeoutMs, const CancellationToken &ct) = 0;
};

class HttpClient
{
public:
    HttpClient() = default;
    explicit HttpClient(std::shared_ptr<IHttpMessageHandler> handler) : m_handler(std::move(handler)) {}

    /// Headers added to every request unless the request already carries that header.
    HttpHeaders &defaultRequestHeaders() { return m_defaultHeaders; }
    int timeoutMs() const { return m_timeoutMs; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    const std::shared_ptr<IHttpMessageHandler> &handler() const { return m_handler; }

    HttpResponse send(HttpRequest request, const CancellationToken &ct = {}) const;
    HttpResponse get(const QUrl &url, const CancellationToken &ct = {}) const;
    HttpResponse head(const QUrl &url, const CancellationToken &ct = {}) const;
    HttpResponse post(const QUrl &url, const QByteArray &content, const QString &contentType, const CancellationToken &ct = {}) const;
    HttpResponse postForm(const QUrl &url, const StringDictionary &form, const CancellationToken &ct = {}) const;
    /// HttpClientExtensions.SendAsync(method, uri, headers, content); a header with no values is not added.
    HttpResponse send(const QString &method, const QUrl &url, const MultiDictionary &headers,
                      const QByteArray &content = {}, const OptString &contentType = std::nullopt,
                      const CancellationToken &ct = {}) const;

private:
    std::shared_ptr<IHttpMessageHandler> m_handler;
    HttpHeaders m_defaultHeaders;
    int m_timeoutMs = 100000; // .NET default: 100 seconds
};

struct CurlCookie
{
    QString domain;
    bool includeSubdomains = false;
    QString path;
    bool secureOnly = false;
    qint64 expires = 0; // unix seconds, 0 = session
    QString name;
    QString value;
    /// Always true, as upstream.
    bool httpOnly = true;
};

/// Proxy for the real handler (WebProxy upstream).
struct HttpProxySettings
{
    /// Proxy address without user info (may carry a path, which Qt ignores).
    QUrl address;
    OptString userName;
    OptString password;
    /// Empty user name and password: use the system default credentials (none on Linux).
    bool useDefaultCredentials = false;
    /// Regular expressions of hosts not to proxy (ProxyConfiguration::convertToBypassRegexArray).
    QStringList bypassList;

    /// WebProxy.IsBypassed: the expressions are matched, ignoring case, against "scheme://host[:port]"
    /// (the port only when it is not the scheme default).
    bool isBypassed(const QUrl &uri) const;
};

/// What HttpClientFactory configured on the handler.
struct HttpHandlerOptions
{
    std::optional<HttpProxySettings> proxy;
    bool verifyCertificates = true;
    /// PEM bundle trusted in addition to the system CA certificates.
    OptString customCaBundlePath;
    /// Cookies from http.cookieFile, sent to their (exact) host.
    QList<CurlCookie> cookies;
};

/// The real IHttpMessageHandler: QNetworkAccessManager, run in a nested event loop until the reply finished.
/// Follows redirects (not from https to http), keeps every response header, never shows authentication prompts.
class QtHttpMessageHandler : public IHttpMessageHandler
{
public:
    explicit QtHttpMessageHandler(HttpHandlerOptions options = {});
    ~QtHttpMessageHandler() override;

    const HttpHandlerOptions &options() const;
    HttpResponse send(const HttpRequest &request, int timeoutMs, const CancellationToken &ct) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class IHttpClientFactory
{
public:
    virtual ~IHttpClientFactory() = default;
    /// A client honouring proxy, TLS verification, custom CA bundle and cookie-file settings, with the
    /// GCM User-Agent and `Cache-Control: no-cache` default headers.
    virtual HttpClient createClient() = 0;
};

class HttpClientFactory : public IHttpClientFactory
{
public:
    HttpClientFactory(IFileSystem &fileSystem, ITrace &trace, ISettings &settings, IConsoleService &console);
    ~HttpClientFactory() override;

    HttpClient createClient() override;
    /// Resolves the proxy configuration (credentials, no_proxy bypass list). Returns false when no proxy is
    /// configured or no_proxy is "*".
    bool tryCreateProxy(HttpProxySettings &proxy);

private:
    struct Private;
    std::unique_ptr<Private> d;
};


/// Parser for Netscape/cURL cookie files (http.cookieFile).
class CurlCookieParser
{
public:
    explicit CurlCookieParser(ITrace &trace) : m_trace(trace) {}

    QList<CurlCookie> parse(const QString &content);

private:
    ITrace &m_trace;
};

} // namespace gcm
