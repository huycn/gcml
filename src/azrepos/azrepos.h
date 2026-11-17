// The Azure Repos host provider. Ports src/Microsoft.AzureRepos: AzureDevOpsConstants, UriHelpers,
// AzureDevOpsRestApi, AzureDevOpsAuthorityCache, AzureReposBindingManager, AzureReposHostProvider, and
// Diagnostics/EntraAuthenticationDiagnostic (upstream in Core; its MSAL cache helper is replaced by our cache).
#pragma once

#include "entra.h"

#include "core/application.h"
#include "core/git.h"
#include "core/hostprovider.h"

#undef GCM_STR // see core/constants.h
#ifdef GCM_DEFINE_STRINGS_AZREPOS
#define GCM_STR GCM_STR_DEFINE
#else
#define GCM_STR GCM_STR_DECLARE
#endif

namespace gcm::azrepos {

// =============================================================================================
// Constants
// =============================================================================================

namespace AzureDevOpsConstants {
    // AAD environment authority base URL
    GCM_STR(AadAuthorityBaseUrl, "https://login.microsoftonline.com");
    // Azure DevOps's app ID + default scopes
    GCM_STR(AzureDevOpsResourceId, "499b84ac-1321-427f-aa17-267ca6975798");
    extern const QStringList AzureDevOpsDefaultScopes;
    // The GCM first party application client ID
    GCM_STR(ClientId, "d735b71b-9eee-4a4f-ad23-421660877ba6");
    // Visual Studio's client ID
    GCM_STR(LegacyClientId, "872cd9fa-d31f-45e0-9eab-6e460a02d1f1");
    GCM_STR(VstsHostSuffix, ".visualstudio.com");
    GCM_STR(AzureDevOpsHost, "dev.azure.com");
    GCM_STR(VssResourceTenantHeader, "X-VSS-ResourceTenant");
    GCM_STR(PatCredentialType, "pat");
    GCM_STR(OAuthCredentialType, "oauth");
    GCM_STR(UrnScheme, "azrepos");
    GCM_STR(UrnOrgPrefix, "org");

    namespace PersonalAccessTokenScopes {
        GCM_STR(ReposWrite, "vso.code_write");
        GCM_STR(ArtifactsRead, "vso.packaging");
    } // namespace PersonalAccessTokenScopes

