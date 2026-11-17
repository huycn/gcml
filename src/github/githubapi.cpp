// Ports GitHubRestApi.cs, GitHubAuthChallenge.cs, GitHubOAuth2Client.cs and the AuthenticationModes enum helpers.
#include "github.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/strings.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSysInfo>

namespace gcm::github {

// =============================================================================================
// AuthenticationModes
// =============================================================================================

namespace {

// Declaration order of the enum (Enum.GetNames)
const QList<EnumName>& modeNames()
{
    static const QList<EnumName> names = {
        { int(AuthenticationModes::None), "None" },
        { int(AuthenticationModes::Basic), "Basic" },
        { int(AuthenticationModes::Browser), "Browser" },
        { int(AuthenticationModes::Pat), "Pat" },
        { int(AuthenticationModes::Device), "Device" },
        { int(AuthenticationModes::OAuth), "OAuth" },
        { int(AuthenticationModes::All), "All" },
    };
    return names;
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
// GitHubRestApi
// =============================================================================================

HttpClient &GitHubRestApi::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
        // Set the common headers and timeout
        m_httpClient->setTimeoutMs(RequestTimeoutMs);
        m_httpClient->defaultRequestHeaders().add(QStringLiteral("Accept"), GitHubConstants::GitHubApiAcceptsHeaderValue);
    }
    return *m_httpClient;
}

QUrl GitHubRestApi::getApiRequestUri(const QUrl &targetUri, const QString &apiUrl)
{
    if (GitHubHostProvider::isGitHubDotCom(targetUri)) {
        return QUrl(QStringLiteral("https://api.github.com/%1").arg(apiUrl));
    }

    QString proximaSubdomain;
    if (GitHubHostProvider::isGitHubProxima(targetUri, &proximaSubdomain)) {
        return QUrl(QStringLiteral("https://api.%1.ghe.com/%2").arg(proximaSubdomain, apiUrl));
    }

    // If we're here, it's GitHub Enterprise via a configured authority
    QString baseUrl = targetUri.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo)
                          .toString(QUrl::FullyEncoded);
    // Check for 'raw.' and likewise 'gist.' in the hostname and remove it to get the correct GHE API URL
    static const QRegularExpression raw(QStringLiteral("^(https?://)raw\\."), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression gist(QStringLiteral("^(https?://)gist\\."), QRegularExpression::CaseInsensitiveOption);
    baseUrl.replace(raw, QStringLiteral("\\1"));
    baseUrl.replace(gist, QStringLiteral("\\1"));
    return QUrl(baseUrl + QStringLiteral("/api/v3/") + apiUrl);
}

AuthenticationResult GitHubRestApi::createPersonalAccessToken(const QUrl &targetUri, const OptString &userName,
                                                              const OptString &password, const OptString &authenticationCode,
                                                              const QStringList &scopes)
{
    EnsureArgument::absoluteUri(targetUri, "targetUri");

    const QUrl requestUri = getApiRequestUri(targetUri, QStringLiteral("authorizations"));
    m_context.trace().writeLine(QStringLiteral("HTTP: POST %1").arg(absoluteUri(requestUri)));

    QStringList quotedScopes;
    for (const QString &scope : scopes) {
        quotedScopes.append(QStringLiteral("\"%1\"").arg(scope));
    }
    const QString jsonContent = QStringLiteral("{ \"scopes\": [%1], \"note\": \"git: %2 on %3 at %4\" }").arg(
        quotedScopes.join(QStringLiteral(", ")),
        absoluteUri(targetUri),
        QSysInfo::machineHostName(),
        QLocale::c().toString(QDateTime::currentDateTime(),
        QStringLiteral("dd-MMM-yyyy HH:mm")
    ));

    HttpRequest request;
    request.method = Constants::Http::MethodPost;
    request.url = requestUri;
    // Upstream labels the JSON body as form content
    request.content = jsonContent.toUtf8();
    request.contentType = QStringLiteral("application/x-www-form-urlencoded; charset=utf-8");
    request.setBasicAuthorization(valueOr(userName), valueOr(password));
    if (!isNullOrWhiteSpace(authenticationCode)) {
        request.headers.add(GitHubConstants::GitHubOptHeader, *authenticationCode);
    }

    const HttpResponse response = httpClient().send(request);
    m_context.trace().writeLine(QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), response.reasonPhrase));
    switch (response.statusCode) {
    case 200:
    case 201:
        return parseSuccessResponse(targetUri, response);
    case 401:
        return parseUnauthorizedResponse(targetUri, authenticationCode, response);
    case 403:
        return parseForbiddenResponse(targetUri, password, response);
    default:
        m_context.trace().writeLine(QStringLiteral("Authentication failed for '%1'.").arg(absoluteUri(targetUri)));
        return AuthenticationResult{GitHubAuthenticationResultType::Failure, std::nullopt};
    }
}

