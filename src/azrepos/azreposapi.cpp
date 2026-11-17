// Ports UriHelpers.cs, AzureDevOpsRestApi.cs, AzureDevOpsAuthorityCache.cs and AzureReposBindingManager.cs.
#include "azrepos.h"

#include "core/commandcontext.h"
#include "core/strings.h"

#include <QRegularExpression>
#include <QSysInfo>
#include <QUuid>

namespace gcm::azrepos {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

/// Guid.TryParse for the forms that appear in Azure DevOps headers; nullopt when not a GUID.
std::optional<QUuid> tryParseGuid(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("^[{(]?[0-9a-fA-F]{8}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{12}[})]?$"));
    const QString s = value.trimmed();
    if (!re.match(s).hasMatch()) {
        return std::nullopt;
    }
    QString hex = s;
    static const QRegularExpression nonHex(QStringLiteral("[^0-9a-fA-F]"));
    hex.remove(nonHex);
    return QUuid(QStringLiteral("%1-%2-%3-%4-%5").arg(hex.mid(0, 8), hex.mid(8, 4), hex.mid(12, 4), hex.mid(16, 4), hex.mid(20)));
}

QString authorityKey(const QString &orgName)
{
    return QStringLiteral("%1.%2:%3/%4.%5").arg(CredCfg::SectionName, AzureDevOpsConstants::UrnScheme, AzureDevOpsConstants::UrnOrgPrefix, orgName,
                                                AzureDevOpsConstants::GitConfiguration::Credential::AzureAuthority);
}

QString orgUserKey(const QString &orgName)
{
    return QStringLiteral("%1.%2:%3/%4.%5").arg(CredCfg::SectionName, AzureDevOpsConstants::UrnScheme, AzureDevOpsConstants::UrnOrgPrefix, orgName,
                                                QStringLiteral("username"));
}

/// The organization of an "azrepos:org/<org>" scope, if the key has one.
OptString orgOfScope(const QString &key)
{
    QString section, property;
    OptString scope;
    if (!trySplitGitConfigKey(key, section, scope, property) || !scope) {
        return std::nullopt;
    }
    const std::optional<QUrl> uri = tryCreateUri(*scope);
    const QString orgPrefix = AzureDevOpsConstants::UrnOrgPrefix + QLatin1Char('/');
    if (!uri || uri->scheme() != AzureDevOpsConstants::UrnScheme || !uri->path().startsWith(orgPrefix)) {
        return std::nullopt;
    }
    return uri->path().mid(orgPrefix.size());
}

} // namespace

// =============================================================================================
// UriHelpers
// =============================================================================================

QString UriHelpers::combinePath(const QString &basePath, const QString &path)
{
    if (!basePath.isEmpty() && !path.isEmpty()) {
        const QChar lastBasePath = basePath.back();
        const QChar firstPath = path.front();
        if (lastBasePath == QLatin1Char('/') && firstPath == QLatin1Char('/')) {
            return basePath + path.mid(1);
        }
        if (lastBasePath != QLatin1Char('/') && firstPath != QLatin1Char('/')) {
            return basePath + QLatin1Char('/') + path;
        }
    }
    return basePath + path;
}

bool UriHelpers::isVisualStudioComHost(const OptString &host)
{
    return host && host->endsWith(AzureDevOpsConstants::VstsHostSuffix, Qt::CaseInsensitive);
}

bool UriHelpers::isVisualStudioComHost(const GitRequest &request)
{
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        throw InvalidOperationException(QStringLiteral("Host name and/or port is invalid."));
    }
    return isVisualStudioComHost(OptString(hostName));
}

bool UriHelpers::isDevAzureComHost(const OptString &host)
{
    return host && host->compare(AzureDevOpsConstants::AzureDevOpsHost, Qt::CaseInsensitive) == 0;
}

bool UriHelpers::isDevAzureComHost(const GitRequest &request)
{
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        throw InvalidOperationException(QStringLiteral("Host name and/or port is invalid."));
    }
    return isDevAzureComHost(OptString(hostName));
}

