// Ports AzureReposHostProvider.cs (with its `azure-repos` commands) and Diagnostics/EntraAuthenticationDiagnostic.cs.
#include "azrepos.h"

#include "core/cmdline.h"
#include "core/commandcontext.h"
#include "core/commands.h"
#include "core/strings.h"

#include <QRegularExpression>

namespace gcm::azrepos {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;
namespace AzCfg = AzureDevOpsConstants::GitConfiguration::Credential;
namespace AzEnv = AzureDevOpsConstants::EnvironmentVariables;

QString useHttpPathKey() { return QStringLiteral("credential.https://dev.azure.com.useHttpPath"); }

} // namespace

AzureReposHostProvider::AzureReposHostProvider(ICommandContext &context)
    : AzureReposHostProvider(
          context, std::make_shared<AzureDevOpsRestApi>(context),
          EntraAuthFactory([&context](const entra::PublicClientConfig &config) { return std::make_shared<entra::EntraAuthentication>(context, config); }),
          std::make_shared<AzureDevOpsAuthorityCache>(context), std::make_shared<AzureReposBindingManager>(context))
{
}

AzureReposHostProvider::AzureReposHostProvider(ICommandContext &context, std::shared_ptr<IAzureDevOpsRestApi> azDevOps,
                                               std::shared_ptr<entra::IEntraAuthentication> entraAuth,
                                               std::shared_ptr<IAzureDevOpsAuthorityCache> authorityCache,
                                               std::shared_ptr<IAzureReposBindingManager> bindingManager)
    : AzureReposHostProvider(context, std::move(azDevOps), EntraAuthFactory([entraAuth](const entra::PublicClientConfig &) { return entraAuth; }),
                             std::move(authorityCache), std::move(bindingManager))
{
    EnsureArgument::notNull(entraAuth, "entraAuth");
}

AzureReposHostProvider::AzureReposHostProvider(ICommandContext &context, std::shared_ptr<IAzureDevOpsRestApi> azDevOps, EntraAuthFactory entraAuthFactory,
                                               std::shared_ptr<IAzureDevOpsAuthorityCache> authorityCache,
                                               std::shared_ptr<IAzureReposBindingManager> bindingManager)
    : m_context(context), m_azDevOps(std::move(azDevOps)), m_authorityCache(std::move(authorityCache)), m_bindingManager(std::move(bindingManager)),
      m_entraAuthFactory(std::move(entraAuthFactory))
{
    EnsureArgument::notNull(m_azDevOps, "azDevOps");
    EnsureArgument::notNull(m_authorityCache, "authorityCache");
    EnsureArgument::notNull(m_bindingManager, "bindingManager");
    if (!m_entraAuthFactory) {
        throw ArgumentNullException(QStringLiteral("Value cannot be null. (Parameter 'entraAuthFactory')"));
    }
}

entra::IEntraAuthentication &AzureReposHostProvider::entraAuth()
{
    // Lazy<IEntraAuthentication>: created with the public client configuration on first use
    if (!m_entraAuth) {
        m_entraAuth = m_entraAuthFactory(getEntraConfig());
    }
    return *m_entraAuth;
}

// =============================================================================================
// IHostProvider
// =============================================================================================

bool AzureReposHostProvider::isSupported(const GitRequest &request)
{
    // We do not recommend unencrypted HTTP communications to Azure Repos, but we report `true` here for HTTP so that
    // we can show a helpful error message for the user in `getCredential`.
    QString hostName;
    std::optional<int> port;
    const QString protocol = valueOr(request.protocol());
    return request.tryGetHostAndPort(hostName, port) &&
           (protocol.compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 || protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) &&
           UriHelpers::isAzureDevOpsHost(hostName);
}

GitResponse AzureReposHostProvider::getCredential(const GitRequest &request)
{
    std::optional<entra::ManagedIdentity> mid;
    if (useManagedIdentity(mid)) {
        m_context.trace().writeLine(QStringLiteral("Getting Entra access token for managed identity ManagedIdentity { Id = %1 }...").arg(mid->id()));
        const entra::EntraAuthResult result = entraAuth().getTokenForManagedIdentity(AzureDevOpsConstants::AzureDevOpsResourceId, *mid);
        return GitResponse(Credential{mid->id(), result.accessToken});
    }

    entra::WorkloadFederationOptions fedOpts;
    if (useWorkloadFederation(fedOpts)) {
        m_context.trace().writeLine(QStringLiteral("Getting Entra access token using WIF (scenario: %1)...").arg(entra::scenarioName(fedOpts.scenario)));
        const entra::EntraAuthResult result = entraAuth().getTokenUsingWorkloadFederation(AzureDevOpsConstants::AzureDevOpsDefaultScopes, fedOpts);
        return GitResponse(Credential{fedOpts.clientId, result.accessToken});
    }

    entra::ServicePrincipalIdentity sp;
    if (useServicePrincipal(sp)) {
        m_context.trace().writeLine(QStringLiteral("Getting Entra access token for service principal %1/%2...").arg(sp.tenantId, sp.id));
        const entra::EntraAuthResult result = entraAuth().getTokenForServicePrincipal(AzureDevOpsConstants::AzureDevOpsDefaultScopes, sp);
        return GitResponse(Credential{sp.id, result.accessToken});
    }

    try {
        return getUserCredential(request);
    } catch (const entra::EntraException &ex) {
        if (isClientConfigException(ex) && entraAuth().publicClientConfig() &&
            entraAuth().publicClientConfig()->clientId == AzureDevOpsConstants::ClientId) {
            const QString legacyClientSetting = QStringLiteral("%1.%2").arg(CredCfg::SectionName, AzCfg::UseLegacyClientId);
            m_context.console().writeWarning(QStringLiteral("Authentication using the new GCM Entra application failed. To retry this command with the legacy Entra application, set '%1' to 'true' in Git configuration.").arg(legacyClientSetting));
            m_context.console().writeInfo(QStringLiteral("If that succeeds, please report the original failure at %1").arg(Constants::HelpUrls::GcmNewIssue));
        }
        throw;
    }
}

