// Ports BitbucketAuthentication.cs and BitbucketHostProvider.cs. Upstream shows its Avalonia view in-process when no
// Bitbucket helper is found; this port uses the `bitbucket prompt` command of git-credential-manager-ui instead, and
// the terminal when that is missing too.
#include "bitbucket.h"
#include "bitbucketresources.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QRegularExpression>

namespace gcm::bitbucket {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

/// Uri.ToString() of the target URI as shown in prompts ("https://bitbucket.org/").
QString display(const QUrl &uri)
{
    return uri.isEmpty() ? QString() : absoluteUri(uri);
}

bool supportsOAuth(AuthenticationModes authModes)
{
    return hasFlag(authModes, AuthenticationModes::OAuth);
}

bool supportsBasicAuth(AuthenticationModes authModes)
{
    return hasFlag(authModes, AuthenticationModes::Basic);
}

} // namespace

// =============================================================================================
// BitbucketAuthentication
// =============================================================================================

BitbucketAuthentication::BitbucketAuthentication(ICommandContext &context)
: BitbucketAuthentication(context, std::make_shared<OAuth2ClientRegistry>(context))
{
}

BitbucketAuthentication::BitbucketAuthentication(ICommandContext &context, std::shared_ptr<IRegistry<bitbucket::BitbucketOAuth2Client>> oauth2ClientRegistry)
: AuthenticationBase(context), m_oauth2ClientRegistry(std::move(oauth2ClientRegistry))
{
    EnsureArgument::notNull(m_oauth2ClientRegistry, "oauth2ClientRegistry");
}

bool BitbucketAuthentication::tryFindBitbucketHelper(QString &command, QString &args)
{
    // A helper speaking the Atlassian.Bitbucket.UI protocol (GCM_BITBUCKET_HELPER / credential.bitbucketHelper)
    if (tryFindHelperCommand(BitbucketConstants::EnvironmentVariables::AuthenticationHelper, BitbucketConstants::GitConfiguration::Credential::AuthenticationHelper, BitbucketConstants::DefaultAuthenticationHelper, command, args)) {
        return true;
    }
    // The Bitbucket command of git-credential-manager-ui (upstream: the in-process view)
    if (tryFindUiHelper(command, args)) {
        args += QStringLiteral("bitbucket ");
        return true;
    }
    return false;
}

CredentialsPromptResult BitbucketAuthentication::getCredentials(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes)
{
    throwIfUserInteractionDisabled();

    // If we don't have a desktop session/GUI then we cannot offer OAuth since the only supported grant is authcode
    // (i.e, using a web browser; device code is not supported).
    if (!m_context.sessionManager().isDesktopSession()) {
        modes = modes & ~AuthenticationModes::OAuth;
    }

    // If the only supported mode is OAuth then just return immediately
    if (modes == AuthenticationModes::OAuth) {
        return CredentialsPromptResult{AuthenticationModes::OAuth, std::nullopt};
    }

    // We need at least one mode!
    if (modes == AuthenticationModes::None) {
        throw ArgumentException(QStringLiteral("Must specify at least one AuthenticationModes (Parameter 'modes')"));
    }

    // Shell out to the UI helper and show the Bitbucket u/p prompt
    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() && tryFindBitbucketHelper(command, args)) {
        return getCredentialsViaHelper(targetUri, userName, modes, command, args);
    }
    return getCredentialsViaTty(targetUri, userName, modes);
}

