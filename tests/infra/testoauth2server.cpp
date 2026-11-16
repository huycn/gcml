#include "infra/testoauth2server.h"

#include "core/strings.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace gcm::test {

namespace {

namespace AE = OAuth2Constants::AuthorizationEndpoint;
namespace AGR = OAuth2Constants::AuthorizationGrantResponse;
namespace TE = OAuth2Constants::TokenEndpoint;
namespace DA = OAuth2Constants::DeviceAuthorization;

std::optional<QStringList> splitScopes(const OptString &scopeStr)
{
    if (!scopeStr) {
        return std::nullopt;
    }
    return scopeStr->split(QLatin1Char(' '));
}

QString joinScopes(const std::optional<QStringList> &scopes) { return scopes ? scopes->join(QLatin1Char(' ')) : QString(); }

/// application/x-www-form-urlencoded body -> dictionary ('+' is a space).
StringDictionary readFormContent(const HttpRequest &request)
{
    QString body = QString::fromUtf8(request.content);
    body.replace(QLatin1Char('+'), QLatin1String("%20"));
    return parseQueryString(body);
}

HttpResponse jsonResponse(int statusCode, const QJsonObject &json)
{
    return TestHttpMessageHandler::createResponse(statusCode, QJsonDocument(json).toJson(QJsonDocument::Compact));
}

} // namespace

// --- TestOAuth2ServerTokenGenerator -----------------------------------------------------------

QString TestOAuth2ServerTokenGenerator::nextValueOrRandom(const QStringList &values, int &index)
{
    if (index < values.size()) {
        return values[index++];
    }
    return QUuid::createUuid().toString(QUuid::Id128).mid(8);
}

QByteArray TestTokenResponse::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("access_token"), accessToken);
    json.insert(QStringLiteral("token_type"), tokenType);
    json.insert(QStringLiteral("refresh_token"), refreshToken);
    json.insert(QStringLiteral("scope"), scope);
    return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

// --- OAuth2Application ------------------------------------------------------------------------

OAuth2Application::AuthCodeGrant OAuth2Application::createAuthorizationCodeGrant(
    TestOAuth2ServerTokenGenerator &generator, const std::optional<QStringList> &scopes, const OptString &redirectUri,
    const OptString &codeChallenge, OAuth2PkceChallengeMethod codeChallengeMethod)
{
    AuthCodeGrant grant{generator.createAuthorizationCode(), scopes, redirectUri, codeChallenge, codeChallengeMethod};
    AuthGrants.append(grant);
    return grant;
}

OAuth2Application::DeviceCodeGrant OAuth2Application::createDeviceCodeGrant(TestOAuth2ServerTokenGenerator &generator,
                                                                            const std::optional<QStringList> &scopes)
{
    const QString deviceCode = generator.createDeviceCode();
    const QString userCode = generator.createUserCode();
    DeviceCodeGrant grant{userCode, deviceCode, scopes, false};
    DeviceGrants.append(grant);
    return grant;
}

bool OAuth2Application::ownsDeviceCodeGrant(const QString &userCode) const
{
    return std::any_of(DeviceGrants.begin(), DeviceGrants.end(), [&](const DeviceCodeGrant &g) { return g.userCode == userCode; });
}

void OAuth2Application::approveDeviceCodeGrant(const QString &userCode)
{
    for (DeviceCodeGrant &grant : DeviceGrants) {
        if (grant.userCode == userCode) {
            grant.approved = true;
            return;
        }
    }
    throw Exception(QStringLiteral("Invalid user code '%1'").arg(userCode));
}

bool OAuth2Application::isDeviceCodeGrantApproved(const QString &deviceCode) const
{
    for (const DeviceCodeGrant &grant : DeviceGrants) {
        if (grant.deviceCode == deviceCode) {
            return grant.approved;
        }
    }
    throw Exception(QStringLiteral("Invalid device code '%1'").arg(deviceCode));
}

TestTokenResponse OAuth2Application::issueTokens(TestOAuth2ServerTokenGenerator &generator,
                                                 const std::optional<QStringList> &scopes)
{
    const QString accessToken = generator.createAccessToken();
    const QString refreshToken = generator.createRefreshToken();
    AccessTokens.set(accessToken, refreshToken);
    RefreshTokens.set(refreshToken, scopes);
    // Keep the same scopes as before
    return TestTokenResponse{Constants::Http::WwwAuthenticateBearerScheme, accessToken, refreshToken, joinScopes(scopes)};
}

