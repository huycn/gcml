// The GitHub host provider. Ports src/GitHub: GitHubConstants, AuthenticationResult, GitHubAuthChallenge,
// GitHubRestApi, GitHubOAuth2Client, GitHubAuthentication, GitHubHostProvider (+ .Commands) and
// Diagnostics/GitHubApiDiagnostic.
#pragma once

#include "core/application.h"
#include "core/auth/authbase.h"
#include "core/auth/oauth2.h"
#include "core/hostprovider.h"

namespace gcm::github {

// =============================================================================================
// Constants
// =============================================================================================

namespace GitHubConstants {
    GCM_STR(GitHubBaseUrlHost, "github.com");
    GCM_STR(GistBaseUrlHost, "gist.github.com");
    GCM_STR(ProximaUrlHostSuffix, ".ghe.com");
    GCM_STR(DefaultAuthenticationHelper, "GitHub.UI");

    // https://github.com/settings/connections/applications/0120e057bd645470c1ed
    GCM_STR(OAuthClientId, "0120e057bd645470c1ed");
    // OAuth2 public client application 'secrets' are required and permitted to be public
    GCM_STR(OAuthClientSecret, "18867509d956965542b521a529a79bb883344c90");
    GCM_STR(OAuthRedirectUri, "http://127.0.0.1/");       // Note that the trailing slash is important!
    GCM_STR(OAuthLegacyRedirectUri, "http://localhost/"); // Note that the trailing slash is important!
    GCM_STR(OAuthAuthorizationEndpointRelativeUri, "/login/oauth/authorize");
    GCM_STR(OAuthTokenEndpointRelativeUri, "/login/oauth/access_token");
    GCM_STR(OAuthDeviceEndpointRelativeUri, "/login/device/code");

    inline const QStringList InvalidUnderscoreLogins = {QStringLiteral("pj_nitin"), QStringLiteral("up_the_irons")};

    GCM_STR(GitHubApiAcceptsHeaderValue, "application/vnd.github.v3+json");
    GCM_STR(GitHubOptHeader, "X-GitHub-OTP");
    GCM_STR(MinimumOnPremOAuthVersion, "3.2");

    namespace TokenScopes {
        GCM_STR(Gist, "gist");
        GCM_STR(Repo, "repo");
    } // namespace TokenScopes

    namespace OAuthScopes {
        GCM_STR(Gist, "gist");
        GCM_STR(Repo, "repo");
        GCM_STR(Workflow, "workflow");
    } // namespace OAuthScopes

    namespace EnvironmentVariables {
        GCM_STR(AuthenticationHelper, "GCM_GITHUB_HELPER");
        GCM_STR(AuthenticationModes, "GCM_GITHUB_AUTHMODES");
        GCM_STR(DevOAuthClientId, "GCM_DEV_GITHUB_CLIENTID");
        GCM_STR(DevOAuthClientSecret, "GCM_DEV_GITHUB_CLIENTSECRET");
        GCM_STR(DevOAuthRedirectUri, "GCM_DEV_GITHUB_REDIRECTURI");
        GCM_STR(AccountFiltering, "GCM_GITHUB_ACCOUNTFILTERING");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(AuthenticationHelper, "gitHubHelper");
        GCM_STR(AuthenticationModes, "gitHubAuthModes");
        GCM_STR(DevOAuthClientId, "gitHubDevClientId");
        GCM_STR(DevOAuthClientSecret, "gitHubDevClientSecret");
        GCM_STR(DevOAuthRedirectUri, "gitHubDevRedirectUri");
        GCM_STR(AccountFiltering, "githubAccountFiltering");
    } // namespace GitConfiguration::Credential
} // namespace GitHubConstants

// =============================================================================================
// Authentication modes ([Flags] enum AuthenticationModes)
// =============================================================================================

enum class AuthenticationModes : int {
    None = 0,
    Basic = 1,
    Browser = 1 << 1,
    Pat = 1 << 2,
    Device = 1 << 3,
    OAuth = Browser | Device,
    All = Basic | OAuth | Pat,
};

constexpr AuthenticationModes operator|(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) | int(b)); }
constexpr AuthenticationModes operator&(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) & int(b)); }
constexpr AuthenticationModes operator~(AuthenticationModes a) { return AuthenticationModes(~int(a) & int(AuthenticationModes::All)); }
constexpr AuthenticationModes &operator|=(AuthenticationModes &a, AuthenticationModes b) { return a = a | b; }
constexpr bool hasFlag(AuthenticationModes value, AuthenticationModes flag) { return (int(value) & int(flag)) != 0; }

/// Enum.ToString of the flags enum: "None", "Pat", "Basic, Pat", "Pat, OAuth", "All", ...
QString modesToString(AuthenticationModes modes);
/// Enum.TryParse(value, ignoreCase: true): comma-separated names or numbers.
bool tryParseAuthenticationModes(const QString &value, AuthenticationModes &modes);

