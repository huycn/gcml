// Port of Authentication/OAuth/OAuth2SystemWebBrowser.cs: the system browser plus a loopback HTTP listener
// (QTcpServer in place of HttpListener) that intercepts the redirect.
#include "core/auth/oauth2.h"

#include "core/strings.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

namespace gcm {

const QString OAuth2SystemWebBrowser::DefaultSuccessHtml = QStringLiteral(
    "<!DOCTYPE html><html><head>\n"
    "<meta name=\"color-scheme\" content=\"light dark\">\n"
    "<style>body{font-family:sans-serif;}dt{font-weight:bold;}dd{margin-bottom:10px;}\n"
    "@media (prefers-color-scheme: dark){body{background:#0d1117;color:#c9d1d9;}a{color:#58a6ff;}}</style>\n"
    "<title>Authentication successful</title></head>\n"
    "<body><h1>Authentication successful</h1><p>You can now close this page.</p></body>\n"
    "</html>");

const QString OAuth2SystemWebBrowser::DefaultFailureHtmlFormat = QStringLiteral(
    "<!DOCTYPE html><html><head>\n"
    "<meta name=\"color-scheme\" content=\"light dark\">\n"
    "<style>body{{font-family:sans-serif;}}dt{{font-weight:bold;}}dd{{margin-bottom:10px;}}\n"
    "@media (prefers-color-scheme: dark){{body{{background:#0d1117;color:#c9d1d9;}}a{{color:#58a6ff;}}}}</style>\n"
    "<title>Authentication failed</title></head>\n"
    "<body><h1>Authentication failed</h1><dl>\n"
    "<dt>Error:</dt><dd>{0}</dd>\n"
    "<dt>Description:</dt><dd>{1}</dd>\n"
    "<dt>URL:</dt><dd>{2}</dd>\n"
    "</dl></body></html>");

namespace {

// Served in the fragment response flow. The parameters live in the URI fragment, which user agents do not send to
// the server, so they are reissued as a form POST to the redirect URI (keeping them out of URLs and logs).
// clazy:exclude-next-line=non-pod-global-static
const QString FragmentFormPostHtml = QStringLiteral(
    "<!DOCTYPE html><html><head>\n"
    "<meta name=\"color-scheme\" content=\"light dark\"><title>Authenticating...</title></head>\n"
    "<body><form id=\"gcm-fragment-form\" method=\"POST\"></form><script>\n"
    "(function () {\n"
    "  var hash = window.location.hash;\n"
    "  if (!hash || hash.length < 2) { return; }\n"
    "  var data = hash.charAt(0) === '#' ? hash.substring(1) : hash;\n"
    "  var form = document.getElementById('gcm-fragment-form');\n"
    "  form.action = window.location.pathname;\n"
    "  data.split('&').forEach(function (pair) {\n"
    "    if (!pair) { return; }\n"
    "    var eq = pair.indexOf('=');\n"
    "    var name = eq < 0 ? pair : pair.substring(0, eq);\n"
    "    var value = eq < 0 ? '' : pair.substring(eq + 1);\n"
    "    var input = document.createElement('input');\n"
    "    input.type = 'hidden';\n"
    "    input.name = decodeURIComponent(name.replace(/\\+/g, ' '));\n"
    "    input.value = decodeURIComponent(value.replace(/\\+/g, ' '));\n"
    "    form.appendChild(input);\n"
    "  });\n"
    "  form.submit();\n"
    "})();\n"
    "</script></body></html>");

/// string.Format with positional {n} placeholders and {{ / }} escapes.
QString formatDotNet(const QString &format, const QStringList &args)
{
    QString out;
    for (qsizetype i = 0; i < format.size(); ++i) {
        const QChar c = format[i];
        if (c == QLatin1Char('{') && i + 1 < format.size() && format[i + 1] == QLatin1Char('{')) {
            out += c;
            ++i;
        } else if (c == QLatin1Char('}') && i + 1 < format.size() && format[i + 1] == QLatin1Char('}')) {
            out += c;
            ++i;
        } else if (c == QLatin1Char('{')) {
            const qsizetype end = format.indexOf(QLatin1Char('}'), i);
            bool ok = false;
            const int index = end > i ? format.mid(i + 1, end - i - 1).toInt(&ok) : -1;
            if (ok && index >= 0 && index < args.size()) {
                out += args[index];
                i = end;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

/// Query / form data: '+' is a space.
StringDictionary parseFormData(QString data)
{
    data.replace(QLatin1Char('+'), QLatin1String("%20"));
    return parseQueryString(data);
}

struct HttpListenerRequest
{
    QString method;
    QString path;  // without query
    QString query; // raw
    StringDictionary headers{Qt::CaseInsensitive};
    QByteArray body;
};

/// Parses a complete request from `buffer`; false while more data is needed.
bool tryParseRequest(const QByteArray &buffer, HttpListenerRequest &request)
{
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    request.method = QString::fromLatin1(requestLine.value(0));
    const QString target = QString::fromUtf8(requestLine.value(1));
    const qsizetype q = target.indexOf(QLatin1Char('?'));
    request.path = q < 0 ? target : target.left(q);
    request.query = q < 0 ? QString() : target.mid(q + 1);
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0) {
            request.headers.set(QString::fromLatin1(line.left(colon)).trimmed(), QString::fromUtf8(line.mid(colon + 1)).trimmed());
        }
    }
    const qsizetype length = request.headers.value(QStringLiteral("Content-Length")).toLongLong();
    if (buffer.size() - (headerEnd + 4) < length) {
        return false;
    }
    request.body = buffer.mid(headerEnd + 4, length);
    return true;
}

void writeResponse(QTcpSocket *socket, const QByteArray &statusLine, const QByteArray &extraHeaders, const QByteArray &body)
{
    QByteArray response = "HTTP/1.1 " + statusLine + "\r\n" + extraHeaders + "Content-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void writeHtml(QTcpSocket *socket, const QString &html)
{
    writeResponse(socket, "200 OK", "Content-Type: text/html; charset=utf-8\r\n", html.toUtf8());
}

void writeRedirect(QTcpSocket *socket, const QString &url)
{
    writeResponse(socket, "302 Found", "Location: " + url.toUtf8() + "\r\n", QByteArray());
}

int getFreeTcpPort()
{
    QTcpServer listener;
    if (!listener.listen(QHostAddress::LocalHost, 0)) {
        throw IOException(QStringLiteral("Failed to find a free TCP port: %1").arg(listener.errorString()));
    }
    return listener.serverPort();
}

} // namespace

OAuth2SystemWebBrowser::OAuth2SystemWebBrowser(ISessionManager &sessionManager, OAuth2WebBrowserOptions options)
    : m_sessionManager(sessionManager), m_options(std::move(options))
{
}

bool OAuth2SystemWebBrowser::isFormUrlEncoded(const OptString &contentType)
{
    if (isNullOrEmpty(contentType)) {
        return false;
    }
    // Compare only the media type (everything up to the first ';'), ignoring parameters such as "; charset=utf-8".
    const QString mediaType = contentType->section(QLatin1Char(';'), 0, 0).trimmed();
    return equalsIgnoreCase(mediaType, Constants::Http::MimeTypeFormUrlEncoded);
}

QUrl OAuth2SystemWebBrowser::updateRedirectUri(const QUrl &uri)
{
    if (!isLoopback(uri)) {
        throw ArgumentException(QStringLiteral("Only localhost is supported as a redirect URI. (Parameter 'uri')"));
    }

    // If a port has been specified use it, otherwise find a free one
    if (isDefaultPort(uri)) {
        QUrl result = uri;
        result.setPort(getFreeTcpPort());
        if (result.path().isEmpty()) {
            result.setPath(QStringLiteral("/")); // UriBuilder.Uri always has a path
        }
        return result;
    }
    return uri;
}

StringDictionary OAuth2SystemWebBrowser::getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri, OAuth2ResponseMode responseMode, const CancellationToken &ct)
{
    if (!isLoopback(redirectUri)) {
        throw ArgumentException(QStringLiteral("Only localhost is supported as a redirect URI. (Parameter 'redirectUri')"));
    }

    // Listen on the loopback address(es) of the redirect host before opening the browser.
    const int port = redirectUri.port(80);
    QList<std::shared_ptr<QTcpServer>> servers;
    QHostAddress address;
    const QList<QHostAddress> addresses = address.setAddress(redirectUri.host())
                                              ? QList<QHostAddress>{address}
                                              : QList<QHostAddress>{QHostAddress::LocalHost, QHostAddress::LocalHostIPv6};
    for (const QHostAddress &a : addresses) {
        auto server = std::make_shared<QTcpServer>();
        if (server->listen(a, quint16(port))) {
            servers.append(server);
        }
    }
    if (servers.isEmpty()) {
        throw IOException(QStringLiteral("Failed to listen on %1").arg(absoluteUri(redirectUri)));
    }

    // Requests outside the redirect path are answered with 404, like HttpListener does for unknown prefixes.
    QString prefixPath = redirectUri.path(QUrl::FullyEncoded);
    if (!prefixPath.endsWith(QLatin1Char('/'))) {
        prefixPath += QLatin1Char('/');
    }

    QEventLoop loop;
    QPointer<QEventLoop> loopGuard(&loop);
    std::optional<StringDictionary> result;

    auto handleRequest = [&](QTcpSocket *socket, const HttpListenerRequest &request) {
        QString path = request.path;
        if (!path.endsWith(QLatin1Char('/'))) {
            path += QLatin1Char('/');
        }
        if (!path.startsWith(prefixPath, Qt::CaseInsensitive)) {
            writeResponse(socket, "404 Not Found", QByteArray(), QByteArray());
            return;
        }

        // Form posts (and the POST that forwards a fragment response) carry the parameters in the body;
        // query responses carry them in the query string.
        StringDictionary parameters(Qt::CaseInsensitive);
        if (equalsIgnoreCase(request.method, Constants::Http::MethodPost) &&
            isFormUrlEncoded(request.headers.tryGet(QStringLiteral("Content-Type")))) {
            for (const auto &[k, v] : parseFormData(QString::fromUtf8(request.body))) {
                parameters.set(k, v);
            }
        } else {
            for (const auto &[k, v] : parseFormData(request.query)) {
                parameters.set(k, v);
            }
        }

        // Fragment mode: the first leg is a parameterless GET; answer with the script that re-posts the fragment.
        if (responseMode == OAuth2ResponseMode::Fragment && parameters.isEmpty()) {
            writeHtml(socket, FragmentFormPostHtml);
            return;
        }

        QString errorCode, errorDescription, errorUri;
        const bool hasError = parameters.tryGetValue(OAuth2Constants::AuthorizationGrantResponse::ErrorCodeParameter, errorCode);
        parameters.tryGetValue(OAuth2Constants::AuthorizationGrantResponse::ErrorDescriptionParameter, errorDescription);
        parameters.tryGetValue(OAuth2Constants::AuthorizationGrantResponse::ErrorUriParameter, errorUri);
        if (hasError) {
            auto formatError = [&](const QString &format) {
                return formatDotNet(format, {isNullOrWhiteSpace(errorCode) ? QStringLiteral("unknown") : errorCode,
                                             isNullOrWhiteSpace(errorDescription) ? QStringLiteral("Unknown error.") : errorDescription,
                                             isNullOrWhiteSpace(errorUri) ? QStringLiteral("none") : errorUri});
            };
            // Prefer redirection options to raw HTML
            if (m_options.failureRedirectFormat) {
                QString format = m_options.failureRedirectFormat->toString();
                format.replace(QLatin1String("%7B"), QLatin1String("{")).replace(QLatin1String("%7D"), QLatin1String("}"));
                writeRedirect(socket, formatError(format));
            } else {
                writeHtml(socket, formatError(m_options.failureResponseHtmlFormat.value_or(DefaultFailureHtmlFormat)));
            }
        } else if (m_options.successRedirect) {
            writeRedirect(socket, m_options.successRedirect->toString(QUrl::FullyEncoded));
        } else {
            writeHtml(socket, m_options.successResponseHtml.value_or(DefaultSuccessHtml));
        }

        result = parameters;
        loop.quit();
    };

    for (const auto &server : std::as_const(servers)) {
        QTcpServer *s = server.get();
        QObject::connect(s, &QTcpServer::newConnection, &loop, [s, &handleRequest, &result] {
            while (QTcpSocket *socket = s->nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer, &handleRequest, &result] {
                    if (result) {
                        return;
                    }
                    buffer->append(socket->readAll());
                    HttpListenerRequest request;
                    if (tryParseRequest(*buffer, request)) {
                        handleRequest(socket, request);
                    }
                });
            }
        });
    }

    ct.onCancel([loopGuard] {
        if (loopGuard) {
            loopGuard->quit();
        }
    });

    m_sessionManager.openBrowser(authorizationUri);

    if (!ct.isCancellationRequested()) {
        loop.exec();
    }

    // Let the final response reach the browser before the listener goes away.
    for (const auto &server : std::as_const(servers)) {
        const QList<QTcpSocket *> sockets = server->findChildren<QTcpSocket *>();
        for (QTcpSocket *socket : sockets) {
            if (socket->bytesToWrite() > 0) {
                socket->waitForBytesWritten(1000);
            }
        }
        server->close();
    }

    if (!result) {
        ct.throwIfCancellationRequested();
        throw OperationCanceledException(QStringLiteral("The operation was canceled."));
    }
    return *result;
}

} // namespace gcm