TestTokenResponse OAuth2Application::createTokenByAuthorizationGrant(TestOAuth2ServerTokenGenerator &generator,
                                                                     const QString &authCode,
                                                                     const OptString &codeVerifier,
                                                                     const OptString &redirectUri)
{
    const auto it = std::find_if(AuthGrants.begin(), AuthGrants.end(), [&](const AuthCodeGrant &g) { return g.code == authCode; });
    if (it == AuthGrants.end()) {
        throw Exception(QStringLiteral("Invalid authorization code '%1'").arg(authCode));
    }
    const AuthCodeGrant grant = *it;

    // Validate the grant's code challenge was constructed from the given code verifier
    if (!isNullOrWhiteSpace(grant.codeChallenge)) {
        if (isNullOrWhiteSpace(codeVerifier)) {
            throw Exception(QStringLiteral("Missing code verifier"));
        }

        switch (grant.codeChallengeMethod) {
        case OAuth2PkceChallengeMethod::Sha256: {
            const QString challenge = base64UrlEncode(
                QCryptographicHash::hash(codeVerifier->toLatin1(), QCryptographicHash::Sha256));
            if (challenge != *grant.codeChallenge) {
                throw Exception(QStringLiteral("Invalid code verifier '%1'").arg(*codeVerifier));
            }
            break;
        }
        case OAuth2PkceChallengeMethod::Plain:
            // The case matters!
            if (*codeVerifier != *grant.codeChallenge) {
                throw Exception(QStringLiteral("Invalid code verifier '%1'").arg(*codeVerifier));
            }
            break;
        }
    }

    // If an explicit redirect URI was used as part of the authorization request then
    // the redirect URI used for the token call must match exactly.
    if (!isNullOrWhiteSpace(grant.redirectUri) && grant.redirectUri != redirectUri) {
        throw Exception(QStringLiteral("Redirect URI must match exactly the one used when requesting the authorization code."));
    }

    // Do not allow auth code reuse
    AuthGrants.erase(it);
    return issueTokens(generator, grant.scopes);
}

TestTokenResponse OAuth2Application::createTokenByRefreshTokenGrant(TestOAuth2ServerTokenGenerator &generator,
                                                                    const QString &refreshToken)
{
    const auto scopes = RefreshTokens.tryGet(refreshToken);
    if (!scopes) {
        throw Exception(QStringLiteral("Invalid refresh token '%1'").arg(refreshToken));
    }
    return issueTokens(generator, *scopes);
}

TestTokenResponse OAuth2Application::createTokenByDeviceCodeGrant(TestOAuth2ServerTokenGenerator &generator,
                                                                  const QString &deviceCode)
{
    const auto it = std::find_if(DeviceGrants.begin(), DeviceGrants.end(),
                                 [&](const DeviceCodeGrant &g) { return g.deviceCode == deviceCode; });
    if (it == DeviceGrants.end()) {
        throw Exception(QStringLiteral("Invalid user code '%1'").arg(deviceCode));
    }
    if (!it->approved) {
        throw Exception(QStringLiteral("Grant with device code '%1' has not been approved'").arg(deviceCode));
    }
    const DeviceCodeGrant grant = *it;

    // Do not allow device code reuse
    DeviceGrants.erase(it);
    return issueTokens(generator, grant.scopes);
}

bool OAuth2Application::isValidRedirect(const QString &url) const
{
    for (const QString &redirectStr : RedirectUris) {
        // We only accept exact matches, including trailing slashes and case sensitivity
        if (redirectStr == url) {
            return true;
        }

        // For loopback URLs _only_ we ignore the port number (same early return as upstream)
        const std::optional<QUrl> uri = tryCreateUri(url);
        const QUrl redirectUri(redirectStr);
        if (uri && isLoopback(*uri) && isLoopback(redirectUri)) {
            // Case-sensitive comparison of scheme, host and path
            return redirectUri.scheme() == uri->scheme() && redirectUri.host() == uri->host() &&
                   redirectUri.path(QUrl::FullyEncoded) == uri->path(QUrl::FullyEncoded);
        }
    }
    return false;
}

QUrl OAuth2Application::validateRedirect(const OptString &redirectUrl) const
{
    // Use default redirect URI if one has not been specified for this grant
    if (!redirectUrl) {
        if (RedirectUris.isEmpty()) {
            throw InvalidOperationException(QStringLiteral("Sequence contains no elements"));
        }
        return QUrl(RedirectUris.first());
    }

    const std::optional<QUrl> uri = tryCreateUri(redirectUrl);
    if (!uri) {
        throw Exception(QStringLiteral("Redirect '%1' is not a valid URL").arg(*redirectUrl));
    }

    // If a redirect URL has been specified, it must match one of those that has been previously registered
    if (!isValidRedirect(*redirectUrl)) {
        throw Exception(QStringLiteral("Redirect URL '%1' does not match any registered values.").arg(*redirectUrl));
    }

    return *uri;
}