    namespace EnvironmentVariables {
        GCM_STR(UseSharedCache, "GCM_AZREPOS_USE_MSFT_CACHE");
        GCM_STR(UseLegacyClientId, "GCM_AZREPOS_USE_LEGACY_CLIENTID");
        GCM_STR(CredentialType, "GCM_AZREPOS_CREDENTIALTYPE");
        GCM_STR(ServicePrincipalId, "GCM_AZREPOS_SERVICE_PRINCIPAL");
        GCM_STR(ServicePrincipalSecret, "GCM_AZREPOS_SP_SECRET");
        GCM_STR(ServicePrincipalCertificateThumbprint, "GCM_AZREPOS_SP_CERT_THUMBPRINT");
        GCM_STR(ServicePrincipalCertificateSendX5C, "GCM_AZREPOS_SP_CERT_SEND_X5C");
        GCM_STR(ManagedIdentity, "GCM_AZREPOS_MANAGEDIDENTITY");
        GCM_STR(WorkloadFederation, "GCM_AZREPOS_WIF");
        GCM_STR(WorkloadFederationClientId, "GCM_AZREPOS_WIF_CLIENTID");
        GCM_STR(WorkloadFederationTenantId, "GCM_AZREPOS_WIF_TENANTID");
        GCM_STR(WorkloadFederationAudience, "GCM_AZREPOS_WIF_AUDIENCE");
        GCM_STR(WorkloadFederationAssertion, "GCM_AZREPOS_WIF_ASSERTION");
        GCM_STR(WorkloadFederationManagedIdentity, "GCM_AZREPOS_WIF_MANAGEDIDENTITY");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(UseSharedCache, "azreposUseMicrosoftSharedCache");
        GCM_STR(UseLegacyClientId, "azreposUseLegacyClientId");
        GCM_STR(CredentialType, "azreposCredentialType");
        GCM_STR(AzureAuthority, "azureAuthority");
        GCM_STR(ServicePrincipal, "azreposServicePrincipal");
        GCM_STR(ServicePrincipalSecret, "azreposServicePrincipalSecret");
        GCM_STR(ServicePrincipalCertificateThumbprint, "azreposServicePrincipalCertificateThumbprint");
        GCM_STR(ServicePrincipalCertificateSendX5C, "azreposServicePrincipalCertificateSendX5C");
        GCM_STR(ManagedIdentity, "azreposManagedIdentity");
        GCM_STR(WorkloadFederation, "azreposWorkloadFederation");
        GCM_STR(WorkloadFederationClientId, "azreposWorkloadFederationClientId");
        GCM_STR(WorkloadFederationTenantId, "azreposWorkloadFederationTenantId");
        GCM_STR(WorkloadFederationAudience, "azreposWorkloadFederationAudience");
        GCM_STR(WorkloadFederationAssertion, "azreposWorkloadFederationAssertion");
        GCM_STR(WorkloadFederationManagedIdentity, "azreposWorkloadFederationManagedIdentity");
    } // namespace GitConfiguration::Credential
} // namespace AzureDevOpsConstants

// =============================================================================================
// UriHelpers
// =============================================================================================

namespace UriHelpers {
    /// Combine two parts of a URI path element with the '/' character.
    QString combinePath(const QString &basePath, const QString &path);
    /// *.visualstudio.com
    bool isVisualStudioComHost(const OptString &host);
    bool isVisualStudioComHost(const GitRequest &request);
    /// dev.azure.com
    bool isDevAzureComHost(const OptString &host);
    bool isDevAzureComHost(const GitRequest &request);
    bool isAzureDevOpsHost(const OptString &host);
    QString getOrganizationName(const QUrl &remoteUri);
    /// The organization URI (https://dev.azure.com/{org} or https://{org}.visualstudio.com) and its name.
    /// Throws InvalidOperationException for non Azure DevOps hosts or when the organization cannot be determined.
    QUrl createOrganizationUri(const QUrl &remoteUri, QString &orgName);
    OptString getFirstPathComponent(const OptString &path);
} // namespace UriHelpers

// =============================================================================================
// REST API
// =============================================================================================

class IAzureDevOpsRestApi
{
public:
    virtual ~IAzureDevOpsRestApi() = default;
    virtual QString getAuthority(const QUrl &organizationUri) = 0;
    virtual QString createPersonalAccessToken(const QUrl &organizationUri, const QString &accessToken, const QStringList &scopes) = 0;
};

class AzureDevOpsRestApi : public IAzureDevOpsRestApi
{
public:
    explicit AzureDevOpsRestApi(ICommandContext &context) : m_context(context) {}

    /// HEAD the organization: a Bearer WWW-Authenticate authority, else the X-VSS-ResourceTenant tenant (the empty
    /// GUID = MSA => .../organizations), else .../common.
    QString getAuthority(const QUrl &organizationUri) override;
    /// Session token (PAT) creation via the identity service found through the location service.
    QString createPersonalAccessToken(const QUrl &organizationUri, const QString &accessToken, const QStringList &scopes) override;

