// Ports BitbucketHelper.cs, Cloud/ and DataCenter/ BitbucketRestApi.cs and BitbucketOAuth2Client.cs, the base
// BitbucketOAuth2Client.cs, BitbucketTokenEndpointResponseJson.cs, BitbucketRestApiRegistry.cs and
// OAuth2ClientRegistry.cs.
#include "bitbucket.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/settings.h"
#include "core/strings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace gcm::bitbucket {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

const QList<EnumName>& modeNames()
{
    static const QList<EnumName> names = {
        { int(AuthenticationModes::None), "None" },
        { int(AuthenticationModes::Basic), "Basic" },
        { int(AuthenticationModes::OAuth), "OAuth" },
        { int(AuthenticationModes::All), "All" },
    };
    return names;
}

/// "HTTP: Response 200 [OK]"
QString responseLine(const HttpResponse &response)
{
    return QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), response.reasonPhrase);
}

void setAuthorization(HttpRequest &request, const OptString &userName, const QString &password, bool isBearerToken)
{
    if (isBearerToken) {
        request.setBearerAuthorization(password);
    } else {
        request.setBasicAuthorization(valueOr(userName), password);
    }
}

} // namespace

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
// BitbucketHelper
// =============================================================================================

QString BitbucketHelper::getBaseUri(const QUrl &remoteUri)
{
    // PathAndQuery split on '/': the first segment is the context path of the instance (e.g. "/bitbucket")
    QString pathAndQuery = remoteUri.path(QUrl::FullyEncoded);
    if (remoteUri.hasQuery()) {
        pathAndQuery += QLatin1Char('?') + remoteUri.query(QUrl::FullyEncoded);
    }
    if (pathAndQuery.isEmpty()) {
        pathAndQuery = QStringLiteral("/");
    }
    const QStringList pathParts = pathAndQuery.split(QLatin1Char('/'));
    const QString pathPart = pathAndQuery.startsWith(QLatin1Char('/')) ? pathParts.value(1) : pathParts.value(0);
    const QString path = !isNullOrWhiteSpace(pathPart) ? QLatin1Char('/') + pathPart : QString();
    const int defaultPort = remoteUri.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
    return QStringLiteral("%1://%2:%3%4").arg(remoteUri.scheme(), remoteUri.host(), QString::number(remoteUri.port(defaultPort)), path);
}

bool BitbucketHelper::isBitbucketOrg(const GitRequest &request)
{
    return isBitbucketOrg(request.getRemoteUri().value_or(QUrl()));
}

bool BitbucketHelper::isBitbucketOrg(const QUrl &targetUri)
{
    return isBitbucketOrg(OptString(targetUri.host()));
}

bool BitbucketHelper::isBitbucketOrg(const OptString &targetHost)
{
    return targetHost && targetHost->compare(cloud::CloudConstants::BitbucketBaseUrlHost, Qt::CaseInsensitive) == 0;
}

namespace {

/// `new Uri(baseUriString + suffix)`: the base carries an explicit port, which System.Uri drops when it is the default.
QUrl baseUriWith(const QUrl &remoteUri, const QString &suffix)
{
    const QUrl url(BitbucketHelper::getBaseUri(remoteUri) + suffix);
    return QUrl(absoluteUri(url));
}

} // namespace

// =============================================================================================
// Cloud REST API
// =============================================================================================

UserInfoResult cloud::BitbucketRestApi::getUserInformation(const OptString &userName, const QString &password, bool isBearerToken)
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    const QUrl requestUri = QUrl(CloudConstants::BitbucketApiUri).resolved(QUrl(QStringLiteral("2.0/user")));
    HttpRequest request;
    request.url = requestUri;
    setAuthorization(request, userName, password, isBearerToken);

    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    const HttpResponse response = m_httpClient->send(request);
    m_context.trace().writeLine(responseLine(response));
    if (response.isSuccessStatusCode()) {
        // Property names match case-insensitively
        const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
        OptString login;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key().compare(QLatin1String("username"), Qt::CaseInsensitive) == 0 && it.value().isString()) {
                login = it.value().toString();
            }
        }
        return UserInfoResult{response.statusCode, login};
    }
    return UserInfoResult{response.statusCode, std::nullopt};
}