// --- TestOAuth2Server -------------------------------------------------------------------------

const QUrl TestOAuth2Server::DeviceCodeVerificationUri(QStringLiteral("https://example.com/devicelogin"));

void TestOAuth2Server::bind(TestHttpMessageHandler &httpHandler)
{
    httpHandler.setup(QStringLiteral("GET"), Endpoints.authorizationEndpoint,
                      [this](const HttpRequest &r) { return onAuthorizationEndpoint(r); });
    if (Endpoints.deviceAuthorizationEndpoint) {
        httpHandler.setup(QStringLiteral("POST"), *Endpoints.deviceAuthorizationEndpoint,
                          [this](const HttpRequest &r) { return onDeviceAuthorizationEndpoint(r); });
    }
    httpHandler.setup(QStringLiteral("POST"), Endpoints.tokenEndpoint,
                      [this](const HttpRequest &r) { return onTokenEndpoint(r); });
}

void TestOAuth2Server::signInDeviceWithUserCode(const QString &userCode)
{
    for (const auto &[id, app] : m_apps) {
        if (app->ownsDeviceCodeGrant(userCode)) {
            app->approveDeviceCodeGrant(userCode);
            return;
        }
    }
    throw Exception(QStringLiteral("Unknown user code '%1'").arg(userCode));
}

OAuth2Application &TestOAuth2Server::findApplication(const OptString &clientId)
{
    const auto app = clientId ? m_apps.tryGet(*clientId) : std::nullopt;
    if (!app) {
        throw Exception(QStringLiteral("Unknown OAuth application '%1'").arg(clientId.value_or(QString())));
    }
    return **app;
}

HttpResponse TestOAuth2Server::onAuthorizationEndpoint(const HttpRequest &request)
{
    if (AuthorizationEndpointInvoked) {
        AuthorizationEndpointInvoked(request);
    }

    const StringDictionary reqQuery = getQueryParameters(request.url);

    // The only supported response type so far is 'code'
    const OptString respType = reqQuery.tryGet(AE::ResponseTypeParameter);
    if (respType != AE::AuthorizationCodeResponseType) {
        throw Exception(QStringLiteral("Unsupported response type '%1'").arg(respType.value_or(QString())));
    }

    // The client/app ID must be specified and must match a known application
    OAuth2Application &app = findApplication(reqQuery.tryGet(OAuth2Constants::ClientIdParameter));

    // Redirect is optional, but if it is specified it must match a registered URL
    const OptString redirectUrlStr = reqQuery.tryGet(OAuth2Constants::RedirectUriParameter);
    const QUrl redirectUri = app.validateRedirect(redirectUrlStr);

    // Scope is optional
    const std::optional<QStringList> scopes = splitScopes(reqQuery.tryGet(OAuth2Constants::ScopeParameter));

    // Code challenge is optional
    const OptString codeChallenge = reqQuery.tryGet(AE::PkceChallengeParameter);

    // Code challenge method is optional and defaults to "plain"
    OAuth2PkceChallengeMethod codeChallengeMethod = OAuth2PkceChallengeMethod::Plain;
    if (const OptString method = reqQuery.tryGet(AE::PkceChallengeMethodParameter)) {
        if (equalsIgnoreCase(*method, AE::PkceChallengeMethodPlain)) {
            codeChallengeMethod = OAuth2PkceChallengeMethod::Plain;
        } else if (equalsIgnoreCase(*method, AE::PkceChallengeMethodS256)) {
            codeChallengeMethod = OAuth2PkceChallengeMethod::Sha256;
        } else {
            throw Exception(QStringLiteral("Unsupported code challenge method '%1'").arg(*method));
        }
    }

    const OAuth2Application::AuthCodeGrant grant =
        app.createAuthorizationCodeGrant(TokenGenerator, scopes, redirectUrlStr, codeChallenge, codeChallengeMethod);

    QString respQuery = AGR::AuthorizationCodeParameter + QLatin1Char('=') + escapeDataString(grant.code);
    // State is optional but must be included in the reply if specified
    if (const OptString state = reqQuery.tryGet(AE::StateParameter)) {
        respQuery += QLatin1Char('&') + AGR::StateParameter + QLatin1Char('=') + escapeDataString(*state);
    }

    // Build the final redirect URI including the auth code
    QUrl location = redirectUri;
    location.setQuery(respQuery);

    HttpResponse response = TestHttpMessageHandler::createResponse(200);
    response.requestUri = request.url;
    response.headers.set(QStringLiteral("Location"), location.toString(QUrl::FullyEncoded));
    return response;
}

