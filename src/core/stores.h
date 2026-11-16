// The Linux credential backing stores. Ports PlaintextCredentialStore.cs, Interop/Posix/GpgPassCredentialStore.cs,
// Gpg.cs, CredentialCacheStore.cs, NullCredentialStore.cs and Interop/Linux/SecretServiceCollection.cs.
#pragma once

#include "core/credential.h"
#include "core/platform.h"

namespace gcm {

class IGit;

/// A credential read from a file of the plaintext or `pass` store.
struct FileCredential
{
    QString fullPath;
    QString service;
    OptString account;
    QString password;
};

/// One file per credential under <root>/<namespace>/<scheme>/<host>[:port]/<path>/<account>.credential:
/// the password on the first line, then `service=` and `account=` lines.
class PlaintextCredentialStore : public ICredentialStore
{
public:
    PlaintextCredentialStore(IFileSystem &fileSystem, QString storeRoot, OptString ns = std::nullopt);

    QString name() override { return QStringLiteral("Plaintext"); }
    QStringList getAccounts(const OptString &service) override;
    std::optional<Credential> get(const OptString &service, const OptString &account) override;
    void addOrUpdate(const QString &service, const QString &account, const QString &secret) override;
    bool remove(const OptString &service, const OptString &account) override;

protected:
    virtual QString credentialFileExtension() const { return QStringLiteral(".credential"); }
    virtual bool tryDeserializeCredential(const QString &path, FileCredential &credential);
    virtual void serializeCredential(const FileCredential &credential);
    /// Parses "<password>\n<key=value lines>" (shared with the `pass` store).
    static bool tryParseCredentialText(const QString &path, const QString &text, FileCredential &credential);

    IFileSystem &m_fileSystem;
    QString m_storeRoot;
    OptString m_namespace;

private:
    QList<FileCredential> enumerate(const OptString &service, const OptString &account);
    void ensureStoreRoot();
    QString createServiceSlug(const OptString &service) const;
};

/// GNU `pass` compatible store: the same layout with GPG-encrypted `.gpg` files and `.gpg-id` recipients.
class GpgPassCredentialStore : public PlaintextCredentialStore
{
public:
    static inline const QString PasswordStoreDirEnvar = QStringLiteral("PASSWORD_STORE_DIR");

    GpgPassCredentialStore(IFileSystem &fileSystem, IGpg &gpg, QString storeRoot, OptString ns = std::nullopt);

    QString name() override { return QStringLiteral("GPG/Pass"); }

protected:
    QString credentialFileExtension() const override { return QStringLiteral(".gpg"); }
    bool tryDeserializeCredential(const QString &path, FileCredential &credential) override;
    void serializeCredential(const FileCredential &credential) override;

private:
    QString getGpgId(const QString &credentialFullPath);
    IGpg &m_gpg;
};

/// Runs `gpg` to encrypt and decrypt files.
class Gpg : public IGpg
{
public:
    Gpg(QString gpgPath, ISessionManager &sessionManager, IProcessManager &processManager);

    QString decryptFile(const QString &path) override;
    void encryptFile(const QString &path, const QString &gpgId, const QString &contents) override;

private:
    void prepareEnvironment(ProcessStartInfo &psi) const;
    QString m_gpgPath;
    ISessionManager &m_sessionManager;
    IProcessManager &m_processManager;
};

/// Git's in-memory credential cache (`git credential-cache`).
class CredentialCacheStore : public ICredentialStore
{
public:
    CredentialCacheStore(IGit &git, const OptString &options);

    QString name() override { return QStringLiteral("Git Credential Cache"); }
    QStringList getAccounts(const OptString &service) override;
    std::optional<Credential> get(const OptString &service, const OptString &account) override;
    void addOrUpdate(const QString &service, const QString &account, const QString &secret) override;
    bool remove(const OptString &service, const OptString &account) override;

private:
    IGit &m_git;
    QString m_options;
};

class NullCredentialStore : public ICredentialStore
{
public:
    QString name() override { return QStringLiteral("No-op"); }
    QStringList getAccounts(const OptString &) override { return {}; }
    std::optional<Credential> get(const OptString &, const OptString &) override { return std::nullopt; }
    void addOrUpdate(const QString &, const QString &, const QString &) override {}
    bool remove(const OptString &, const OptString &) override { return false; }
};

/// The freedesktop.org Secret Service through libsecret, with upstream's schema and attributes.
class SecretServiceCollection : public ICredentialStore
{
public:
    enum class Operation {
        ConnectBus,
        OpenService,
        Search,
        Unlock,
        Store,
        Erase,
        Load,
    };

    explicit SecretServiceCollection(OptString ns);

    QString name() override { return QStringLiteral("freedesktop.org Secret Service"); }
    QStringList getAccounts(const OptString &service) override;
    std::optional<Credential> get(const OptString &service, const OptString &account) override;
    void addOrUpdate(const QString &service, const QString &account, const QString &secret) override;
    bool remove(const OptString &service, const OptString &account) override;

    /// The exception for a failed operation, with a hint for well-known errors (public for tests).
    static InteropException createException(Operation operation, const OptString &errorDomain, int errorCode,
                                            const OptString &errorMessage);

private:
    QList<std::pair<OptString, Credential>> enumerate(const OptString &service, const OptString &account);
    QString createServiceName(const QString &service) const;
    OptString m_namespace;
};

} // namespace gcm