// =============================================================================================
// Data Center REST API
// =============================================================================================

HttpClient &datacenter::BitbucketRestApi::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    return *m_httpClient;
}

QUrl datacenter::BitbucketRestApi::apiUri() const
{
    const std::optional<QUrl> remoteUri = m_context.settings().remoteUri();
    if (!remoteUri) {
        throw ArgumentException(QStringLiteral("RemoteUri must be defined to generate Bitbucket DC OAuth2 endpoint Urls"));
    }
    return baseUriWith(*remoteUri, QStringLiteral("/rest/"));
}

UserInfoResult datacenter::BitbucketRestApi::getUserInformation(const OptString &userName, const QString &password, bool isBearerToken)
{
    QString validate;
    if (m_context.settings().tryGetSetting(BitbucketConstants::EnvironmentVariables::ValidateStoredCredentials, CredCfg::SectionName, BitbucketConstants::GitConfiguration::Credential::ValidateStoredCredentials, validate) && !toBooleanyOrDefault(validate, true)) {
        m_context.trace().writeLine(QStringLiteral("Skipping retreival of user information due to %1 = %2").arg(BitbucketConstants::GitConfiguration::Credential::ValidateStoredCredentials, validate));
        return UserInfoResult{200, DataCenterConstants::OAuthUserName};
    }

    // Bitbucket Server/DC doesn't actually provide a REST API we can use to trade an access_token for the owning
    // username, therefore this is always going to return a placeholder username, however this call does provide a
    // way to validate the credentials we do have
    const QUrl requestUri = apiUri().resolved(QUrl(QStringLiteral("api/1.0/users")));
    HttpRequest request;
    request.url = requestUri;
    setAuthorization(request, userName, password, isBearerToken);

    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    const HttpResponse response = httpClient().send(request);
    m_context.trace().writeLine(responseLine(response));
    if (response.isSuccessStatusCode()) {
        // No REST API in BBS that can be used to return just my user account based on my login, but we can prove
        // the credentials work.
        return UserInfoResult{200, DataCenterConstants::OAuthUserName};
    }
    return UserInfoResult{response.statusCode, std::nullopt};
}

bool datacenter::BitbucketRestApi::isOAuthInstalled()
{
    const QUrl requestUri(absoluteUri(apiUri()) + QStringLiteral("oauth2/1.0/client"));
    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    const HttpResponse response = httpClient().get(requestUri);
    m_context.trace().writeLine(responseLine(response));
    // Accessed anonymously so no access, but it does exist
    return response.statusCode == 401;
}

QList<AuthenticationMethod> datacenter::BitbucketRestApi::getAuthenticationMethods()
{
    QList<AuthenticationMethod> authenticationMethods;
    const QUrl requestUri(absoluteUri(apiUri()) + QStringLiteral("authconfig/1.0/login-options"));
    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    const HttpResponse response = httpClient().get(requestUri);
    m_context.trace().writeLine(responseLine(response));
    if (response.isSuccessStatusCode()) {
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(response.content, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            throw Exception(QStringLiteral("The JSON value could not be converted to LoginOptions."));
        }
        // Property names match case-insensitively
        QJsonArray results;
        const QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key().compare(QLatin1String("results"), Qt::CaseInsensitive) == 0) {
                results = it.value().toArray();
            }
        }
        auto hasType = [&](const char *type) {
            return std::any_of(results.begin(), results.end(), [type](const QJsonValue &v) {
                const QJsonObject option = v.toObject();
                for (auto it = option.begin(); it != option.end(); ++it) {
                    if (it.key().compare(QLatin1String("type"), Qt::CaseInsensitive) == 0 && it.value().toString() == QLatin1String(type)) {
                        return true;
                    }
                }
                return false;
            });
        };
        if (hasType("LOGIN_FORM")) {
            authenticationMethods.append(AuthenticationMethod::BasicAuth);
        }
        if (hasType("IDP")) {
            authenticationMethods.append(AuthenticationMethod::Sso);
        }
    }
    return authenticationMethods;
}