bool UriHelpers::isAzureDevOpsHost(const OptString &host)
{
    return isVisualStudioComHost(host) || isDevAzureComHost(host);
}

QString UriHelpers::getOrganizationName(const QUrl &remoteUri)
{
    QString orgName;
    createOrganizationUri(remoteUri, orgName);
    return orgName;
}

OptString UriHelpers::getFirstPathComponent(const OptString &path)
{
    if (!path) {
        return std::nullopt;
    }
    const QStringList parts = path->split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? OptString() : OptString(parts.constFirst());
}

QUrl UriHelpers::createOrganizationUri(const QUrl &remoteUri, QString &orgName)
{
    EnsureArgument::absoluteUri(remoteUri, "remoteUri");
    orgName.clear();

    if (!isAzureDevOpsHost(remoteUri.host())) {
        throw InvalidOperationException(QStringLiteral("Host is not Azure DevOps."));
    }

    QUrl org;
    org.setScheme(remoteUri.scheme());
    org.setHost(remoteUri.host());
    if (!isDefaultPort(remoteUri)) {
        org.setPort(remoteUri.port());
    }

    // Extract the organization name for Azure ('dev.azure.com') style URLs. The older *.visualstudio.com URLs
    // contained the organization name in the host already.
    if (isDevAzureComHost(remoteUri.host())) {
        // Uri.AbsolutePath: the escaped path
        const OptString firstPathComponent = getFirstPathComponent(remoteUri.path(QUrl::FullyEncoded));
        const OptString remoteUriUserName = getUserName(remoteUri);
        // Prefer getting the org name from the path: dev.azure.com/{org}
        if (!isNullOrWhiteSpace(firstPathComponent)) {
            orgName = *firstPathComponent;
        }
        // Failing that try using the username: {org}@dev.azure.com
        else if (!isNullOrWhiteSpace(remoteUriUserName)) {
            orgName = *remoteUriUserName;
        } else {
            throw InvalidOperationException(QStringLiteral("Cannot determine the organization name for this 'dev.azure.com' remote URL. Ensure the `credential.useHttpPath` configuration value is set, or set the organization name as the user in the remote URL '{org}@dev.azure.com'."));
        }
        org.setPath(QLatin1Char('/') + orgName, QUrl::TolerantMode);
    } else if (isVisualStudioComHost(remoteUri.host())) {
        // {org}.visualstudio.com
        orgName = remoteUri.host().left(remoteUri.host().size() - AzureDevOpsConstants::VstsHostSuffix.size());
        org.setPath(QStringLiteral("/"));
    }
    return org;
}

// =============================================================================================
// AzureDevOpsRestApi
// =============================================================================================

HttpClient &AzureDevOpsRestApi::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
        // Configure the HTTP client with standard headers for Azure Repos API calls
        m_httpClient->defaultRequestHeaders().add(QStringLiteral("Accept"), Constants::Http::MimeTypeJson);
    }
    return *m_httpClient;
}

