// The real HTTP message handler over QNetworkAccessManager (HttpClientHandler upstream).
#include "core/http.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QTimer>

namespace gcm {

namespace {

/// Splits a (possibly combined) WWW-Authenticate value into its challenges. A comma separates parameters as well as
/// challenges; a new challenge starts with a token that is followed by a space or the end, not by '='.
QStringList splitChallenges(const QString &value)
{
    QStringList segments;
    QString current;
    bool inQuotes = false;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value[i];
        if (c == QLatin1Char('"') && (i == 0 || value[i - 1] != QLatin1Char('\\'))) {
            inQuotes = !inQuotes;
        }
        if ((c == QLatin1Char(',') || c == QLatin1Char('\n')) && !inQuotes) {
            segments.append(current);
            current.clear();
        } else {
            current += c;
        }
    }
    segments.append(current);

    static const QRegularExpression challengeStart(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+(\\s+[^=\\s]|\\s*$)"));
    QStringList challenges;
    for (const QString &segment : std::as_const(segments)) {
        const QString trimmed = segment.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (challenges.isEmpty() || challengeStart.match(trimmed).hasMatch()) {
            challenges.append(trimmed);
        } else {
            challenges.last() += QStringLiteral(", ") + trimmed;
        }
    }
    return challenges;
}

} // namespace

struct QtHttpMessageHandler::Private
{
    HttpHandlerOptions options;
    std::unique_ptr<QNetworkAccessManager> nam;

    QNetworkAccessManager &manager()
    {
        if (!nam) {
            nam = std::make_unique<QNetworkAccessManager>();
            nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
            nam->setProxy(QNetworkProxy::NoProxy);
            // Never prompt for (or supply) credentials: a 401/407 is handed back to the caller.
            QObject::connect(nam.get(), &QNetworkAccessManager::authenticationRequired, [](QNetworkReply *, QAuthenticator *) {});

            if (!options.cookies.isEmpty()) {
                // Like CookieContainer.Add(new Uri(scheme://domain), new Cookie(name, value)): host-only cookies.
                auto *jar = new QNetworkCookieJar(nam.get());
                for (const CurlCookie &c : std::as_const(options.cookies)) {
                    QString domain = c.domain;
                    while (domain.startsWith(QLatin1Char('.'))) {
                        domain.remove(0, 1);
                    }
                    QUrl url;
                    url.setScheme(c.secureOnly ? QStringLiteral("https") : QStringLiteral("http"));
                    url.setHost(domain);
                    url.setPath(QStringLiteral("/"));
                    jar->setCookiesFromUrl({QNetworkCookie(c.name.toUtf8(), c.value.toUtf8())}, url);
                }
                nam->setCookieJar(jar);
            }
        }
        return *nam;
    }

    void applyProxy(const QUrl &url)
    {
        if (!options.proxy || options.proxy->isBypassed(url)) {
            manager().setProxy(QNetworkProxy::NoProxy);
            return;
        }
        const HttpProxySettings &p = *options.proxy;
        QNetworkProxy proxy(QNetworkProxy::HttpProxy, p.address.host(),
                            quint16(p.address.port(p.address.scheme() == QLatin1String("https") ? 443 : 80)));
        if (!p.useDefaultCredentials) {
            proxy.setUser(p.userName.value_or(QString()));
            proxy.setPassword(p.password.value_or(QString()));
        }
        manager().setProxy(proxy);
    }
};

QtHttpMessageHandler::QtHttpMessageHandler(HttpHandlerOptions options) : d(std::make_unique<Private>())
{
    d->options = std::move(options);
}

QtHttpMessageHandler::~QtHttpMessageHandler() = default;

const HttpHandlerOptions &QtHttpMessageHandler::options() const { return d->options; }

HttpResponse QtHttpMessageHandler::send(const HttpRequest &request, int timeoutMs, const CancellationToken &ct)
{
    ct.throwIfCancellationRequested();

    QNetworkAccessManager &nam = d->manager();
    d->applyProxy(request.url);

    QNetworkRequest req(request.url);
    req.setMaximumRedirectsAllowed(50);
    for (const auto &[name, values] : request.headers.all()) {
        for (const QString &value : values) {
            req.setRawHeader(name.toUtf8(), value.toUtf8());
        }
    }
    if (!request.content.isEmpty() && request.contentType) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, *request.contentType);
    }

    if (request.url.scheme() == QLatin1String("https")) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        if (!d->options.verifyCertificates) {
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        } else if (d->options.customCaBundlePath) {
            ssl.addCaCertificates(*d->options.customCaBundlePath, QSsl::Pem);
        }
        req.setSslConfiguration(ssl);
    }

    const QByteArray method = request.method.toUpper().toLatin1();
    QPointer<QNetworkReply> reply;
    if (method == "GET") {
        reply = nam.get(req);
    } else if (method == "HEAD") {
        reply = nam.head(req);
    } else {
        reply = nam.sendCustomRequest(req, method, request.content);
    }

    QEventLoop loop;
    QPointer<QEventLoop> loopGuard(&loop);
    bool timedOut = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        if (reply) {
            reply->abort();
        }
    });
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    ct.onCancel([reply, loopGuard] {
        if (reply) {
            reply->abort();
        }
        if (loopGuard) {
            loopGuard->quit();
        }
    });
    if (timeoutMs > 0) {
        timer.start(timeoutMs);
    }
    if (!reply->isFinished() && !ct.isCancellationRequested()) {
        loop.exec();
    }
    timer.stop();

    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)> owner(reply.data(), [](QNetworkReply *r) {
        if (r) {
            r->deleteLater();
        }
    });

    if (ct.isCancellationRequested()) {
        throw OperationCanceledException(QStringLiteral("The operation was canceled."));
    }
    if (timedOut) {
        throw TimeoutException(QStringLiteral("The request was canceled due to the configured HttpClient.Timeout of %1 seconds elapsing.").arg(timeoutMs / 1000.0));
    }

    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status.isValid()) {
        throw HttpRequestException(reply->errorString());
    }

    HttpResponse response;
    response.statusCode = status.toInt();
    response.reasonPhrase = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    response.requestUri = reply->url();
    for (const auto &[name, value] : reply->rawHeaderPairs()) {
        const QString headerName = QString::fromLatin1(name);
        const QString headerValue = QString::fromUtf8(value);
        if (headerName.compare(QLatin1String("WWW-Authenticate"), Qt::CaseInsensitive) == 0 ||
            headerName.compare(QLatin1String("Proxy-Authenticate"), Qt::CaseInsensitive) == 0) {
            for (const QStringList challenges = splitChallenges(headerValue); const QString &challenge : challenges) {
                response.headers.add(headerName, challenge);
            }
        } else {
            response.headers.add(headerName, headerValue);
        }
    }
    response.content = reply->readAll();
    return response;
}

} // namespace gcm
