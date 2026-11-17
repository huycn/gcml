// The Bitbucket host provider (Bitbucket Cloud and Bitbucket Data Center / Server). Ports src/Atlassian.Bitbucket:
// constants, BitbucketHelper, the REST APIs and OAuth2 clients of Cloud and Data Center with their registries,
// BitbucketTokenEndpointResponseJson, BitbucketAuthentication and BitbucketHostProvider.
#pragma once

#include "core/auth/authentication.h"
#include "core/hostprovider.h"

#undef GCM_STR // see core/constants.h
#ifdef GCM_DEFINE_STRINGS_BITBUCKET
#define GCM_STR GCM_STR_DEFINE
#else
#define GCM_STR GCM_STR_DECLARE
#endif

namespace gcm::bitbucket {

// =============================================================================================
// Constants
// =============================================================================================

enum class AuthenticationModes : int {
    None = 0,
    Basic = 1,
    OAuth = 1 << 1,
    All = Basic | OAuth,
};

constexpr AuthenticationModes operator|(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) | int(b)); }
constexpr AuthenticationModes operator&(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) & int(b)); }
constexpr AuthenticationModes operator~(AuthenticationModes a) { return AuthenticationModes(~int(a) & int(AuthenticationModes::All)); }
constexpr AuthenticationModes &operator|=(AuthenticationModes &a, AuthenticationModes b) { return a = a | b; }
constexpr bool hasFlag(AuthenticationModes value, AuthenticationModes flag) { return (int(value) & int(flag)) != 0; }

/// Enum.ToString of the flags enum: "None", "Basic", "OAuth", "All".
QString modesToString(AuthenticationModes modes);
/// Enum.TryParse(value, ignoreCase: true): comma-separated names or numbers.
bool tryParseAuthenticationModes(const QString &value, AuthenticationModes &modes);

namespace BitbucketConstants {
    GCM_STR(Id, "bitbucket");
    GCM_STR(Name, "Bitbucket");
    GCM_STR(DefaultAuthenticationHelper, "Atlassian.Bitbucket.UI");

    namespace EnvironmentVariables {
        GCM_STR(AuthenticationHelper, "GCM_BITBUCKET_HELPER");
        GCM_STR(AuthenticationModes, "GCM_BITBUCKET_AUTHMODES");
        GCM_STR(AlwaysRefreshCredentials, "GCM_BITBUCKET_ALWAYS_REFRESH_CREDENTIALS");
        GCM_STR(ValidateStoredCredentials, "GCM_BITBUCKET_VALIDATE_STORED_CREDENTIALS");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(AuthenticationHelper, "bitbucketHelper");
        GCM_STR(AuthenticationModes, "bitbucketAuthModes");
        GCM_STR(AlwaysRefreshCredentials, "bitbucketAlwaysRefreshCredentials");
        GCM_STR(ValidateStoredCredentials, "bitbucketValidateStoredCredentials");
    } // namespace GitConfiguration::Credential

    namespace HelpUrls {
        GCM_STR(DataCenterPasswordReset, "/passwordreset");
        GCM_STR(DataCenterLogin, "/login");
        GCM_STR(PasswordReset, "https://bitbucket.org/account/password/reset/");
        GCM_STR(SignUp, "https://bitbucket.org/account/signup/");
        GCM_STR(TwoFactor, "https://support.atlassian.com/bitbucket-cloud/docs/enable-two-step-verification/");
    } // namespace HelpUrls
} // namespace BitbucketConstants

namespace cloud::CloudConstants {
    GCM_STR(BitbucketBaseUrlHost, "bitbucket.org");
    GCM_STR(BitbucketApiUri, "https://api.bitbucket.org");
    // Sourcetree's values, as GCM for Windows (upstream: "use the GCM client ID and secret once we have this approved")
    GCM_STR(OAuth2ClientId, "HJdmKXV87DsmC9zSWB");
    GCM_STR(OAuth2ClientSecret, "wwWw47VB9ZHwMsD4Q4rAveHkbxNrMp3n");
    GCM_STR(OAuth2RedirectUri, "http://localhost:34106/");
    GCM_STR(OAuth2AuthorizationEndpoint, "https://bitbucket.org/site/oauth2/authorize");
    GCM_STR(OAuth2TokenEndpoint, "https://bitbucket.org/site/oauth2/access_token");

    namespace OAuthScopes {
        GCM_STR(RepositoryWrite, "repository:write");
        GCM_STR(Account, "account");
    } // namespace OAuthScopes

