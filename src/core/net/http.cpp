// Port of HttpRequestExtensions.cs, HttpClientExtensions.cs, HttpClientFactory.cs and CurlCookie.cs, plus the parts
// of System.Net.Http (HttpClient defaults, EnsureSuccessStatusCode, FormUrlEncodedContent) the code relies on.
#include "core/http.h"

#include "core/constants.h"
#include "core/console.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/trace.h"
#include "core/trace2.h"

#include <QRegularExpression>

namespace gcm {

// =============================================================================================
// HttpRequest / HttpResponse
// =============================================================================================

void HttpRequest::setBasicAuthorization(const QString &userName, const QString &password)
{
    const QByteArray base64 = (userName + QLatin1Char(':') + password).toUtf8().toBase64();
    headers.set(QStringLiteral("Authorization"), Constants::Http::WwwAuthenticateBasicScheme + QLatin1Char(' ') + QString::fromLatin1(base64));
}

void HttpRequest::setBearerAuthorization(const QString &token)
{
    headers.set(QStringLiteral("Authorization"), Constants::Http::WwwAuthenticateBearerScheme + QLatin1Char(' ') + token);
}

void HttpRequest::setFormContent(const StringDictionary &form)
{
    // FormUrlEncodedContent: Uri.EscapeDataString with spaces as '+'
    auto encode = [](const QString &s) { return escapeDataString(s).replace(QLatin1String("%20"), QLatin1String("+")); };
    QStringList pairs;
    for (const auto &[key, value] : form) {
        pairs.append(encode(key) + QLatin1Char('=') + encode(value));
    }
    content = pairs.join(QLatin1Char('&')).toUtf8();
    contentType = Constants::Http::MimeTypeFormUrlEncoded;
}

void HttpRequest::setJsonContent(const QByteArray &json)
{
    content = json;
    contentType = Constants::Http::MimeTypeJson + QStringLiteral("; charset=utf-8");
}

void HttpResponse::ensureSuccessStatusCode() const
{
    if (!isSuccessStatusCode()) {
        throw HttpRequestException(QStringLiteral("Response status code does not indicate success: %1 (%2).").arg(QString::number(statusCode), reasonPhrase));
    }
}

// =============================================================================================
// HttpClient
// =============================================================================================

HttpResponse HttpClient::send(HttpRequest request, const CancellationToken &ct) const
{
    for (const auto &[name, values] : m_defaultHeaders.all()) {
        if (!request.headers.contains(name)) {
            for (const QString &value : values) {
                request.headers.add(name, value);
            }
        }
    }

    if (m_handler) {
        return m_handler->send(request, m_timeoutMs, ct);
    }
    QtHttpMessageHandler handler;
    return handler.send(request, m_timeoutMs, ct);
}

HttpResponse HttpClient::get(const QUrl &url, const CancellationToken &ct) const
{
    HttpRequest request;
    request.method = Constants::Http::MethodGet;
    request.url = url;
    return send(std::move(request), ct);
}

HttpResponse HttpClient::head(const QUrl &url, const CancellationToken &ct) const
{
    HttpRequest request;
    request.method = QStringLiteral("HEAD");
    request.url = url;
    return send(std::move(request), ct);
}

HttpResponse HttpClient::post(const QUrl &url, const QByteArray &content, const QString &contentType,
                              const CancellationToken &ct) const
{
    HttpRequest request;
    request.method = Constants::Http::MethodPost;
    request.url = url;
    request.content = content;
    request.contentType = contentType;
    return send(std::move(request), ct);
}

HttpResponse HttpClient::postForm(const QUrl &url, const StringDictionary &form, const CancellationToken &ct) const
{
    HttpRequest request;
    request.method = Constants::Http::MethodPost;
    request.url = url;
    request.setFormContent(form);
    return send(std::move(request), ct);
}

HttpResponse HttpClient::send(const QString &method, const QUrl &url, const MultiDictionary &headers, const QByteArray &content, const OptString &contentType, const CancellationToken &ct) const
{
    HttpRequest request;
    request.method = method;
    request.url = url;
    request.content = content;
    request.contentType = contentType;
    for (const auto &[name, values] : headers) {
        for (const QString &value : values) {
            request.headers.add(name, value);
        }
    }
    return send(std::move(request), ct);
}

// =============================================================================================
// HttpProxySettings
// =============================================================================================

bool HttpProxySettings::isBypassed(const QUrl &uri) const
{
    QString input = uri.scheme() + QStringLiteral("://") + uri.host();
    if (!isDefaultPort(uri)) {
        input += QLatin1Char(':') + QString::number(uri.port());
    }
    for (const QString &pattern : bypassList) {
        if (QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(input).hasMatch()) {
            return true;
        }
    }
    return false;
}

// =============================================================================================
// CurlCookieParser
// =============================================================================================

QList<CurlCookie> CurlCookieParser::parse(const QString &content)
{
    QList<CurlCookie> cookies;
    if (isNullOrWhiteSpace(content)) {
        return cookies;
    }

    const QString httpOnlyPrefix = QStringLiteral("#HttpOnly_");
    static const QRegularExpression newlines(QStringLiteral("[\\r\\n]"));
    const QStringList lines = content.split(newlines, Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() >= 7 && (!parts[0].startsWith(QLatin1Char('#')) || parts[0].startsWith(httpOnlyPrefix))) {
            CurlCookie cookie;
            cookie.domain = parts[0].startsWith(httpOnlyPrefix) ? parts[0].mid(httpOnlyPrefix.size()) : parts[0];
            cookie.includeSubdomains = equalsIgnoreCase(parts[1], QStringLiteral("TRUE"));
            if (!cookie.includeSubdomains) {
                while (cookie.domain.startsWith(QLatin1Char('.'))) {
                    cookie.domain.remove(0, 1);
                }
            }
            cookie.path = isNullOrWhiteSpace(parts[2]) ? QStringLiteral("/") : parts[2];
            cookie.secureOnly = equalsIgnoreCase(parts[3], QStringLiteral("TRUE"));
            bool ok = false;
            const qint64 expires = parts[4].trimmed().toLongLong(&ok);
            cookie.expires = ok ? expires : 0;
            cookie.name = parts[5];
            cookie.value = parts[6];
            cookie.httpOnly = true;
            cookies.append(cookie);
        } else {
            m_trace.writeLine(QStringLiteral("Invalid cookie line: %1").arg(line));
        }
    }
    return cookies;
}

