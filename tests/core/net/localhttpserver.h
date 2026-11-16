// A tiny HTTP/1.1 server on 127.0.0.1 for the tests of the real handler. It serves requests from the event loop the
// handler under test runs while it waits, so everything stays on one thread.
#pragma once

#include "core/dictionary.h"

#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>

namespace gcm::test {

struct LocalHttpRequest
{
    QString method;
    QString target; // path + query
    StringDictionary headers{Qt::CaseInsensitive};
    QByteArray body;
};

class LocalHttpServer
{
public:
    /// Returns the complete raw response (status line, headers, blank line, body). An empty answer closes the
    /// connection without responding.
    using Handler = std::function<QByteArray(const LocalHttpRequest &)>;

    explicit LocalHttpServer(Handler handler) : m_handler(std::move(handler))
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    LocalHttpRequest request;
                    if (!tryParse(*buffer, request)) {
                        return;
                    }
                    buffer->clear();
                    requests.append(request);
                    const QByteArray response = m_handler(request);
                    if (!response.isEmpty()) {
                        socket->write(response);
                    }
                    socket->disconnectFromHost();
                });
            }
        });
    }

    quint16 port() const { return m_server.serverPort(); }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path); }

    QList<LocalHttpRequest> requests;

    static QByteArray response(int status, const QByteArray &reason, const QList<QByteArray> &headers = {},
                               const QByteArray &body = {})
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
        for (const QByteArray &h : headers) {
            out += h + "\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        return out;
    }

private:
    static bool tryParse(const QByteArray &buffer, LocalHttpRequest &request)
    {
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return false;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        request.method = QString::fromLatin1(requestLine.value(0));
        request.target = QString::fromUtf8(requestLine.value(1));
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                const QString name = QString::fromLatin1(line.left(colon)).trimmed();
                const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
                request.headers.set(name, request.headers.contains(name) ? request.headers.value(name) + QStringLiteral(", ") + value : value);
            }
        }
        const qsizetype length = request.headers.value(QStringLiteral("Content-Length")).toLongLong();
        if (buffer.size() - (headerEnd + 4) < length) {
            return false;
        }
        request.body = buffer.mid(headerEnd + 4, length);
        return true;
    }

    QTcpServer m_server;
    Handler m_handler;
};

} // namespace gcm::test
