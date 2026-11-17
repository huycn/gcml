// Ports GitHubHostProvider.cs, GitHubHostProvider.Commands.cs and Diagnostics/GitHubApiDiagnostic.cs.
#include "github.h"

#include "core/cmdline.h"
#include "core/commandcontext.h"
#include "core/commands.h"
#include "core/constants.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QVersionNumber>

namespace gcm::github {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

/// Scopes requested by the OAuth flows (browser and device code)
QStringList gitHubOAuthScopes()
{
    return {
        GitHubConstants::OAuthScopes::Repo,
        GitHubConstants::OAuthScopes::Gist,
        GitHubConstants::OAuthScopes::Workflow,
    };
}

/// Scopes of the personal access tokens created from a username and password
QStringList gitHubCredentialScopes()
{
    return {
        GitHubConstants::TokenScopes::Gist,
        GitHubConstants::TokenScopes::Repo,
    };
}

bool isGitHubDotComHost(const QString &hostName)
{
    // github.com or gist.github.com are both considered dotcom
    return hostName.compare(GitHubConstants::GitHubBaseUrlHost, Qt::CaseInsensitive) == 0 ||
           hostName.compare(GitHubConstants::GistBaseUrlHost, Qt::CaseInsensitive) == 0;
}

bool isGitHubProximaHost(const QString &hostName, QString *subdomain)
{
    const bool isProxima = hostName.endsWith(GitHubConstants::ProximaUrlHostSuffix, Qt::CaseInsensitive);
    if (subdomain) {
        // Normalize the subdomain (DNS is not case-sensitive)
        *subdomain = isProxima ? hostName.left(hostName.size() - GitHubConstants::ProximaUrlHostSuffix.size()).toLower() : QString();
    }
    return isProxima;
}

/// System.Version.TryParse: two to four non-negative integer components.
std::optional<QVersionNumber> tryParseVersion(const OptString &value)
{
    if (!value) {
        return std::nullopt;
    }
    const QStringList parts = value->split(QLatin1Char('.'));
    if (parts.size() < 2 || parts.size() > 4) {
        return std::nullopt;
    }
    QList<int> numbers;
    for (const QString &part : parts) {
        bool ok = false;
        const int n = part.trimmed().toInt(&ok);
        if (!ok || n < 0) {
            return std::nullopt;
        }
        numbers.append(n);
    }
    return QVersionNumber(numbers);
}

} // namespace

GitHubHostProvider::GitHubHostProvider(ICommandContext &context)
    : GitHubHostProvider(context, std::make_shared<GitHubRestApi>(context), std::make_shared<GitHubAuthentication>(context))
{
}

GitHubHostProvider::GitHubHostProvider(ICommandContext &context, std::shared_ptr<IGitHubRestApi> gitHubApi,
                                       std::shared_ptr<IGitHubAuthentication> gitHubAuth)
    : m_context(context), m_gitHubApi(std::move(gitHubApi)), m_gitHubAuth(std::move(gitHubAuth))
{
    EnsureArgument::notNull(m_gitHubApi, "gitHubApi");
    EnsureArgument::notNull(m_gitHubAuth, "gitHubAuth");
}

// --- detection ------------------------------------------------------------------------------