// =============================================================================================
// HttpClientFactory
// =============================================================================================

struct HttpClientFactory::Private
{
    IFileSystem &fileSystem;
    ITrace &trace;
    ISettings &settings;
    IConsoleService &console;
};

HttpClientFactory::HttpClientFactory(IFileSystem &fileSystem, ITrace &trace, ISettings &settings, IConsoleService &console)
    : d(std::make_unique<Private>(Private{fileSystem, trace, settings, console}))
{
}

HttpClientFactory::~HttpClientFactory() = default;

HttpClient HttpClientFactory::createClient()
{
    d->trace.writeLine(QStringLiteral("Creating new HTTP client instance..."));

    HttpHandlerOptions options;
    HttpProxySettings proxy;
    if (tryCreateProxy(proxy)) {
        d->trace.writeLine(QStringLiteral("HTTP client is using the configured proxy."));
        options.proxy = proxy;
    }

    // Configure server certificate verification and warn if we're bypassing validation.
    // IsCertificateVerificationEnabled takes precedence over custom TLS cert verification.
    if (!d->settings.isCertificateVerificationEnabled()) {
        d->trace.writeLine(QStringLiteral("TLS certificate verification has been disabled."));
        d->console.writeWarning(QStringLiteral("----------------- SECURITY WARNING ----------------"));
        d->console.writeWarning(QStringLiteral("| TLS certificate verification has been disabled! |"));
        d->console.writeWarning(QStringLiteral("---------------------------------------------------"));
        d->console.writeWarning(QStringLiteral("HTTPS connections may not be secure. See %1 for more information.")
                                    .arg(Constants::HelpUrls::GcmTlsVerification));
        options.verifyCertificates = false;
    } else if (const OptString bundle = d->settings.customCertificateBundlePath(); !isNullOrWhiteSpace(bundle)) {
        d->trace.writeLine(QStringLiteral("Custom certificate verification has been enabled with certificate bundle at %1").arg(*bundle));
        if (!d->fileSystem.fileExists(*bundle)) {
            throw IOException(QStringLiteral("Custom certificate bundle not found at path: %1").arg(*bundle));
        }
        options.customCaBundlePath = bundle;
    }

    // Send the cookies of http.cookieFile (written by libcurl)
    if (const OptString cookieFilePath = d->settings.customCookieFilePath();
        !isNullOrWhiteSpace(cookieFilePath) && d->fileSystem.fileExists(*cookieFilePath)) {
        d->trace.writeLine(QStringLiteral("Custom cookie file has been enabled with %1").arg(*cookieFilePath));
        const QString contents = d->fileSystem.readAllText(*cookieFilePath);
        options.cookies = CurlCookieParser(d->trace).parse(contents);
        d->trace.writeLine(QStringLiteral("Configured to automatically send cookie header."));
    }

    HttpClient client(std::make_shared<QtHttpMessageHandler>(std::move(options)));
    client.defaultRequestHeaders().set(QStringLiteral("User-Agent"), Constants::getHttpUserAgent());
    client.defaultRequestHeaders().set(QStringLiteral("Cache-Control"), QStringLiteral("no-cache"));
    return client;
}