CredentialsPromptResult BitbucketAuthentication::getCredentialsViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes)
{
    throwIfTerminalPromptsDisabled();
    IConsoleService &console = m_context.console();

    auto basic = [&]() {
        console.writeLine(QStringLiteral("Enter Bitbucket credentials for '%1'...").arg(display(targetUri)));
        if (!isNullOrWhiteSpace(userName)) {
            // Don't need to prompt for the username if it has been specified already
            console.writeLine(QStringLiteral("Username: %1").arg(*userName));
        } else {
            userName = console.promptText(QStringLiteral("Username"));
        }
        const QString password = console.promptSecret(QStringLiteral("Password"));
        return CredentialsPromptResult{AuthenticationModes::Basic, Credential{valueOr(userName), password}};
    };

    switch (modes) {
        case AuthenticationModes::Basic: return basic();
        case AuthenticationModes::OAuth: return CredentialsPromptResult{AuthenticationModes::OAuth, std::nullopt};
        case AuthenticationModes::None: throw ArgumentOutOfRangeException(QStringLiteral("At least one AuthenticationModes must be supplied (Parameter 'modes')"));
        default: break;
    }

    QStringList labels;
    QList<AuthenticationModes> values;
    if (hasFlag(modes, AuthenticationModes::OAuth)) {
        labels.append(QStringLiteral("OAuth"));
        values.append(AuthenticationModes::OAuth);
    }
    if (hasFlag(modes, AuthenticationModes::Basic)) {
        labels.append(QStringLiteral("Username/password"));
        values.append(AuthenticationModes::Basic);
    }
    const int choice = console.promptSelection(QStringLiteral("Select an authentication method for '%1'").arg(display(targetUri)), labels);
    switch (values.value(choice, AuthenticationModes::None)) {
        case AuthenticationModes::OAuth: return CredentialsPromptResult{AuthenticationModes::OAuth, std::nullopt};
        case AuthenticationModes::Basic: return basic();
        default: throw Exception(QStringLiteral("Exception of type 'System.Exception' was thrown."));
    }
}

CredentialsPromptResult BitbucketAuthentication::getCredentialsViaHelper(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes, const QString &command, const QString &args)
{
    QString promptArgs = args + QStringLiteral("prompt");
    if (!BitbucketHelper::isBitbucketOrg(targetUri)) {
        promptArgs += QStringLiteral(" --url %1").arg(quoteCmdArg(display(targetUri)));
    }
    if (!isNullOrWhiteSpace(userName)) {
        promptArgs += QStringLiteral(" --username %1").arg(quoteCmdArg(*userName));
    }
    if (hasFlag(modes, AuthenticationModes::Basic)) {
        promptArgs += QStringLiteral(" --show-basic");
    }
    if (hasFlag(modes, AuthenticationModes::OAuth)) {
        promptArgs += QStringLiteral(" --show-oauth");
    }

    const StringDictionary output = invokeHelper(command, promptArgs);
    if (const OptString mode = output.tryGet(QStringLiteral("mode")); mode && mode->compare(QLatin1String("oauth"), Qt::CaseInsensitive) == 0) {
        return CredentialsPromptResult{AuthenticationModes::OAuth, std::nullopt};
    }
    const OptString resultUserName = output.tryGet(QStringLiteral("username"));
    if (!resultUserName) {
        throw Exception(QStringLiteral("Missing username in response"));
    }
    const OptString password = output.tryGet(QStringLiteral("password"));
    if (!password) {
        throw Exception(QStringLiteral("Missing password in response"));
    }
    return CredentialsPromptResult{AuthenticationModes::Basic, Credential{*resultUserName, *password}};
}

OAuth2TokenResult BitbucketAuthentication::createOAuthCredentials(const GitRequest &request)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("bitbucket"), QStringLiteral("oauth_browser"));
    throwIfUserInteractionDisabled();

    OAuth2WebBrowserOptions browserOptions;
    browserOptions.successResponseHtml = BitbucketResources::authenticationResponseSuccessHtml;
    browserOptions.failureResponseHtmlFormat = BitbucketResources::authenticationResponseFailureHtmlFormat;
    OAuth2SystemWebBrowser browser(m_context.sessionManager(), browserOptions);

    const std::shared_ptr<bitbucket::BitbucketOAuth2Client> oauth2Client = m_oauth2ClientRegistry->get(request);
    const OAuth2AuthorizationCodeResult authCodeResult = oauth2Client->getAuthorizationCode(browser, CancellationToken());
    return oauth2Client->getTokenByAuthorizationCode(authCodeResult, CancellationToken());
}

OAuth2TokenResult BitbucketAuthentication::refreshOAuthCredentials(const GitRequest &request, const QString &refreshToken)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("bitbucket"), QStringLiteral("oauth_refresh"));
    return m_oauth2ClientRegistry->get(request)->getTokenByRefreshToken(refreshToken, CancellationToken());
}

QString BitbucketAuthentication::getRefreshTokenServiceName(const GitRequest &request)
{
    return m_oauth2ClientRegistry->get(request)->getRefreshTokenServiceName(request);
}

