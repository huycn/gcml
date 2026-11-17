// Microsoft Entra ID authentication: the port of Core/Authentication/Entra/*. Upstream builds on MSAL
// (Microsoft.Identity.Client), which has no C++ equivalent on Linux; this is an Entra client of our own speaking the
// same protocols:
//
//  - public client (user sign-in): silent renewal with cached refresh tokens, interactive sign-in through the system
//    browser (authorization code + PKCE, loopback redirect) or the device code flow;
//  - confidential client: service principal with a client secret or a certificate (RS256 client assertion signed
//    with OpenSSL), workload identity federation (generic assertion, managed identity, GitHub Actions OIDC token);
//  - managed identity: IMDS, App Service, Cloud Shell and Azure Arc endpoints;
//  - a token cache of our own (MSAL's cache is not read), kept in the Secret Service with a plaintext file fallback,
//    as upstream's MSAL cache on Linux.
//
// The authentication broker (WAM / Mac SSO / Linux identity broker) is not available: MSAL needs its native runtime
// for it. It is reported as "unavailable", as MSAL does when no broker is installed.
#pragma once

#include "core/auth/authbase.h"
#include "core/constants.h"

#include <QDateTime>

namespace gcm::entra {

// =============================================================================================
// Errors (the MsalException hierarchy)
// =============================================================================================

/// MsalException: an error with an error code (e.g. "invalid_client").
class EntraException : public Exception
{
public:
    EntraException(QString errorCode, QString message) : Exception(std::move(message)), m_errorCode(std::move(errorCode)) {}
    const QString &errorCode() const noexcept { return m_errorCode; }
    const char *typeName() const noexcept override { return "MsalException"; }

private:
    QString m_errorCode;
};

/// MsalClientException: a failure on the client side (configuration, cancelled browser flow, ...).
class EntraClientException : public EntraException
{
public:
    using EntraException::EntraException;
    const char *typeName() const noexcept override { return "MsalClientException"; }
};

/// MsalServiceException: an error answered by the identity service.
class EntraServiceException : public EntraException
{
public:
    EntraServiceException(QString errorCode, QString message, int statusCode = 0, OptString claims = std::nullopt)
        : EntraException(std::move(errorCode), std::move(message)), m_statusCode(statusCode), m_claims(std::move(claims)) {}
    int statusCode() const noexcept { return m_statusCode; }
    const OptString &claims() const noexcept { return m_claims; }
    /// MSAL: request_timeout / service_not_available, HTTP 408, 429 and 5xx except 501 and 505.
    bool isRetryable() const noexcept;
    const char *typeName() const noexcept override { return "MsalServiceException"; }

private:
    int m_statusCode;
    OptString m_claims;
};

/// MsalUiRequiredException: the silent flow cannot complete; the user must interact.
class EntraUiRequiredException : public EntraServiceException
{
public:
    using EntraServiceException::EntraServiceException;
    const char *typeName() const noexcept override { return "MsalUiRequiredException"; }
};

/// MsalThrottledServiceException
class EntraThrottledServiceException : public EntraServiceException
{
public:
    using EntraServiceException::EntraServiceException;
    const char *typeName() const noexcept override { return "MsalThrottledServiceException"; }
};

/// IntuneAppProtectionPolicyRequiredException
class IntuneAppProtectionPolicyRequiredException : public EntraServiceException
{
public:
    using EntraServiceException::EntraServiceException;
    const char *typeName() const noexcept override { return "IntuneAppProtectionPolicyRequiredException"; }
};

/// MsalError constants used by GCM.
namespace EntraError {
    GCM_STR(InvalidClient, "invalid_client");
    GCM_STR(UnauthorizedClient, "unauthorized_client");
    GCM_STR(InvalidGrant, "invalid_grant");
    GCM_STR(InteractionRequired, "interaction_required");
    GCM_STR(AuthenticationCanceled, "authentication_canceled");
    GCM_STR(AccessDenied, "access_denied");
    GCM_STR(InvalidRequest, "invalid_request");
    GCM_STR(CodeExpired, "code_expired");
    GCM_STR(RedirectUriValidationFailed, "redirect_uri_validation_failed");
    GCM_STR(RequestTimeout, "request_timeout");
    GCM_STR(ServiceNotAvailable, "service_not_available");
    GCM_STR(ManagedIdentityRequestFailed, "managed_identity_request_failed");
    GCM_STR(ManagedIdentityUnreachableNetwork, "managed_identity_unreachable_network");
    GCM_STR(InvalidManagedIdentityResponse, "invalid_managed_identity_response");
    GCM_STR(UserAssignedManagedIdentityNotSupported, "user_assigned_managed_identity_not_supported");
} // namespace EntraError

// =============================================================================================
// Accounts, results, identities
// =============================================================================================

/// IEntraAccount / EntraAccount. Both fields compare case-insensitively (as MSAL's AccountId and UPNs).
struct EntraAccount
{
    /// Opaque, stable identifier ("<object id>.<tenant id>"); survives UPN renames.
    OptString homeAccountId;
    /// User principal name (typically an email address); suitable for display.
    OptString userName;