GitHubUserInfo GitHubRestApi::getUserInfo(const QUrl &targetUri, const QString &accessToken)
{
    const QUrl requestUri = getApiRequestUri(targetUri, QStringLiteral("user"));
    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    HttpRequest request;
    request.url = requestUri;
    request.setBearerAuthorization(accessToken);
    const HttpResponse response = httpClient().send(request);
    m_context.trace().writeLine(QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), response.reasonPhrase));
    response.ensureSuccessStatusCode();

    const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
    return GitHubUserInfo{obj.value(QStringLiteral("login")).toString()};
}

GitHubMetaInfo GitHubRestApi::getMetaInfo(const QUrl &targetUri)
{
    const QUrl requestUri = getApiRequestUri(targetUri, QStringLiteral("meta"));
    m_context.trace().writeLine(QStringLiteral("HTTP: GET %1").arg(absoluteUri(requestUri)));
    HttpRequest request;
    request.url = requestUri;
    const HttpResponse response = httpClient().send(request);
    m_context.trace().writeLine(QStringLiteral("HTTP: Response %1 [%2]").arg(QString::number(response.statusCode), response.reasonPhrase));
    response.ensureSuccessStatusCode();

    const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
    GitHubMetaInfo info;
    const QJsonValue version = obj.value(QStringLiteral("installed_version"));
    if (version.isString()) {
        info.installedVersion = version.toString();
    }
    info.verifiablePasswordAuthentication = obj.value(QStringLiteral("verifiable_password_authentication")).toBool();
    return info;
}

AuthenticationResult GitHubRestApi::parseForbiddenResponse(const QUrl &targetUri, const OptString &password, const HttpResponse &response)
{
    // This API only supports Basic authentication. If a valid OAuth token is supplied as the password, then a
    // Forbidden response is returned instead of an Unauthorized. In that case, the supplied password is an OAuth
    // token and is valid and we don't need to create a new personal access token.
    if (response.contentString().contains(QLatin1String("This API can only be accessed with username and password Basic Auth"))) {
        m_context.trace().writeLine(QStringLiteral("Authentication success: user supplied personal access token for '%1'.").arg(absoluteUri(targetUri)));
        return AuthenticationResult{GitHubAuthenticationResultType::Success, password};
    }
    m_context.trace().writeLine(QStringLiteral("Authentication failed for '%1'.").arg(absoluteUri(targetUri)));
    return AuthenticationResult{GitHubAuthenticationResultType::Failure, std::nullopt};
}

AuthenticationResult GitHubRestApi::parseUnauthorizedResponse(const QUrl &targetUri, const OptString &authenticationCode,
                                                              const HttpResponse &response)
{
    const QStringList otp = response.headers.values(GitHubConstants::GitHubOptHeader);
    if (isNullOrWhiteSpace(authenticationCode) && !otp.isEmpty()) {
        if (otp.constFirst().contains(QLatin1String("app"))) {
            m_context.trace().writeLine(QStringLiteral("Two-factor app authentication code required for '%1'.").arg(absoluteUri(targetUri)));
            return AuthenticationResult{GitHubAuthenticationResultType::TwoFactorApp, std::nullopt};
        }
        m_context.trace().writeLine(QStringLiteral("Two-factor SMS authentication code required for '%1'.").arg(absoluteUri(targetUri)));
        return AuthenticationResult{GitHubAuthenticationResultType::TwoFactorSms, std::nullopt};
    }
    m_context.trace().writeLine(QStringLiteral("Authentication failed for '%1'.").arg(absoluteUri(targetUri)));
    return AuthenticationResult{GitHubAuthenticationResultType::Failure, std::nullopt};
}