HttpResponse TestOAuth2Server::onDeviceAuthorizationEndpoint(const HttpRequest &request)
{
    if (DeviceAuthorizationEndpointInvoked) {
        DeviceAuthorizationEndpointInvoked(request);
    }

    const StringDictionary formData = readFormContent(request);
    OAuth2Application &app = findApplication(formData.tryGet(OAuth2Constants::ClientIdParameter));
    const std::optional<QStringList> scopes = splitScopes(formData.tryGet(OAuth2Constants::ScopeParameter));

    const OAuth2Application::DeviceCodeGrant grant = app.createDeviceCodeGrant(TokenGenerator, scopes);

    QJsonObject json;
    json.insert(QStringLiteral("device_code"), grant.deviceCode);
    json.insert(QStringLiteral("user_code"), grant.userCode);
    json.insert(QStringLiteral("verification_uri"), absoluteUri(DeviceCodeVerificationUri));
    return jsonResponse(200, json);
}

HttpResponse TestOAuth2Server::onTokenEndpoint(const HttpRequest &request)
{
    if (TokenEndpointInvoked) {
        TokenEndpointInvoked(request);
    }

    const StringDictionary formData = readFormContent(request);

    const OptString grantType = formData.tryGet(TE::GrantTypeParameter);
    if (!grantType) {
        throw Exception(QStringLiteral("Missing grant type"));
    }
    const OptString clientId = formData.tryGet(OAuth2Constants::ClientIdParameter);
    if (!clientId) {
        throw Exception(QStringLiteral("Missing client ID in request body"));
    }
    OAuth2Application &app = findApplication(clientId);

    TestTokenResponse tokenResp;
    if (equalsIgnoreCase(*grantType, TE::AuthorizationCodeGrantType)) {
        const OptString authCode = formData.tryGet(TE::AuthorizationCodeParameter);
        if (!authCode) {
            throw Exception(QStringLiteral("Missing authorization code parameter"));
        }
        const OptString codeVerifier = formData.tryGet(TE::PkceVerifierParameter);

        // If the authorization code grant was created with a redirect URI then the exact same redirect URI
        // MUST be used for the token request per RFC 6749 section 4.1.3.
        const OptString redirectUriStr = formData.tryGet(OAuth2Constants::RedirectUriParameter);
        if (redirectUriStr) {
            app.validateRedirect(redirectUriStr);
        }

        tokenResp = app.createTokenByAuthorizationGrant(TokenGenerator, *authCode, codeVerifier, redirectUriStr);
    } else if (equalsIgnoreCase(*grantType, TE::RefreshTokenGrantType)) {
        const OptString refreshToken = formData.tryGet(TE::RefreshTokenParameter);
        if (!refreshToken) {
            throw Exception(QStringLiteral("Missing refresh token parameter"));
        }
        tokenResp = app.createTokenByRefreshTokenGrant(TokenGenerator, *refreshToken);
    } else if (equalsIgnoreCase(*grantType, DA::DeviceCodeGrantType)) {
        const OptString deviceCode = formData.tryGet(DA::DeviceCodeParameter);
        if (!deviceCode) {
            throw Exception(QStringLiteral("Missing device code parameter"));
        }
        if (!app.isDeviceCodeGrantApproved(*deviceCode)) {
            QJsonObject error;
            error.insert(QStringLiteral("error"), DA::Errors::AuthorizationPending);
            return jsonResponse(400, error);
        }
        tokenResp = app.createTokenByDeviceCodeGrant(TokenGenerator, *deviceCode);
    } else {
        throw Exception(QStringLiteral("Unknown grant type '%1'").arg(*grantType));
    }

    HttpResponse response = TestHttpMessageHandler::createResponse(200, tokenResp.toJson());
    response.requestUri = request.url;
    return response;
}

// --- TestOAuth2WebBrowser ---------------------------------------------------------------------

StringDictionary TestOAuth2WebBrowser::getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &,
                                                                 OAuth2ResponseMode, const CancellationToken &ct)
{
    HttpRequest request;
    request.method = QStringLiteral("GET");
    request.url = authorizationUri;
    const HttpResponse response = m_handler->send(request, 0, ct);
    if (!response.isSuccessStatusCode()) {
        throw HttpRequestException(QStringLiteral("Response status code does not indicate success: %1 (%2).")
                                       .arg(response.statusCode)
                                       .arg(response.reasonPhrase));
    }
    return getQueryParameters(QUrl(response.headers.value(QStringLiteral("Location")).value_or(QString())));
}

} // namespace gcm::test