// =============================================================================================
// BitbucketHostProvider
// =============================================================================================

BitbucketHostProvider::BitbucketHostProvider(ICommandContext &context)
    : BitbucketHostProvider(context, std::make_shared<BitbucketAuthentication>(context), std::make_shared<BitbucketRestApiRegistry>(context))
{
}

BitbucketHostProvider::BitbucketHostProvider(ICommandContext &context, std::shared_ptr<IBitbucketAuthentication> bitbucketAuth, std::shared_ptr<IRegistry<IBitbucketRestApi>> restApiRegistry)
: m_context(context), m_bitbucketAuth(std::move(bitbucketAuth)), m_restApiRegistry(std::move(restApiRegistry))
{
    EnsureArgument::notNull(m_bitbucketAuth, "bitbucketAuth");
    EnsureArgument::notNull(m_restApiRegistry, "restApiRegistry");
}

bool BitbucketHostProvider::isSupported(const GitRequest &request)
{
    const QStringList wwwAuth = request.wwwAuth();
    if (std::any_of(wwwAuth.begin(), wwwAuth.end(), [](const QString &x) { return x.contains(QLatin1String("realm=\"Atlassian Bitbucket\""), Qt::CaseInsensitive); })) {
        return true;
    }

    // Split port number and hostname from host request argument
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        return false;
    }

    // We do not recommend unencrypted HTTP communications to Bitbucket, but it is possible. Therefore, we report
    // `true` here for HTTP so that we can show a helpful error message for the user in `getCredential`.
    const QString protocol = valueOr(request.protocol());
    return (protocol.compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 || protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) &&
           hostName.endsWith(cloud::CloudConstants::BitbucketBaseUrlHost, Qt::CaseInsensitive);
}

bool BitbucketHostProvider::isSupported(const HttpResponse *response)
{
    if (!response) {
        return false;
    }
    // Identify Bitbucket on-prem instances from the HTTP response using the Atlassian specific header X-AREQUESTID
    const bool supported = response->headers.contains(QStringLiteral("X-AREQUESTID"));
    m_context.trace().writeLine(QStringLiteral("Host is%1 supported as Bitbucket").arg(supported ? QString() : QStringLiteral("n't")));
    return supported;
}

GitResponse BitbucketHostProvider::getCredential(const GitRequest &request)
{
    // We should not allow unencrypted communication and should inform the user
    if (!m_context.settings().allowUnsafeRemotes() && valueOr(request.protocol()).compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 && BitbucketHelper::isBitbucketOrg(request)) {
        throw Exception(QStringLiteral("Unencrypted HTTP is not recommended for Bitbucket.org. Ensure the repository remote URL is using HTTPS or see %1 about how to allow unsafe remotes.").arg(Constants::HelpUrls::GcmUnsafeRemotes));
    }

    const AuthenticationModes authModes = getSupportedAuthenticationModes(request);
    std::optional<Credential> credential = getStoredCredentials(request, authModes);
    if (!credential) {
        credential = getRefreshedCredentials(request, authModes);
    }
    return GitResponse(*credential);
}

std::optional<Credential> BitbucketHostProvider::getStoredCredentials(const GitRequest &request, AuthenticationModes authModes)
{
    ITrace &trace = m_context.trace();
    QString alwaysRefresh;
    if (m_context.settings().tryGetSetting(BitbucketConstants::EnvironmentVariables::AlwaysRefreshCredentials, CredCfg::SectionName, BitbucketConstants::GitConfiguration::Credential::AlwaysRefreshCredentials, alwaysRefresh) && toBooleanyOrDefault(alwaysRefresh, false)) {
        trace.writeLine(QStringLiteral("Ignore stored credentials"));
        return std::nullopt;
    }

    const QString credentialService = getServiceName(request.getRemoteUri().value_or(QUrl()));
    trace.writeLine(QStringLiteral("Look for existing credentials under %1 ...").arg(credentialService));

    const std::optional<Credential> credentials = getCredential(credentialService, request.userName());
    if (!credentials) {
        trace.writeLine(QStringLiteral("No stored credentials found"));
        return std::nullopt;
    }
    trace.writeLineSecrets(QStringLiteral("Found stored credentials: %1/{0}").arg(credentials->account), {credentials->password});

    // Check credentials are still valid
    if (!validateCredentialsWork(request, *credentials, authModes)) {
        return std::nullopt;
    }
    return credentials;
}

