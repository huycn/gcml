// Ports GitLabConstants.cs, GitLabOAuth2Client.cs, GitLabAuthentication.cs and GitLabHostProvider.cs.
// Upstream shows its Avalonia view in-process when no GitLab helper is found; this port uses the `gitlab prompt`
// command of git-credential-manager-ui instead, and the terminal when that is missing too.
#include "gitlab.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/strings.h"

namespace gcm::gitlab {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

/// https://docs.gitlab.com/ee/integration/oauth_provider.html#authorized-applications
QStringList gitLabOAuthScopes()
{
    return {QStringLiteral("write_repository"), QStringLiteral("read_repository")};
}

/// Uri.ToString() of the target URI as shown in prompts ("https://gitlab.com/").
QString display(const QUrl &uri)
{
    return uri.isEmpty() ? QString() : absoluteUri(uri);
}

const QList<EnumName> &modeNames()
{
    static const QList<EnumName> names = {
        { int(AuthenticationModes::None), "None" },
        { int(AuthenticationModes::Basic), "Basic" },
        { int(AuthenticationModes::Browser), "Browser" },
        { int(AuthenticationModes::Pat), "Pat" },
        { int(AuthenticationModes::All), "All" },
    };
    return names;
}

AuthenticationPromptResult promptResult(AuthenticationModes mode, std::optional<Credential> credential = std::nullopt)
{
    return AuthenticationPromptResult{mode, std::move(credential)};
}

} // namespace

// =============================================================================================
// Constants and modes
// =============================================================================================

bool GitLabConstants::isGitLabDotCom(const QUrl &uri)
{
    return uri.host().compare(QUrl(GitLabDotCom).host(), Qt::CaseInsensitive) == 0;
}

bool GitLabConstants::isGitLabDotComClientId(const QString &clientId)
{
    return clientId.compare(OAuthClientId, Qt::CaseInsensitive) == 0;
}

QString modesToString(AuthenticationModes modes)
{
    return formatFlagsEnum(int(modes), modeNames());
}

bool tryParseAuthenticationModes(const QString &value, AuthenticationModes &modes)
{
    int result = 0;
    const bool ok = tryParseFlagsEnum(value, modeNames(), result);
    modes = AuthenticationModes(result);
    return ok;
}

// =============================================================================================
// GitLabOAuth2Client
// =============================================================================================

namespace {

OAuth2ServerEndpoints createEndpoints(const QUrl &baseUri)
{
    return OAuth2ServerEndpoints(baseUri.resolved(QUrl(GitLabConstants::OAuthAuthorizationEndpointRelativeUri)),
                                 baseUri.resolved(QUrl(GitLabConstants::OAuthTokenEndpointRelativeUri)));
}

QUrl getRedirectUri(ISettings &settings)
{
    // Check for developer override value
    QString value;
    if (settings.tryGetSetting(GitLabConstants::EnvironmentVariables::DevOAuthRedirectUri, CredCfg::SectionName, GitLabConstants::GitConfiguration::Credential::DevOAuthRedirectUri, value)) {
        if (const std::optional<QUrl> redirectUri = tryCreateUri(value)) {
            return *redirectUri;
        }
    }
    return QUrl(GitLabConstants::OAuthRedirectUri);
}

OptString getClientSecret(ISettings &settings)
{
    // Check for developer override value
    QString clientSecret;
    if (settings.tryGetSetting(GitLabConstants::EnvironmentVariables::DevOAuthClientSecret, CredCfg::SectionName, GitLabConstants::GitConfiguration::Credential::DevOAuthClientSecret, clientSecret)) {
        return clientSecret;
    }
    // no secret necessary
    return std::nullopt;
}

} // namespace

QString GitLabOAuth2Client::getClientId(ISettings &settings)
{
    // Check for developer override value
    QString clientId;
    if (settings.tryGetSetting(GitLabConstants::EnvironmentVariables::DevOAuthClientId, CredCfg::SectionName, GitLabConstants::GitConfiguration::Credential::DevOAuthClientId, clientId)) {
        return clientId;
    }
    return GitLabConstants::OAuthClientId;
}