bool HttpClientFactory::tryCreateProxy(HttpProxySettings &proxy)
{
    proxy = HttpProxySettings();
    // Try to extract the proxy URI from the environment or Git config
    const std::optional<ProxyConfiguration> proxyConfig = d->settings.getProxyConfiguration();
    if (!proxyConfig) {
        return false;
    }

    // Inform the user if they are using a deprecated proxy configuration
    if (proxyConfig->isDeprecatedSource) {
        d->trace.writeLine(QStringLiteral("Using a deprecated proxy configuration."));
        d->console.writeWarning(QStringLiteral("Using a deprecated proxy configuration. See %1 for more information.").arg(Constants::HelpUrls::GcmHttpProxyGuide));
    }

    // If NO_PROXY is set to the value "*" then libcurl disables proxying. We should mirror this.
    if (proxyConfig->noProxyRaw && equalsIgnoreCase(*proxyConfig->noProxyRaw, QStringLiteral("*"))) {
        d->trace.writeLine(QStringLiteral("NO_PROXY value set to \"*\"; disabling proxy settings"));
        return false;
    }

    // Proxy info for tracing
    MultiDictionary dict;
    dict.set(QStringLiteral("address"), {absoluteUri(proxyConfig->address)});

    proxy.address = proxyConfig->address;
    // For an empty user name AND password use the system default credentials
    if (!(isNullOrEmpty(proxyConfig->userName) && isNullOrEmpty(proxyConfig->password))) {
        proxy.userName = proxyConfig->userName.value_or(QString());
        proxy.password = proxyConfig->password.value_or(QString());
        dict.set(QStringLiteral("username"), {*proxy.userName});
        dict.set(QStringLiteral("password"), {*proxy.password});
    } else {
        proxy.useDefaultCredentials = true;
        dict.set(QStringLiteral("useDefaultCredentials"), {QStringLiteral("true")});
    }

    // libcurl's no_proxy host list becomes a list of regular expressions
    if (!isNullOrWhiteSpace(proxyConfig->noProxyRaw)) {
        dict.set(QStringLiteral("noProxy"), {*proxyConfig->noProxyRaw});
        const QStringList bypassRegexs = ProxyConfiguration::convertToBypassRegexArray(proxyConfig->noProxyRaw);
        bool valid = true;
        for (const QString &regex : bypassRegexs) {
            valid = valid && QRegularExpression(regex).isValid();
        }
        if (valid) {
            dict.set(QStringLiteral("bypass"), {bypassRegexs.join(QLatin1Char(','))});
            proxy.bypassList = bypassRegexs;
        } else {
            const QString message = QStringLiteral("Failed to convert proxy bypass hosts to regular expressions; ignoring bypass list");
            d->trace.writeLine(message);
            Trace2::writeError(message);
            dict.set(QStringLiteral("bypass"), {QStringLiteral("<< failed to convert >>")});
        }
    }

    d->trace.writeLine(QStringLiteral("Created a WebProxy instance:"));
    d->trace.writeDictionarySecrets(dict, {QStringLiteral("password")});
    return true;
}