bool GitHubHostProvider::isSupported(const GitRequest &request)
{
    // We do not support unencrypted HTTP communications to GitHub, but we report `true` here for HTTP so that we
    // can show a helpful error message for the user in `generateCredential`.
    const QString protocol = valueOr(request.protocol());
    if (protocol.compare(QLatin1String("http"), Qt::CaseInsensitive) != 0 &&
        protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    // Split port number and hostname from host request argument
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        return false;
    }

    if (isGitHubDotComHost(hostName) || isGitHubProximaHost(hostName, nullptr)) {
        return true;
    }

    const QStringList domains = hostName.split(QLatin1Char('.'));

    // github[.subdomain].domain.tld
    if (domains.size() >= 3 && domains[0].compare(QLatin1String("github"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    // gist.github[.subdomain].domain.tld
    if (domains.size() >= 4 && domains[0].compare(QLatin1String("gist"), Qt::CaseInsensitive) == 0 &&
        domains[1].compare(QLatin1String("github"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    return false;
}

bool GitHubHostProvider::isSupported(const HttpResponse *response)
{
    // Look for a known GitHub.com/GHES header
    return response && response->headers.contains(QStringLiteral("X-GitHub-Request-Id"));
}

bool GitHubHostProvider::isGitHubDotCom(const OptString &targetUrl)
{
    const std::optional<QUrl> uri = tryCreateUri(targetUrl);
    return uri && isGitHubDotCom(*uri);
}

bool GitHubHostProvider::isGitHubDotCom(const QUrl &targetUri)
{
    EnsureArgument::absoluteUri(targetUri, "targetUri");
    return isGitHubDotComHost(targetUri.host());
}

bool GitHubHostProvider::isGitHubProxima(const QUrl &targetUri, QString *subdomain)
{
    EnsureArgument::absoluteUri(targetUri, "targetUri");
    return isGitHubProximaHost(targetUri.host(), subdomain);
}

QUrl GitHubHostProvider::normalizeUri(const QUrl &uri)
{
    // Special case for gist.github.com which are git backed repositories under the hood. Credentials for these
    // repositories are the same as the one stored with "github.com". Same for gist.github[.subdomain].domain.tld.
    // The general form was already checked via isSupported.
    const QString host = uri.host();
    const qsizetype firstDot = host.indexOf(QLatin1Char('.'));
    if (firstDot > -1 && host.left(firstDot).compare(QLatin1String("gist"), Qt::CaseInsensitive) == 0) {
        return QUrl(QStringLiteral("https://") + host.mid(firstDot + 1));
    }
    return uri;
}

QString GitHubHostProvider::getServiceName(const GitRequest &request)
{
    // Get the remote URI without user information
    const std::optional<QUrl> baseUri = request.getRemoteUri(false);
    if (!baseUri) {
        throw InvalidOperationException(QStringLiteral("Failed to determine the remote URI of the request."));
    }
    return getServiceName(*baseUri);
}

QString GitHubHostProvider::getServiceName(const QUrl &baseUri)
{
    // Normalise the URI and trim the trailing slash
    QString url = absoluteUri(normalizeUri(baseUri));
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url;
}

// --- get / store / erase --------------------------------------------------------------------

GitResponse GitHubHostProvider::getCredential(const GitRequest &request)
{
    ITrace &trace = m_context.trace();
    const QString service = getServiceName(request);
    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());

    // If we have a specific username then we can try and find an existing credential for that account. If not, we
    // should check what accounts are available in the store and prompt the user if there are multiple options.
    OptString userName = request.userName();
    bool addAccount = false;
    bool filtered = false;
    if (isNullOrWhiteSpace(userName)) {
        QStringList accounts = m_context.credentialStore().getAccounts(service);
        trace.writeLine(QStringLiteral("Found %1 accounts in the store for service=%2%3").arg(QString::number(accounts.size()), service, accounts.isEmpty() ? QStringLiteral(".") : QStringLiteral(":")));
        for (const QString &account : std::as_const(accounts)) {
            trace.writeLine(QStringLiteral("  %1").arg(account));
        }

        filtered = filterAccounts(remoteUri, request.wwwAuth(), accounts);

        if (accounts.size() == 1) {
            trace.writeLine(QStringLiteral("Only one account available - using that one!"));
            userName = accounts.constFirst();
        } else if (accounts.size() > 1) {
            trace.writeLine(QStringLiteral("Multiple accounts available - prompting user to select one..."));
            userName = m_gitHubAuth->selectAccount(remoteUri, accounts);
            addAccount = !userName.has_value();
        }
    }

    // Always try and locate an existing credential in the OS credential store unless we're being told to
    // explicitly add a new account OR have specifically filtered out irrelevant accounts. If the account lookup
    // failed for another reason we should still try to lookup an existing credential.
    std::optional<Credential> credential;
    if (addAccount) {
        trace.writeLine(QStringLiteral("Adding a new account!"));
    } else if (!isNullOrWhiteSpace(userName) || !filtered) {
        trace.writeLine(QStringLiteral("Looking for existing credential in store with service=%1 account=%2...").arg(service, valueOr(userName)));
        credential = m_context.credentialStore().get(service, userName);
    }

    if (!credential) {
        trace.writeLine(QStringLiteral("No existing credentials found."));
        // No existing credential was found, create a new one
        trace.writeLine(QStringLiteral("Creating new credential..."));
        credential = generateCredential(remoteUri, userName);
        trace.writeLine(QStringLiteral("Credential created."));
    } else {
        trace.writeLine(QStringLiteral("Existing credential found."));
    }

    return GitResponse(*credential);
}

bool GitHubHostProvider::filterAccounts(const QUrl &remoteUri, const QStringList &wwwAuth, QStringList &accounts)
{
    ITrace &trace = m_context.trace();
    if (!isGitHubDotCom(remoteUri)) {
        trace.writeLine(QStringLiteral("No account filtering outside of GitHub.com."));
        return false;
    }

    // Allow the user to disable account filtering until this feature stabilises. Default to enabled.
    QString enableFilteringStr;
    const bool enableFiltering = !m_context.settings().tryGetSetting(
        GitHubConstants::EnvironmentVariables::AccountFiltering,
        CredCfg::SectionName,
        GitHubConstants::GitConfiguration::Credential::AccountFiltering,
        enableFilteringStr
    ) || toBooleanyOrDefault(enableFilteringStr, true);

    if (!enableFiltering) {
        trace.writeLine(QStringLiteral("Account filtering is disabled."));
        return false;
    }

    trace.writeLine(QStringLiteral("Account filtering is enabled."));

    // If we have a WWW-Authenticate header then we can try and use any domain hint information to filter the list
    // of accounts to only those that are valid for that domain. We only expect one challenge header to be
    // returned, but if we're given more we just select the first.
    const QList<GitHubAuthChallenge> challenges = GitHubAuthChallenge::fromHeaders(wwwAuth);
    if (challenges.isEmpty()) {
        return false;
    }
    const GitHubAuthChallenge &authChallenge = challenges.constFirst();
    trace.writeLine(QStringLiteral("Filtering based on WWW-Authenticate header information..."));
    QStringList matched;
    for (const QString &account : std::as_const(accounts)) {
        if (authChallenge.isDomainMember(account)) {
            matched.append(account);
        }
    }
    accounts = matched;
    trace.writeLine(isNullOrWhiteSpace(authChallenge.domain())
                        ? QStringLiteral("Matched %1 accounts with public domain:").arg(accounts.size())
                        : QStringLiteral("Matched %1 accounts with domain=%2:").arg(QString::number(accounts.size()), *authChallenge.domain()));
    for (const QString &account : std::as_const(accounts)) {
        trace.writeLine(QStringLiteral("  %1").arg(account));
    }
    return true;
}

void GitHubHostProvider::storeCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);

    // WIA-authentication is signaled to Git as an empty username/password pair and we will get called to 'store'
    // these WIA credentials. We avoid storing empty credentials.
    if (isNullOrWhiteSpace(request.userName()) && isNullOrWhiteSpace(request.password())) {
        m_context.trace().writeLine(QStringLiteral("Not storing empty credential."));
        return;
    }

    // Add or update the credential in the store.
    m_context.trace().writeLine(QStringLiteral("Storing credential with service=%1 account=%2...").arg(service, valueOr(request.userName())));
    m_context.credentialStore().addOrUpdate(service, valueOr(request.userName()), valueOr(request.password()));
    m_context.trace().writeLine(QStringLiteral("Credential was successfully stored."));
}

void GitHubHostProvider::eraseCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);

    // Try to locate an existing credential
    m_context.trace().writeLine(QStringLiteral("Erasing stored credential in store with service=%1 account=%2...").arg(service, valueOr(request.userName())));
    if (m_context.credentialStore().remove(service, request.userName())) {
        m_context.trace().writeLine(QStringLiteral("Credential was successfully erased."));
    } else {
        m_context.trace().writeLine(QStringLiteral("No credential was erased."));
    }
}

// --- credential generation ------------------------------------------------------------------

Credential GitHubHostProvider::generateCredential(const QUrl &remoteUri, const OptString &userName)
{
    // We should not allow unencrypted communication and should inform the user
    if (!m_context.settings().allowUnsafeRemotes() && remoteUri.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) {
        throw Exception(QStringLiteral("Unencrypted HTTP is not recommended for GitHub. Ensure the repository remote URL is using HTTPS or see %1 about how to allow unsafe remotes.").arg(Constants::HelpUrls::GcmUnsafeRemotes));
    }

    const QString service = getServiceName(remoteUri);
    const AuthenticationModes authModes = getSupportedAuthenticationModes(remoteUri);
    const AuthenticationPromptResult promptResult = m_gitHubAuth->getAuthentication(remoteUri, userName, authModes);

    switch (promptResult.authenticationMode) {
    case AuthenticationModes::Basic: {
        const Credential patCredential = generatePersonalAccessToken(remoteUri, promptResult.credential.value_or(Credential()));
        // HACK: Store the PAT immediately in case this PAT is not valid for SSO. We don't know if this PAT is valid
        // for SAML SSO and if it's not Git will fail with a 403 and call neither 'store' or 'erase'. The user is
        // expected to fiddle with the PAT permissions manually on the web and then retry the Git operation. We must
        // store the PAT now so they can resume/repeat the operation with the same, now SSO authorized, PAT.
        // See: https://github.com/git-ecosystem/git-credential-manager/issues/133
        m_context.credentialStore().addOrUpdate(service, patCredential.account, patCredential.password);
        return patCredential;
    }
    case AuthenticationModes::Browser:
        return generateOAuthCredential(remoteUri, userName, true);
    case AuthenticationModes::Device:
        return generateOAuthCredential(remoteUri, userName, false);
    case AuthenticationModes::Pat: {
        // The token returned by the user should be good to use directly as the password for Git
        const Credential prompted = promptResult.credential.value_or(Credential());
        // Resolve the GitHub user handle if we don't have a specific username already from the initial request.
        // The reason for this is GitHub requires a (any?) value for the username when Git makes calls to GitHub.
        QString login = prompted.account;
        if (login.isEmpty()) {
            login = m_gitHubApi->getUserInfo(remoteUri, prompted.password).login;
        }
        return Credential{login, prompted.password};
    }
    default:
        throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values. (Parameter 'promptResult')"));
    }
}

Credential GitHubHostProvider::generateOAuthCredential(const QUrl &targetUri, const OptString &loginHint, bool useBrowser)
{
    const OAuth2TokenResult result = useBrowser ? m_gitHubAuth->getOAuthTokenViaBrowser(targetUri, gitHubOAuthScopes(), loginHint)
                                                : m_gitHubAuth->getOAuthTokenViaDeviceCode(targetUri, gitHubOAuthScopes());

    // Resolve the GitHub user handle
    const GitHubUserInfo userInfo = m_gitHubApi->getUserInfo(targetUri, result.accessToken);
    return Credential{userInfo.login, result.accessToken};
}

Credential GitHubHostProvider::generatePersonalAccessToken(const QUrl &targetUri, const Credential &credentials)
{
    AuthenticationResult result = m_gitHubApi->createPersonalToken(targetUri, credentials, std::nullopt, gitHubCredentialScopes());

    OptString token;
    if (result.type == GitHubAuthenticationResultType::Success) {
        m_context.trace().writeLine(QStringLiteral("Token acquisition for '%1' succeeded").arg(absoluteUri(targetUri)));
        token = result.token;
    } else if (result.type == GitHubAuthenticationResultType::TwoFactorApp || result.type == GitHubAuthenticationResultType::TwoFactorSms) {
        const bool isSms = result.type == GitHubAuthenticationResultType::TwoFactorSms;
        const QString authCode = m_gitHubAuth->getTwoFactorCode(targetUri, isSms);
        result = m_gitHubApi->createPersonalToken(targetUri, credentials, authCode, gitHubCredentialScopes());
        if (result.type == GitHubAuthenticationResultType::Success) {
            m_context.trace().writeLine(QStringLiteral("Token acquisition for '%1' succeeded.").arg(absoluteUri(targetUri)));
            token = result.token;
        }
    }

    if (token) {
        // Resolve the GitHub user handle
        const GitHubUserInfo userInfo = m_gitHubApi->getUserInfo(targetUri, *token);
        return Credential{userInfo.login, *token};
    }

    throw Exception(QStringLiteral("Interactive logon for '%1' failed.").arg(absoluteUri(targetUri)));
}

AuthenticationModes GitHubHostProvider::getSupportedAuthenticationModes(const QUrl &targetUri)
{
    ITrace &trace = m_context.trace();

    // Check for an explicit override for supported authentication modes
    QString authModesStr;
    if (m_context.settings().tryGetSetting(GitHubConstants::EnvironmentVariables::AuthenticationModes, CredCfg::SectionName,
                                           GitHubConstants::GitConfiguration::Credential::AuthenticationModes, authModesStr)) {
        AuthenticationModes authModes;
        if (tryParseAuthenticationModes(authModesStr, authModes) && authModes != AuthenticationModes::None) {
            trace.writeLine(QStringLiteral("Supported authentication modes override present: %1").arg(modesToString(authModes)));
            return authModes;
        }
        trace.writeLine(QStringLiteral("Invalid value for supported authentication modes override setting: '%1'").arg(authModesStr));
    }

    // GitHub.com should use OAuth or manual PAT based authentication only, never basic auth as of 13th November 2020
    // https://developer.github.com/changes/2020-02-14-deprecating-oauth-auth-endpoint
    const AuthenticationModes dotComModes = AuthenticationModes::OAuth | AuthenticationModes::Pat;
    if (isGitHubDotCom(targetUri)) {
        trace.writeLine(QStringLiteral("%1 is github.com - authentication schemes: '%2'").arg(absoluteUri(targetUri), modesToString(dotComModes)));
        return dotComModes;
    }

    const AuthenticationModes proximaModes = AuthenticationModes::OAuth | AuthenticationModes::Pat;
    if (isGitHubProxima(targetUri)) {
        trace.writeLine(QStringLiteral("%1 is Proxima - authentication schemes: '%2'").arg(absoluteUri(targetUri), modesToString(proximaModes)));
        return proximaModes;
    }

    // For GitHub Enterprise we must do some detection of supported modes
    trace.writeLine(QStringLiteral("%1 is GitHub Enterprise - checking for supported authentication schemes...").arg(absoluteUri(targetUri)));

    try {
        const GitHubMetaInfo metaInfo = m_gitHubApi->getMetaInfo(targetUri);

        // All Enterprise/AE instances support PATs
        AuthenticationModes modes = AuthenticationModes::Pat;

        // If the server says it supports basic auth, we can use that too!
        if (metaInfo.verifiablePasswordAuthentication) {
            modes |= AuthenticationModes::Basic;
        }

        // If the version is unknown, we *assume* it supports OAuth.
        // If the server version at least the minimum required, we *know* we can use OAuth.
        const std::optional<QVersionNumber> version = tryParseVersion(metaInfo.installedVersion);
        if (!version || *version >= QVersionNumber::fromString(GitHubConstants::MinimumOnPremOAuthVersion)) {
            modes |= AuthenticationModes::OAuth;
        }

        trace.writeLine(QStringLiteral("GitHub Enterprise instance has version '%1' and supports authentication schemes: %2").arg(valueOr(metaInfo.installedVersion), modesToString(modes)));
        return modes;
    } catch (const std::exception &ex) {
        const QString message = QStringLiteral("Failed to query '%1' for supported authentication schemes.").arg(absoluteUri(targetUri));
        trace.writeLine(message);
        trace.writeException(ex);
        Trace2::writeError(message);
        m_context.console().writeWarning(message);

        // Fall-back to offering all modes so the user is never blocked from authenticating by at least one mode
        return AuthenticationModes::All;
    }
}

// --- diagnostics ----------------------------------------------------------------------------

QList<std::shared_ptr<IDiagnostic>> GitHubHostProvider::getDiagnostics()
{
    return {std::make_shared<GitHubApiDiagnostic>(m_gitHubApi, m_context)};
}

void GitHubApiDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    const QUrl targetUri(QStringLiteral("https://github.com"));
    reporter.reportInfo(QStringLiteral("Using '%1' as API target.").arg(absoluteUri(targetUri)));
    reporter.reportProgress(QStringLiteral("Querying '/meta' endpoint"));
    m_api->getMetaInfo(targetUri);
}