bool AzureDevOpsRestApi::tryGetAuthorityFromHeader(const OptString &header, QString &authority)
{
    authority.clear();
    if (!header) {
        return false;
    }
    // We're looking for a "Bearer" scheme header with a parameter
    const QString value = header->trimmed();
    const qsizetype space = value.indexOf(QLatin1Char(' '));
    if (space < 0 || value.left(space).compare(Constants::Http::WwwAuthenticateBearerScheme, Qt::CaseInsensitive) != 0) {
        return false;
    }
    static const QRegularExpression re(QStringLiteral("^authorization_uri=(?<authority>.+)$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(value.mid(space + 1).trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    authority = match.captured(QStringLiteral("authority"));
    return true;
}

bool AzureDevOpsRestApi::tryGetFirstJsonStringField(const OptString &json, const QString &fieldName, QString &value)
{
    value.clear();
    if (isNullOrWhiteSpace(json)) {
        return false;
    }
    // Find the '"<field>" : "<value>"' portion of the JSON content
    const QRegularExpression re(QStringLiteral("\"%1\"\\s*\\:\\s*\"(?<value>[^\"]+)\"").arg(QRegularExpression::escape(fieldName)),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(*json);
    if (!match.hasMatch()) {
        return false;
    }
    value = match.captured(QStringLiteral("value"));
    return true;
}

QString AzureDevOpsRestApi::getAuthority(const QUrl &organizationUri)
{
    EnsureArgument::absoluteUri(organizationUri, "organizationUri");
    ITrace &trace = m_context.trace();

    const QString authorityBase = AzureDevOpsConstants::AadAuthorityBaseUrl;
    const QString commonAuthority = authorityBase + QStringLiteral("/common");
    // We should be using "/common" or "/consumer" as the authority for MSA but since Azure DevOps uses MSA
    // pass-through (an internal hack to support MSA and AAD accounts in the same auth stack), which actually need to
    // consult the "/organizations" authority instead.
    const QString msaAuthority = authorityBase + QStringLiteral("/organizations");

    trace.writeLine(QStringLiteral("HTTP: HEAD %1").arg(absoluteUri(organizationUri)));
    const HttpResponse response = httpClient().head(organizationUri);
    trace.writeLine(QStringLiteral("HTTP: Response code ignored."));
    trace.writeLine(QStringLiteral("Inspecting headers..."));

    // Check WWW-Authenticate headers first; we prefer these
    const QStringList wwwAuthenticate = response.wwwAuthenticate();
    for (const QString &header : wwwAuthenticate) {
        QString authority;
        if (tryGetAuthorityFromHeader(header, authority)) {
            trace.writeLine(QStringLiteral("Found WWW-Authenticate header with Bearer authority '%1'.").arg(authority));
            return authority;
        }
    }

    // We didn't find a bearer WWW-Auth header; check for the X-VSS-ResourceTenant header
    if (response.headers.contains(AzureDevOpsConstants::VssResourceTenantHeader)) {
        const QStringList tenantIds = response.headers.values(AzureDevOpsConstants::VssResourceTenantHeader);
        // Take the first tenant ID that isn't an empty GUID
        for (const QString &tenantId : tenantIds) {
            const std::optional<QUuid> guid = tryParseGuid(tenantId);
            if (guid && !guid->isNull()) {
                trace.writeLine(QStringLiteral("Found %1 header with AAD tenant ID '%2'.").arg(AzureDevOpsConstants::VssResourceTenantHeader, tenantId));
                return authorityBase + QLatin1Char('/') + tenantId;
            }
        }
        // If we have exactly one empty GUID then this is a MSA backed organization
        if (tenantIds.size() == 1) {
            const std::optional<QUuid> guid = tryParseGuid(tenantIds[0]);
            if (guid && guid->isNull()) {
                trace.writeLine(QStringLiteral("Found %1 header with MSA tenant ID (empty GUID).").arg(AzureDevOpsConstants::VssResourceTenantHeader));
                return msaAuthority;
            }
        }
    }

    // Use the common authority if we can't determine a specific one
    trace.writeLine(QStringLiteral("Unable to determine AAD/MSA tenant - falling back to common authority"));
    return commonAuthority;
}

QUrl AzureDevOpsRestApi::getIdentityServiceUri(const QUrl &organizationUri, const QString &accessToken)
{
    const QString locationServicePath = QStringLiteral("_apis/ServiceDefinitions/LocationService2/951917AC-A960-4999-8464-E3F0AA25B381");
    QUrl requestUri = organizationUri;
    requestUri.setPath(UriHelpers::combinePath(organizationUri.path().isEmpty() ? QStringLiteral("/") : organizationUri.path(), locationServicePath));
    requestUri.setQuery(QStringLiteral("api-version=1.0"));

    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    HttpRequest request;
    request.url = requestUri;
    request.setBearerAuthorization(accessToken);
    const HttpResponse response = httpClient().send(request);
    m_context.trace().writeLine(QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), httpStatusCodeName(response.statusCode)));
    if (response.isSuccessStatusCode()) {
        QString location;
        if (tryGetFirstJsonStringField(response.contentString(), QStringLiteral("location"), location)) {
            if (const std::optional<QUrl> identityService = tryCreateUri(location)) {
                return *identityService;
            }
        }
    }
    throw Exception(QStringLiteral("Failed to find location service"));
}

QString AzureDevOpsRestApi::createPersonalAccessToken(const QUrl &organizationUri, const QString &accessToken, const QStringList &scopes)
{
    const QString sessionTokenUrl = QStringLiteral("_apis/token/sessiontokens?api-version=1.0&tokentype=compact");

    EnsureArgument::absoluteUri(organizationUri, "organizationUri");
    if (!UriHelpers::isAzureDevOpsHost(organizationUri.host())) {
        throw ArgumentException(QStringLiteral("Provided URI '%1' is not a valid Azure DevOps hostname (Parameter 'organizationUri')").arg(absoluteUri(organizationUri)));
    }

    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Getting Azure DevOps Identity Service endpoint..."));
    const QUrl identityServiceUri = getIdentityServiceUri(organizationUri, accessToken);
    trace.writeLine(QStringLiteral("Identity Service endpoint is '%1'.").arg(absoluteUri(identityServiceUri)));

    const QUrl requestUri = identityServiceUri.resolved(QUrl(sessionTokenUrl));
    trace.writeLine(QStringLiteral("HTTP: POST %1").arg(absoluteUri(requestUri)));

    const QString json = QStringLiteral("{ \"scope\" : \"%1\", \"displayName\" : \"Git: %2 on %3\" }").arg(scopes.join(QLatin1Char(' ')), absoluteUri(organizationUri), QSysInfo::machineHostName());
    HttpRequest request;
    request.method = Constants::Http::MethodPost;
    request.url = requestUri;
    request.setJsonContent(json.toUtf8());
    request.setBearerAuthorization(accessToken);
    const HttpResponse response = httpClient().send(request);
    trace.writeLine(QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), httpStatusCodeName(response.statusCode)));

    const QString responseText = response.contentString();
    if (!isNullOrWhiteSpace(responseText)) {
        QString value;
        if (response.isSuccessStatusCode()) {
            if (tryGetFirstJsonStringField(responseText, QStringLiteral("token"), value)) {
                return value;
            }
        } else if (tryGetFirstJsonStringField(responseText, QStringLiteral("message"), value)) {
            throw Exception(QStringLiteral("Failed to create PAT: %1").arg(value));
        }
    }
    throw Exception(QStringLiteral("Failed to create PAT"));
}