bool AzureReposHostProvider::isClientConfigException(const std::exception &ex)
{
    // Not a client configuration error!
    if (dynamic_cast<const entra::EntraUiRequiredException *>(&ex) || dynamic_cast<const entra::EntraThrottledServiceException *>(&ex) ||
        dynamic_cast<const entra::IntuneAppProtectionPolicyRequiredException *>(&ex)) {
        return false;
    }
    if (const auto *svcEx = dynamic_cast<const entra::EntraServiceException *>(&ex)) {
        // Not retryable and no additionally required claims means this is likely a client configuration issue
        // (e.g. invalid client ID, missing or invalid redirect URI, etc.)
        return (svcEx->errorCode() == entra::EntraError::InvalidClient || svcEx->errorCode() == entra::EntraError::UnauthorizedClient) &&
               !svcEx->isRetryable() && isNullOrWhiteSpace(svcEx->claims());
    }
    return false;
}

GitResponse AzureReposHostProvider::getUserCredential(const GitRequest &request)
{
    if (usePersonalAccessTokens()) {
        const QUrl remoteWithUserUri = request.getRemoteUri(true).value_or(QUrl());
        const QString service = getServiceName(remoteWithUserUri);
        const OptString account = getAccountNameForCredentialQuery(request);

        m_context.trace().writeLine(QStringLiteral("Looking for existing credential in store with service=%1 account=%2...").arg(service, valueOr(account)));

        std::optional<Credential> credential = m_context.credentialStore().get(service, account);
        if (!credential) {
            m_context.trace().writeLine(QStringLiteral("No existing credentials found."));
            // No existing credential was found, create a new one
            m_context.trace().writeLine(QStringLiteral("Creating new credential..."));
            credential = generatePersonalAccessToken(request);
            m_context.trace().writeLine(QStringLiteral("Credential created."));
        } else {
            m_context.trace().writeLine(QStringLiteral("Existing credential found."));
        }
        return GitResponse(*credential);
    }

    // Include the username request here so that we may use it as an override for user account lookups when
    // getting Entra access tokens.
    const entra::EntraAuthResult result = getEntraAccessToken(request);
    return GitResponse(Credential{result.account ? valueOr(result.account->userName) : QString(), result.accessToken});
}

void AzureReposHostProvider::storeCredential(const GitRequest &request)
{
    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());
    std::optional<entra::ManagedIdentity> mid;
    entra::WorkloadFederationOptions fedOpts;
    entra::ServicePrincipalIdentity sp;
    if (useManagedIdentity(mid)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to store for managed identity authentication."));
    } else if (useWorkloadFederation(fedOpts)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to store for federated identity authentication."));
    } else if (useServicePrincipal(sp)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to store for service principal authentication."));
    } else if (usePersonalAccessTokens()) {
        const QString service = getServiceName(remoteUri);
        // We always store credentials against the given username argument for both vs.com and dev.azure.com-style URLs.
        const QString account = valueOr(request.userName());
        // Add or update the credential in the store.
        m_context.trace().writeLine(QStringLiteral("Storing credential with service=%1 account=%2...").arg(service, account));
        m_context.credentialStore().addOrUpdate(service, account, valueOr(request.password()));
        m_context.trace().writeLine(QStringLiteral("Credential was successfully stored."));
    } else {
        const QString orgName = UriHelpers::getOrganizationName(remoteUri);
        m_context.trace().writeLine(QStringLiteral("Signing user %1 in to organization '%2'...").arg(valueOr(request.userName()), orgName));
        m_bindingManager->signIn(orgName, valueOr(request.userName()));
    }
}

void AzureReposHostProvider::eraseCredential(const GitRequest &request)
{
    const QUrl remoteUri = request.getRemoteUri().value_or(QUrl());
    std::optional<entra::ManagedIdentity> mid;
    entra::WorkloadFederationOptions fedOpts;
    entra::ServicePrincipalIdentity sp;
    if (useManagedIdentity(mid)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to erase for managed identity authentication."));
    } else if (useWorkloadFederation(fedOpts)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to erase for federated identity authentication."));
    } else if (useServicePrincipal(sp)) {
        m_context.trace().writeLine(QStringLiteral("Nothing to erase for service principal authentication."));
    } else if (usePersonalAccessTokens()) {
        const QString service = getServiceName(remoteUri);
        const OptString account = getAccountNameForCredentialQuery(request);
        // Try to locate an existing credential
        m_context.trace().writeLine(QStringLiteral("Erasing stored credential in store with service=%1 account=%2...").arg(service, valueOr(account)));
        if (m_context.credentialStore().remove(service, account)) {
            m_context.trace().writeLine(QStringLiteral("Credential was successfully erased."));
        } else {
            m_context.trace().writeLine(QStringLiteral("No credential was erased."));
        }
    } else {
        const QString orgName = UriHelpers::getOrganizationName(remoteUri);
        m_context.trace().writeLine(QStringLiteral("Signing out of organization '%1'...").arg(orgName));
        m_bindingManager->signOut(orgName);
        // Clear the authority cache in case this was the reason for failure
        m_authorityCache->eraseAuthority(orgName);
    }
}