    /// "Bearer authorization_uri=<authority>" (scheme and parameter of one WWW-Authenticate value).
    static bool tryGetAuthorityFromHeader(const OptString &header, QString &authority);
    /// The first `"<field>" : "<value>"` anywhere in the JSON text (case-insensitive), as upstream's regex.
    static bool tryGetFirstJsonStringField(const OptString &json, const QString &fieldName, QString &value);

private:
    HttpClient &httpClient();
    QUrl getIdentityServiceUri(const QUrl &organizationUri, const QString &accessToken);
    ICommandContext &m_context;
    std::optional<HttpClient> m_httpClient;
};

// =============================================================================================
// Authority cache and user bindings (Git configuration)
// =============================================================================================

class IAzureDevOpsAuthorityCache
{
public:
    virtual ~IAzureDevOpsAuthorityCache() = default;
    /// The cached authority of an organization, or nullopt.
    virtual OptString getAuthority(const QString &orgName) = 0;
    virtual void updateAuthority(const QString &orgName, const QString &authority) = 0;
    virtual void eraseAuthority(const QString &orgName) = 0;
    virtual void clear() = 0;
};

/// credential.azrepos:org/<org>.azureAuthority in the global Git configuration.
class AzureDevOpsAuthorityCache : public IAzureDevOpsAuthorityCache
{
public:
    explicit AzureDevOpsAuthorityCache(ICommandContext &context) : AzureDevOpsAuthorityCache(context.trace(), context.git()) {}
    AzureDevOpsAuthorityCache(ITrace &trace, IGit &git) : m_trace(trace), m_git(git) {}

    OptString getAuthority(const QString &orgName) override;
    void updateAuthority(const QString &orgName, const QString &authority) override;
    void eraseAuthority(const QString &orgName) override;
    void clear() override;

private:
    ITrace &m_trace;
    IGit &m_git;
};

struct AzureReposBinding
{
    /// A local binding to "" means "do not inherit the global binding".
    static const QString NoInherit;

    QString organization;
    OptString globalUserName;
    OptString localUserName;
};

class IAzureReposBindingManager
{
public:
    virtual ~IAzureReposBindingManager() = default;
    virtual std::optional<AzureReposBinding> getBinding(const QString &orgName) = 0;
    virtual void bind(const QString &orgName, const QString &userName, bool local) = 0;
    virtual void unbind(const QString &orgName, bool local) = 0;
    /// All bindings (of one organization when `orgName` is set), in the order of the global then local configuration.
    virtual QList<AzureReposBinding> getBindings(const OptString &orgName = std::nullopt) = 0;

    // AzureReposUserManagerExtensions
    /// The user to sign in with: the local binding, else the global one; null with a "no inherit" local binding.
    OptString getUser(const QString &orgName);
    /// Records a successful sign-in (see the table in the implementation).
    void signIn(const QString &orgName, const QString &userName);
    /// Forgets the user so the next sign-in asks again.
    void signOut(const QString &orgName);
};

/// credential.azrepos:org/<org>.username in the global and local Git configuration.
class AzureReposBindingManager : public IAzureReposBindingManager
{
public:
    explicit AzureReposBindingManager(ICommandContext &context) : AzureReposBindingManager(context.trace(), context.git()) {}
    AzureReposBindingManager(ITrace &trace, IGit &git) : m_trace(trace), m_git(git) {}

    std::optional<AzureReposBinding> getBinding(const QString &orgName) override;
    void bind(const QString &orgName, const QString &userName, bool local) override;
    void unbind(const QString &orgName, bool local) override;
    QList<AzureReposBinding> getBindings(const OptString &orgName = std::nullopt) override;

private:
    ITrace &m_trace;
    IGit &m_git;
};

// =============================================================================================
// Host provider
// =============================================================================================

class AzureReposHostProvider : public IHostProvider, public IConfigurableComponent, public ICommandProvider, public IDiagnosticProvider
{
public:
    using EntraAuthFactory = std::function<std::shared_ptr<entra::IEntraAuthentication>(const entra::PublicClientConfig &)>;