// =============================================================================================
// AzureDevOpsAuthorityCache
// =============================================================================================

OptString AzureDevOpsAuthorityCache::getAuthority(const QString &orgName)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    m_trace.writeLine(QStringLiteral("Looking up cached authority for organization '%1'...").arg(orgName));
    QString authority;
    if (m_git.getConfiguration().tryGet(GitConfigurationLevel::Global, GitConfigurationType::Raw, authorityKey(orgName), authority)) {
        return authority;
    }
    return std::nullopt;
}

void AzureDevOpsAuthorityCache::updateAuthority(const QString &orgName, const QString &authority)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    m_trace.writeLine(QStringLiteral("Updating cached authority for '%1' to '%2'...").arg(orgName, authority));
    m_git.getConfiguration().set(GitConfigurationLevel::Global, authorityKey(orgName), authority);
}

void AzureDevOpsAuthorityCache::eraseAuthority(const QString &orgName)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    m_trace.writeLine(QStringLiteral("Removing cached authority for '%1'...").arg(orgName));
    m_git.getConfiguration().unset(GitConfigurationLevel::Global, authorityKey(orgName));
}

void AzureDevOpsAuthorityCache::clear()
{
    m_trace.writeLine(QStringLiteral("Clearing all cached authorities..."));
    IGitConfiguration &config = m_git.getConfiguration();
    QStringList orgKeys;
    config.enumerate(GitConfigurationLevel::Global, CredCfg::SectionName, AzureDevOpsConstants::GitConfiguration::Credential::AzureAuthority,
                     [&](const GitConfigurationEntry &entry) {
                         if (orgOfScope(entry.key) &&
                             std::none_of(orgKeys.begin(), orgKeys.end(), [&](const QString &k) { return gitConfigKeyEquals(k, entry.key); })) {
                             orgKeys.append(entry.key);
                         }
                         return true;
                     });
    for (const QString &orgKey : std::as_const(orgKeys)) {
        config.unset(GitConfigurationLevel::Global, orgKey);
    }
}