GitLabOAuth2Client::GitLabOAuth2Client(HttpClient httpClient, ISettings &settings, const QUrl &baseUri)
    : OAuth2Client(std::move(httpClient), createEndpoints(baseUri), getClientId(settings), getRedirectUri(settings), getClientSecret(settings))
{
}

// =============================================================================================
// GitLabAuthentication
// =============================================================================================

bool GitLabAuthentication::tryFindGitLabHelper(QString &command, QString &args)
{
    // A helper speaking the GitLab.UI protocol (GCM_GITLAB_HELPER / credential.gitLabHelper, default "GitLab.UI")
    if (tryFindHelperCommand(GitLabConstants::EnvironmentVariables::AuthenticationHelper, GitLabConstants::GitConfiguration::Credential::AuthenticationHelper, GitLabConstants::DefaultAuthenticationHelper, command, args)) {
        return true;
    }
    // The GitLab command of git-credential-manager-ui (upstream: the in-process view)
    if (tryFindUiHelper(command, args)) {
        args += QStringLiteral("gitlab ");
        return true;
    }
    return false;
}

HttpClient &GitLabAuthentication::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    return *m_httpClient;
}

AuthenticationPromptResult GitLabAuthentication::getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes)
{
    // If we cannot start a browser then don't offer the option
    if (!m_context.sessionManager().isWebBrowserAvailable()) {
        modes = modes & ~AuthenticationModes::Browser;
    }

    // We need at least one mode!
    if (modes == AuthenticationModes::None) {
        throw ArgumentException(QStringLiteral("Must specify at least one AuthenticationModes (Parameter 'modes')"));
    }

    throwIfUserInteractionDisabled();

    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() && tryFindGitLabHelper(command, args)) {
        return getAuthenticationViaHelper(targetUri, userName, modes, command, args);
    }
    return getAuthenticationViaTty(targetUri, userName, modes);
}

AuthenticationPromptResult GitLabAuthentication::getAuthenticationViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes)
{
    throwIfTerminalPromptsDisabled();
    IConsoleService &console = m_context.console();

    // Basic and Pat ask for the user name the same way; only the secret's label differs
    auto withSecret = [&](AuthenticationModes mode, const QString &secretLabel) {
        console.writeLine(QStringLiteral("Enter GitLab credentials for '%1'...").arg(display(targetUri)));
        if (isNullOrWhiteSpace(userName)) {
            userName = console.promptText(QStringLiteral("Username"));
        } else {
            console.writeLine(QStringLiteral("Username: %1").arg(*userName));
        }
        const QString secret = console.promptSecret(secretLabel);
        return promptResult(mode, Credential{valueOr(userName), secret});
    };

    switch (modes) {
    case AuthenticationModes::Basic: return withSecret(AuthenticationModes::Basic, QStringLiteral("Password"));
    case AuthenticationModes::Pat: return withSecret(AuthenticationModes::Pat, QStringLiteral("Personal access token"));
    case AuthenticationModes::Browser: return promptResult(AuthenticationModes::Browser);
    case AuthenticationModes::None: throw ArgumentOutOfRangeException(QStringLiteral("At least one AuthenticationModes must be supplied (Parameter 'modes')"));
    default: break;
    }

    QStringList labels;
    QList<AuthenticationModes> values;
    auto add = [&](AuthenticationModes mode, const QString &label) {
        if (hasFlag(modes, mode)) {
            labels.append(label);
            values.append(mode);
        }
    };
    add(AuthenticationModes::Browser, QStringLiteral("Web browser"));
    add(AuthenticationModes::Pat, QStringLiteral("Personal access token"));
    add(AuthenticationModes::Basic, QStringLiteral("Username/password"));
    const int choice = console.promptSelection(QStringLiteral("Select an authentication method for '%1'").arg(display(targetUri)), labels);
    switch (values.value(choice, AuthenticationModes::None)) {
    case AuthenticationModes::Browser: return promptResult(AuthenticationModes::Browser);
    case AuthenticationModes::Basic: return withSecret(AuthenticationModes::Basic, QStringLiteral("Password"));
    case AuthenticationModes::Pat: return withSecret(AuthenticationModes::Pat, QStringLiteral("Personal access token"));
    default: throw Exception(QStringLiteral("Exception of type 'System.Exception' was thrown."));
    }
}