// =============================================================================================
// REST API
// =============================================================================================

enum class GitHubAuthenticationResultType {
    Success,
    Failure,
    TwoFactorApp,
    TwoFactorSms,
};

struct AuthenticationResult
{
    GitHubAuthenticationResultType type = GitHubAuthenticationResultType::Failure;
    OptString token;
};

struct GitHubUserInfo
{
    QString login;
};

struct GitHubMetaInfo
{
    OptString installedVersion;
    bool verifiablePasswordAuthentication = false;
};

class IGitHubRestApi
{
public:
    virtual ~IGitHubRestApi() = default;

    virtual AuthenticationResult createPersonalAccessToken(const QUrl &targetUri, const OptString &userName, const OptString &password, const OptString &authenticationCode, const QStringList &scopes) = 0;
    virtual GitHubUserInfo getUserInfo(const QUrl &targetUri, const QString &accessToken) = 0;
    virtual GitHubMetaInfo getMetaInfo(const QUrl &targetUri) = 0;

    /// GitHubRestApiExtensions.CreatePersonalTokenAsync
    AuthenticationResult createPersonalToken(const QUrl &targetUri, const std::optional<Credential> &credentials, const OptString &authenticationCode, const QStringList &scopes)
    {
        return createPersonalAccessToken(
            targetUri,
            credentials ? OptString(credentials->account) : std::nullopt,
            credentials ? OptString(credentials->password) : std::nullopt,
            authenticationCode,
            scopes
        );
    }
};

class GitHubRestApi : public IGitHubRestApi
{
public:
    /// The maximum wait time for a network request before timing out
    static constexpr int RequestTimeoutMs = 15 * 1000;

    explicit GitHubRestApi(ICommandContext &context) : m_context(context) {}

    AuthenticationResult createPersonalAccessToken(const QUrl &targetUri, const OptString &userName, const OptString &password, const OptString &authenticationCode, const QStringList &scopes) override;
    GitHubUserInfo getUserInfo(const QUrl &targetUri, const QString &accessToken) override;
    GitHubMetaInfo getMetaInfo(const QUrl &targetUri) override;

    /// api.github.com for GitHub.com, api.<sub>.ghe.com for Proxima, <host>/api/v3 for GitHub Enterprise Server
    /// (without a leading raw. or gist.).
    static QUrl getApiRequestUri(const QUrl &targetUri, const QString &apiUrl);

private:
    HttpClient &httpClient();
    AuthenticationResult parseSuccessResponse(const QUrl &targetUri, const HttpResponse &response);
    AuthenticationResult parseUnauthorizedResponse(const QUrl &targetUri, const OptString &authenticationCode, const HttpResponse &response);
    AuthenticationResult parseForbiddenResponse(const QUrl &targetUri, const OptString &password, const HttpResponse &response);

    ICommandContext &m_context;
    std::optional<HttpClient> m_httpClient;
};

// =============================================================================================
// WWW-Authenticate challenge
// =============================================================================================

class GitHubAuthChallenge
{
public:
    GitHubAuthChallenge() = default;
    GitHubAuthChallenge(OptString domain, OptString enterprise) : m_domain(std::move(domain)), m_enterprise(std::move(enterprise)) {}

    /// One challenge per `Basic realm="GitHub" ...` header (case-insensitive); domain_hint / enterprise_hint parsed.
    static QList<GitHubAuthChallenge> fromHeaders(const QStringList &headers);

    const OptString &domain() const { return m_domain; }
    const OptString &enterprise() const { return m_enterprise; }

    /// True when the user name belongs to the challenge's domain: `<name>_<shortcode>` for EMU accounts, names
    /// without a shortcode (or the known logins with an underscore) for the public domain.
    bool isDomainMember(const OptString &userName) const;

    /// Case-insensitive comparison of domain and enterprise.
    bool operator==(const GitHubAuthChallenge &other) const;

private:
    OptString m_domain;
    OptString m_enterprise;
};

// =============================================================================================
// OAuth2 client
// =============================================================================================

class GitHubOAuth2Client : public OAuth2Client
{
public:
    GitHubOAuth2Client(HttpClient httpClient, ISettings &settings, const QUrl &baseUri);

protected:
    /// The GitHub auth server returns 'scope' as a comma-separated list rather than the space-separated
    /// format of RFC 6749 section 3.3.
    bool tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result) override;
};

// =============================================================================================
// Authentication (prompts)
// =============================================================================================

struct AuthenticationPromptResult
{
    AuthenticationModes authenticationMode = AuthenticationModes::None;
    std::optional<Credential> credential;
};

class IGitHubAuthentication
{
public:
    virtual ~IGitHubAuthentication() = default;

