// The GitLab host provider. Ports src/GitLab: GitLabConstants, GitLabOAuth2Client, GitLabAuthentication and
// GitLabHostProvider.
#pragma once

#include "core/auth/authentication.h"
#include "core/hostprovider.h"

#undef GCM_STR // see core/constants.h
#ifdef GCM_DEFINE_STRINGS_GITLAB
#define GCM_STR GCM_STR_DEFINE
#else
#define GCM_STR GCM_STR_DECLARE
#endif

namespace gcm::gitlab {

// =============================================================================================
// Constants
// =============================================================================================

namespace GitLabConstants {
    GCM_STR(GitLabDotCom, "https://gitlab.com");
    GCM_STR(DefaultAuthenticationHelper, "GitLab.UI");

    // Owned by https://gitlab.com/gitcredentialmanager
    GCM_STR(OAuthClientId, "172b9f227872b5dde33f4d9b1db06a6a5515ae79508e7a00c973c85ce490671e");
    GCM_STR(OAuthRedirectUri, "http://127.0.0.1/");
    // https://docs.gitlab.com/ee/api/oauth2.html#authorization-code-flow
    GCM_STR(OAuthAuthorizationEndpointRelativeUri, "/oauth/authorize");
    GCM_STR(OAuthTokenEndpointRelativeUri, "/oauth/token");

    namespace EnvironmentVariables {
        GCM_STR(DevOAuthClientId, "GCM_DEV_GITLAB_CLIENTID");
        GCM_STR(DevOAuthClientSecret, "GCM_DEV_GITLAB_CLIENTSECRET");
        GCM_STR(DevOAuthRedirectUri, "GCM_DEV_GITLAB_REDIRECTURI");
        GCM_STR(AuthenticationModes, "GCM_GITLAB_AUTHMODES");
        GCM_STR(AuthenticationHelper, "GCM_GITLAB_HELPER");
    } // namespace EnvironmentVariables

    namespace GitConfiguration::Credential {
        GCM_STR(AuthenticationModes, "gitLabAuthModes");
        GCM_STR(DevOAuthClientId, "gitLabDevClientId");
        GCM_STR(DevOAuthClientSecret, "gitLabDevClientSecret");
        GCM_STR(DevOAuthRedirectUri, "gitLabDevRedirectUri");
        GCM_STR(AuthenticationHelper, "gitLabHelper");
    } // namespace GitConfiguration::Credential

    namespace HelpUrls {
        GCM_STR(GitLab, "https://aka.ms/gcm/gitlab");
    } // namespace HelpUrls

    /// The host is gitlab.com (case-insensitive).
    bool isGitLabDotCom(const QUrl &uri);
    bool isGitLabDotComClientId(const QString &clientId);
} // namespace GitLabConstants

// =============================================================================================
// Authentication modes ([Flags] enum AuthenticationModes)
// =============================================================================================

enum class AuthenticationModes : int {
    None = 0,
    Basic = 1,
    Browser = 1 << 1,
    Pat = 1 << 2,
    All = Basic | Browser | Pat,
};

constexpr AuthenticationModes operator|(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) | int(b)); }
constexpr AuthenticationModes operator&(AuthenticationModes a, AuthenticationModes b) { return AuthenticationModes(int(a) & int(b)); }
constexpr AuthenticationModes operator~(AuthenticationModes a) { return AuthenticationModes(~int(a) & int(AuthenticationModes::All)); }
constexpr AuthenticationModes &operator|=(AuthenticationModes &a, AuthenticationModes b) { return a = a | b; }
constexpr bool hasFlag(AuthenticationModes value, AuthenticationModes flag) { return (int(value) & int(flag)) != 0; }

/// Enum.ToString of the flags enum: "None", "Basic, Pat", "All", ...
QString modesToString(AuthenticationModes modes);
/// Enum.TryParse(value, ignoreCase: true): comma-separated names or numbers.
bool tryParseAuthenticationModes(const QString &value, AuthenticationModes &modes);