    bool operator==(const EntraAccount &other) const;
};

/// IEntraAuthenticationResult
struct EntraAuthResult
{
    QString accessToken;
    /// Absent for managed identities and confidential clients.
    std::optional<EntraAccount> account;
};

/// Public client application configuration (PublicClientConfig).
struct PublicClientConfig
{
    /// Application (client) ID of the Entra application registration.
    QString clientId;
    /// Use legacy Microsoft Account (MSA) passthrough. Microsoft first-party applications only.
    bool isMsaPassthroughEnabled = false;
    /// Use the shared Microsoft Developer Tools identity cache. Recorded in TRACE2 only: this port keeps its own cache.
    bool useSharedCache = false;
    bool supportsWindowsBroker = false;
    bool supportsMacBroker = false;
    bool supportsLinuxBroker = false;
};

enum class InteractionMode {
    Auto = 0,
    EmbeddedWebView,
    SystemWebView,
    DeviceCode,
};

/// "Auto", "Embedded web view", "System web view", "Device code"
QString displayName(InteractionMode mode);
/// The lower-case enum name used in TRACE2 and the helper protocol ("auto", "systemwebview", ...).
QString modeName(InteractionMode mode);

/// ServicePrincipalIdentity
struct ServicePrincipalIdentity
{
    QString id;
    QString tenantId;
    /// A certificate with its private key (PEM), found by X509Utils::getCertificateByThumbprint.
    struct Certificate
    {
        QString thumbprint;
        /// DER encoded certificate.
        QByteArray der;
        /// PEM encoded private key (PKCS#8).
        QByteArray privateKeyPem;
    };
    std::optional<Certificate> certificate;
    OptString clientSecret;
    bool sendX5C = false;
};

/// ManagedIdentity: "system", "{uuid}" / "id://{uuid}" (user-assigned client ID) or "resource://{uuid}".
class ManagedIdentity
{
public:
    enum class Kind { System, ClientId, ResourceId };

    static const ManagedIdentity &system();
    static bool tryCreate(const OptString &id, ManagedIdentity &mi);
    /// Throws ArgumentException("Invalid managed identity id '<id>' (Parameter 'id')").
    static ManagedIdentity create(const QString &id);

    /// "system", "id://<lower-case uuid>" or "resource://<lower-case uuid>"
    const QString &id() const { return m_id; }
    Kind kind() const { return m_kind; }
    /// The user-assigned client or resource ID (empty for the system identity).
    const QString &value() const { return m_value; }
    bool operator==(const ManagedIdentity &other) const { return m_id == other.m_id; }

private:
    ManagedIdentity(QString id, Kind kind, QString value) : m_id(std::move(id)), m_kind(kind), m_value(std::move(value)) {}
    QString m_id;
    Kind m_kind = Kind::System;
    QString m_value;
};

enum class WorkloadFederationScenario {
    /// Federate via pre-computed client assertion.
    Generic,
    /// Federate via an access token for an Entra ID Managed Identity.
    ManagedIdentity,
    /// Federate via a GitHub Actions OIDC token.
    GitHubActions,
};

/// "Generic", "ManagedIdentity", "GitHubActions"
QString scenarioName(WorkloadFederationScenario scenario);

class WorkloadFederationOptions
{
public:
    static inline const QString DefaultAudience = Constants::DefaultWorkloadFederationAudience;

    WorkloadFederationScenario scenario = WorkloadFederationScenario::Generic;
    QString tenantId;
    QString clientId;
    /// A null audience means the default.
    void setAudience(const OptString &audience) { m_audience = audience.value_or(DefaultAudience); }
    const QString &audience() const { return m_audience; }
    QString genericClientAssertion;
    QString managedIdentityId;
    QUrl gitHubTokenRequestUrl;
    QString gitHubTokenRequestToken;

private:
    QString m_audience = DefaultAudience;
};

/// X509Utils.GetCertificateByThumbprint: .NET's CurrentUser\My store on Linux is the directory
/// ~/.dotnet/corefx/cryptography/x509stores/my of PKCS#12 files (no password). When not found it throws, like .NET then opening the LocalMachine store on Unix.
std::optional<ServicePrincipalIdentity::Certificate> getCertificateByThumbprint(const QString &userHomePath, const QString &thumbprint);

// =============================================================================================
// Token cache
// =============================================================================================

/// Where the serialized token cache lives: the Secret Service, or a plaintext file as fallback.
class ITokenCacheStorage
{
public:
    virtual ~ITokenCacheStorage() = default;
    /// The serialized cache, empty when there is none. Throws on access failure.
    virtual QByteArray load() = 0;
    virtual void save(const QByteArray &data) = 0;
    /// A short description for traces and `diagnose` ("Secret Service item '...'", "file '...'").
    virtual QString description() const = 0;
    /// Serialize load-modify-save sequences between GCM processes (a lock file next to the cache).
    virtual void lock() {}
    virtual void unlock() {}
};

/// The user cache (public clients) or the application cache (confidential clients).
enum class TokenCacheKind { User, App };

/// Creates the storage for a cache. `fallback` selects the plaintext file (used when the Secret Service fails).
using TokenCacheStorageFactory = std::function<std::unique_ptr<ITokenCacheStorage>(TokenCacheKind kind, bool fallback)>;

/// The default storages: Secret Service items "Git Credential Manager Entra token cache (user|app)", or the files
/// <userData>/entra/user.cache and <userData>/entra/app.cache (mode 0600).
std::unique_ptr<ITokenCacheStorage> createDefaultTokenCacheStorage(ICommandContext &context, TokenCacheKind kind, bool fallback);

/// The in-memory model of a token cache (serialized as JSON).
struct TokenCache
{
    struct Account
    {
        QString homeAccountId;
        QString environment;
        QString tenantId;
        QString userName;
    };
    struct RefreshToken
    {
        QString homeAccountId;
        QString environment;
        QString clientId;
        QString secret;
    };
    struct AccessToken
    {
        /// Empty for application tokens.
        QString homeAccountId;
        QString environment;
        QString clientId;
        QString tenantId;
        /// Requested scopes, space separated, lower case, sorted.
        QString scopes;
        QString secret;
        qint64 expiresOn = 0;
    };
    QList<Account> accounts;
    QList<RefreshToken> refreshTokens;
    QList<AccessToken> accessTokens;