Credential BitbucketHostProvider::getRefreshedCredentials(const GitRequest &request, AuthenticationModes authModes)
{
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Refresh credentials..."));

    // Check for presence of refresh_token entry in credential store
    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());
    const QString refreshTokenService = getRefreshTokenServiceName(remoteUri);

    trace.writeLine(QStringLiteral("Checking for refresh token stored against user: '%1'...").arg(valueOr(request.userName())));
    // request.userName can either be null or the registered Bitbucket username of the user attempting the git
    // operation. With null the first refresh token stored for Bitbucket is found (refresh tokens are stored against
    // the resolved Bitbucket username); with a username the refresh token must exist under it. Without a refresh token
    // a full OAuth authorization flow is initiated.
    const std::optional<Credential> refreshToken = supportsOAuth(authModes) ? getCredential(refreshTokenService, request.userName()) : std::nullopt;

    if (!refreshToken) {
        trace.writeLine(QStringLiteral("No stored refresh token found"));
        // There is no refresh token either because this is a non-2FA enabled account (where OAuth is not required),
        // or because we previously erased the RT.
        trace.writeLine(QStringLiteral("Prompt for credentials..."));

        const CredentialsPromptResult result = m_bitbucketAuth->getCredentials(remoteUri, request.userName(), authModes);
        if (result.authenticationMode == AuthenticationModes::None) {
            const QString message = QStringLiteral("User cancelled credential prompt");
            trace.writeLine(message);
            throw Exception(message);
        }

        switch (result.authenticationMode) {
        case AuthenticationModes::Basic:
            // Return the valid credential
            return result.credential.value_or(Credential());
        case AuthenticationModes::OAuth:
            // If the user wants to use OAuth fall through to interactive auth
            break;
        default:
            throw ArgumentOutOfRangeException(QStringLiteral("Unexpected AuthenticationModes returned from prompt"));
        }
        // Fall through to the start of the interactive OAuth authentication flow
    } else {
        trace.writeLineSecrets(QStringLiteral("Found stored refresh token: {0}"), {refreshToken->password});
        try {
            trace.writeLine(QStringLiteral("Refreshing OAuth credentials using refresh token..."));
            const OAuth2TokenResult refreshResult = m_bitbucketAuth->refreshOAuthCredentials(request, refreshToken->password);
            return resolveCredsAndStoreRefreshToken(request, refreshResult);
        } catch (const OAuth2Exception &ex) {
            const QString message = QStringLiteral("Failed to refresh existing OAuth credential using refresh token");
            trace.writeLine(message);
            trace.writeException(ex);
            Trace2::writeError(message);
            // We failed to refresh the AT using the RT; log the refresh failure and fall through to restart the
            // OAuth authentication flow
        }
    }

    // We failed to use the refresh token either because it didn't exist, or because the refresh token is no longer
    // valid. Either way we must now try authenticating using OAuth interactively.
    trace.writeLine(QStringLiteral("Starting OAuth authentication flow..."));
    const OAuth2TokenResult oauthResult = m_bitbucketAuth->createOAuthCredentials(request);
    return resolveCredsAndStoreRefreshToken(request, oauthResult);
}

Credential BitbucketHostProvider::resolveCredsAndStoreRefreshToken(const GitRequest &request, const OAuth2TokenResult &tokenSet)
{
    ITrace &trace = m_context.trace();
    const QString refreshTokenService = getRefreshTokenServiceName(request.getRemoteUri().value_or(QUrl()));

    // Resolve the username to authenticate the git call
    trace.writeLine(QStringLiteral("Resolving username for OAuth credential..."));
    const QString bitbucketUsername = resolveOAuthUserName(request, tokenSet.accessToken);
    trace.writeLine(QStringLiteral("Username for OAuth credential is '%1'").arg(bitbucketUsername));

    // Store the new refresh token in the credential store against the resolved Bitbucket username
    trace.writeLine(QStringLiteral("Storing new refresh token against user: '%1'...").arg(bitbucketUsername));
    addOrUpdateCredential(refreshTokenService, bitbucketUsername, valueOr(tokenSet.refreshToken));
    trace.writeLine(QStringLiteral("Refresh token was successfully stored."));

    // Return the new AT as the credential
    return Credential{bitbucketUsername, tokenSet.accessToken};
}