AuthenticationResult GitHubRestApi::parseSuccessResponse(const QUrl &targetUri, const HttpResponse &response)
{
    static const QRegularExpression tokenRe(QStringLiteral("\\s*\"token\"\\s*:\\s*\"([^\"]+)\"\\s*"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = tokenRe.match(response.contentString());
    if (!match.hasMatch()) {
        m_context.trace().writeLine(QStringLiteral("Authentication for '%1' failed.").arg(absoluteUri(targetUri)));
        return AuthenticationResult{GitHubAuthenticationResultType::Failure, std::nullopt};
    }
    m_context.trace().writeLine(QStringLiteral("Authentication success: new personal access token for '%1' created.").arg(absoluteUri(targetUri)));
    return AuthenticationResult{GitHubAuthenticationResultType::Success, match.captured(1)};
}

// =============================================================================================
// GitHubAuthChallenge
// =============================================================================================

QList<GitHubAuthChallenge> GitHubAuthChallenge::fromHeaders(const QStringList &headers)
{
    static const QRegularExpression basicRe(QStringLiteral("Basic\\s+(?<props1>.*)realm=\"GitHub\"(?<props2>.*)"),
                                            QRegularExpression::CaseInsensitiveOption);
    QList<GitHubAuthChallenge> challenges;
    for (const QString &header : headers) {
        const QRegularExpressionMatch match = basicRe.match(header);
        if (!match.hasMatch()) {
            continue;
        }
        // Properties are space separated key="value" pairs (keys case-insensitive)
        StringDictionary props(Qt::CaseInsensitive);
        const QString str = match.captured(QStringLiteral("props1")) + match.captured(QStringLiteral("props2"));
        const QStringList propList = str.split(QLatin1Char(' '));
        for (const QString &prop : propList) {
            const qsizetype delim = prop.indexOf(QLatin1Char('='));
            if (delim < 0) {
                continue;
            }
            QString value = prop.mid(delim + 1);
            while (value.startsWith(QLatin1Char('"'))) {
                value.remove(0, 1);
            }
            while (value.endsWith(QLatin1Char('"'))) {
                value.chop(1);
            }
            props.set(prop.left(delim).trimmed(), value);
        }
        // The enterprise shortcode is provided in the `domain_hint` property, whereas the enterprise name/slug is
        // provided in the `enterprise_hint` property.
        challenges.append(GitHubAuthChallenge(props.tryGet(QStringLiteral("domain_hint")), props.tryGet(QStringLiteral("enterprise_hint"))));
    }
    return challenges;
}

bool GitHubAuthChallenge::isDomainMember(const OptString &userName) const
{
    if (isNullOrWhiteSpace(userName)) {
        return false;
    }
    const qsizetype delim = userName->lastIndexOf(QLatin1Char('_'));
    if (delim < 0) {
        return isNullOrWhiteSpace(m_domain);
    }
    // Check for users that contain underscores but are not EMU logins
    if (GitHubConstants::InvalidUnderscoreLogins.contains(*userName, Qt::CaseInsensitive)) {
        return isNullOrWhiteSpace(m_domain);
    }
    const QString shortCode = userName->mid(delim + 1);
    return m_domain && m_domain->compare(shortCode, Qt::CaseInsensitive) == 0;
}

bool GitHubAuthChallenge::operator==(const GitHubAuthChallenge &other) const
{
    auto eq = [](const OptString &a, const OptString &b) {
        return a.has_value() == b.has_value() && (!a || a->compare(*b, Qt::CaseInsensitive) == 0);
    };
    return eq(m_domain, other.m_domain) && eq(m_enterprise, other.m_enterprise);
}

// =============================================================================================
// GitHubOAuth2Client
// =============================================================================================

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;

OAuth2ServerEndpoints createEndpoints(const QUrl &uri)
{
    // Ensure that the base URI is normalized to support Gist subdomains
    const QUrl baseUri = GitHubHostProvider::normalizeUri(uri);
    OAuth2ServerEndpoints endpoints(baseUri.resolved(QUrl(GitHubConstants::OAuthAuthorizationEndpointRelativeUri)),
                                    baseUri.resolved(QUrl(GitHubConstants::OAuthTokenEndpointRelativeUri)));
    endpoints.deviceAuthorizationEndpoint = baseUri.resolved(QUrl(GitHubConstants::OAuthDeviceEndpointRelativeUri));
    return endpoints;
}

QString getClientId(ISettings &settings)
{
    // Check for developer override value
    QString clientId;
    if (settings.tryGetSetting(GitHubConstants::EnvironmentVariables::DevOAuthClientId, CredCfg::SectionName,
                               GitHubConstants::GitConfiguration::Credential::DevOAuthClientId, clientId)) {
        return clientId;
    }
    return GitHubConstants::OAuthClientId;
}

QUrl getRedirectUri(ISettings &settings, const QUrl &targetUri)
{
    // Check for developer override value
    QString value;
    if (settings.tryGetSetting(GitHubConstants::EnvironmentVariables::DevOAuthRedirectUri, CredCfg::SectionName,
                               GitHubConstants::GitConfiguration::Credential::DevOAuthRedirectUri, value)) {
        if (const std::optional<QUrl> redirectUri = tryCreateUri(value)) {
            return *redirectUri;
        }
    }
    // Only GitHub.com supports the new OAuth redirect URI today
    return QUrl(GitHubHostProvider::isGitHubDotCom(targetUri) ? GitHubConstants::OAuthRedirectUri : GitHubConstants::OAuthLegacyRedirectUri);
}

QString getClientSecret(ISettings &settings)
{
    // Check for developer override value
    QString clientSecret;
    if (settings.tryGetSetting(GitHubConstants::EnvironmentVariables::DevOAuthClientSecret, CredCfg::SectionName,
                               GitHubConstants::GitConfiguration::Credential::DevOAuthClientSecret, clientSecret)) {
        return clientSecret;
    }
    return GitHubConstants::OAuthClientSecret;
}

} // namespace

GitHubOAuth2Client::GitHubOAuth2Client(HttpClient httpClient, ISettings &settings, const QUrl &baseUri)
    : OAuth2Client(std::move(httpClient), createEndpoints(baseUri), getClientId(settings), getRedirectUri(settings, baseUri),
                   getClientSecret(settings))
{
}

bool GitHubOAuth2Client::tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result)
{
    if (!OAuth2Json::tryParseTokenResponse(json, result)) {
        return false;
    }
    // GitHub returns a comma-separated list of scopes, rather than the expected space-delimited list as defined
    // in RFC 6749 section 3.3. (The space split of the standard parser is undone first.)
    if (!result.scopes.isEmpty()) {
        result.scopes = result.scopes.join(QLatin1Char(' ')).split(QLatin1Char(','));
    }
    return true;
}

} // namespace gcm::github
