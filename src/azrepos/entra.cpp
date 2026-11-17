// The Entra model: errors, accounts, identities (ManagedIdentity.cs, WorkloadFederationOptions.cs,
// InteractionMode.cs, EntraAccount.cs), the token cache and its storage (replacing MSAL's cache helper), and
// X509Utils.GetCertificateByThumbprint.
#include "entra.h"

#include "core/commandcontext.h"
#include "core/stores.h"
#include "core/strings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace gcm::entra {

// =============================================================================================
// Errors, names
// =============================================================================================

bool EntraServiceException::isRetryable() const noexcept
{
    // MsalServiceException.IsRetryable
    return errorCode() == EntraError::RequestTimeout || errorCode() == EntraError::ServiceNotAvailable || m_statusCode == 408 ||
           m_statusCode == 429 || (m_statusCode >= 500 && m_statusCode != 501 && m_statusCode != 505);
}

bool EntraAccount::operator==(const EntraAccount &other) const
{
    auto eq = [](const OptString &a, const OptString &b) {
        return a.has_value() == b.has_value() && (!a || a->compare(*b, Qt::CaseInsensitive) == 0);
    };
    return eq(homeAccountId, other.homeAccountId) && eq(userName, other.userName);
}

QString displayName(InteractionMode mode)
{
    switch (mode) {
    case InteractionMode::Auto: return QStringLiteral("Auto");
    case InteractionMode::EmbeddedWebView: return QStringLiteral("Embedded web view");
    case InteractionMode::SystemWebView: return QStringLiteral("System web view");
    case InteractionMode::DeviceCode: return QStringLiteral("Device code");
    }
    throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values. (Parameter 'mode')"));
}

QString modeName(InteractionMode mode)
{
    switch (mode) {
    case InteractionMode::Auto: return QStringLiteral("auto");
    case InteractionMode::EmbeddedWebView: return QStringLiteral("embeddedwebview");
    case InteractionMode::SystemWebView: return QStringLiteral("systemwebview");
    case InteractionMode::DeviceCode: return QStringLiteral("devicecode");
    }
    return QString();
}

QString scenarioName(WorkloadFederationScenario scenario)
{
    switch (scenario) {
    case WorkloadFederationScenario::Generic: return QStringLiteral("Generic");
    case WorkloadFederationScenario::ManagedIdentity: return QStringLiteral("ManagedIdentity");
    case WorkloadFederationScenario::GitHubActions: return QStringLiteral("GitHubActions");
    }
    return QString();
}

// =============================================================================================
// ManagedIdentity
// =============================================================================================

namespace {

/// Guid.TryParse: the common textual forms (with or without hyphens, braces or parentheses); "D" format, lower case.
std::optional<QString> tryParseGuid(const QString &value)
{
    const QString s = value.trimmed();
    QUuid uuid(s);
    if (uuid.isNull()) {
        // QUuid does not take the 32-digit form without hyphens ("N" format)
        static const QRegularExpression n(QStringLiteral("^[0-9a-fA-F]{32}$"));
        if (!n.match(s).hasMatch()) {
            // An all-zero GUID is valid but parses to a null QUuid
            static const QRegularExpression zero(QStringLiteral("^[{(]?0{8}-?0{4}-?0{4}-?0{4}-?0{12}[})]?$"));
            return zero.match(s).hasMatch() ? std::optional<QString>(QStringLiteral("00000000-0000-0000-0000-000000000000")) : std::nullopt;
        }
        uuid = QUuid(QStringLiteral("%1-%2-%3-%4-%5").arg(s.mid(0, 8), s.mid(8, 4), s.mid(12, 4), s.mid(16, 4), s.mid(20)));
    }
    return uuid.toString(QUuid::WithoutBraces).toLower();
}

} // namespace

const ManagedIdentity &ManagedIdentity::system()
{
    static const ManagedIdentity s(QStringLiteral("system"), Kind::System, QString());
    return s;
}

