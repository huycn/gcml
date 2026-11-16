// Git credential protocol request/response. Ports GitRequest.cs and GitResponse.cs.
#pragma once

#include "core/common.h"
#include "core/credential.h"
#include "core/dictionary.h"
#include "core/gitcapabilities.h"

namespace gcm {

class GitRequest
{
public:
    explicit GitRequest(const StringDictionary &dict);
    explicit GitRequest(MultiDictionary dict);

    GitCapabilities capabilities() const;
    OptString protocol() const { return getArgumentOrDefault(QStringLiteral("protocol")); }
    OptString host() const { return getArgumentOrDefault(QStringLiteral("host")); }
    OptString path() const { return getArgumentOrDefault(QStringLiteral("path")); }
    OptString authType() const { return getArgumentOrDefault(QStringLiteral("authtype")); }
    OptString credential() const { return getArgumentOrDefault(QStringLiteral("credential")); }
    OptString userName() const { return getArgumentOrDefault(QStringLiteral("username")); }
    OptString password() const { return getArgumentOrDefault(QStringLiteral("password")); }
    bool isEphemeral() const;
    QStringList wwwAuth() const { return getMultiArgumentOrDefault(QStringLiteral("wwwauth")); }
    /// GCM-owned `state[]` entries with the "gcm." prefix stripped; invalid entries are dropped.
    const StringDictionary &state() const;

    OptString operator[](const QString &key) const { return getArgumentOrDefault(key); }
    OptString getArgumentOrDefault(const QString &key) const;
    QStringList getMultiArgumentOrDefault(const QString &key) const;
    bool tryGetArgument(const QString &key, QString &value) const;
    bool tryGetMultiArgument(const QString &key, QStringList &values) const;
    /// False when host is absent or the port is not a number.
    bool tryGetHostAndPort(QString &host, std::optional<int> &port) const;
    /// nullopt when protocol or host is missing. The path may carry `?query` and `#fragment`.
    std::optional<QUrl> getRemoteUri(bool includeUser = false) const;

    const MultiDictionary &arguments() const { return m_dict; }

private:
    MultiDictionary m_dict;
    mutable std::optional<GitCapabilities> m_capabilities;
    mutable std::optional<StringDictionary> m_state;
};

class GitResponse
{
public:
    /// Equivalent to ok(credential).
    explicit GitResponse(Credential credential);

    static GitResponse ok(Credential credential, bool isEphemeral = false, OptString authType = std::nullopt);
    /// The helper expects another round (multistage auth); emitted as `continue=1` when negotiated.
    static GitResponse continue_(Credential credential, bool isEphemeral = false, OptString authType = std::nullopt);
    /// The user cancelled: Git is told to stop (`quit=1`).
    static GitResponse cancel();
    /// Nothing to offer: Git falls through to the next helper / its own prompt.
    static GitResponse yield();

    const std::optional<Credential> &credential() const { return m_credential; }
    bool isCredentialEphemeral() const { return m_ephemeral; }
    const OptString &authType() const { return m_authType; }
    bool isContinue() const { return m_continue; }
    bool isCancelled() const { return m_cancelled; }
    bool isYielded() const { return m_yielded; }

    const StringDictionary &state() const { return m_state; }
    /// Validates key/value (throws ArgumentException); silently ignored on cancelled/yielded responses.
    void setState(const QString &key, const QString &value);
    GitResponse &withState(const QString &key, const QString &value)
    {
        setState(key, value);
        return *this;
    }

    /// Extra `key=value` lines written after the credential (keys case-insensitive).
    StringDictionary additionalProperties{Qt::CaseInsensitive};

private:
    GitResponse(std::optional<Credential> credential, bool isEphemeral, OptString authType, bool isContinue, bool isCancelled, bool isYielded);
    std::optional<Credential> m_credential;
    bool m_ephemeral = false;
    OptString m_authType;
    bool m_continue = false;
    bool m_cancelled = false;
    bool m_yielded = false;
    StringDictionary m_state;
};

} // namespace gcm