std::shared_ptr<IBitbucketRestApi> BitbucketRestApiRegistry::get(const GitRequest &request)
{
    if (!BitbucketHelper::isBitbucketOrg(request)) {
        if (!m_dataCenterApi) {
            m_dataCenterApi = std::make_shared<datacenter::BitbucketRestApi>(m_context);
        }
        return m_dataCenterApi;
    }
    if (!m_cloudApi) {
        m_cloudApi = std::make_shared<cloud::BitbucketRestApi>(m_context);
    }
    return m_cloudApi;
}

// =============================================================================================
// OAuth2 clients
// =============================================================================================

bool tryParseBitbucketTokenResponse(const QString &json, OAuth2TokenResult &result)
{
    // BitbucketCustomTokenEndpointResponseJsonConverter: exact property names, "scopes" (not "scope") for the scopes,
    // a non-string value for a string property fails the parse, a non-integer expires_in is ignored.
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    OptString accessToken, tokenType, refreshToken, scope;
    std::optional<int> expiresIn;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        auto readString = [&](OptString &target) {
            if (value.isNull()) {
                target.reset();
                return true;
            }
            if (!value.isString()) {
                return false;
            }
            target = value.toString();
            return true;
        };
        bool ok = true;
        if (key == QLatin1String("access_token")) {
            ok = readString(accessToken);
        } else if (key == QLatin1String("token_type")) {
            ok = readString(tokenType);
        } else if (key == QLatin1String("refresh_token")) {
            ok = readString(refreshToken);
        } else if (key == QLatin1String("scopes")) {
            ok = readString(scope);
        } else if (key == QLatin1String("expires_in")) {
            const double d = value.toDouble(-1);
            expiresIn = value.isDouble() && d >= 0 && d <= 4294967295.0 && d == std::floor(d) ? std::optional<int>(int(std::min(d, 2147483647.0))) : std::nullopt;
        }
        if (!ok) {
            return false;
        }
    }
    result = OAuth2TokenResult();
    result.accessToken = valueOr(accessToken);
    result.tokenType = valueOr(tokenType);
    result.refreshToken = refreshToken;
    result.expiresInSecs = expiresIn;
    if (scope) {
        result.scopes = scope->split(QLatin1Char(' '));
    }
    return true;
}

BitbucketOAuth2Client::BitbucketOAuth2Client(HttpClient httpClient, Config config)
: OAuth2Client(std::move(httpClient), std::move(config.endpoints), std::move(config.clientId), std::move(config.redirectUri), std::move(config.clientSecret), false)
{
}

QString BitbucketOAuth2Client::getRefreshTokenServiceName(const GitRequest &request) const
{
    return BitbucketHostProvider::getRefreshTokenServiceName(request.getRemoteUri(false).value_or(QUrl()));
}

bool BitbucketOAuth2Client::tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result)
{
    return tryParseBitbucketTokenResponse(json, result);
}