bool ManagedIdentity::tryCreate(const OptString &id, ManagedIdentity &mi)
{
    if (isNullOrWhiteSpace(id)) {
        return false;
    }
    if (id->compare(QLatin1String("system"), Qt::CaseInsensitive) == 0) {
        mi = system();
        return true;
    }
    // {uuid} => user-assigned client ID
    if (const std::optional<QString> guid = tryParseGuid(*id)) {
        mi = ManagedIdentity(QStringLiteral("id://") + *guid, Kind::ClientId, *guid);
        return true;
    }
    // (resource|id)://{uuid}
    if (const std::optional<QUrl> uri = tryCreateUri(id)) {
        if (const std::optional<QString> guid = tryParseGuid(uri->host())) {
            if (uri->scheme().compare(QLatin1String("id"), Qt::CaseInsensitive) == 0) {
                mi = ManagedIdentity(QStringLiteral("id://") + *guid, Kind::ClientId, *guid);
                return true;
            }
            if (uri->scheme().compare(QLatin1String("resource"), Qt::CaseInsensitive) == 0) {
                mi = ManagedIdentity(QStringLiteral("resource://") + *guid, Kind::ResourceId, *guid);
                return true;
            }
        }
    }
    // Unknown
    return false;
}

ManagedIdentity ManagedIdentity::create(const QString &id)
{
    ManagedIdentity mi = system();
    if (!tryCreate(id, mi)) {
        throw ArgumentException(QStringLiteral("Invalid managed identity id '%1' (Parameter 'id')").arg(id));
    }
    return mi;
}

// =============================================================================================
// TokenCache (JSON)
// =============================================================================================

TokenCache TokenCache::fromJson(const QByteArray &json)
{
    TokenCache cache;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray accountsJson = root.value(QStringLiteral("accounts")).toArray();
    for (const QJsonValue &v : accountsJson) {
        const QJsonObject o = v.toObject();
        cache.accounts.append({
            o.value(QStringLiteral("home_account_id")).toString(), o.value(QStringLiteral("environment")).toString(),
            o.value(QStringLiteral("tenant_id")).toString(), o.value(QStringLiteral("username")).toString(),
        });
    }
    const QJsonArray refreshTokensJson = root.value(QStringLiteral("refresh_tokens")).toArray();
    for (const QJsonValue &v : refreshTokensJson) {
        const QJsonObject o = v.toObject();
        cache.refreshTokens.append({
            o.value(QStringLiteral("home_account_id")).toString(), o.value(QStringLiteral("environment")).toString(),
            o.value(QStringLiteral("client_id")).toString(), o.value(QStringLiteral("secret")).toString(),
        });
    }
    const QJsonArray accessTokensJson = root.value(QStringLiteral("access_tokens")).toArray();
    for (const QJsonValue &v : accessTokensJson) {
        const QJsonObject o = v.toObject();
        cache.accessTokens.append({
            o.value(QStringLiteral("home_account_id")).toString(), o.value(QStringLiteral("environment")).toString(),
            o.value(QStringLiteral("client_id")).toString(), o.value(QStringLiteral("tenant_id")).toString(),
            o.value(QStringLiteral("scopes")).toString(), o.value(QStringLiteral("secret")).toString(),
            qint64(o.value(QStringLiteral("expires_on")).toDouble()),
        });
    }
    return cache;
}