void AzureReposHostProvider::throwIfUnsafeRemote(const GitRequest &request)
{
    if (!m_context.settings().allowUnsafeRemotes() && valueOr(request.protocol()).compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) {
        throw Exception(QStringLiteral("Unencrypted HTTP is not recommended for Azure Repos. Ensure the repository remote URL is using HTTPS or see %1 about how to allow unsafe remotes.").arg(Constants::HelpUrls::GcmUnsafeRemotes));
    }
}

Credential AzureReposHostProvider::generatePersonalAccessToken(const GitRequest &request)
{
    throwIfUnsafeRemote(request);
    ITrace &trace = m_context.trace();

    const QUrl remoteUserUri = request.getRemoteUri(true).value_or(QUrl());
    QString orgName;
    const QUrl orgUri = UriHelpers::createOrganizationUri(remoteUserUri, orgName);

    // Determine the Entra authentication authority for this organization
    trace.writeLine(QStringLiteral("Determining Entra authentication authority..."));
    const QString authAuthority = m_azDevOps->getAuthority(orgUri);
    trace.writeLine(QStringLiteral("Authority is '%1'.").arg(authAuthority));

    // Get an Entra access token for the Azure DevOps SPS
    trace.writeLine(QStringLiteral("Getting Entra access token..."));
    const entra::EntraAuthResult result = entraAuth().getTokenForUser(AzureDevOpsConstants::AzureDevOpsDefaultScopes, authAuthority);
    const QString userName = result.account ? valueOr(result.account->userName) : QString();
    trace.writeLineSecrets(QStringLiteral("Acquired Entra access token. Account='%1' Token='{0}'").arg(userName), {result.accessToken});

    // Ask the Azure DevOps instance to create a new PAT
    const QStringList patScopes = {AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite, AzureDevOpsConstants::PersonalAccessTokenScopes::ArtifactsRead};
    trace.writeLine(QStringLiteral("Creating Azure DevOps PAT with scopes '%1'...").arg(patScopes.join(QStringLiteral(", "))));
    const QString pat = m_azDevOps->createPersonalAccessToken(orgUri, result.accessToken, patScopes);
    trace.writeLineSecrets(QStringLiteral("PAT created. PAT='{0}'"), {pat});
    return Credential{userName, pat};
}

entra::EntraAuthResult AzureReposHostProvider::getEntraAccessToken(const GitRequest &request)
{
    throwIfUnsafeRemote(request);
    ITrace &trace = m_context.trace();

    const QUrl remoteWithUserUri = request.getRemoteUri(true).value_or(QUrl());
    OptString userName = request.userName();
    QString orgName;
    const QUrl orgUri = UriHelpers::createOrganizationUri(remoteWithUserUri, orgName);

    trace.writeLine(QStringLiteral("Determining Entra authority for Azure DevOps organization '%1'...").arg(orgName));
    QString authAuthority;
    if (tryGetAuthorityFromHeaders(request.wwwAuth(), authAuthority)) {
        trace.writeLine(QStringLiteral("Authority was found in WWW-Authenticate headers from Git request."));
    } else {
        // Try to get the authority from the cache
        const OptString cached = m_authorityCache->getAuthority(orgName);
        if (!cached) {
            // If there is no cached value we must query for it and cache it for future use
            trace.writeLine(QStringLiteral("No cached authority value - querying %1 for authority...").arg(absoluteUri(orgUri)));
            authAuthority = m_azDevOps->getAuthority(orgUri);
            m_authorityCache->updateAuthority(orgName, authAuthority);
        } else {
            trace.writeLine(QStringLiteral("Authority was found in cache."));
            authAuthority = *cached;
        }
    }
    trace.writeLine(QStringLiteral("Authority is '%1'.").arg(authAuthority));

    // If the remote URI is a classic "*.visualstudio.com" host name and we have a user specified from the remote then
    // take that as the current Entra/MSA user in the first instance.
    //
    // For "dev.azure.com" host names we only use the user info part of the remote when this doesn't match the Azure
    // DevOps organization name. Azure DevOps "borrows" the username part of the remote URL to include the organization
    // name (not an actual username).
    //
    // If we have no specified user from the remote (or this is org@dev.azure.com/org/..) then query the user manager
    // for a bound user for this organization, if one exists...
    const QString host = remoteWithUserUri.host();
    if (!isNullOrWhiteSpace(userName) &&
        (UriHelpers::isVisualStudioComHost(host) || (UriHelpers::isAzureDevOpsHost(host) && orgName.compare(*userName, Qt::CaseInsensitive) != 0))) {
        trace.writeLine(QStringLiteral("Using username as specified in remote."));
    } else {
        trace.writeLine(QStringLiteral("Looking up user for organization '%1'...").arg(orgName));
        userName = m_bindingManager->getUser(orgName);
    }

    std::optional<entra::EntraAccount> account;
    if (isNullOrWhiteSpace(userName)) {
        trace.writeLine(QStringLiteral("No user found."));
    } else {
        trace.writeLine(QStringLiteral("Looking for cached Entra account matching username '%1'...").arg(*userName));
        const QList<entra::EntraAccount> cachedAccounts = entraAuth().getUserAccounts();
        for (const entra::EntraAccount &a : cachedAccounts) {
            if (a.userName && a.userName->compare(*userName, Qt::CaseInsensitive) == 0) {
                account = a;
                break;
            }
        }
        trace.writeLine(account ? QStringLiteral("Found cached account '%1'").arg(valueOr(account->homeAccountId)) : QStringLiteral("No cached account found."));
    }

    // Get an Entra access token for the Azure DevOps SPS
    trace.writeLine(QStringLiteral("Getting Entra access token..."));
    const entra::EntraAuthResult result = entraAuth().getTokenForUser(AzureDevOpsConstants::AzureDevOpsDefaultScopes, authAuthority, account);
    trace.writeLineSecrets(QStringLiteral("Acquired Entra access token. Account='%1' Token='{0}'").arg(result.account ? valueOr(result.account->userName) : QString()), {result.accessToken});
    return result;
}

