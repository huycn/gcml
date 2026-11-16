// Port of GenericHostProvider.cs and GenericOAuthConfig.cs
#include "core/generic.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/strings.h"

namespace gcm {

namespace CredCfg = Constants::GitConfiguration::Credential;
namespace Envars = Constants::EnvironmentVariables;

// =============================================================================================
// GenericOAuthConfig
// =============================================================================================

namespace {

/// Uri.TryCreate(baseUri, relativeOrAbsolute)
std::optional<QUrl> tryCreateRelative(const std::optional<QUrl> &baseUri, const QString &value)
{
    if (value.isEmpty()) {
        return baseUri;
    }
    if (const std::optional<QUrl> absolute = tryCreateUri(value)) {
        return absolute;
    }
    if (!baseUri) {
        return std::nullopt;
    }
    const QUrl relative(value, QUrl::TolerantMode);
    if (!relative.isValid()) {
        return std::nullopt;
    }
    return baseUri->resolved(relative);
}

/// A Gitea remote URL is always '{host}/{prefix}/{owner}/{repo}' (no nested groups): dropping the last two path
/// segments leaves '{host}/{prefix}', where the OAuth endpoints live. With fewer than two segments (e.g. when Git
/// omits the path) the endpoints stay at the host root.
QUrl createGiteaEndpointUri(const QUrl &remoteUri, const QString &endpoint)
{
    QStringList segments = remoteUri.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    segments = segments.mid(0, std::max<qsizetype>(0, segments.size() - 2));
    QUrl uri = remoteUri.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment);
    const QString prefixPath = segments.join(QLatin1Char('/'));
    // UriBuilder.Path adds the leading '/'
    uri.setPath(QLatin1Char('/') + (prefixPath.isEmpty() ? endpoint : prefixPath + QLatin1Char('/') + endpoint));
    return uri;
}

QString uriString(const std::optional<QUrl> &uri)
{
    return uri ? absoluteUri(*uri) : QString();
}

} // namespace