QByteArray TokenCache::toJson() const
{
    QJsonArray accountsJson, refreshJson, accessJson;
    for (const Account &a : accounts) {
        accountsJson.append(QJsonObject {
            { QStringLiteral("home_account_id"), a.homeAccountId}, {QStringLiteral("environment"), a.environment },
            { QStringLiteral("tenant_id"), a.tenantId}, {QStringLiteral("username"), a.userName },
        });
    }
    for (const RefreshToken &r : refreshTokens) {
        refreshJson.append(QJsonObject {
            { QStringLiteral("home_account_id"), r.homeAccountId}, {QStringLiteral("environment"), r.environment },
            { QStringLiteral("client_id"), r.clientId}, {QStringLiteral("secret"), r.secret },
        });
    }
    for (const AccessToken &t : accessTokens) {
        accessJson.append(QJsonObject {
            { QStringLiteral("home_account_id"), t.homeAccountId}, {QStringLiteral("environment"), t.environment },
            { QStringLiteral("client_id"), t.clientId}, {QStringLiteral("tenant_id"), t.tenantId },
            { QStringLiteral("scopes"), t.scopes}, {QStringLiteral("secret"), t.secret },
            { QStringLiteral("expires_on"), double(t.expiresOn) },
        });
    }
    const QJsonObject root{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("accounts"), accountsJson },
        { QStringLiteral("refresh_tokens"), refreshJson },
        { QStringLiteral("access_tokens"), accessJson },
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// =============================================================================================
// Token cache storages
// =============================================================================================

namespace {

QString kindName(TokenCacheKind kind)
{
    return kind == TokenCacheKind::User ? QStringLiteral("user") : QStringLiteral("app");
}

/// A lock file in <userData>/entra, shared by both storages of a cache.
class CacheLock
{
public:
    explicit CacheLock(QString path) : m_path(std::move(path)) {}
    void lock()
    {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        m_lock = std::make_unique<QLockFile>(m_path);
        m_lock->setStaleLockTime(30 * 1000);
        // Wait at most 30 seconds for another GCM process; then go ahead without the lock (as MSAL's cache helper)
        if (!m_lock->tryLock(30 * 1000)) {
            m_lock.reset();
        }
    }
    void unlock() { m_lock.reset(); }

private:
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
};

/// The cache as a single item of the GCM Secret Service schema.
class SecretServiceCacheStorage : public ITokenCacheStorage
{
public:
    SecretServiceCacheStorage(TokenCacheKind kind, QString lockPath)
        : m_service(QStringLiteral("gcm-entra-token-cache://") + kindName(kind)), m_lock(std::move(lockPath)) {}

    QByteArray load() override
    {
        const std::optional<Credential> c = m_store.get(m_service, account());
        return c ? c->password.toUtf8() : QByteArray();
    }
    void save(const QByteArray &data) override { m_store.addOrUpdate(m_service, account(), QString::fromUtf8(data)); }
    QString description() const override { return QStringLiteral("Secret Service item '%1'").arg(m_service); }
    void lock() override { m_lock.lock(); }
    void unlock() override { m_lock.unlock(); }

private:
    static QString account() { return QStringLiteral("token-cache"); }
    SecretServiceCollection m_store{std::nullopt};
    QString m_service;
    CacheLock m_lock;
};

/// The plaintext fallback: a file readable by the user only.
class FileCacheStorage : public ITokenCacheStorage
{
public:
    FileCacheStorage(QString path, QString lockPath) : m_path(std::move(path)), m_lock(std::move(lockPath)) {}

    QByteArray load() override
    {
        QFile f(m_path);
        if (!f.exists()) {
            return {};
        }
        if (!f.open(QIODevice::ReadOnly)) {
            throw IOException(QStringLiteral("Failed to read token cache '%1': %2").arg(m_path, f.errorString()));
        }
        return f.readAll();
    }
    void save(const QByteArray &data) override
    {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QSaveFile f(m_path);
        if (!f.open(QIODevice::WriteOnly)) {
            throw IOException(QStringLiteral("Failed to write token cache '%1': %2").arg(m_path, f.errorString()));
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(data);
        if (!f.commit()) {
            throw IOException(QStringLiteral("Failed to write token cache '%1': %2").arg(m_path, f.errorString()));
        }
    }
    QString description() const override { return QStringLiteral("file '%1'").arg(m_path); }
    void lock() override { m_lock.lock(); }
    void unlock() override { m_lock.unlock(); }

private:
    QString m_path;
    CacheLock m_lock;
};

} // namespace

std::unique_ptr<ITokenCacheStorage> createDefaultTokenCacheStorage(ICommandContext &context, TokenCacheKind kind, bool fallback)
{
    const QString dir = QDir(context.fileSystem().userDataDirectoryPath()).filePath(QStringLiteral("entra"));
    const QString lockPath = QDir(dir).filePath(kindName(kind) + QStringLiteral(".lock"));
    if (fallback) {
        return std::make_unique<FileCacheStorage>(QDir(dir).filePath(kindName(kind) + QStringLiteral(".cache")), lockPath);
    }
    return std::make_unique<SecretServiceCacheStorage>(kind, lockPath);
}

// =============================================================================================
// X509Utils.GetCertificateByThumbprint
// =============================================================================================

namespace {

QString normalizeThumbprint(const QString &thumbprint)
{
    QString t;
    for (const QChar c : thumbprint) {
        if (c.isLetterOrNumber()) {
            t += c.toUpper();
        }
    }
    return t;
}

QByteArray toDer(X509 *cert)
{
    const int len = i2d_X509(cert, nullptr);
    if (len <= 0) {
        return {};
    }
    QByteArray der(len, Qt::Uninitialized);
    auto *p = reinterpret_cast<unsigned char *>(der.data());
    i2d_X509(cert, &p);
    return der;
}

QByteArray toPem(EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    QByteArray pem;
    if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
        char *data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        pem = QByteArray(data, int(len));
    }
    BIO_free(bio);
    return pem;
}

/// Loads a PKCS#12 file without a password (as .NET writes its X509Store files on Linux).
std::optional<ServicePrincipalIdentity::Certificate> loadPkcs12(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = f.readAll();
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData());
    PKCS12 *p12 = d2i_PKCS12(nullptr, &p, data.size());
    if (!p12) {
        return std::nullopt;
    }
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
    std::optional<ServicePrincipalIdentity::Certificate> result;
    // .NET stores the files with an empty password; try that, then no password at all
    if (PKCS12_parse(p12, "", &key, &cert, nullptr) != 1 && PKCS12_parse(p12, nullptr, &key, &cert, nullptr) != 1) {
        key = nullptr;
        cert = nullptr;
    }
    if (cert) {
        ServicePrincipalIdentity::Certificate c;
        c.der = toDer(cert);
        c.thumbprint = QString::fromLatin1(QCryptographicHash::hash(c.der, QCryptographicHash::Sha1).toHex().toUpper());
        if (key) {
            c.privateKeyPem = toPem(key);
        }
        result = c;
    }
    EVP_PKEY_free(key);
    X509_free(cert);
    PKCS12_free(p12);
    ERR_clear_error();
    return result;
}

} // namespace