// --- commands -------------------------------------------------------------------------------

std::shared_ptr<Command> GitHubHostProvider::createCommand()
{
    auto rootCmd = std::make_shared<ProviderCommand>(*this);
    const Option urlOpt = Option::value(QStringLiteral("--url"), QStringLiteral("URL of the GitHub instance to target, otherwise use GitHub.com"));

    // list [--url <url>]
    auto listCmd = std::make_shared<Command>(QStringLiteral("list"), QStringLiteral("List all known GitHub accounts."));
    listCmd->addOption(urlOpt);
    listCmd->setHandler([this](const ParseResult &r) { return listAccounts(r.value(QStringLiteral("--url"))); });

    // login [--url <url>] [--username <username>] [--device | --browser/--web | --pat/--token <token>]
    auto loginCmd = std::make_shared<Command>(QStringLiteral("login"), QStringLiteral("Add a GitHub account."));
    loginCmd->addOption(urlOpt);
    loginCmd->addOption(Option::value(QStringLiteral("--username"), QStringLiteral("User name to authenticate with")));
    loginCmd->addOption(Option::flag(QStringLiteral("--device"), QStringLiteral("Use device flow to authenticate")));
    loginCmd->addOption(Option{{QStringLiteral("--web"), QStringLiteral("--browser")}, QStringLiteral("Use a web browser to authenticate")});
    loginCmd->addOption(Option{{QStringLiteral("--pat"), QStringLiteral("--token")}, QStringLiteral("Use personal access token to authenticate"), true});
    loginCmd->addOption(Option::flag(QStringLiteral("--force"), QStringLiteral("Force re-authentication even if a credential already exists for the account")));
    // AddOptionSet(OptionArity.ZeroOrOne, device, browser, token): options are named after their longest alias
    loginCmd->addValidator([](const ParseResult &r) -> OptString {
        const int count = int(r.has(QStringLiteral("--device"))) + int(r.has(QStringLiteral("--web"))) + int(r.has(QStringLiteral("--pat")));
        if (count > 1) {
            return QStringLiteral("Can only specify one of: --device, --browser, --token");
        }
        return std::nullopt;
    });
    loginCmd->setHandler([this](const ParseResult &r) {
        return addAccount(r.value(QStringLiteral("--url")), r.value(QStringLiteral("--username")), r.has(QStringLiteral("--device")),
                          r.has(QStringLiteral("--web")), r.value(QStringLiteral("--pat")), r.has(QStringLiteral("--force")));
    });

    // logout <account> [--url <url>]
    auto logoutCmd = std::make_shared<Command>(QStringLiteral("logout"), QStringLiteral("Remove a GitHub account."));
    logoutCmd->addArgument(Argument{QStringLiteral("account"), QStringLiteral("Account to remove"), true});
    logoutCmd->addOption(urlOpt);
    logoutCmd->setHandler([this](const ParseResult &r) {
        return removeAccount(valueOr(r.argument(QStringLiteral("account"))), r.value(QStringLiteral("--url")));
    });

    rootCmd->addCommand(listCmd);
    rootCmd->addCommand(loginCmd);
    rootCmd->addCommand(logoutCmd);
    return rootCmd;
}