AuthenticationModes BitbucketHostProvider::getSupportedAuthenticationModes(const GitRequest &request)
{
    ITrace &trace = m_context.trace();
    const QString remote = display(request.getRemoteUri().value_or(QUrl()));

    // Check for an explicit override for supported authentication modes
    QString authModesStr;
    if (m_context.settings().tryGetSetting(BitbucketConstants::EnvironmentVariables::AuthenticationModes, CredCfg::SectionName, BitbucketConstants::GitConfiguration::Credential::AuthenticationModes, authModesStr)) {
        AuthenticationModes authModes;
        if (tryParseAuthenticationModes(authModesStr, authModes) && authModes != AuthenticationModes::None) {
            trace.writeLine(QStringLiteral("Supported authentication modes override present: %1").arg(modesToString(authModes)));
            return authModes;
        }
        trace.writeLine(QStringLiteral("Invalid value for supported authentication modes override setting: '%1'").arg(authModesStr));
    }

    // It isn't possible to detect what Bitbucket.org is expecting so return the predefined answers.
    if (BitbucketHelper::isBitbucketOrg(request)) {
        // Bitbucket should use Basic, OAuth or manual PAT based authentication only
        trace.writeLine(QStringLiteral("%1 is bitbucket.org - authentication schemes: '%2'").arg(remote, modesToString(cloud::CloudConstants::DotOrgAuthenticationModes)));
        return cloud::CloudConstants::DotOrgAuthenticationModes;
    }

    // For Bitbucket DC/Server the supported modes can be detected
    trace.writeLine(QStringLiteral("%1 is Bitbucket DC - checking for supported authentication schemes...").arg(remote));

    try {
        AuthenticationModes modes = AuthenticationModes::None;
        if (m_restApiRegistry->get(request)->getAuthenticationMethods().contains(AuthenticationMethod::BasicAuth)) {
            modes |= AuthenticationModes::Basic;
        }
        if (m_restApiRegistry->get(request)->isOAuthInstalled()) {
            modes |= AuthenticationModes::OAuth;
        }
        trace.writeLine(QStringLiteral("Bitbucket DC/Server instance supports authentication schemes: %1").arg(modesToString(modes)));
        return modes;
    } catch (const std::exception &ex) {
        const QString message = QStringLiteral("Failed to query '%1' for supported authentication schemes.").arg(remote);
        trace.writeLine(message);
        trace.writeException(ex);
        Trace2::writeError(message);
        m_context.console().writeWarning(message);

        // Fall-back to offering all modes so the user is never blocked from authenticating by at least one mode
        return AuthenticationModes::All;
    }
}

void BitbucketHostProvider::storeCredential(const GitRequest &request)
{
    // It doesn't matter if this is an OAuth access token, or the literal username & password because we store them
    // the same way, against the same credential key in the store. The OAuth refresh token is already stored on the
    // 'get' request.
    const QString service = getServiceName(request.getRemoteUri().value_or(QUrl()));
    m_context.trace().writeLine(QStringLiteral("Storing credential..."));
    addOrUpdateCredential(service, valueOr(request.userName()), valueOr(request.password()));
    m_context.trace().writeLine(QStringLiteral("Credential was successfully stored."));
}

void BitbucketHostProvider::eraseCredential(const GitRequest &request)
{
    // Erase the stored credential (which may be either the literal username & password, or the OAuth access token).
    // We don't need to erase the OAuth refresh token because on the next 'get' request, if the RT is bad we will
    // erase and reacquire a new one at that point.
    const QString service = getServiceName(request.getRemoteUri().value_or(QUrl()));
    m_context.trace().writeLine(QStringLiteral("Erasing credential..."));
    if (m_context.credentialStore().remove(service, request.userName())) {
        m_context.trace().writeLine(QStringLiteral("Credential was successfully erased."));
    } else {
        m_context.trace().writeLine(QStringLiteral("Credential was not erased."));
    }
}