QString httpStatusCodeName(int statusCode)
{
    switch (statusCode) {
        case 100: return QStringLiteral("Continue");
        case 101: return QStringLiteral("SwitchingProtocols");
        case 200: return QStringLiteral("OK");
        case 201: return QStringLiteral("Created");
        case 202: return QStringLiteral("Accepted");
        case 203: return QStringLiteral("NonAuthoritativeInformation");
        case 204: return QStringLiteral("NoContent");
        case 205: return QStringLiteral("ResetContent");
        case 206: return QStringLiteral("PartialContent");
        case 300: return QStringLiteral("Ambiguous");
        case 301: return QStringLiteral("Moved");
        case 302: return QStringLiteral("Redirect");
        case 303: return QStringLiteral("RedirectMethod");
        case 304: return QStringLiteral("NotModified");
        case 305: return QStringLiteral("UseProxy");
        case 307: return QStringLiteral("TemporaryRedirect");
        case 308: return QStringLiteral("PermanentRedirect");
        case 400: return QStringLiteral("BadRequest");
        case 401: return QStringLiteral("Unauthorized");
        case 402: return QStringLiteral("PaymentRequired");
        case 403: return QStringLiteral("Forbidden");
        case 404: return QStringLiteral("NotFound");
        case 405: return QStringLiteral("MethodNotAllowed");
        case 406: return QStringLiteral("NotAcceptable");
        case 407: return QStringLiteral("ProxyAuthenticationRequired");
        case 408: return QStringLiteral("RequestTimeout");
        case 409: return QStringLiteral("Conflict");
        case 410: return QStringLiteral("Gone");
        case 411: return QStringLiteral("LengthRequired");
        case 412: return QStringLiteral("PreconditionFailed");
        case 413: return QStringLiteral("RequestEntityTooLarge");
        case 414: return QStringLiteral("RequestUriTooLong");
        case 415: return QStringLiteral("UnsupportedMediaType");
        case 416: return QStringLiteral("RequestedRangeNotSatisfiable");
        case 417: return QStringLiteral("ExpectationFailed");
        case 421: return QStringLiteral("MisdirectedRequest");
        case 422: return QStringLiteral("UnprocessableEntity");
        case 423: return QStringLiteral("Locked");
        case 424: return QStringLiteral("FailedDependency");
        case 426: return QStringLiteral("UpgradeRequired");
        case 428: return QStringLiteral("PreconditionRequired");
        case 429: return QStringLiteral("TooManyRequests");
        case 431: return QStringLiteral("RequestHeaderFieldsTooLarge");
        case 451: return QStringLiteral("UnavailableForLegalReasons");
        case 500: return QStringLiteral("InternalServerError");
        case 501: return QStringLiteral("NotImplemented");
        case 502: return QStringLiteral("BadGateway");
        case 503: return QStringLiteral("ServiceUnavailable");
        case 504: return QStringLiteral("GatewayTimeout");
        case 505: return QStringLiteral("HttpVersionNotSupported");
        case 511: return QStringLiteral("NetworkAuthenticationRequired");
        default: return QString::number(statusCode);
    }
}

} // namespace gcm