namespace {

/// Option<Uri>: the value as an absolute URI (System.CommandLine fails the parse for an invalid one).
std::optional<QUrl> parseUrlOption(const OptString &url)
{
    if (!url) {
        return std::nullopt;
    }
    std::optional<QUrl> uri = tryCreateUri(url);
    if (!uri) {
        throw ArgumentException(QStringLiteral("Cannot parse argument '%1' for option '--url' as expected type 'System.Uri'.").arg(*url));
    }
    return uri;
}

} // namespace

int GitHubHostProvider::listAccounts(const OptString &url)
{
    const std::optional<QUrl> uri = parseUrlOption(url);
    const QString service = !uri || isGitHubDotCom(*uri) ? QStringLiteral("https://%1").arg(GitHubConstants::GitHubBaseUrlHost) : getServiceName(*uri);
    const QStringList accounts = m_context.credentialStore().getAccounts(service);
    for (const QString &account : accounts) {
        m_context.console().writeLine(account);
    }
    return 0;
}

int GitHubHostProvider::addAccount(const OptString &url, OptString userName, bool device, bool browser, const OptString &token, bool force)
{
    // Default to GitHub.com
    const QUrl uri = parseUrlOption(url).value_or(QUrl(QStringLiteral("https://%1").arg(GitHubConstants::GitHubBaseUrlHost)));

    // Prefer the username specified on the command-line
    if (!userName && !uri.userName().isEmpty()) {
        userName = uri.userName();
    }

    const QString service = getServiceName(uri);

    // If we've already got a credential for this account then we can skip the login flow (so long as the user
    // isn't explicitly forcing a re-authentication).
    if (!isNullOrWhiteSpace(userName) && !force) {
        const QStringList existingAccounts = m_context.credentialStore().getAccounts(service);
        if (existingAccounts.contains(*userName, Qt::CaseInsensitive)) {
            QString prettyUrl = absoluteUri(uri);
            while (prettyUrl.endsWith(QLatin1Char('/'))) {
                prettyUrl.chop(1);
            }
            m_context.console().writeLine(QStringLiteral("Account '%1' already has credentials for %2; use --force to re-authenticate").arg(*userName, prettyUrl));
            return 0;
        }
    }

    Credential credential;
    if (token) {
        // Resolve the GitHub user handle if the user didn't supply one
        if (valueOr(userName).isEmpty()) {
            userName = m_gitHubApi->getUserInfo(uri, *token).login;
        }
        credential = Credential{*userName, *token};
    } else if (device || browser) {
        credential = generateOAuthCredential(uri, userName, browser);
    } else {
        credential = generateCredential(uri, userName);
    }

    m_context.credentialStore().addOrUpdate(service, credential.account, credential.password);
    return 0;
}

int GitHubHostProvider::removeAccount(const QString &account, const OptString &url)
{
    const std::optional<QUrl> uri = parseUrlOption(url);
    const QString service = !uri || isGitHubDotCom(*uri) ? QStringLiteral("https://%1").arg(GitHubConstants::GitHubBaseUrlHost)
                                                         : getServiceName(*uri);
    if (!m_context.credentialStore().remove(service, account)) {
        m_context.console().writeWarning(QStringLiteral("no such account '%1' found.").arg(account));
        return -1;
    }
    return 0;
}

} // namespace gcm::github