namespace {
GCM_DECLARE_EXCEPTION(CryptographicException, Exception)
GCM_DECLARE_EXCEPTION(PlatformNotSupportedException, NotSupportedException)
} // namespace

std::optional<ServicePrincipalIdentity::Certificate> getCertificateByThumbprint(const QString &userHomePath, const QString &thumbprint)
{
    const QString wanted = normalizeThumbprint(thumbprint);
    const QDir store(QDir(userHomePath).filePath(QStringLiteral(".dotnet/corefx/cryptography/x509stores/my")));
    // .NET names each file after the certificate's thumbprint; check that one first, then every file
    QStringList candidates{store.filePath(wanted + QStringLiteral(".pfx"))};
    const QStringList files = store.entryList({QStringLiteral("*.pfx")}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        candidates.append(store.filePath(file));
    }
    for (const QString &path : std::as_const(candidates)) {
        if (const std::optional<ServicePrincipalIdentity::Certificate> c = loadPkcs12(path); c && c->thumbprint == wanted) {
            return c;
        }
    }
    // Upstream then searches the LocalMachine "My" store, which .NET cannot open on Unix: a certificate that is not in
    // the user store always ends in this exception (never in the provider's "unable to find certificate" error).
    // (StorePal.Unix: a CryptographicException wrapping a PlatformNotSupportedException, both with this message)
    const QString message = QStringLiteral("Unix LocalMachine X509Store is limited to the Root and CertificateAuthority stores.");
    throw CryptographicException(message, std::make_shared<PlatformNotSupportedException>(message));
}

} // namespace gcm::entra