QString BitbucketHostProvider::resolveOAuthUserName(const GitRequest &request, const QString &accessToken)
{
    const UserInfoResult result = m_restApiRegistry->get(request)->getUserInformation(std::nullopt, accessToken, true);
    if (result.succeeded()) {
        return valueOr(result.userName);
    }
    throw Exception(QStringLiteral("Failed to resolve username. HTTP: %1").arg(httpStatusCodeName(result.statusCode)));
}

QString BitbucketHostProvider::resolveBasicAuthUserName(const GitRequest &request, const OptString &userName, const QString &password)
{
    const UserInfoResult result = m_restApiRegistry->get(request)->getUserInformation(userName, password, false);
    if (result.succeeded()) {
        return valueOr(result.userName);
    }
    throw Exception(QStringLiteral("Failed to resolve username. HTTP: %1").arg(httpStatusCodeName(result.statusCode)));
}

bool BitbucketHostProvider::validateCredentialsWork(const GitRequest &request, const Credential &credentials, AuthenticationModes authModes)
{
    ITrace &trace = m_context.trace();
    QString validate;
    if (m_context.settings().tryGetSetting(BitbucketConstants::EnvironmentVariables::ValidateStoredCredentials, CredCfg::SectionName, BitbucketConstants::GitConfiguration::Credential::ValidateStoredCredentials, validate) && !toBooleanyOrDefault(validate, true)) {
        trace.writeLine(QStringLiteral("Skipping validation of stored credentials due to %1 = %2").arg(BitbucketConstants::GitConfiguration::Credential::ValidateStoredCredentials, validate));
        return true;
    }

    // TODO (upstream): ideally we'd also check if the credentials have expired based on some local metadata.
    const QString remote = display(request.getRemoteUri().value_or(QUrl()));
    trace.writeLineSecrets(QStringLiteral("Validate credentials (%1/{0}) are fresh for %2 ...").arg(credentials.account, remote), {credentials.password});

    // Bitbucket supports both OAuth + Basic Auth unless there is explicit GCM configuration. The credentials could
    // be for either scheme therefore need to potentially test both possibilities.
    if (supportsOAuth(authModes)) {
        try {
            resolveOAuthUserName(request, credentials.password);
            trace.writeLine(QStringLiteral("Validated existing credentials using OAuth"));
            return true;
        } catch (const std::exception &ex) {
            const QString message = QStringLiteral("Failed to validate existing credentials using OAuth");
            trace.writeLine(message);
            trace.writeException(ex);
            Trace2::writeError(message);
        }
    }

    if (supportsBasicAuth(authModes)) {
        try {
            resolveBasicAuthUserName(request, credentials.account, credentials.password);
            trace.writeLine(QStringLiteral("Validated existing credentials using BasicAuth"));
            return true;
        } catch (const std::exception &ex) {
            const QString message = QStringLiteral("Failed to validate existing credentials using Basic Auth");
            trace.writeLine(message);
            trace.writeException(ex);
            Trace2::writeError(message);
            return false;
        }
    }

    return true;
}

// --- chunked secrets ------------------------------------------------------------------------
// Credential stores with a maximum secret size (`maxCredentialSize() > 0`) get long secrets split into chunks stored
// under "<service>/chunks" with accounts "<account>_<index>"; the usual service holds the descriptor "chunks=<n>".

QString BitbucketHostProvider::getChunkAccount(const QString &account, int chunkIndex)
{
    return QStringLiteral("%1_%2").arg(account).arg(chunkIndex);
}

QString BitbucketHostProvider::getChunkSecret(int chunkCount)
{
    return QStringLiteral("chunks=%1").arg(chunkCount);
}

QString BitbucketHostProvider::getChunkServiceName(const QString &service)
{
    return service + QStringLiteral("/chunks");
}