bool AzureReposHostProvider::tryGetAuthorityFromHeaders(const QStringList &headers, QString &authority)
{
    authority.clear();
    static const QRegularExpression regex(QStringLiteral("authorization_uri=\"?(?<authority>.+)\"?"), QRegularExpression::CaseInsensitiveOption);
    for (const QString &header : headers) {
        const QRegularExpressionMatch match = regex.match(header);
        if (match.hasMatch()) {
            QString value = match.captured(QStringLiteral("authority"));
            // Trim(new[] { '"', '\'' })
            while (!value.isEmpty() && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\''))) {
                value.remove(0, 1);
            }
            while (!value.isEmpty() && (value.back() == QLatin1Char('"') || value.back() == QLatin1Char('\''))) {
                value.chop(1);
            }
            authority = value;
            return true;
        }
    }
    return false;
}

// =============================================================================================
// Settings
// =============================================================================================

entra::PublicClientConfig AzureReposHostProvider::getEntraConfig()
{
    const auto [clientId, isLegacy] = getClientAppInfo();
    m_context.trace().writeLine(isLegacy ? QStringLiteral("Using legacy Entra client ID '%1'").arg(clientId) : QStringLiteral("Using new Entra client ID '%1'").arg(clientId));
    entra::PublicClientConfig config;
    config.clientId = clientId;
    config.isMsaPassthroughEnabled = true;
    config.useSharedCache = getUseSharedCache();
    config.supportsWindowsBroker = true;
    // Only the new client ID supports broker on Mac and Linux
    config.supportsMacBroker = !isLegacy;
    config.supportsLinuxBroker = !isLegacy;
    return config;
}

bool AzureReposHostProvider::getUseSharedCache()
{
    const bool defaultValue = true; // prefer using the shared Microsoft dev cache
    QString str;
    if (m_context.settings().tryGetSetting(AzEnv::UseSharedCache, CredCfg::SectionName, AzCfg::UseSharedCache, str)) {
        return toBooleanyOrDefault(str, defaultValue);
    }
    return defaultValue;
}

std::pair<QString, bool> AzureReposHostProvider::getClientAppInfo()
{
    // Check for override to use the legacy client ID
    QString str;
    if (m_context.settings().tryGetSetting(AzEnv::UseLegacyClientId, CredCfg::SectionName, AzCfg::UseLegacyClientId, str) && isTruthy(str)) {
        return {AzureDevOpsConstants::LegacyClientId, true};
    }
    return {AzureDevOpsConstants::ClientId, false};
}

QString AzureReposHostProvider::getServiceName(const QUrl &remoteUri)
{
    auto trim = [](QString s) {
        while (s.endsWith(QLatin1Char('/'))) {
            s.chop(1);
        }
        return s;
    };
    // dev.azure.com: never stored against the full path, because useHttpPath is forced to true to get the
    // organization name from Git
    if (UriHelpers::isDevAzureComHost(remoteUri.host())) {
        QString orgName;
        return trim(absoluteUri(UriHelpers::createOrganizationUri(remoteUri, orgName)));
    }
    // *.visualstudio.com: if we're given the full path for an older URL then we should respect that in the service name
    if (UriHelpers::isVisualStudioComHost(remoteUri.host())) {
        return trim(absoluteUri(withoutUserInfo(remoteUri)));
    }
    throw InvalidOperationException(QStringLiteral("Host is not Azure DevOps."));
}

OptString AzureReposHostProvider::getAccountNameForCredentialQuery(const GitRequest &request)
{
    QString hostName;
    std::optional<int> port;
    if (!request.tryGetHostAndPort(hostName, port)) {
        throw InvalidOperationException(QStringLiteral("Failed to parse host name and/or port"));
    }
    // dev.azure.com: the given username is ignored because Azure DevOps recommends adding the organization name as the
    // user in the remote URL; null matches all possible credentials (regardless of the account).
    if (UriHelpers::isDevAzureComHost(OptString(hostName))) {
        return std::nullopt;
    }
    // *.visualstudio.com: respect any specified username in the remote URL/request arguments.
    if (UriHelpers::isVisualStudioComHost(OptString(hostName))) {
        return request.userName();
    }
    throw InvalidOperationException(QStringLiteral("Host is not Azure DevOps."));
}