namespace {

/// A developer override of an OAuth setting, else `defaultValue`.
OptString setting(ISettings &settings, const QString &envar, const QString &property, const OptString &defaultValue)
{
    QString value;
    if (settings.tryGetSetting(envar, CredCfg::SectionName, property, value)) {
        return value;
    }
    return defaultValue;
}

QUrl redirectUri(ISettings &settings, const QString &envar, const QString &property, const QString &defaultValue)
{
    QString value;
    if (settings.tryGetSetting(envar, CredCfg::SectionName, property, value)) {
        if (const std::optional<QUrl> uri = tryCreateUri(value)) {
            return *uri;
        }
    }
    return QUrl(defaultValue);
}

namespace CC = cloud::CloudConstants;
namespace DC = datacenter::DataCenterConstants;

bitbucket::BitbucketOAuth2Client::Config cloudConfig(ISettings &settings)
{
    bitbucket::BitbucketOAuth2Client::Config config;
    config.endpoints = OAuth2ServerEndpoints(QUrl(CC::OAuth2AuthorizationEndpoint), QUrl(CC::OAuth2TokenEndpoint));
    config.clientId = *setting(settings, CC::EnvironmentVariables::OAuthClientId, CC::GitConfiguration::Credential::OAuthClientId, CC::OAuth2ClientId);
    config.redirectUri = redirectUri(settings, CC::EnvironmentVariables::OAuthRedirectUri, CC::GitConfiguration::Credential::OAuthRedirectUri, CC::OAuth2RedirectUri);
    config.clientSecret = setting(settings, CC::EnvironmentVariables::OAuthClientSecret, CC::GitConfiguration::Credential::OAuthClientSecret, CC::OAuth2ClientSecret);
    return config;
}

bitbucket::BitbucketOAuth2Client::Config dataCenterConfig(ISettings &settings)
{
    bitbucket::BitbucketOAuth2Client::Config config;
    const std::optional<QUrl> remoteUri = settings.remoteUri();
    if (!remoteUri) {
        throw ArgumentException(QStringLiteral("RemoteUri must be defined to generate Bitbucket DC OAuth2 endpoint Urls"));
    }
    config.endpoints = OAuth2ServerEndpoints(baseUriWith(*remoteUri, QStringLiteral("/rest/oauth2/latest/authorize")), baseUriWith(*remoteUri, QStringLiteral("/rest/oauth2/latest/token")));

    const OptString clientId = setting(settings, DC::EnvironmentVariables::OAuthClientId, DC::GitConfiguration::Credential::OAuthClientId, std::nullopt);
    if (!clientId) {
        throw ArgumentException(QStringLiteral("Bitbucket DC OAuth Client ID must be defined"));
    }
    config.clientId = *clientId;
    config.redirectUri = redirectUri(settings, DC::EnvironmentVariables::OAuthRedirectUri, DC::GitConfiguration::Credential::OAuthRedirectUri, DC::OAuth2RedirectUri);
    config.clientSecret = setting(settings, DC::EnvironmentVariables::OAuthClientSecret, DC::GitConfiguration::Credential::OAuthClientSecret, std::nullopt);
    if (!config.clientSecret) {
        throw ArgumentException(QStringLiteral("Bitbucket DC OAuth Client Secret must be defined"));
    }
    return config;
}

} // namespace

cloud::BitbucketOAuth2Client::BitbucketOAuth2Client(HttpClient httpClient, ISettings &settings)
: bitbucket::BitbucketOAuth2Client(std::move(httpClient), cloudConfig(settings))
{
}

QStringList cloud::BitbucketOAuth2Client::scopes() const
{
    return {CC::OAuthScopes::RepositoryWrite, CC::OAuthScopes::Account};
}

datacenter::BitbucketOAuth2Client::BitbucketOAuth2Client(HttpClient httpClient, ISettings &settings)
: bitbucket::BitbucketOAuth2Client(std::move(httpClient), dataCenterConfig(settings))
{
}

QStringList datacenter::BitbucketOAuth2Client::scopes() const
{
    return {
        DC::OAuthScopes::PublicRepos,
        DC::OAuthScopes::RepoRead,
        DC::OAuthScopes::RepoWrite,
    };
}

HttpClient &OAuth2ClientRegistry::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    return *m_httpClient;
}

std::shared_ptr<bitbucket::BitbucketOAuth2Client> OAuth2ClientRegistry::get(const GitRequest &request)
{
    if (!BitbucketHelper::isBitbucketOrg(request)) {
        if (!m_dataCenterClient) {
            m_dataCenterClient = std::make_shared<datacenter::BitbucketOAuth2Client>(httpClient(), m_context.settings());
        }
        return m_dataCenterClient;
    }
    if (!m_cloudClient) {
        m_cloudClient = std::make_shared<cloud::BitbucketOAuth2Client>(httpClient(), m_context.settings());
    }
    return m_cloudClient;
}

} // namespace gcm::bitbucket
