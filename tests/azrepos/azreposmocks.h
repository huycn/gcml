// Hand-written stand-ins for the Moq mocks of the Azure Repos tests. Strict: a call that was not set up (or with other
// arguments) throws, which the code under test sees like a failure -- as Moq's MockBehavior.Strict does. The identity
// methods of the Entra mock are loose (upstream uses default Moq mocks with It.IsAny for them): they record the call
// and answer the configured result.
#pragma once

#include "azrepos/azrepos.h"
#include "core/strings.h"

namespace gcm::azrepos::test {

[[noreturn]] inline void unexpectedCall(const char *what)
{
    throw Exception(QStringLiteral("unexpected call: %1").arg(QLatin1String(what)));
}

class MockAzureDevOpsRestApi : public IAzureDevOpsRestApi
{
public:
    /// organization URI (absoluteUri) -> authority
    QList<std::pair<QString, QString>> Authorities;
    /// (organization URI, access token) -> PAT
    QList<std::tuple<QString, QString, QString>> Pats;
    QStringList GetAuthorityCalls;
    QList<QStringList> PatScopes;

    QString getAuthority(const QUrl &organizationUri) override
    {
        const QString uri = absoluteUri(organizationUri);
        GetAuthorityCalls.append(uri);
        for (const auto &[setupUri, authority] : std::as_const(Authorities)) {
            if (setupUri == uri) {
                return authority;
            }
        }
        unexpectedCall("getAuthority");
    }

    QString createPersonalAccessToken(const QUrl &organizationUri, const QString &accessToken, const QStringList &scopes) override
    {
        const QString uri = absoluteUri(organizationUri);
        for (const auto &[setupUri, setupToken, pat] : std::as_const(Pats)) {
            if (setupUri == uri && setupToken == accessToken) {
                PatScopes.append(scopes);
                return pat;
            }
        }
        unexpectedCall("createPersonalAccessToken");
    }
};

class MockAzureDevOpsAuthorityCache : public IAzureDevOpsAuthorityCache
{
public:
    /// getAuthority setups: organization (nullopt: any) -> result
    QList<std::pair<OptString, OptString>> Authorities;
    /// Expected updateAuthority calls (organization, authority)
    QList<std::pair<QString, QString>> ExpectedUpdates;
    QList<std::pair<QString, QString>> Updates;
    bool AllowErase = false;
    QStringList Erased;
    bool AllowClear = false;
    int ClearCalls = 0;

    OptString getAuthority(const QString &orgName) override
    {
        for (const auto &[org, authority] : std::as_const(Authorities)) {
            if (!org || *org == orgName) {
                return authority;
            }
        }
        unexpectedCall("getAuthority");
    }
    void updateAuthority(const QString &orgName, const QString &authority) override
    {
        if (!ExpectedUpdates.contains(std::pair{orgName, authority})) {
            unexpectedCall("updateAuthority");
        }
        Updates.append({orgName, authority});
    }
    void eraseAuthority(const QString &orgName) override
    {
        if (!AllowErase) {
            unexpectedCall("eraseAuthority");
        }
        Erased.append(orgName);
    }
    void clear() override
    {
        if (!AllowClear) {
            unexpectedCall("clear");
        }
        ++ClearCalls;
    }
};

class MockAzureReposBindingManager : public IAzureReposBindingManager
{
public:
    /// getBinding setups: organization -> binding (nullopt: no binding)
    QList<std::pair<QString, std::optional<AzureReposBinding>>> Bindings;
    /// Recorded bind/unbind calls, when allowed: "bind <org> <user> <local>", "unbind <org> <local>"
    bool AllowWrites = false;
    QStringList Calls;
    std::optional<QList<AzureReposBinding>> AllBindings;

