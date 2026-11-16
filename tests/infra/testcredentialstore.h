// Port of TestInfrastructure/Objects/TestCredentialStore.cs (same query semantics as upstream's fake:
// get/getAccounts match the service exactly, remove treats nullopt as a wildcard).
#pragma once

#include "core/credential.h"
#include "core/strings.h"

namespace gcm::test {

struct TestCredential
{
    QString service;
    QString account;
    QString password;
    Credential toCredential() const { return {account, password}; }
};

class TestCredentialStore : public ICredentialStore
{
public:
    int MaxCredentialSize = 0;

    // ICredentialStore
    QString name() override { return QStringLiteral("Test Credential Store"); }
    int maxCredentialSize() override { return MaxCredentialSize; }
    QStringList getAccounts(const OptString &service) override
    {
        QStringList accounts;
        for (const QList<TestCredential> credentials = query(service, std::nullopt); const TestCredential &c : credentials) {
            if (!accounts.contains(c.account)) {
                accounts.append(c.account);
            }
        }
        return accounts;
    }
    std::optional<Credential> get(const OptString &service, const OptString &account) override
    {
        if (const auto c = tryGet(service, account)) {
            return c->toCredential();
        }
        return std::nullopt;
    }
    void addOrUpdate(const QString &service, const QString &account, const QString &secret) override
    {
        add(service, account, secret);
    }
    bool remove(const OptString &service, const OptString &account) override
    {
        for (qsizetype i = 0; i < m_store.size(); ++i) {
            if ((!service || m_store[i].service == *service) && (!account || m_store[i].account == *account)) {
                m_store.removeAt(i);
                return true;
            }
        }
        return false;
    }

    // Test helpers
    qsizetype count() const { return m_store.size(); }
    std::optional<TestCredential> tryGet(const OptString &service, const OptString &account) const
    {
        const QList<TestCredential> matches = query(service, account);
        if (matches.isEmpty()) {
            return std::nullopt;
        }
        return matches.first();
    }
    void add(const QString &service, const TestCredential &credential)
    {
        TestCredential c = credential;
        c.service = service;
        if (TestCredential *existing = find(service, c.account)) {
            *existing = c;
        } else {
            m_store.append(c);
        }
    }
    TestCredential add(const QString &service, const QString &account, const QString &secret)
    {
        const TestCredential c{service, account, secret};
        add(service, c);
        return c;
    }
    bool contains(const OptString &service, const OptString &account) const { return tryGet(service, account).has_value(); }
    const QList<TestCredential> &credentials() const { return m_store; }

private:
    TestCredential *find(const QString &service, const QString &account)
    {
        for (TestCredential &c : m_store) {
            if (c.service == service && c.account == account) {
                return &c;
            }
        }
        return nullptr;
    }
    QList<TestCredential> query(const OptString &service, const OptString &account) const
    {
        QList<TestCredential> result;
        if (!service) {
            return result; // upstream's fake never stores a null service
        }
        if (isNullOrWhiteSpace(account)) {
            // All credentials of the service
            for (const TestCredential &c : m_store) {
                if (c.service == *service) {
                    result.append(c);
                }
            }
        }
        // The credential matching both service and account
        if (account) {
            for (const TestCredential &c : m_store) {
                if (c.service == *service && c.account == *account) {
                    result.append(c);
                }
            }
        }
        return result;
    }

    QList<TestCredential> m_store;
};

} // namespace gcm::test