bool AzureReposHostProvider::usePersonalAccessTokens()
{
    // Default to using OAuth tokens
    const bool defaultValue = false;
    QString valueStr;
    if (m_context.settings().tryGetSetting(AzEnv::CredentialType, CredCfg::SectionName, AzCfg::CredentialType, valueStr)) {
        m_context.trace().writeLine(QStringLiteral("Azure Repos credential type override set to '%1'").arg(valueStr));
        const QString v = valueStr.toLower();
        if (v == AzureDevOpsConstants::PatCredentialType) {
            return true;
        }
        if (v == AzureDevOpsConstants::OAuthCredentialType) {
            return false;
        }
        m_context.console().writeWarning(QStringLiteral("unknown Azure Repos credential type '%1' - using OAuth").arg(valueStr));
        return defaultValue;
    }
    return defaultValue;
}

bool AzureReposHostProvider::useServicePrincipal(entra::ServicePrincipalIdentity &sp)
{
    ISettings &settings = m_context.settings();
    QString spStr;
    if (!settings.tryGetSetting(AzEnv::ServicePrincipalId, CredCfg::SectionName, AzCfg::ServicePrincipal, spStr) || isNullOrWhiteSpace(spStr)) {
        return false;
    }

    // "<tenant>/<client>"
    const qsizetype slash = spStr.indexOf(QLatin1Char('/'));
    const QString tenantId = slash < 0 ? spStr : spStr.left(slash);
    const OptString clientId = slash < 0 ? OptString() : OptString(spStr.mid(slash + 1));
    if (isNullOrWhiteSpace(tenantId)) {
        m_context.console().writeError(QStringLiteral("unable to use configured service principal - missing tenant ID in configuration"));
        return false;
    }
    if (isNullOrWhiteSpace(clientId)) {
        m_context.console().writeError(QStringLiteral("unable to use configured service principal - missing client ID in configuration"));
        return false;
    }

    sp = entra::ServicePrincipalIdentity();
    sp.id = *clientId;
    sp.tenantId = tenantId;

    QString clientSecret, certThumbprint;
    const bool hasClientSecret = settings.tryGetSetting(AzEnv::ServicePrincipalSecret, CredCfg::SectionName, AzCfg::ServicePrincipalSecret, clientSecret);
    const bool hasCertThumbprint = settings.tryGetSetting(AzEnv::ServicePrincipalCertificateThumbprint, CredCfg::SectionName, AzCfg::ServicePrincipalCertificateThumbprint, certThumbprint);

    if (hasCertThumbprint && hasClientSecret) {
        m_context.console().writeWarning(QStringLiteral("both service principal client secret and certificate thumbprint are configured - using certificate"));
    }

    if (hasCertThumbprint) {
        QString sendX5C;
        sp.sendX5C = settings.tryGetSetting(AzEnv::ServicePrincipalCertificateSendX5C, CredCfg::SectionName, AzCfg::ServicePrincipalCertificateSendX5C, sendX5C) &&
                     toBooleanyOrDefault(sendX5C, false);
        sp.certificate = entra::getCertificateByThumbprint(m_context.fileSystem().userHomePath(), certThumbprint);
        if (!sp.certificate) {
            m_context.console().writeError(QStringLiteral("unable to find certificate with thumbprint '%1' for service principal").arg(certThumbprint));
            return false;
        }
    } else if (hasClientSecret) {
        sp.clientSecret = clientSecret;
    }
    return true;
}

bool AzureReposHostProvider::useManagedIdentity(std::optional<entra::ManagedIdentity> &mid)
{
    QString miStr;
    if (!m_context.settings().tryGetSetting(AzEnv::ManagedIdentity, CredCfg::SectionName, AzCfg::ManagedIdentity, miStr) || isNullOrWhiteSpace(miStr)) {
        mid.reset();
        return false;
    }
    mid = entra::ManagedIdentity::create(miStr);
    return true;
}

