// Credentials and credential stores. Ports Credential.cs, ICredentialStore.cs, CredentialStore.cs and the
// Linux-capable backing stores (secretservice, gpg/pass, cache, plaintext, none).
#pragma once

#include "core/common.h"

namespace gcm {

class ICommandContext;

/// A username/secret pair (ICredential / GitCredential upstream).
struct Credential
{
    QString account;
    QString password;

    bool operator==(const Credential &) const = default;
};

class ICredentialStore
{
public:
    virtual ~ICredentialStore() = default;

    virtual QString name() = 0;
    /// Maximum secret length in characters; <= 0 means unlimited.
    virtual int maxCredentialSize() { return 0; }
    /// nullopt service matches every service.
    virtual QStringList getAccounts(const OptString &service) = 0;
    /// nullopt service/account match everything. Returns the first match.
    virtual std::optional<Credential> get(const OptString &service, const OptString &account) = 0;
    virtual void addOrUpdate(const QString &service, const QString &account, const QString &secret) = 0;
    virtual bool remove(const OptString &service, const OptString &account) = 0;
};

/// The `gpg` operations GpgPassCredentialStore needs (IGpg upstream).
class IGpg
{
public:
    virtual ~IGpg() = default;

    virtual QString decryptFile(const QString &path) = 0;
    virtual void encryptFile(const QString &path, const QString &gpgId, const QString &contents) = 0;
};

/// Selects and lazily creates the backing store from GCM_CREDENTIAL_STORE / credential.credentialStore.
/// Linux has no default store: an unset/unknown value throws the upstream help text listing the options.
class CredentialStore : public ICredentialStore
{
public:
    explicit CredentialStore(ICommandContext &context);
    ~CredentialStore() override;

    QString name() override;
    int maxCredentialSize() override;
    QStringList getAccounts(const OptString &service) override;
    std::optional<Credential> get(const OptString &service, const OptString &account) override;
    void addOrUpdate(const QString &service, const QString &account, const QString &secret) override;
    bool remove(const OptString &service, const OptString &account) override;

private:
    void ensureBackingStore();
    ICommandContext &m_context;
    std::unique_ptr<IGpg> m_gpg;
    std::unique_ptr<ICredentialStore> m_backingStore;
};

} // namespace gcm