bool GenericOAuthConfig::tryGet(ITrace &trace, ISettings &settings, const GitRequest &request, GenericOAuthConfig &config)
{
    config = GenericOAuthConfig();
    std::optional<QUrl> authzEndpointUri, tokenEndpointUri;
    const std::optional<QUrl> remoteUri = request.getRemoteUri();

    const QStringList wwwAuth = request.wwwAuth();
    if (remoteUri) {
        bool giteaRealm = std::any_of(wwwAuth.begin(), wwwAuth.end(), [](const QString &x) {
            return x.contains(QLatin1String("Basic realm=\"Gitea\""), Qt::CaseInsensitive);
        });
        if (giteaRealm) {
            trace.writeLine(QStringLiteral("Using universal Gitea OAuth configuration"));
            config.clientId = WellKnown::GiteaClientId;
            authzEndpointUri = createGiteaEndpointUri(*remoteUri, WellKnown::GiteaAuthzEndpoint);
            tokenEndpointUri = createGiteaEndpointUri(*remoteUri, WellKnown::GiteaTokenEndpoint);
            config.redirectUri = WellKnown::LocalIPv4RedirectUri;
        }
    }

    QString authzEndpoint;
    if (settings.tryGetSetting(Envars::OAuthAuthzEndpoint, CredCfg::SectionName, CredCfg::OAuthAuthzEndpoint, authzEndpoint)) {
        authzEndpointUri = tryCreateRelative(remoteUri, authzEndpoint);
    }
    if (!authzEndpointUri) {
        trace.writeLine(QStringLiteral("Invalid OAuth configuration - missing/invalid authorize endpoint: %1").arg(authzEndpoint));
        return false;
    }

    QString tokenEndpoint;
    if (settings.tryGetSetting(Envars::OAuthTokenEndpoint, CredCfg::SectionName, CredCfg::OAuthTokenEndpoint, tokenEndpoint)) {
        tokenEndpointUri = tryCreateRelative(remoteUri, tokenEndpoint);
    }
    if (!tokenEndpointUri) {
        trace.writeLine(QStringLiteral("Invalid OAuth configuration - missing/invalid token endpoint: %1").arg(tokenEndpoint));
        return false;
    }

    // The device code endpoint is optional
    std::optional<QUrl> deviceEndpointUri;
    QString deviceEndpoint;
    if (settings.tryGetSetting(Envars::OAuthDeviceEndpoint, CredCfg::SectionName, CredCfg::OAuthDeviceEndpoint, deviceEndpoint)) {
        deviceEndpointUri = tryCreateRelative(remoteUri, deviceEndpoint);
        if (!deviceEndpointUri) {
            trace.writeLine(QStringLiteral("Invalid OAuth configuration - invalid device endpoint: %1").arg(deviceEndpoint));
        }
    }

    config.endpoints = OAuth2ServerEndpoints(*authzEndpointUri, *tokenEndpointUri);
    config.endpoints.deviceAuthorizationEndpoint = deviceEndpointUri;

    QString value;
    if (settings.tryGetSetting(Envars::OAuthClientId, CredCfg::SectionName, CredCfg::OAuthClientId, value)) {
        config.clientId = value;
    }
    if (settings.tryGetSetting(Envars::OAuthClientSecret, CredCfg::SectionName, CredCfg::OAuthClientSecret, value)) {
        config.clientSecret = value;
    }

    if (settings.tryGetSetting(Envars::OAuthRedirectUri, CredCfg::SectionName, CredCfg::OAuthRedirectUri, value)) {
        if (const std::optional<QUrl> redirect = tryCreateUri(value)) {
            config.redirectUri = *redirect;
        }
    }
    if (config.redirectUri.isEmpty()) {
        config.redirectUri = QUrl(QStringLiteral("http://127.0.0.1"));
    }

    if (settings.tryGetSetting(Envars::OAuthScopes, CredCfg::SectionName, CredCfg::OAuthScopes, value) && !isNullOrWhiteSpace(value)) {
        config.scopes = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }

    // Defaults to true
    config.useAuthHeader = !settings.tryGetSetting(Envars::OAuthClientAuthHeader, CredCfg::SectionName, CredCfg::OAuthClientAuthHeader, value) || isTruthy(value);

    // The response mode is optional (default: the server's default, i.e. 'query')
    if (settings.tryGetSetting(Envars::OAuthResponseMode, CredCfg::SectionName, CredCfg::OAuthResponseMode, value) && !isNullOrWhiteSpace(value)) {
        OAuth2ResponseMode mode;
        if (OAuth2ResponseModes::tryParse(value, mode)) {
            config.responseMode = mode;
        } else {
            trace.writeLine(QStringLiteral("Invalid OAuth configuration - unknown response mode '%1'; using default").arg(value));
        }
    }

    config.defaultUserName = settings.tryGetSetting(Envars::OAuthDefaultUserName, CredCfg::SectionName, CredCfg::OAuthDefaultUserName, value) ? value : QStringLiteral("OAUTH_USER");
    return true;
}

// =============================================================================================
// GenericHostProvider
// =============================================================================================

GenericHostProvider::GenericHostProvider(ICommandContext &context)
    : GenericHostProvider(context,
                          std::make_shared<BasicAuthentication>(context),
                          std::make_shared<WindowsIntegratedAuthentication>(context),
                          std::make_shared<OAuthAuthentication>(context))
{
}

GenericHostProvider::GenericHostProvider(ICommandContext &context,
                                         std::shared_ptr<IBasicAuthentication> basicAuth,
                                         std::shared_ptr<IWindowsIntegratedAuthentication> winAuth,
                                         std::shared_ptr<IOAuthAuthentication> oauth)
    : m_context(context), m_basicAuth(std::move(basicAuth)), m_winAuth(std::move(winAuth)), m_oauth(std::move(oauth))
{
    EnsureArgument::notNull(m_basicAuth, "basicAuth");
    EnsureArgument::notNull(m_winAuth, "winAuth");
    EnsureArgument::notNull(m_oauth, "oauth");
}