AuthenticationPromptResult GitLabAuthentication::getAuthenticationViaHelper(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes,
                                                                            const QString &command, const QString &args)
{
    QString promptArgs = args + QStringLiteral("prompt");
    if (!isNullOrWhiteSpace(userName)) {
        promptArgs += QStringLiteral(" --username %1").arg(quoteCmdArg(*userName));
    }
    promptArgs += QStringLiteral(" --url %1").arg(quoteCmdArg(display(targetUri)));
    if (hasFlag(modes, AuthenticationModes::Basic)) {
        promptArgs += QStringLiteral(" --basic");
    }
    if (hasFlag(modes, AuthenticationModes::Browser)) {
        promptArgs += QStringLiteral(" --browser");
    }
    if (hasFlag(modes, AuthenticationModes::Pat)) {
        promptArgs += QStringLiteral(" --pat");
    }

    const StringDictionary resultDict = invokeHelper(command, promptArgs);
    const OptString responseMode = resultDict.tryGet(QStringLiteral("mode"));
    if (!responseMode) {
        throw Exception(QStringLiteral("Missing 'mode' in response"));
    }

    const QString mode = responseMode->toLower();
    if (mode == QLatin1String("pat")) {
        const OptString pat = resultDict.tryGet(QStringLiteral("pat"));
        if (!pat) {
            throw Exception(QStringLiteral("Missing 'pat' in response"));
        }
        // Username is optional for PATs
        return promptResult(AuthenticationModes::Pat, Credential{valueOr(resultDict.tryGet(QStringLiteral("username"))), *pat});
    }
    if (mode == QLatin1String("browser")) {
        return promptResult(AuthenticationModes::Browser);
    }
    if (mode == QLatin1String("basic")) {
        const OptString resultUserName = resultDict.tryGet(QStringLiteral("username"));
        if (!resultUserName) {
            throw Exception(QStringLiteral("Missing 'username' in response"));
        }
        const OptString password = resultDict.tryGet(QStringLiteral("password"));
        if (!password) {
            throw Exception(QStringLiteral("Missing 'password' in response"));
        }
        return promptResult(AuthenticationModes::Basic, Credential{*resultUserName, *password});
    }
    throw Exception(QStringLiteral("Unknown mode value in response '%1'").arg(*responseMode));
}

OAuth2TokenResult GitLabAuthentication::getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes)
{
    throwIfUserInteractionDisabled();

    GitLabOAuth2Client oauthClient(httpClient(), m_context.settings(), targetUri);

    // We require a desktop session to launch the user's default web browser
    if (!m_context.sessionManager().isDesktopSession()) {
        throw InvalidOperationException(QStringLiteral("Browser authentication requires a desktop session"));
    }

    OAuth2SystemWebBrowser browser(m_context.sessionManager(), OAuth2WebBrowserOptions());

    // Write message to the terminal (if any is attached) for some feedback that we're waiting for a web response
    m_context.console().writeInfo(QStringLiteral("please complete authentication in your browser..."));

    const OAuth2AuthorizationCodeResult authCodeResult = oauthClient.getAuthorizationCode(scopes, browser, CancellationToken());
    return oauthClient.getTokenByAuthorizationCode(authCodeResult, CancellationToken());
}

OAuth2TokenResult GitLabAuthentication::getOAuthTokenViaRefresh(const QUrl &targetUri, const QString &refreshToken)
{
    GitLabOAuth2Client oauthClient(httpClient(), m_context.settings(), targetUri);
    return oauthClient.getTokenByRefreshToken(refreshToken, CancellationToken());
}

// =============================================================================================
// GitLabHostProvider
// =============================================================================================

GitLabHostProvider::GitLabHostProvider(ICommandContext &context)
    : GitLabHostProvider(context, std::make_shared<GitLabAuthentication>(context))
{
}

GitLabHostProvider::GitLabHostProvider(ICommandContext &context, std::shared_ptr<IGitLabAuthentication> gitLabAuth)
    : HostProvider(context), m_gitLabAuth(std::move(gitLabAuth))
{
    EnsureArgument::notNull(m_gitLabAuth, "gitLabAuth");
}