bool AzureReposHostProvider::useWorkloadFederation(entra::WorkloadFederationOptions &fedOpts)
{
    ISettings &settings = m_context.settings();
    QString wifStr;
    if (!settings.tryGetSetting(AzEnv::WorkloadFederation, CredCfg::SectionName, AzCfg::WorkloadFederation, wifStr)) {
        return false;
    }

    entra::WorkloadFederationScenario scenario;
    const QString w = wifStr.toLower();
    if (w == QLatin1String("generic")) {
        scenario = entra::WorkloadFederationScenario::Generic;
    } else if (w == QLatin1String("mi") || w == QLatin1String("managedidentity")) {
        scenario = entra::WorkloadFederationScenario::ManagedIdentity;
    } else if (w == QLatin1String("github") || w == QLatin1String("githubactions")) {
        scenario = entra::WorkloadFederationScenario::GitHubActions;
    } else {
        // Unknown scenario value
        return false;
    }

    QString clientId, tenantId;
    const bool hasClientId = settings.tryGetSetting(AzEnv::WorkloadFederationClientId, CredCfg::SectionName, AzCfg::WorkloadFederationClientId, clientId);
    const bool hasTenantId = settings.tryGetSetting(AzEnv::WorkloadFederationTenantId, CredCfg::SectionName, AzCfg::WorkloadFederationTenantId, tenantId);
    if (!hasClientId || !hasTenantId) {
        m_context.console().writeError(QStringLiteral("both client ID and tenant ID are required for workload federation"));
        return false;
    }

    // Audience is optional - the default is "api://AzureADTokenExchange"
    QString audience;
    if (!settings.tryGetSetting(AzEnv::WorkloadFederationAudience, CredCfg::SectionName, AzCfg::WorkloadFederationAudience, audience) || isNullOrWhiteSpace(audience)) {
        audience = entra::WorkloadFederationOptions::DefaultAudience;
    }

    entra::WorkloadFederationOptions opts;
    opts.scenario = scenario;
    opts.clientId = clientId;
    opts.tenantId = tenantId;
    opts.setAudience(audience);

    switch (scenario) {
    case entra::WorkloadFederationScenario::Generic: {
        QString assertion;
        if (!settings.tryGetSetting(AzEnv::WorkloadFederationAssertion, CredCfg::SectionName, AzCfg::WorkloadFederationAssertion, assertion) || isNullOrWhiteSpace(assertion)) {
            m_context.console().writeError(QStringLiteral("assertion is required for the generic workload federation scenario"));
            return false;
        }
        // Check if this value points to a file containing the actual assertion (file://<path>)
        if (const std::optional<QUrl> assertionUri = tryCreateUri(assertion); assertionUri && assertionUri->scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0) {
            const QString filePath = assertionUri->toLocalFile();
            if (!m_context.fileSystem().fileExists(filePath)) {
                m_context.console().writeError(QStringLiteral("assertion file not found: %1").arg(filePath));
                return false;
            }
            m_context.trace().writeLine(QStringLiteral("Reading workload federation assertion from file '%1'...").arg(filePath));
            assertion = m_context.fileSystem().readAllText(filePath).trimmed();
            if (isNullOrWhiteSpace(assertion)) {
                m_context.console().writeError(QStringLiteral("assertion file is empty: %1").arg(filePath));
                return false;
            }
        }
        opts.genericClientAssertion = assertion;
        break;
    }
    case entra::WorkloadFederationScenario::ManagedIdentity: {
        QString managedIdentity;
        if (!settings.tryGetSetting(AzEnv::WorkloadFederationManagedIdentity, CredCfg::SectionName, AzCfg::WorkloadFederationManagedIdentity, managedIdentity) || isNullOrWhiteSpace(managedIdentity)) {
            m_context.console().writeError(QStringLiteral("managed identity is required for the managed identity workload federation scenario"));
            return false;
        }
        opts.managedIdentityId = managedIdentity;
        break;
    }
    case entra::WorkloadFederationScenario::GitHubActions: {
        const StringDictionary &env = m_context.environment().variables();
        const std::optional<QUrl> tokenRequestUri = tryCreateUri(env.tryGet(Constants::EnvironmentVariables::GitHubActionsTokenRequestUrl));
        if (!tokenRequestUri) {
            m_context.console().writeError(QStringLiteral("unable to get valid token request URL from environment variable for the GitHub Actions workload federation scenario"));
            return false;
        }
        const OptString tokenRequestToken = env.tryGet(Constants::EnvironmentVariables::GitHubActionsTokenRequestToken);
        if (isNullOrWhiteSpace(tokenRequestToken)) {
            m_context.console().writeError(QStringLiteral("unable to get valid token request token from environment variable for the GitHub Actions workload federation scenario"));
            return false;
        }
        opts.gitHubTokenRequestUrl = *tokenRequestUri;
        opts.gitHubTokenRequestToken = *tokenRequestToken;
        break;
    }
    }
    fedOpts = opts;
    return true;
}

// =============================================================================================
// IConfigurableComponent
// =============================================================================================

void AzureReposHostProvider::configure(ConfigurationTarget target)
{
    const GitConfigurationLevel level = target == ConfigurationTarget::System ? GitConfigurationLevel::System : GitConfigurationLevel::Global;
    IGitConfiguration &targetConfig = m_context.git().getConfiguration();
    QString currentValue;
    if (targetConfig.tryGet(useHttpPathKey(), false, currentValue) && isTruthy(currentValue)) {
        m_context.trace().writeLine(QStringLiteral("Git configuration 'credential.useHttpPath' is already set to 'true' for https://dev.azure.com."));
    } else {
        m_context.trace().writeLine(QStringLiteral("Setting Git configuration 'credential.useHttpPath' to 'true' for https://dev.azure.com..."));
        targetConfig.set(level, useHttpPathKey(), QStringLiteral("true"));
    }
}

void AzureReposHostProvider::unconfigure(ConfigurationTarget target)
{
    m_context.trace().writeLine(QStringLiteral("Clearing Git configuration 'credential.useHttpPath' for https://dev.azure.com..."));
    const GitConfigurationLevel level = target == ConfigurationTarget::System ? GitConfigurationLevel::System : GitConfigurationLevel::Global;
    // (The Git for Windows "manager" helper check of upstream does not apply on Linux.)
    m_context.git().getConfiguration().unset(level, useHttpPathKey());
}

