// Port of GitRequest.cs and GitResponse.cs
#include "core/gitrequest.h"

#include "core/constants.h"
#include "core/strings.h"

#include <QRegularExpression>

namespace gcm {

// =============================================================================================
// GitRequest
// =============================================================================================

GitRequest::GitRequest(const StringDictionary &dict) : m_dict(dict.caseSensitivity())
{
    // Transform input from 1:1 to 1:n
    for (const auto &[key, value] : dict) {
        m_dict.set(key, {value});
    }
}

GitRequest::GitRequest(MultiDictionary dict) : m_dict(std::move(dict))
{
}

GitCapabilities GitRequest::capabilities() const
{
    if (!m_capabilities) {
        GitCapabilities caps = GitCapabilities::None;
        for (const QStringList names = getMultiArgumentOrDefault(QStringLiteral("capability")); const QString &name : names) {
            caps |= GitCapabilitiesUtils::parseName(name);
        }
        m_capabilities = caps;
    }
    return *m_capabilities;
}

bool GitRequest::isEphemeral() const
{
    return isTruthy(getArgumentOrDefault(Constants::CredentialProtocol::EphemeralKey));
}

const StringDictionary &GitRequest::state() const
{
    if (!m_state) {
        const QString prefix = Constants::CredentialProtocol::GcmStatePrefix;
        StringDictionary result;
        for (const QStringList entries = getMultiArgumentOrDefault(Constants::CredentialProtocol::StateKey); const QString &entry : entries) {
            // Malformed entries (no '=' or an empty key) are silently discarded, per the protocol
            const qsizetype sep = entry.indexOf(QLatin1Char('='));
            if (sep <= 0) {
                continue;
            }
            const QString rawKey = entry.left(sep);
            const QString value = entry.mid(sep + 1);
            // Only consume our own namespace
            if (!rawKey.startsWith(prefix)) {
                continue;
            }
            const QString key = rawKey.mid(prefix.size());
            // Skip entries that would not round-trip through our own emitter
            if (!GitStateValidation::isValidKey(key) || !GitStateValidation::isValidValue(value)) {
                continue;
            }
            result.set(key, value);
        }
        m_state = std::move(result);
    }
    return *m_state;
}

OptString GitRequest::getArgumentOrDefault(const QString &key) const
{
    QString value;
    return tryGetArgument(key, value) ? OptString(value) : std::nullopt;
}

QStringList GitRequest::getMultiArgumentOrDefault(const QString &key) const
{
    return m_dict.value(key);
}

bool GitRequest::tryGetArgument(const QString &key, QString &value) const
{
    value.clear();
    const QStringList *values = m_dict.find(key);
    if (!values || values->isEmpty()) {
        return false;
    }
    value = values->first();
    return true;
}

bool GitRequest::tryGetMultiArgument(const QString &key, QStringList &values) const
{
    values.clear();
    const QStringList *found = m_dict.find(key);
    if (!found) {
        return false;
    }
    values = *found;
    return true;
}

bool GitRequest::tryGetHostAndPort(QString &host, std::optional<int> &port) const
{
    host.clear();
    port.reset();
    const OptString hostArg = this->host();
    if (!hostArg) {
        return false;
    }

    // Split port number and hostname from host input argument
    const QStringList hostParts = hostArg->split(QLatin1Char(':'));
    host = hostParts[0];
    if (hostParts.size() > 1) {
        int portInt = 0;
        if (!tryToInt32(hostParts[1], portInt)) {
            return false;
        }
        port = portInt;
    }
    return true;
}

std::optional<QUrl> GitRequest::getRemoteUri(bool includeUser) const
{
    const OptString protocol = this->protocol();
    const OptString host = this->host();
    if (!protocol || !host) {
        return std::nullopt;
    }

    // UriBuilder(protocol, host[:port]) [+ user name] [+ path?query#fragment]. UriBuilder.ToString concatenates the
    // host and the path, so anything after a '/' in the host argument ends up in front of the path.
    const qsizetype slash = host->indexOf(QLatin1Char('/'));
    const QString hostAndPort = slash < 0 ? *host : host->left(slash);
    const QString hostPathPrefix = slash < 0 ? QString() : host->mid(slash);
    const QStringList hostParts = hostAndPort.split(QLatin1Char(':'));
    QUrl uri;
    uri.setScheme(*protocol);
    uri.setHost(hostParts[0]);
    int port = 0;
    if (hostParts.size() > 1 && tryToInt32(hostParts[1], port)) {
        uri.setPort(port);
    }

    const OptString user = userName();
    if (includeUser && !isNullOrEmpty(user)) {
        uri.setUserName(escapeDataString(*user), QUrl::TolerantMode);
    }

    QString path = QStringLiteral("/");
    if (const OptString pathArg = this->path()) {
        static const QRegularExpression separators(QStringLiteral("[?#]"));
        const QStringList pathParts = pathArg->split(separators);
        // The first piece is the path
        path = pathParts[0].startsWith(QLatin1Char('/')) ? pathParts[0] : QLatin1Char('/') + pathParts[0];
        if (pathParts.size() == 3) {
            // path, query and fragment
            uri.setQuery(pathParts[1], QUrl::TolerantMode);
            uri.setFragment(pathParts[2], QUrl::TolerantMode);
        } else if (pathParts.size() == 2 && pathArg->contains(QLatin1Char('?'))) {
            uri.setQuery(pathParts[1], QUrl::TolerantMode);
        } else if (pathParts.size() == 2 && pathArg->contains(QLatin1Char('#'))) {
            uri.setFragment(pathParts[1], QUrl::TolerantMode);
        }
    }
    uri.setPath(hostPathPrefix + path, QUrl::TolerantMode);

    if (!uri.isValid() || uri.scheme().isEmpty()) {
        throw Exception(QStringLiteral("Invalid URI: The URI scheme or host is not valid."));
    }
    return uri;
}

// =============================================================================================
// GitResponse
// =============================================================================================

GitResponse::GitResponse(std::optional<Credential> credential, bool isEphemeral, OptString authType, bool isContinue, bool isCancelled, bool isYielded)
:   m_credential(std::move(credential)),
    m_ephemeral(isEphemeral),
    m_authType(std::move(authType)),
    m_continue(isContinue),
    m_cancelled(isCancelled), m_yielded(isYielded)
{
    // At most one of Continue, Cancel, Yield may be set (Ok is "none of them").
    if ((isContinue && isCancelled) || (isContinue && isYielded) || (isCancelled && isYielded)) {
        throw ArgumentException(QStringLiteral("A response can be at most one of Continue, Cancel, or Yield."));
    }
    if ((isCancelled || isYielded) && m_credential) {
        throw ArgumentException(QStringLiteral("A cancelled or yielded response cannot carry a credential. (Parameter 'credential')"));
    }
    if (!isCancelled && !isYielded && !m_credential) {
        throw ArgumentNullException(QStringLiteral("A non-cancelled, non-yielded response must carry a credential. Use Cancel() or Yield() instead. (Parameter 'credential')"));
    }
}

GitResponse::GitResponse(Credential credential) : GitResponse(std::move(credential), false, std::nullopt, false, false, false)
{
}

GitResponse GitResponse::ok(Credential credential, bool isEphemeral, OptString authType)
{
    return GitResponse(std::move(credential), isEphemeral, std::move(authType), false, false, false);
}

GitResponse GitResponse::continue_(Credential credential, bool isEphemeral, OptString authType)
{
    return GitResponse(std::move(credential), isEphemeral, std::move(authType), true, false, false);
}

GitResponse GitResponse::cancel()
{
    return GitResponse(std::nullopt, false, std::nullopt, false, true, false);
}

GitResponse GitResponse::yield()
{
    return GitResponse(std::nullopt, false, std::nullopt, false, false, true);
}

void GitResponse::setState(const QString &key, const QString &value)
{
    GitStateValidation::validateKey(key);
    GitStateValidation::validateValue(value);
    if (m_cancelled || m_yielded) {
        return;
    }
    m_state.set(key, value);
}

} // namespace gcm