    /// The chosen account, or nullopt to add a new account.
    virtual OptString selectAccount(const QUrl &targetUri, const QStringList &accounts) = 0;
    virtual AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) = 0;
    virtual QString getTwoFactorCode(const QUrl &targetUri, bool isSms) = 0;
    virtual OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes, const OptString &loginHint) = 0;
    virtual OAuth2TokenResult getOAuthTokenViaDeviceCode(const QUrl &targetUri, const QStringList &scopes) = 0;
};

/// Graphical prompts go to a helper speaking the GitHub.UI protocol: GCM_GITHUB_HELPER / credential.gitHubHelper
/// (default "GitHub.UI") when present, else the `github` commands of git-credential-manager-ui. Without a helper,
/// or without a desktop session, the terminal is used.
class GitHubAuthentication : public AuthenticationBase, public IGitHubAuthentication
{
public:
    static inline const QStringList AuthorityIds = {QStringLiteral("github")};

    explicit GitHubAuthentication(ICommandContext &context) : AuthenticationBase(context) {}

    OptString selectAccount(const QUrl &targetUri, const QStringList &accounts) override;
    AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override;
    QString getTwoFactorCode(const QUrl &targetUri, bool isSms) override;
    OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes, const OptString &loginHint) override;
    OAuth2TokenResult getOAuthTokenViaDeviceCode(const QUrl &targetUri, const QStringList &scopes) override;

protected:
    /// The GitHub helper: command and argument prefix (ending in a space when non-empty).
    bool tryFindGitHubHelper(QString &command, QString &args);

private:
    AuthenticationPromptResult getAuthenticationViaHelper(const QUrl &targetUri, OptString userName, AuthenticationModes modes,
                                                          const QString &command, const QString &args);
    AuthenticationPromptResult getAuthenticationViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes);
    HttpClient &httpClient();

    std::optional<HttpClient> m_httpClient;
};

// =============================================================================================
// Host provider
// =============================================================================================

class GitHubHostProvider : public IHostProvider, public ICommandProvider, public IDiagnosticProvider
{
public:
    explicit GitHubHostProvider(ICommandContext &context);
    GitHubHostProvider(ICommandContext &context, std::shared_ptr<IGitHubRestApi> gitHubApi,
                       std::shared_ptr<IGitHubAuthentication> gitHubAuth);

    QString id() const override { return QStringLiteral("github"); }
    QString name() const override { return QStringLiteral("GitHub"); }
    QStringList supportedAuthorityIds() const override { return GitHubAuthentication::AuthorityIds; }
    bool isSupported(const GitRequest &request) override;
    bool isSupported(const HttpResponse *response) override;
    GitResponse getCredential(const GitRequest &request) override;
    void storeCredential(const GitRequest &request) override;
    void eraseCredential(const GitRequest &request) override;

    // ICommandProvider: github list | login | logout
    std::shared_ptr<Command> createCommand() override;
    // IDiagnosticProvider
    QList<std::shared_ptr<IDiagnostic>> getDiagnostics() override;

    /// Public for tests (internal upstream).
    Credential generateCredential(const QUrl &remoteUri, const OptString &userName);
    AuthenticationModes getSupportedAuthenticationModes(const QUrl &targetUri);
    static QString getServiceName(const GitRequest &request);

    static bool isGitHubDotCom(const OptString &targetUrl);
    static bool isGitHubDotCom(const QUrl &targetUri);
    static bool isGitHubProxima(const QUrl &targetUri, QString *subdomain = nullptr);
    /// gist.<host> -> https://<host> (gists share the credentials of the main host).
    static QUrl normalizeUri(const QUrl &uri);

private:
    static QString getServiceName(const QUrl &baseUri);
    bool filterAccounts(const QUrl &remoteUri, const QStringList &wwwAuth, QStringList &accounts);
    Credential generateOAuthCredential(const QUrl &targetUri, const OptString &loginHint, bool useBrowser);
    Credential generatePersonalAccessToken(const QUrl &targetUri, const Credential &credentials);
    int listAccounts(const OptString &url);
    int addAccount(const OptString &url, OptString userName, bool device, bool browser, const OptString &token, bool force);
    int removeAccount(const QString &account, const OptString &url);

    ICommandContext &m_context;
    std::shared_ptr<IGitHubRestApi> m_gitHubApi;
    std::shared_ptr<IGitHubAuthentication> m_gitHubAuth;
};

class GitHubApiDiagnostic : public Diagnostic
{
public:
    GitHubApiDiagnostic(std::shared_ptr<IGitHubRestApi> api, ICommandContext &context)
        : Diagnostic(QStringLiteral("GitHub API"), context), m_api(std::move(api)) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;

private:
    std::shared_ptr<IGitHubRestApi> m_api;
};

} // namespace gcm::github