// =============================================================================================
// ICommandProvider
// =============================================================================================

std::shared_ptr<Command> AzureReposHostProvider::createCommand()
{
    // clear-cache
    auto clearCacheCmd = std::make_shared<Command>(QStringLiteral("clear-cache"), QStringLiteral("Clear the Azure authority cache"));
    clearCacheCmd->setHandler([this](const ParseResult &) {
        m_authorityCache->clear();
        m_context.console().writeLine(QStringLiteral("Authority cache cleared"));
        return 0;
    });

    // list <organization> [--show-remotes] [--verbose]
    auto listCmd = std::make_shared<Command>(QStringLiteral("list"), QStringLiteral("List all user account bindings"));
    listCmd->addArgument(Argument{QStringLiteral("organization"), QStringLiteral("(optional) Filter results by Azure DevOps organization name"), false});
    listCmd->addOption(Option::flag(QStringLiteral("--show-remotes"), QStringLiteral("Also show Azure DevOps remote user bindings for the current repository")));
    listCmd->addOption(Option{{QStringLiteral("--verbose"), QStringLiteral("-v")}, QStringLiteral("Verbose output - show remote URLs")});
    listCmd->setHandler([this](const ParseResult &r) {
        return this->listCmd(r.argument(QStringLiteral("organization")), r.has(QStringLiteral("--show-remotes")), r.has(QStringLiteral("--verbose")));
    });

    // bind <organization> <username> [--local]
    // (upstream adds the same organization argument object to both bind and unbind)
    const Argument orgArg{QStringLiteral("organization"), QStringLiteral("Azure DevOps organization name"), true, true};
    const Option localOpt = Option::flag(QStringLiteral("--local"), QStringLiteral("Target the local repository Git configuration"));
    auto bindCmd = std::make_shared<Command>(QStringLiteral("bind"), QStringLiteral("Bind a user account to an Azure DevOps organization"));
    bindCmd->addArgument(orgArg);
    bindCmd->addArgument(Argument{QStringLiteral("username"), QStringLiteral("Username or email (e.g.: alice@example.com)"), true});
    bindCmd->addOption(localOpt);
    bindCmd->setHandler([this](const ParseResult &r) {
        return this->bindCmd(valueOr(r.argument(QStringLiteral("organization"))), valueOr(r.argument(QStringLiteral("username"))), r.has(QStringLiteral("--local")));
    });

    // unbind <organization> [--local]
    auto unbindCmd = std::make_shared<Command>(QStringLiteral("unbind"), QStringLiteral("Remove user account binding for an Azure DevOps organization"));
    unbindCmd->addArgument(orgArg);
    unbindCmd->addOption(localOpt);
    unbindCmd->setHandler([this](const ParseResult &r) { return this->unbindCmd(valueOr(r.argument(QStringLiteral("organization"))), r.has(QStringLiteral("--local"))); });

    auto rootCmd = std::make_shared<ProviderCommand>(*this);
    rootCmd->addCommand(listCmd);
    rootCmd->addCommand(bindCmd);
    rootCmd->addCommand(unbindCmd);
    rootCmd->addCommand(clearCacheCmd);
    return rootCmd;
}