    std::optional<AzureReposBinding> getBinding(const QString &orgName) override
    {
        for (const auto &[org, binding] : std::as_const(Bindings)) {
            if (org == orgName) {
                return binding;
            }
        }
        unexpectedCall("getBinding");
    }
    void bind(const QString &orgName, const QString &userName, bool local) override
    {
        if (!AllowWrites) {
            unexpectedCall("bind");
        }
        Calls.append(QStringLiteral("bind %1 %2 %3").arg(orgName, userName, local ? QStringLiteral("local") : QStringLiteral("global")));
    }
    void unbind(const QString &orgName, bool local) override
    {
        if (!AllowWrites) {
            unexpectedCall("unbind");
        }
        Calls.append(QStringLiteral("unbind %1 %2").arg(orgName, local ? QStringLiteral("local") : QStringLiteral("global")));
    }
    QList<AzureReposBinding> getBindings(const OptString &orgName) override
    {
        if (!AllBindings) {
            unexpectedCall("getBindings");
        }
        QList<AzureReposBinding> result;
        for (const AzureReposBinding &b : std::as_const(*AllBindings)) {
            if (!orgName || b.organization.compare(*orgName, Qt::CaseInsensitive) == 0) {
                result.append(b);
            }
        }
        return result;
    }
};

struct GetTokenForUserCall
{
    QStringList scopes;
    OptString authority;
    std::optional<entra::EntraAccount> account;
    entra::InteractionMode mode;
    bool operator==(const GetTokenForUserCall &) const = default;
};

class MockEntraAuthentication : public entra::IEntraAuthentication
{
public:
    std::optional<entra::PublicClientConfig> Config;
    std::optional<QList<entra::EntraAccount>> Accounts;
    /// getTokenForUser setups: expected call -> result (or the failure to throw)
    QList<std::pair<GetTokenForUserCall, std::function<entra::EntraAuthResult()>>> UserTokens;
    QList<GetTokenForUserCall> UserTokenCalls;

    // Loose identity methods: the result, and the recorded arguments
    entra::EntraAuthResult IdentityResult;
    QList<std::pair<QString, entra::ManagedIdentity>> ManagedIdentityCalls;
    QList<std::pair<QStringList, entra::ServicePrincipalIdentity>> ServicePrincipalCalls;
    QList<std::pair<QStringList, entra::WorkloadFederationOptions>> WorkloadFederationCalls;

    void setupTokenForUser(const QStringList &scopes, const OptString &authority, const std::optional<entra::EntraAccount> &account,
                           std::function<entra::EntraAuthResult()> result)
    {
        UserTokens.append({GetTokenForUserCall{scopes, authority, account, entra::InteractionMode::Auto}, std::move(result)});
    }
    int tokenForUserCalls(const QStringList &scopes, const OptString &authority, const std::optional<entra::EntraAccount> &account) const
    {
        return int(UserTokenCalls.count(GetTokenForUserCall{scopes, authority, account, entra::InteractionMode::Auto}));
    }

    const std::optional<entra::PublicClientConfig> &publicClientConfig() const override { return Config; }
    entra::InteractionMode getInteractionMode() override { unexpectedCall("getInteractionMode"); }
    QList<entra::EntraAccount> getUserAccounts() override
    {
        if (!Accounts) {
            unexpectedCall("getUserAccounts");
        }
        return *Accounts;
    }
    bool removeUserAccount(const entra::EntraAccount &) override { unexpectedCall("removeUserAccount"); }
    entra::EntraAuthResult getTokenForUser(const QStringList &scopes, const OptString &authority, const std::optional<entra::EntraAccount> &account,
                                           entra::InteractionMode interactionMode) override
    {
        const GetTokenForUserCall call{scopes, authority, account, interactionMode};
        UserTokenCalls.append(call);
        for (const auto &[setup, result] : std::as_const(UserTokens)) {
            if (setup == call) {
                return result();
            }
        }
        unexpectedCall("getTokenForUser");
    }
    entra::EntraAuthResult getTokenForServicePrincipal(const QStringList &scopes, const entra::ServicePrincipalIdentity &sp) override
    {
        ServicePrincipalCalls.append({scopes, sp});
        return IdentityResult;
    }
    entra::EntraAuthResult getTokenForManagedIdentity(const QString &resource, const entra::ManagedIdentity &mi) override
    {
        ManagedIdentityCalls.append({resource, mi});
        return IdentityResult;
    }
    entra::EntraAuthResult getTokenUsingWorkloadFederation(const QStringList &scopes, const entra::WorkloadFederationOptions &fedOpts) override
    {
        WorkloadFederationCalls.append({scopes, fedOpts});
        return IdentityResult;
    }
};

inline entra::EntraAuthResult createAuthResult(const QString &upn, const QString &token)
{
    return entra::EntraAuthResult{token, entra::EntraAccount{std::nullopt, upn}};
}

} // namespace gcm::azrepos::test