QStringList GenericHostProvider::supportedAuthorityIds() const
{
    return BasicAuthentication::AuthorityIds + WindowsIntegratedAuthentication::AuthorityIds;
}

bool GenericHostProvider::isSupported(const GitRequest &request)
{
    // All possible protocols (HTTP, HTTPS, SMTP, IMAP, ...)
    return !isNullOrWhiteSpace(request.protocol());
}

QString GenericHostProvider::getServiceName(const GitRequest &request) const
{
    const std::optional<QUrl> uri = request.getRemoteUri(false);
    if (!uri) {
        throw InvalidOperationException(QStringLiteral("Unable to determine the remote URL of the request."));
    }
    QString service = absoluteUri(*uri);
    while (service.endsWith(QLatin1Char('/'))) {
        service.chop(1);
    }
    return service;
}

GitResponse GenericHostProvider::getCredential(const GitRequest &request)
{
    // Try and locate an existing credential in the OS credential store
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Looking for existing credential in store with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));

    const std::optional<Credential> credential = m_context.credentialStore().get(service, request.userName());
    if (!credential) {
        trace.writeLine(QStringLiteral("No existing credentials found."));
        trace.writeLine(QStringLiteral("Creating new credential..."));
        return generateCredential(request);
    }
    trace.writeLine(QStringLiteral("Existing credential found."));
    return GitResponse(*credential);
}

void GenericHostProvider::storeCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();
    // WIA authentication is signalled to Git as an empty user name/password pair; do not store it.
    if (isNullOrWhiteSpace(request.userName()) && isNullOrWhiteSpace(request.password())) {
        trace.writeLine(QStringLiteral("Not storing empty credential."));
    } else {
        trace.writeLine(QStringLiteral("Storing credential with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));
        m_context.credentialStore().addOrUpdate(service, request.userName().value_or(QString()), request.password().value_or(QString()));
        trace.writeLine(QStringLiteral("Credential was successfully stored."));
    }
}

void GenericHostProvider::eraseCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Erasing stored credential in store with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));
    if (m_context.credentialStore().remove(service, request.userName())) {
        trace.writeLine(QStringLiteral("Credential was successfully erased."));
    } else {
        trace.writeLine(QStringLiteral("No credential was erased."));
    }
}