int AzureReposHostProvider::listCmd(const OptString &organization, bool showRemotes, bool verbose)
{
    IConsoleService &console = m_context.console();

    // Get all organization bindings from the user manager
    const QList<AzureReposBinding> bindings = m_bindingManager->getBindings(organization);

    // If we are asked to also show remotes we build the remote binding map
    struct RemoteBinding
    {
        QString remote;
        bool isPush;
        QUrl uri;
    };
    QList<std::pair<QString, RemoteBinding>> orgRemotes;
    if (showRemotes) {
        if (!m_context.git().isInsideRepository()) {
            console.writeWarning(QStringLiteral("not inside a git repository (--show-remotes has no effect)"));
        }
        auto isAzureDevOpsHttpRemote = [](const OptString &url, QUrl &uri) {
            const std::optional<QUrl> u = tryCreateUri(url);
            if (!u) {
                return false;
            }
            uri = *u;
            return (uri.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 || uri.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) &&
                   UriHelpers::isAzureDevOpsHost(uri.host());
        };
        const QList<GitRemote> remotes = m_context.git().getRemotes();
        for (const GitRemote &remote : remotes) {
            QUrl fetchUri, pushUri;
            if (isAzureDevOpsHttpRemote(remote.fetchUrl, fetchUri)) {
                orgRemotes.append({UriHelpers::getOrganizationName(fetchUri), RemoteBinding{remote.name, false, fetchUri}});
            }
            if (isAzureDevOpsHttpRemote(remote.pushUrl, pushUri)) {
                orgRemotes.append({UriHelpers::getOrganizationName(pushUri), RemoteBinding{remote.name, true, pushUri}});
            }
        }
    }

    const bool isFiltered = !isNullOrWhiteSpace(organization);
    const QString indent = isFiltered ? QString() : QStringLiteral("  ");

    // The set of all organization names (organization names are not case sensitive)
    QStringList orgNames;
    auto addOrg = [&](const QString &org) {
        if (!orgNames.contains(org, Qt::CaseInsensitive)) {
            orgNames.append(org);
        }
    };
    for (const AzureReposBinding &b : bindings) {
        addOrg(b.organization);
    }
    for (const auto &[org, binding] : std::as_const(orgRemotes)) {
        addOrg(org);
    }

    for (const QString &orgName : std::as_const(orgNames)) {
        if (!isFiltered) {
            console.writeLine(QStringLiteral("%1:").arg(orgName));
        }

        // Print organization bindings
        for (const AzureReposBinding &binding : bindings) {
            // (the binding map is keyed by the exact organization name)
            if (binding.organization != orgName) {
                continue;
            }
            if (binding.globalUserName) {
                console.writeLine(QStringLiteral("%1(global) -> %2").arg(indent, *binding.globalUserName));
            }
            if (binding.localUserName) {
                const QString value = binding.localUserName->isEmpty() ? QStringLiteral("(no inherit)") : *binding.localUserName;
                console.writeLine(QStringLiteral("%1(local)  -> %2").arg(indent, value));
            }
        }

        // Print remote bindings, grouped by remote in order of appearance
        QStringList remoteNames;
        for (const auto &[org, binding] : std::as_const(orgRemotes)) {
            if (org == orgName && !remoteNames.contains(binding.remote)) {
                remoteNames.append(binding.remote);
            }
        }
        for (const QString &remoteName : std::as_const(remoteNames)) {
            console.writeLine(QStringLiteral("%1%2:").arg(indent, remoteName));
            for (const auto &[org, binding] : std::as_const(orgRemotes)) {
                if (org != orgName || binding.remote != remoteName) {
                    continue;
                }
                // User names in dev.azure.com URLs cannot always be used as actual user names, because this field is
                // used to send the Azure DevOps organization name. dev.azure.com URLs show as "inherit" if the user
                // name matches the organization name.
                OptString userName, password;
                if (!tryGetUserInfo(binding.uri, userName, password) || !userName ||
                    (UriHelpers::isDevAzureComHost(binding.uri.host()) && userName->compare(orgName, Qt::CaseInsensitive) == 0)) {
                    userName = QStringLiteral("(inherit)");
                }
                const QString url = verbose ? QUrl::fromPercentEncoding(absoluteUri(withoutUserInfo(binding.uri)).toUtf8()) + QLatin1Char(' ') : QString();
                console.writeLine(binding.isPush ? QStringLiteral("%1  %2(push)  -> %3").arg(indent, url, *userName) : QStringLiteral("%1  %2(fetch) -> %3").arg(indent, url, *userName));
            }
        }
    }
    return 0;
}

int AzureReposHostProvider::bindCmd(const QString &organization, const QString &userName, bool local)
{
    if (local && !m_context.git().isInsideRepository()) {
        m_context.console().writeError(QStringLiteral("not inside a git repository (cannot use --local)"));
        return -1;
    }
    m_bindingManager->bind(organization, userName, local);
    return 0;
}

int AzureReposHostProvider::unbindCmd(const QString &organization, bool local)
{
    if (local && !m_context.git().isInsideRepository()) {
        m_context.console().writeError(QStringLiteral("not inside a git repository (cannot use --local)"));
        return -1;
    }
    m_bindingManager->unbind(organization, local);
    return 0;
}

// =============================================================================================
// Diagnostics
// =============================================================================================

QList<std::shared_ptr<IDiagnostic>> AzureReposHostProvider::getDiagnostics()
{
    return {std::make_shared<EntraAuthenticationDiagnostic>(m_context)};
}

void EntraAuthenticationDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    const entra::TokenCacheStorageFactory factory = m_storageFactory ? m_storageFactory : entra::TokenCacheStorageFactory([this](entra::TokenCacheKind kind, bool fallback) {
        return entra::createDefaultTokenCacheStorage(m_context, kind, fallback);
    });
    for (const entra::TokenCacheKind kind : {entra::TokenCacheKind::User, entra::TokenCacheKind::App}) {
        reporter.reportProgress(kind == entra::TokenCacheKind::User ? QStringLiteral("Checking Git Credential Manager token cache")
                                                                    : QStringLiteral("Checking Git Credential Manager application token cache"));
        std::unique_ptr<entra::ITokenCacheStorage> storage = factory(kind, false);
        std::unique_ptr<entra::ITokenCacheStorage> fallback = factory(kind, true);
        reporter.reportInfo(QStringLiteral("Storage: %1").arg(storage->description()));
        reporter.reportInfo(QStringLiteral("Fallback storage: %1").arg(fallback->description()));
        reporter.reportProgress(QStringLiteral("Verifying token cache persistence"));
        auto message = [](const std::exception &ex) {
            const auto *gcmEx = dynamic_cast<const Exception *>(&ex);
            return gcmEx ? gcmEx->message() : QString::fromUtf8(ex.what());
        };
        try {
            storage->load();
            continue;
        } catch (const std::exception &ex) {
            // Authentication falls back to the file then (as upstream, whose diagnostic only checks the fallback file)
            reporter.reportInfo(QStringLiteral("Secure storage unavailable: %1").arg(message(ex)));
        }
        reporter.reportProgress(QStringLiteral("Verifying fallback token cache persistence"));
        try {
            fallback->load();
        } catch (const std::exception &ex) {
            reporter.reportWarning(QStringLiteral("Failed cache persistence test: %1").arg(message(ex)));
        }
    }
}

} // namespace gcm::azrepos