// =============================================================================================
// AzureReposBindingManager
// =============================================================================================

std::optional<AzureReposBinding> AzureReposBindingManager::getBinding(const QString &orgName)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    const QString orgKey = orgUserKey(orgName);
    IGitConfiguration &config = m_git.getConfiguration();

    m_trace.writeLine(QStringLiteral("Looking up organization binding for '%1'...").arg(orgName));

    // Can only check local config if we are inside a repository
    QString localUser, globalUser;
    const bool hasLocal = m_git.isInsideRepository() && config.tryGet(GitConfigurationLevel::Local, GitConfigurationType::Raw, orgKey, localUser);
    const bool hasGlobal = config.tryGet(GitConfigurationLevel::Global, GitConfigurationType::Raw, orgKey, globalUser);
    if (hasLocal || hasGlobal) {
        return AzureReposBinding{orgName, hasGlobal ? OptString(globalUser) : std::nullopt, hasLocal ? OptString(localUser) : std::nullopt};
    }
    // No bound user
    return std::nullopt;
}

void AzureReposBindingManager::bind(const QString &orgName, const QString &userName, bool local)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    IGitConfiguration &config = m_git.getConfiguration();
    const QString key = orgUserKey(orgName);
    if (local) {
        m_trace.writeLine(userName == AzureReposBinding::NoInherit
                              ? QStringLiteral("Setting binding to 'do not inherit' for organization '%1' in local repository...").arg(orgName)
                              : QStringLiteral("Binding user '%1' to organization '%2' in local repository...").arg(userName, orgName));
        if (m_git.isInsideRepository()) {
            config.set(GitConfigurationLevel::Local, key, userName);
        } else {
            m_trace.writeLine(QStringLiteral("Cannot set local configuration binding - not inside a repository!"));
        }
    } else {
        EnsureArgument::notNullOrWhiteSpace(userName, "userName");
        m_trace.writeLine(QStringLiteral("Binding user '%1' to organization '%2' in global configuration...").arg(userName, orgName));
        config.set(GitConfigurationLevel::Global, key, userName);
    }
}

void AzureReposBindingManager::unbind(const QString &orgName, bool local)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    IGitConfiguration &config = m_git.getConfiguration();
    const QString key = orgUserKey(orgName);
    if (local) {
        m_trace.writeLine(QStringLiteral("Unbinding organization '%1' in local repository...").arg(orgName));
        if (m_git.isInsideRepository()) {
            config.unset(GitConfigurationLevel::Local, key);
        } else {
            m_trace.writeLine(QStringLiteral("Cannot set local configuration binding - not inside a repository!"));
        }
    } else {
        m_trace.writeLine(QStringLiteral("Unbinding organization '%1' in global configuration...").arg(orgName));
        config.unset(GitConfigurationLevel::Global, key);
    }
}