GitResponse GenericHostProvider::generateCredential(const GitRequest &request)
{
    ITrace &trace = m_context.trace();

    // Only *warn* about HTTP remotes: this provider supports all protocols and never blocked HTTP.
    if (!m_context.settings().allowUnsafeRemotes() && equalsIgnoreCase(request.protocol().value_or(QString()), QStringLiteral("http"))) {
        m_context.console().writeWarning(QStringLiteral("use of unencrypted HTTP remote URLs is not recommended; see %1 for more information.").arg(Constants::HelpUrls::GcmUnsafeRemotes));
    }

    const std::optional<QUrl> uriOpt = request.getRemoteUri();
    if (!uriOpt) {
        throw InvalidOperationException(QStringLiteral("Unable to determine the remote URL of the request."));
    }
    const QUrl uri = *uriOpt;

    // An OAuth configuration for this remote?
    GenericOAuthConfig oauthConfig;
    if (GenericOAuthConfig::tryGet(trace, m_context.settings(), request, oauthConfig)) {
        trace.writeLine(QStringLiteral("Found generic OAuth configuration for '%1':").arg(absoluteUri(uri)));
        trace.writeLine(QStringLiteral("\tAuthzEndpoint   = %1").arg(absoluteUri(oauthConfig.endpoints.authorizationEndpoint)));
        trace.writeLine(QStringLiteral("\tTokenEndpoint   = %1").arg(absoluteUri(oauthConfig.endpoints.tokenEndpoint)));
        trace.writeLine(QStringLiteral("\tDeviceEndpoint  = %1").arg(uriString(oauthConfig.endpoints.deviceAuthorizationEndpoint)));
        trace.writeLine(QStringLiteral("\tClientId        = %1").arg(oauthConfig.clientId.value_or(QString())));
        trace.writeLine(QStringLiteral("\tClientSecret    = %1").arg(oauthConfig.clientSecret.value_or(QString())));
        trace.writeLine(QStringLiteral("\tRedirectUri     = %1").arg(absoluteUri(oauthConfig.redirectUri)));
        trace.writeLine(QStringLiteral("\tScopes          = [%1]").arg(oauthConfig.scopes.join(QStringLiteral(", "))));
        trace.writeLine(QStringLiteral("\tUseAuthHeader   = %1").arg(oauthConfig.useAuthHeader ? QStringLiteral("True") : QStringLiteral("False")));
        trace.writeLine(QStringLiteral("\tDefaultUserName = %1").arg(oauthConfig.defaultUserName));
        return GitResponse(getOAuthAccessToken(uri, request.userName(), oauthConfig));
    }

    // Windows Integrated Authentication: detected only on Windows upstream, so never here.
    if (equalsIgnoreCase(uri.scheme(), QStringLiteral("http")) || equalsIgnoreCase(uri.scheme(), QStringLiteral("https"))) {
        if (isWindowsAuthAllowed()) {
            trace.writeLine(QStringLiteral("Skipping check for Windows Integrated Authentication on %1.").arg(PlatformUtils::getPlatformInformation().operatingSystemType));
        } else {
            trace.writeLine(QStringLiteral("Windows Integrated Authentication detection has been disabled."));
        }
    } else {
        trace.writeLine(QStringLiteral("Skipping check for Windows Integrated Authentication on non-HTTP(S) remotes."));
    }

    // Use basic authentication
    trace.writeLine(QStringLiteral("Prompting for basic credentials..."));
    return GitResponse(m_basicAuth->getCredentials(absoluteUri(uri), request.userName()));
}

namespace {

/// Enum.TryParse<OAuthAuthenticationModes>(value, ignoreCase: true): names or numbers, comma separated.
bool tryParseModes(const QString &value, OAuthAuthenticationModes &modes)
{
    modes = OAuthAuthenticationModes::None;
    const QStringList parts = value.split(QLatin1Char(','));
    int result = 0;
    for (const QString &raw : parts) {
        const QString part = raw.trimmed();
        bool ok = false;
        int number = part.toInt(&ok);
        if (ok) {
            result |= number;
        } else if (equalsIgnoreCase(part, QStringLiteral("None"))) {
            result |= int(OAuthAuthenticationModes::None);
        } else if (equalsIgnoreCase(part, QStringLiteral("Browser"))) {
            result |= int(OAuthAuthenticationModes::Browser);
        } else if (equalsIgnoreCase(part, QStringLiteral("DeviceCode"))) {
            result |= int(OAuthAuthenticationModes::DeviceCode);
        } else if (equalsIgnoreCase(part, QStringLiteral("All"))) {
            result |= int(OAuthAuthenticationModes::All);
        } else {
            return false;
        }
    }
    modes = OAuthAuthenticationModes(result);
    return true;
}

QString modesToString(OAuthAuthenticationModes modes)
{
    switch (modes) {
        case OAuthAuthenticationModes::None: return QStringLiteral("None");
        case OAuthAuthenticationModes::Browser: return QStringLiteral("Browser");
        case OAuthAuthenticationModes::DeviceCode: return QStringLiteral("DeviceCode");
        case OAuthAuthenticationModes::All: return QStringLiteral("All");
    }
    return QString::number(int(modes));
}

} // namespace