/// The modes GitLab.com supports.
constexpr AuthenticationModes DotComAuthenticationModes = AuthenticationModes::All;

// =============================================================================================
// OAuth2 client
// =============================================================================================

class GitLabOAuth2Client : public OAuth2Client
{
public:
    GitLabOAuth2Client(HttpClient httpClient, ISettings &settings, const QUrl &baseUri);

    /// GCM_DEV_GITLAB_CLIENTID / credential.gitLabDevClientId, else GitLab.com's client id.
    static QString getClientId(ISettings &settings);
};

// =============================================================================================
// Authentication (prompts)
// =============================================================================================

struct AuthenticationPromptResult
{
    AuthenticationModes authenticationMode = AuthenticationModes::None;
    /// Basic and Pat; the account may be null for a token entered without a user name.
    std::optional<Credential> credential;
};

class IGitLabAuthentication
{
public:
    virtual ~IGitLabAuthentication() = default;
    virtual AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) = 0;
    virtual OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes) = 0;
    virtual OAuth2TokenResult getOAuthTokenViaRefresh(const QUrl &targetUri, const QString &refreshToken) = 0;
};

/// Graphical prompts go to a helper speaking the GitLab.UI protocol: GCM_GITLAB_HELPER / credential.gitLabHelper
/// (default "GitLab.UI") when present, else the `gitlab` command of git-credential-manager-ui. Without a helper,
/// or without a desktop session, the terminal is used.
class GitLabAuthentication : public AuthenticationBase, public IGitLabAuthentication
{
public:
    explicit GitLabAuthentication(ICommandContext &context) : AuthenticationBase(context) {}

    AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override;
    OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes) override;
    OAuth2TokenResult getOAuthTokenViaRefresh(const QUrl &targetUri, const QString &refreshToken) override;

protected:
    /// The GitLab helper: command and argument prefix (ending in a space when non-empty).
    bool tryFindGitLabHelper(QString &command, QString &args);

private:
    AuthenticationPromptResult getAuthenticationViaHelper(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes,
                                                          const QString &command, const QString &args);
    AuthenticationPromptResult getAuthenticationViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes);
    HttpClient &httpClient();

    std::optional<HttpClient> m_httpClient;
};

// =============================================================================================
// Host provider
// =============================================================================================

class GitLabHostProvider : public HostProvider
{
public:
    explicit GitLabHostProvider(ICommandContext &context);
    GitLabHostProvider(ICommandContext &context, std::shared_ptr<IGitLabAuthentication> gitLabAuth);

    QString id() const override { return QStringLiteral("gitlab"); }
    QString name() const override { return QStringLiteral("GitLab"); }
    bool isSupported(const GitRequest &request) override;
    bool isSupported(const HttpResponse *response) override;

    /// Stores freshly generated OAuth tokens (access token and refresh token) as a side effect; removes an
    /// expired OAuth access token and refreshes it with the stored refresh token.
    GitResponse getCredential(const GitRequest &request) override;
    /// Also deletes the stored refresh token.
    void eraseCredential(const GitRequest &request) override;
    Credential generateCredential(const GitRequest &request) override;

    /// Public for tests (internal upstream).
    AuthenticationModes getSupportedAuthenticationModes(const QUrl &targetUri);
    /// The service name of the refresh token: the host of the service prefixed with "oauth-refresh-token.".
    QString getRefreshTokenServiceName(const GitRequest &request);

    /// The account GitLab expects with OAuth access tokens.
    static const QString OAuthUserName; // "oauth2"

private:
    /// A generated credential and, for OAuth, the refresh token to store with it.
    struct GeneratedCredential
    {
        Credential credential;
        bool isOAuth = false;
        OptString refreshToken;
    };
    GeneratedCredential generate(const GitRequest &request);
    GeneratedCredential oauthCredential(const OAuth2TokenResult &result) const;
    bool isOAuthTokenExpired(const QUrl &baseUri, const QString &accessToken);

    std::shared_ptr<IGitLabAuthentication> m_gitLabAuth;
};

} // namespace gcm::gitlab