QList<AzureReposBinding> AzureReposBindingManager::getBindings(const OptString &orgName)
{
    StringDictionary globalUsers(Qt::CaseInsensitive);
    StringDictionary localUsers(Qt::CaseInsensitive);
    IGitConfiguration &config = m_git.getConfiguration();

    auto extractUserBinding = [&](StringDictionary &dict) {
        return [&](const GitConfigurationEntry &entry) {
            if (const OptString entryOrgName = orgOfScope(entry.key)) {
                if (isNullOrWhiteSpace(orgName) || entryOrgName->compare(*orgName, Qt::CaseInsensitive) == 0) {
                    dict.set(*entryOrgName, entry.value);
                }
            }
            return true;
        };
    };

    // Only enumerate local configuration if we are inside a repository
    if (m_git.isInsideRepository()) {
        config.enumerate(GitConfigurationLevel::Local, CredCfg::SectionName, CredCfg::UserName, extractUserBinding(localUsers));
    }
    config.enumerate(GitConfigurationLevel::Global, CredCfg::SectionName, CredCfg::UserName, extractUserBinding(globalUsers));

    // globalUsers.Keys.Union(localUsers.Keys)
    QStringList orgs;
    for (const auto &[org, user] : globalUsers) {
        orgs.append(org);
    }
    for (const auto &[org, user] : localUsers) {
        if (!orgs.contains(org, Qt::CaseInsensitive)) {
            orgs.append(org);
        }
    }
    QList<AzureReposBinding> bindings;
    for (const QString &org : std::as_const(orgs)) {
        bindings.append(AzureReposBinding{org, globalUsers.tryGet(org), localUsers.tryGet(org)});
    }
    return bindings;
}

// =============================================================================================
// AzureReposUserManagerExtensions
// =============================================================================================

OptString IAzureReposBindingManager::getUser(const QString &orgName)
{
    const std::optional<AzureReposBinding> binding = getBinding(orgName);
    if (!binding || binding->localUserName == OptString(AzureReposBinding::NoInherit)) {
        return std::nullopt;
    }
    return binding->localUserName ? binding->localUserName : binding->globalUserName;
}

void IAzureReposBindingManager::signIn(const QString &orgName, const QString &userName)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");
    EnsureArgument::notNullOrWhiteSpace(userName, "userName");

    // Try to bind the user to the organization.
    //
    //   A = User to sign-in
    //   B = Another user
    //   - = No user
    //
    //  Global | Local | -> | Global | Local
    // --------|-------|----|--------|-------
    //    -    |   -   | -> |   A    |   -
    //    -    |   A   | -> |   A    |   -
    //    -    |   B   | -> |   A    |   -
    //    A    |   -   | -> |   A    |   -
    //    A    |   A   | -> |   A    |   -
    //    A    |   B   | -> |   A    |   -
    //    B    |   -   | -> |   B    |   A
    //    B    |   A   | -> |   B    |   A
    //    B    |   B   | -> |   B    |   A
    //
    const std::optional<AzureReposBinding> existing = getBinding(orgName);
    const OptString global = existing ? existing->globalUserName : std::nullopt;
    const OptString local = existing ? existing->localUserName : std::nullopt;
    if (global && global->compare(userName, Qt::CaseInsensitive) != 0) {
        // Global is bound to a different user (B); bind this user locally (-> B | A). Skip the write if local is
        // already correct.
        if (!local || local->compare(userName, Qt::CaseInsensitive) != 0) {
            bind(orgName, userName, true);
        }
    } else {
        // Global is absent or already matches; ensure global is set and local is clear.
        if (!global) {
            bind(orgName, userName, false);
        }
        if (local) {
            unbind(orgName, true);
        }
    }
}

void IAzureReposBindingManager::signOut(const QString &orgName)
{
    EnsureArgument::notNullOrWhiteSpace(orgName, "orgName");

    // Unbind the organization so we prompt the user to select a user on the next attempt.
    //
    //   U = User
    //   X = Do not inherit (valid in local only)
    //   - = No user
    //
    //  Global | Local | -> | Global | Local
    // --------|-------|----|--------|-------
    //    -    |   -   | -> |   -    |   -
    //    -    |   U   | -> |   -    |   -
    //    -    |   X   | -> |   -    |   -
    //    U    |   -   | -> |   U    |   X
    //    U    |   X   | -> |   U    |   X
    //    U    |   U   | -> |   U    |   X
    //
    const std::optional<AzureReposBinding> existing = getBinding(orgName);
    if (!existing) {
        // Nothing to do!
    } else if (!existing->globalUserName) {
        unbind(orgName, true);
    } else {
        bind(orgName, AzureReposBinding::NoInherit, true);
    }
}

} // namespace gcm::azrepos