    constexpr AuthenticationModes DotOrgAuthenticationModes = AuthenticationModes::Basic | AuthenticationModes::OAuth;

    namespace EnvironmentVariables {
        GCM_STR(OAuthClientId, "GCM_BITBUCKET_CLOUD_CLIENTID");
        GCM_STR(OAuthClientSecret, "GCM_BITBUCKET_CLOUD_CLIENTSECRET");
        GCM_STR(OAuthRedirectUri, "GCM_BITBUCKET_CLOUD_OAUTH_REDIRECTURI");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(OAuthClientId, "cloudOAuthClientId");
        GCM_STR(OAuthClientSecret, "cloudOAuthClientSecret");
        GCM_STR(OAuthRedirectUri, "cloudOauthRedirectUri");
    } // namespace GitConfiguration::Credential
} // namespace cloud::CloudConstants

namespace datacenter::DataCenterConstants {
    namespace OAuthScopes {
        GCM_STR(PublicRepos, "PUBLIC_REPOS");
        GCM_STR(RepoWrite, "REPO_WRITE");
        GCM_STR(RepoRead, "REPO_READ");
    } // namespace OAuthScopes

    GCM_STR(OAuth2RedirectUri, "http://localhost:34106/");
    constexpr AuthenticationModes ServerAuthenticationModes = AuthenticationModes::Basic | AuthenticationModes::OAuth;
    GCM_STR(OAuthUserName, "OAUTH_USERNAME");

    namespace EnvironmentVariables {
        GCM_STR(OAuthClientId, "GCM_BITBUCKET_DATACENTER_CLIENTID");
        GCM_STR(OAuthClientSecret, "GCM_BITBUCKET_DATACENTER_CLIENTSECRET");
        GCM_STR(OAuthRedirectUri, "GCM_BITBUCKET_DATACENTER_OAUTH_REDIRECTURI");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(OAuthClientId, "bitbucketDataCenterOAuthClientId");
        GCM_STR(OAuthClientSecret, "bitbucketDataCenterOAuthClientSecret");
        GCM_STR(OAuthRedirectUri, "bitbucketDataCenterOauthRedirectUri");
    } // namespace GitConfiguration::Credential
} // namespace datacenter::DataCenterConstants

// =============================================================================================
// BitbucketHelper
// =============================================================================================

namespace BitbucketHelper {
    /// "<scheme>://<host>:<port>[/<first path segment>]" -- the base of a Data Center instance (with its context path).
    QString getBaseUri(const QUrl &remoteUri);
    bool isBitbucketOrg(const GitRequest &request);
    bool isBitbucketOrg(const QUrl &targetUri);
    bool isBitbucketOrg(const OptString &targetHost);
} // namespace BitbucketHelper

// =============================================================================================
// REST APIs
// =============================================================================================

enum class AuthenticationMethod {
    BasicAuth,
    Sso,
};

/// RestApiResult<IUserInfo>: the status of the call and, on success, the user name.
struct UserInfoResult
{
    int statusCode = 0;
    OptString userName;
    /// 200..299
    bool succeeded() const { return statusCode > 199 && statusCode < 300; }
};

class IBitbucketRestApi
{
public:
    virtual ~IBitbucketRestApi() = default;
    virtual UserInfoResult getUserInformation(const OptString &userName, const QString &password, bool isBearerToken) = 0;
    virtual bool isOAuthInstalled() = 0;
    virtual QList<AuthenticationMethod> getAuthenticationMethods() = 0;
};

/// IRegistry<T>: the Cloud or the Data Center flavour for a request.
template <typename T> class IRegistry
{
public:
    virtual ~IRegistry() = default;
    virtual std::shared_ptr<T> get(const GitRequest &request) = 0;
};

namespace cloud {
/// GET https://api.bitbucket.org/2.0/user. OAuth is always installed; there is no API for the login methods.
class BitbucketRestApi : public IBitbucketRestApi
{
public:
    explicit BitbucketRestApi(ICommandContext &context) : m_context(context) {}
    UserInfoResult getUserInformation(const OptString &userName, const QString &password, bool isBearerToken) override;
    bool isOAuthInstalled() override { return true; }
    /// Determined later by attempting authenticated requests: always empty.
    QList<AuthenticationMethod> getAuthenticationMethods() override { return {}; }

private:
    ICommandContext &m_context;
    std::optional<HttpClient> m_httpClient;
};
} // namespace cloud