    explicit AzureReposHostProvider(ICommandContext &context);
    AzureReposHostProvider(ICommandContext &context, std::shared_ptr<IAzureDevOpsRestApi> azDevOps,
                           std::shared_ptr<entra::IEntraAuthentication> entraAuth, std::shared_ptr<IAzureDevOpsAuthorityCache> authorityCache,
                           std::shared_ptr<IAzureReposBindingManager> bindingManager);
    AzureReposHostProvider(ICommandContext &context, std::shared_ptr<IAzureDevOpsRestApi> azDevOps, EntraAuthFactory entraAuthFactory,
                           std::shared_ptr<IAzureDevOpsAuthorityCache> authorityCache, std::shared_ptr<IAzureReposBindingManager> bindingManager);

    // IHostProvider
    QString id() const override { return QStringLiteral("azure-repos"); }
    QString name() const override { return QStringLiteral("Azure Repos"); }
    QStringList supportedAuthorityIds() const override { return entra::EntraAuthentication::AuthorityIds; }
    bool isSupported(const GitRequest &request) override;
    /// Azure DevOps Server (TFS) is handled by the generic provider (basic auth and WIA detection).
    bool isSupported(const HttpResponse *) override { return false; }
    GitResponse getCredential(const GitRequest &request) override;
    void storeCredential(const GitRequest &request) override;
    void eraseCredential(const GitRequest &request) override;

    // IConfigurableComponent: credential.https://dev.azure.com.useHttpPath = true
    QString componentName() const override { return QStringLiteral("Azure Repos provider"); }
    void configure(ConfigurationTarget target) override;
    void unconfigure(ConfigurationTarget target) override;

    // ICommandProvider: azure-repos list | bind | unbind | clear-cache
    std::shared_ptr<Command> createCommand() override;

    // IDiagnosticProvider: the Entra authentication diagnostic
    QList<std::shared_ptr<IDiagnostic>> getDiagnostics() override;

    // Public for tests (internal upstream)
    bool getUseSharedCache();
    bool usePersonalAccessTokens();
    static bool tryGetAuthorityFromHeaders(const QStringList &headers, QString &authority);

private:
    GitResponse getUserCredential(const GitRequest &request);
    static bool isClientConfigException(const std::exception &ex);
    void throwIfUnsafeRemote(const GitRequest &request);
    Credential generatePersonalAccessToken(const GitRequest &request);
    entra::EntraAuthResult getEntraAccessToken(const GitRequest &request);
    entra::PublicClientConfig getEntraConfig();
    std::pair<QString, bool> getClientAppInfo();
    entra::IEntraAuthentication &entraAuth();
    static QString getServiceName(const QUrl &remoteUri);
    static OptString getAccountNameForCredentialQuery(const GitRequest &request);
    bool useServicePrincipal(entra::ServicePrincipalIdentity &sp);
    bool useManagedIdentity(std::optional<entra::ManagedIdentity> &mid);
    bool useWorkloadFederation(entra::WorkloadFederationOptions &fedOpts);
    int listCmd(const OptString &organization, bool showRemotes, bool verbose);
    int bindCmd(const QString &organization, const QString &userName, bool local);
    int unbindCmd(const QString &organization, bool local);

    ICommandContext &m_context;
    std::shared_ptr<IAzureDevOpsRestApi> m_azDevOps;
    std::shared_ptr<IAzureDevOpsAuthorityCache> m_authorityCache;
    std::shared_ptr<IAzureReposBindingManager> m_bindingManager;
    EntraAuthFactory m_entraAuthFactory;
    std::shared_ptr<entra::IEntraAuthentication> m_entraAuth;
};

/// EntraAuthenticationDiagnostic: the location of the token caches and a persistence test of each.
class EntraAuthenticationDiagnostic : public Diagnostic
{
public:
    explicit EntraAuthenticationDiagnostic(ICommandContext &context, entra::TokenCacheStorageFactory storageFactory = nullptr)
        : Diagnostic(QStringLiteral("Microsoft Entra authentication"), context), m_storageFactory(std::move(storageFactory)) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;

private:
    entra::TokenCacheStorageFactory m_storageFactory;
};

} // namespace gcm::azrepos