Credential GenericHostProvider::getOAuthAccessToken(const QUrl &remoteUri, const OptString &userName,
                                                    const GenericOAuthConfig &config)
{
    ITrace &trace = m_context.trace();
    ICredentialStore &store = m_context.credentialStore();

    const QString oauthUser = userName.value_or(config.defaultUserName);
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    OAuth2Client client(*m_httpClient,
                        config.endpoints,
                        config.clientId.value_or(QString()),
                        config.redirectUri,
                        config.clientSecret,
                        config.useAuthHeader,
                        config.responseMode);

    // "refresh_token." prepended to the host gives a (hopefully) unique service name; appending "/refresh_token"
    // could clash with credential.useHttpPath remotes.
    QUrl refreshUri = remoteUri;
    refreshUri.setHost(QStringLiteral("refresh_token.%1").arg(remoteUri.host()));
    QString refreshService = absoluteUri(refreshUri);
    while (refreshService.endsWith(QLatin1Char('/'))) {
        refreshService.chop(1);
    }

    // Try to use a refresh token if we have one
    if (const std::optional<Credential> refreshToken = store.get(refreshService, userName)) {
        try {
            const OAuth2TokenResult refreshResult = client.getTokenByRefreshToken(refreshToken->password, CancellationToken());
            // Store the new refresh token if we have been given one
            if (!isNullOrWhiteSpace(refreshResult.refreshToken)) {
                store.addOrUpdate(refreshService, refreshToken->account, *refreshResult.refreshToken);
            }
            return Credential{oauthUser, refreshResult.accessToken};
        } catch (const OAuth2Exception &ex) {
            // It may have expired or been revoked: fall through to an interactive flow
            trace.writeLine(QStringLiteral("Failed to use refresh token."));
            trace.writeException(ex);
        }
    }

    // Which interactive OAuth mode to use, starting with any preference in the configuration
    OAuthAuthenticationModes supportedModes = OAuthAuthenticationModes::All;
    QString authModesStr;
    if (m_context.settings().tryGetSetting(Envars::OAuthAuthenticationModes, CredCfg::SectionName, CredCfg::OAuthAuthenticationModes, authModesStr)) {
        // As upstream: a value that does not parse leaves no mode at all (Enum.TryParse's default).
        if (tryParseModes(authModesStr, supportedModes) && supportedModes != OAuthAuthenticationModes::None) {
            trace.writeLine(QStringLiteral("Supported authentication modes override present: %1").arg(modesToString(supportedModes)));
        } else {
            trace.writeLine(QStringLiteral("Invalid value for supported authentication modes override setting: '%1'").arg(authModesStr));
        }
    }

    // Without a device code endpoint that mode is not an option
    if (!config.supportsDeviceCode()) {
        supportedModes = supportedModes & ~OAuthAuthenticationModes::DeviceCode;
    }

    // Prompt the user to select a mode
    const OAuthAuthenticationModes mode = m_oauth->getAuthenticationMode(absoluteUri(remoteUri), supportedModes);
    OAuth2TokenResult tokenResult;
    switch (mode) {
        case OAuthAuthenticationModes::Browser:
            tokenResult = m_oauth->getTokenByBrowser(client, config.scopes);
            break;
        case OAuthAuthenticationModes::DeviceCode:
            tokenResult = m_oauth->getTokenByDeviceCode(client, config.scopes);
            break;
        default:
            throw Exception(QStringLiteral("No authentication mode selected!"));
    }

    // Store the refresh token if we have one
    if (!isNullOrWhiteSpace(tokenResult.refreshToken)) {
        store.addOrUpdate(refreshService, oauthUser, *tokenResult.refreshToken);
    }

    return Credential{oauthUser, tokenResult.accessToken};
}

bool GenericHostProvider::isWindowsAuthAllowed()
{
    if (!m_context.settings().isWindowsIntegratedAuthenticationEnabled()) {
        return false;
    }

    // COMPAT: the old GCM's "basic" authority disabled smart detection (provider, NTLM, ...) so the user could
    // enter a user name/password or PAT; honour it.
    const OptString authority = m_context.settings().legacyAuthorityOverride();
    return !(authority && BasicAuthentication::AuthorityIds.contains(*authority, Qt::CaseInsensitive));
}

} // namespace gcm