bool GitLabHostProvider::isSupported(const GitRequest &request)
{
    // We do not support unencrypted HTTP communications to GitLab, but we report `true` here for HTTP so that we
    // can show a helpful error message for the user in `generateCredential`.
    const QString protocol = valueOr(request.protocol());
    if (protocol.compare(QLatin1String("http"), Qt::CaseInsensitive) != 0 && protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    if (const std::optional<QUrl> remoteUri = request.getRemoteUri(); remoteUri && GitLabConstants::isGitLabDotCom(*remoteUri)) {
        return true;
    }

    // Split port number and hostname from host request argument
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        return false;
    }

    // GitLab[.subdomain].domain.tld
    const QStringList domains = hostName.split(QLatin1Char('.'));
    if (domains.size() >= 3 && domains[0].compare(QLatin1String("gitlab"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    const QStringList wwwAuth = request.wwwAuth();
    return std::any_of(wwwAuth.begin(), wwwAuth.end(), [](const QString &x) { return x.contains(QLatin1String("realm=\"GitLab\"")); });
}

bool GitLabHostProvider::isSupported(const HttpResponse *response)
{
    // as seen at eg. https://salsa.debian.org/apt-team/apt.git
    // not always present https://gitlab.com/gitlab-org/gitlab/-/issues/349464
    return response && response->headers.contains(QStringLiteral("X-Gitlab-Feature-Category"));
}

Credential GitLabHostProvider::generateCredential(const GitRequest &request)
{
    return generate(request).credential;
}

GitLabHostProvider::GeneratedCredential GitLabHostProvider::generate(const GitRequest &request)
{
    // We should not allow unencrypted communication and should inform the user
    if (!m_context.settings().allowUnsafeRemotes() && valueOr(request.protocol()).compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) {
        throw Exception(QStringLiteral("Unencrypted HTTP is not recommended for GitLab. Ensure the repository remote URL is using HTTPS or see %1 about how to allow unsafe remotes.").arg(Constants::HelpUrls::GcmUnsafeRemotes));
    }

    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());
    const AuthenticationModes authModes = getSupportedAuthenticationModes(remoteUri);
    const AuthenticationPromptResult result = m_gitLabAuth->getAuthentication(remoteUri, request.userName(), authModes);

    switch (result.authenticationMode) {
    case AuthenticationModes::Basic:
    case AuthenticationModes::Pat:
        return GeneratedCredential{result.credential.value_or(Credential()), false, std::nullopt};
    case AuthenticationModes::Browser:
        return oauthCredential(m_gitLabAuth->getOAuthTokenViaBrowser(remoteUri, gitLabOAuthScopes()));
    default:
        throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values. (Parameter 'promptResult')"));
    }
}

GitLabHostProvider::GeneratedCredential GitLabHostProvider::oauthCredential(const OAuth2TokenResult &result) const
{
    // username must be 'oauth2' https://docs.gitlab.com/ee/api/oauth2.html#access-git-over-https-with-access-token
    return GeneratedCredential{Credential{OAuthUserName, result.accessToken}, true, result.refreshToken};
}

AuthenticationModes GitLabHostProvider::getSupportedAuthenticationModes(const QUrl &targetUri)
{
    ITrace &trace = m_context.trace();

    // Check for an explicit override for supported authentication modes
    QString authModesStr;
    if (m_context.settings().tryGetSetting(GitLabConstants::EnvironmentVariables::AuthenticationModes, CredCfg::SectionName, GitLabConstants::GitConfiguration::Credential::AuthenticationModes, authModesStr)) {
        AuthenticationModes authModes;
        if (tryParseAuthenticationModes(authModesStr, authModes) && authModes != AuthenticationModes::None) {
            trace.writeLine(QStringLiteral("Supported authentication modes override present: %1").arg(modesToString(authModes)));
            return authModes;
        }
        trace.writeLine(QStringLiteral("Invalid value for supported authentication modes override setting: '%1'").arg(authModesStr));
    }

    // GitLab.com has well-known supported auth modes
    if (GitLabConstants::isGitLabDotCom(targetUri)) {
        return DotComAuthenticationModes;
    }

    // Try to detect what auth modes are available for this non-GitLab.com host. Assume that PATs are always
    // available to give at least one option to users!
    AuthenticationModes modes = AuthenticationModes::Pat;

    // If there is a configured OAuth client ID (that isn't GitLab.com's client ID) then assume OAuth is possible.
    const QString oauthClientId = GitLabOAuth2Client::getClientId(m_context.settings());
    if (!GitLabConstants::isGitLabDotComClientId(oauthClientId)) {
        modes |= AuthenticationModes::Browser;
    } else {
        // Tell the user that they may wish to configure OAuth for this GitLab instance
        m_context.console().writeWarning(QStringLiteral("missing OAuth configuration for %1 - see %2 for more information").arg(targetUri.host(), GitLabConstants::HelpUrls::GitLab));
    }

    // Would like to query password_authentication_enabled_for_git, but can't unless logged in
    // https://gitlab.com/gitlab-org/gitlab/-/issues/349463. For now assume password auth is always available.
    modes |= AuthenticationModes::Basic;
    return modes;
}

GitResponse GitLabHostProvider::getCredential(const GitRequest &request)
{
    ITrace &trace = m_context.trace();
    ICredentialStore &store = m_context.credentialStore();
    const QString service = getServiceName(request);
    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());

    std::optional<Credential> credential = store.get(service, request.userName());
    if (credential && credential->account == OAuthUserName && isOAuthTokenExpired(remoteUri, credential->password)) {
        trace.writeLine(QStringLiteral("Removing expired OAuth access token..."));
        store.remove(service, credential->account);
        credential.reset();
    }

    if (credential) {
        return GitResponse(*credential);
    }

    std::optional<GeneratedCredential> generated;
    const QString refreshService = getRefreshTokenServiceName(request);
    if (const std::optional<Credential> refreshToken = store.get(refreshService, request.userName())) {
        trace.writeLine(QStringLiteral("Refreshing OAuth token..."));
        try {
            generated = oauthCredential(m_gitLabAuth->getOAuthTokenViaRefresh(remoteUri, refreshToken->password));
        } catch (const std::exception &e) {
            const auto *gcmEx = dynamic_cast<const Exception *>(&e);
            m_context.console().writeError(QStringLiteral("OAuth token refresh failed: %1").arg(gcmEx ? gcmEx->message() : QString::fromUtf8(e.what())));
        }
    }

    if (!generated) {
        generated = generate(request);
    }

    if (generated->isOAuth) {
        trace.writeLine(QStringLiteral("Pre-emptively storing OAuth access and refresh tokens..."));
        // A freshly-generated OAuth credential: store it, since we know it to be valid (whereas Git will only
        // store the credential if git push succeeds)
        store.addOrUpdate(service, generated->credential.account, generated->credential.password);
        // Store the refresh token under a separate service (upstream stores a null secret when there is none)
        store.addOrUpdate(refreshService, generated->credential.account, valueOr(generated->refreshToken));
    }

    return GitResponse(generated->credential);
}

bool GitLabHostProvider::isOAuthTokenExpired(const QUrl &baseUri, const QString &accessToken)
{
    // https://docs.gitlab.com/ee/api/oauth2.html#retrieve-the-token-information
    const QUrl infoUri = baseUri.resolved(QUrl(QStringLiteral("/oauth/token/info")));
    HttpClient httpClient = m_context.httpClientFactory().createClient();
    httpClient.setTimeoutMs(15 * 1000);
    httpClient.defaultRequestHeaders().set(QStringLiteral("Authorization"), QStringLiteral("Bearer %1").arg(accessToken));
    try {
        return httpClient.get(infoUri).statusCode == 401;
    } catch (const std::exception &e) {
        const auto *gcmEx = dynamic_cast<const Exception *>(&e);
        m_context.console().writeError(QStringLiteral("OAuth token info request failed: %1").arg(gcmEx ? gcmEx->message() : QString::fromUtf8(e.what())));
        return false;
    }
}

QString GitLabHostProvider::getRefreshTokenServiceName(const GitRequest &request)
{
    QUrl uri(getServiceName(request));
    uri.setHost(QStringLiteral("oauth-refresh-token.") + uri.host());
    return absoluteUri(uri);
}

void GitLabHostProvider::eraseCredential(const GitRequest &request)
{
    // delete any refresh token too
    m_context.credentialStore().remove(getRefreshTokenServiceName(request), OAuthUserName);
    HostProvider::eraseCredential(request);
}

} // namespace gcm::gitlab