void BitbucketHostProvider::addOrUpdateCredential(const QString &service, const QString &account, const QString &secret)
{
    ITrace &trace = m_context.trace();
    ICredentialStore &store = m_context.credentialStore();
    const int chunkSize = store.maxCredentialSize();
    if (chunkSize > 0 && secret.size() > chunkSize) {
        QStringList chunks;
        for (qsizetype i = 0; i < secret.size(); i += chunkSize) {
            chunks.append(secret.mid(i, chunkSize));
        }
        trace.writeLine(QStringLiteral("Storing credential of length %1 in %2 chunks of size %3 for service: %4 and account: %5").arg(QString::number(secret.size()), QString::number(chunks.size()), QString::number(chunkSize), service, account));

        // The chunks go to a separate service (nested under the provided service). This ensures that retrieving
        // credentials for the provided service cannot trip up on the chunks when looking up with a null account.
        const QString chunkService = getChunkServiceName(service);
        for (int index = 0; index < chunks.size(); ++index) {
            const QString chunkAccount = getChunkAccount(account, index);
            trace.writeLineSecrets(QStringLiteral("Storing chunk for service: %1 and account: %2: {0}").arg(chunkService, chunkAccount), {chunks[index]});
            store.addOrUpdate(chunkService, chunkAccount, chunks[index]);
        }

        // A chunk descriptor against the account in the usual service tells (on get) that chunks were stored, and
        // how many to query.
        store.addOrUpdate(service, account, getChunkSecret(int(chunks.size())));
    } else {
        trace.writeLine(QStringLiteral("Storing credential of length %1 for service: %2 and account: %3").arg(QString::number(secret.size()), service, account));
        // A single chunk (the case on all credential stores except `wincredman`): stored as the old code used to.
        store.addOrUpdate(service, account, secret);
    }
}

std::optional<Credential> BitbucketHostProvider::getCredential(const QString &service, const OptString &account)
{
    ITrace &trace = m_context.trace();
    ICredentialStore &store = m_context.credentialStore();
    const std::optional<Credential> credential = store.get(service, account);
    if (!credential) {
        return std::nullopt;
    }

    // If the credential store has a valid configured chunk size, we have to support reading a chunked secret.
    static const QRegularExpression chunkRegex(QStringLiteral("^chunks=(?<count>\\d+)$"));
    const QRegularExpressionMatch match = store.maxCredentialSize() > 0 ? chunkRegex.match(credential->password) : QRegularExpressionMatch();
    if (match.hasMatch()) {
        const int chunkCount = match.captured(QStringLiteral("count")).toInt();
        // The account stored in the located credential builds the chunk identifiers
        const QString credentialAccount = credential->account;
        trace.writeLine(QStringLiteral("Found chunked credential (chunks=%1) for service: %2 and account: %3  stored against account: %4").arg(QString::number(chunkCount), service, valueOr(account), credentialAccount));

        QString secret;
        const QString chunkedService = getChunkServiceName(service);
        for (int index = 0; index < chunkCount; ++index) {
            const QString chunkAccount = getChunkAccount(credentialAccount, index);
            const std::optional<Credential> chunk = store.get(chunkedService, chunkAccount);
            if (!chunk) {
                trace.writeLine(QStringLiteral("Chunk %1 was unexpectedly null").arg(index));
                return std::nullopt;
            }
            trace.writeLineSecrets(QStringLiteral("Found chunk for service: %1 and account: %2: {0}").arg(chunkedService, chunkAccount), {chunk->password});
            secret += chunk->password;
        }
        // All the chunks stitched together into a singular secret, with the original account
        return Credential{credentialAccount, secret};
    }

    trace.writeLine(QStringLiteral("Found non-chunked credential for service: %1 and account: %2 stored against account: %3").arg(service, valueOr(account), credential->account));
    return credential;
}

QString BitbucketHostProvider::getServiceName(const QUrl &remoteUri)
{
    QString service = absoluteUri(withoutUserInfo(remoteUri));
    while (service.endsWith(QLatin1Char('/'))) {
        service.chop(1);
    }
    return service;
}

QString BitbucketHostProvider::getRefreshTokenServiceName(const QUrl &remoteUri)
{
    // The refresh token key never includes the path component. Instead we use the path component to specify this is
    // the "refresh_token".
    QUrl uri = withoutUserInfo(remoteUri);
    uri.setPath(QStringLiteral("/refresh_token"));
    QString service = absoluteUri(uri);
    while (service.endsWith(QLatin1Char('/'))) {
        service.chop(1);
    }
    return service;
}

} // namespace gcm::bitbucket