namespace datacenter {
/// The REST API of Bitbucket Data Center / Server below `<base>/rest/` of the remote URI in the settings.
class BitbucketRestApi : public IBitbucketRestApi
{
public:
    explicit BitbucketRestApi(ICommandContext &context) : m_context(context) {}
    /// Validates the credentials with GET api/1.0/users; Data Center cannot tell the user name, so the result
    /// carries the placeholder OAUTH_USERNAME.
    UserInfoResult getUserInformation(const OptString &userName, const QString &password, bool isBearerToken) override;
    /// GET oauth2/1.0/client answers 401 anonymously when the OAuth plugin is installed.
    bool isOAuthInstalled() override;
    /// LOGIN_FORM => BasicAuth, IDP => Sso (GET authconfig/1.0/login-options).
    QList<AuthenticationMethod> getAuthenticationMethods() override;

private:
    QUrl apiUri() const;
    HttpClient &httpClient();
    ICommandContext &m_context;
    std::optional<HttpClient> m_httpClient;
};
} // namespace datacenter

class BitbucketRestApiRegistry : public IRegistry<IBitbucketRestApi>
{
public:
    explicit BitbucketRestApiRegistry(ICommandContext &context) : m_context(context) {}
    std::shared_ptr<IBitbucketRestApi> get(const GitRequest &request) override;

private:
    ICommandContext &m_context;
    std::shared_ptr<IBitbucketRestApi> m_cloudApi;
    std::shared_ptr<IBitbucketRestApi> m_dataCenterApi;
};

// =============================================================================================
// OAuth2 clients
// =============================================================================================

/// Parses a token endpoint response the Bitbucket way: the non-standard "scopes" property (not "scope") carries
/// the scopes; property names are matched exactly.
bool tryParseBitbucketTokenResponse(const QString &json, OAuth2TokenResult &result);

class BitbucketOAuth2Client : public OAuth2Client
{
public:
    /// The configuration of a client, read in upstream's order (endpoints, client id, redirect URI, secret).
    struct Config
    {
        OAuth2ServerEndpoints endpoints;
        QString clientId;
        QUrl redirectUri;
        OptString clientSecret;
    };
    /// The client id and secret go in the request body (no Authorization header).
    BitbucketOAuth2Client(HttpClient httpClient, Config config);

    virtual QStringList scopes() const = 0;
    /// "<scheme>://<host>[:port]/refresh_token": the refresh token key never includes the path of the remote.
    QString getRefreshTokenServiceName(const GitRequest &request) const;

    using OAuth2Client::getAuthorizationCode;
    OAuth2AuthorizationCodeResult getAuthorizationCode(IOAuth2WebBrowser &browser, const CancellationToken &ct)
    {
        return getAuthorizationCode(scopes(), browser, ct);
    }

protected:
    bool tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result) override;
};

namespace cloud {
class BitbucketOAuth2Client : public bitbucket::BitbucketOAuth2Client
{
public:
    BitbucketOAuth2Client(HttpClient httpClient, ISettings &settings);
    /// repository:write, account
    QStringList scopes() const override;
};
} // namespace cloud

namespace datacenter {
class BitbucketOAuth2Client : public bitbucket::BitbucketOAuth2Client
{
public:
    /// Throws ArgumentException when the client id or secret is not configured, or the settings carry no remote URI.
    BitbucketOAuth2Client(HttpClient httpClient, ISettings &settings);
    /// PUBLIC_REPOS, REPO_READ, REPO_WRITE
    QStringList scopes() const override;
};
} // namespace datacenter

class OAuth2ClientRegistry : public IRegistry<bitbucket::BitbucketOAuth2Client>
{
public:
    explicit OAuth2ClientRegistry(ICommandContext &context) : m_context(context) {}
    std::shared_ptr<bitbucket::BitbucketOAuth2Client> get(const GitRequest &request) override;

private:
    HttpClient &httpClient();
    ICommandContext &m_context;
    std::optional<HttpClient> m_httpClient;
    std::shared_ptr<bitbucket::BitbucketOAuth2Client> m_cloudClient;
    std::shared_ptr<bitbucket::BitbucketOAuth2Client> m_dataCenterClient;
};

// =============================================================================================
// Authentication (prompts)
// =============================================================================================

struct CredentialsPromptResult
{
    AuthenticationModes authenticationMode = AuthenticationModes::None;
    /// Basic only.
    std::optional<Credential> credential;
};