    static TokenCache fromJson(const QByteArray &json);
    QByteArray toJson() const;
};

// =============================================================================================
// IEntraAuthentication / EntraAuthentication
// =============================================================================================

class IEntraAuthentication
{
public:
    virtual ~IEntraAuthentication() = default;
    /// The public client configuration; null when public client APIs cannot be called.
    virtual const std::optional<PublicClientConfig> &publicClientConfig() const = 0;
    virtual InteractionMode getInteractionMode() = 0;
    /// The user accounts that have signed in to the public client application before.
    virtual QList<EntraAccount> getUserAccounts() = 0;
    virtual bool removeUserAccount(const EntraAccount &account) = 0;
    virtual EntraAuthResult getTokenForUser(const QStringList &scopes, const OptString &authority = std::nullopt,
                                            const std::optional<EntraAccount> &account = std::nullopt,
                                            InteractionMode interactionMode = InteractionMode::Auto) = 0;
    virtual EntraAuthResult getTokenForServicePrincipal(const QStringList &scopes, const ServicePrincipalIdentity &sp) = 0;
    virtual EntraAuthResult getTokenForManagedIdentity(const QString &resource, const ManagedIdentity &mi) = 0;
    virtual EntraAuthResult getTokenUsingWorkloadFederation(const QStringList &scopes, const WorkloadFederationOptions &fedOpts) = 0;
};

class EntraAuthentication : public AuthenticationBase, public IEntraAuthentication
{
public:
    static inline const QStringList AuthorityIds = {
        QStringLiteral("msa"),  QStringLiteral("microsoft"),   QStringLiteral("microsoftaccount"),
        QStringLiteral("aad"),  QStringLiteral("azure"),       QStringLiteral("azuredirectory"),
        QStringLiteral("live"), QStringLiteral("liveconnect"), QStringLiteral("liveid"),
    };

    /// The cloud instance of the default authority.
    static inline const QString DefaultAuthority = QStringLiteral("https://login.microsoftonline.com/common");

    explicit EntraAuthentication(ICommandContext &context, std::optional<PublicClientConfig> publicClientConfig = std::nullopt,
                                 TokenCacheStorageFactory cacheStorageFactory = nullptr);
    ~EntraAuthentication() override;

    const std::optional<PublicClientConfig> &publicClientConfig() const override { return m_publicClientConfig; }
    InteractionMode getInteractionMode() override;
    QList<EntraAccount> getUserAccounts() override;
    bool removeUserAccount(const EntraAccount &account) override;
    EntraAuthResult getTokenForUser(const QStringList &scopes, const OptString &authority = std::nullopt,
                                    const std::optional<EntraAccount> &account = std::nullopt,
                                    InteractionMode interactionMode = InteractionMode::Auto) override;
    EntraAuthResult getTokenForServicePrincipal(const QStringList &scopes, const ServicePrincipalIdentity &sp) override;
    EntraAuthResult getTokenForManagedIdentity(const QString &resource, const ManagedIdentity &mi) override;
    EntraAuthResult getTokenUsingWorkloadFederation(const QStringList &scopes, const WorkloadFederationOptions &fedOpts) override;

    /// The user preference for the authentication broker (GCM_MSAUTH_USEBROKER / credential.msauthUseBroker,
    /// default true). Public for tests (internal upstream).
    bool isBrokerEnabled();

    /// Test seam: the code generator of the authorization code flow (PKCE verifier, state).
    void setCodeGenerator(std::shared_ptr<IOAuth2CodeGenerator> generator);
    /// Test seam: the browser of the authorization code flow (default: the system browser with a loopback listener).
    void setWebBrowser(std::shared_ptr<IOAuth2WebBrowser> browser);
    /// Test seam: the clock (seconds since the epoch) used for token expiry.
    void setClock(std::function<qint64()> clock);

private:
    std::optional<PublicClientConfig> m_publicClientConfig;
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace gcm::entra