class IBitbucketAuthentication
{
public:
    virtual ~IBitbucketAuthentication() = default;
    virtual CredentialsPromptResult getCredentials(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) = 0;
    virtual OAuth2TokenResult createOAuthCredentials(const GitRequest &request) = 0;
    virtual OAuth2TokenResult refreshOAuthCredentials(const GitRequest &request, const QString &refreshToken) = 0;
    virtual QString getRefreshTokenServiceName(const GitRequest &request) = 0;
};

/// Graphical prompts go to a helper speaking the Atlassian.Bitbucket.UI protocol: GCM_BITBUCKET_HELPER /
/// credential.bitbucketHelper (default "Atlassian.Bitbucket.UI") when present, else the `bitbucket` command of
/// git-credential-manager-ui. Without a helper, or without a desktop session, the terminal is used.
class BitbucketAuthentication : public AuthenticationBase, public IBitbucketAuthentication
{
public:
    static const QStringList AuthorityIds;

    explicit BitbucketAuthentication(ICommandContext &context);
    BitbucketAuthentication(ICommandContext &context, std::shared_ptr<IRegistry<bitbucket::BitbucketOAuth2Client>> oauth2ClientRegistry);

    CredentialsPromptResult getCredentials(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override;
    OAuth2TokenResult createOAuthCredentials(const GitRequest &request) override;
    OAuth2TokenResult refreshOAuthCredentials(const GitRequest &request, const QString &refreshToken) override;
    QString getRefreshTokenServiceName(const GitRequest &request) override;

protected:
    /// The Bitbucket helper: command and argument prefix (ending in a space when non-empty). Virtual for tests.
    virtual bool tryFindBitbucketHelper(QString &command, QString &args);

private:
    CredentialsPromptResult getCredentialsViaHelper(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes,
                                                    const QString &command, const QString &args);
    CredentialsPromptResult getCredentialsViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes);

    std::shared_ptr<IRegistry<bitbucket::BitbucketOAuth2Client>> m_oauth2ClientRegistry;
};

// =============================================================================================
// Host provider
// =============================================================================================

class BitbucketHostProvider : public IHostProvider
{
public:
    explicit BitbucketHostProvider(ICommandContext &context);
    BitbucketHostProvider(ICommandContext &context, std::shared_ptr<IBitbucketAuthentication> bitbucketAuth,
                          std::shared_ptr<IRegistry<IBitbucketRestApi>> restApiRegistry);

    QString id() const override { return BitbucketConstants::Id; }
    QString name() const override { return BitbucketConstants::Name; }
    QStringList supportedAuthorityIds() const override { return BitbucketAuthentication::AuthorityIds; }
    bool isSupported(const GitRequest &request) override;
    bool isSupported(const HttpResponse *response) override;
    GitResponse getCredential(const GitRequest &request) override;
    /// Access tokens and passwords are stored alike; secrets longer than the store's maximum are split into chunks.
    void storeCredential(const GitRequest &request) override;
    /// The refresh token is kept: a bad one is replaced on the next `get`.
    void eraseCredential(const GitRequest &request) override;

    AuthenticationModes getSupportedAuthenticationModes(const GitRequest &request);

    // Public for tests (internal upstream)
    static QString getChunkAccount(const QString &account, int chunkIndex);
    static QString getChunkSecret(int chunkCount);
    static QString getChunkServiceName(const QString &service);
    static QString getServiceName(const QUrl &remoteUri);
    static QString getRefreshTokenServiceName(const QUrl &remoteUri);

private:
    std::optional<Credential> getStoredCredentials(const GitRequest &request, AuthenticationModes authModes);
    Credential getRefreshedCredentials(const GitRequest &request, AuthenticationModes authModes);
    Credential resolveCredsAndStoreRefreshToken(const GitRequest &request, const OAuth2TokenResult &tokenSet);
    QString resolveOAuthUserName(const GitRequest &request, const QString &accessToken);
    QString resolveBasicAuthUserName(const GitRequest &request, const OptString &userName, const QString &password);
    bool validateCredentialsWork(const GitRequest &request, const Credential &credentials, AuthenticationModes authModes);
    void addOrUpdateCredential(const QString &service, const QString &account, const QString &secret);
    std::optional<Credential> getCredential(const QString &service, const OptString &account);

    ICommandContext &m_context;
    std::shared_ptr<IBitbucketAuthentication> m_bitbucketAuth;
    std::shared_ptr<IRegistry<IBitbucketRestApi>> m_restApiRegistry;
};

} // namespace gcm::bitbucket
