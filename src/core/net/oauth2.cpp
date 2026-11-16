// Port of Authentication/OAuth/{OAuth2Client,OAuth2CryptographicGenerator,OAuth2ResponseMode}.cs and Json/*.cs
#include "core/auth/oauth2.h"

#include "core/strings.h"
#include "core/trace2.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

namespace gcm {

namespace AE = OAuth2Constants::AuthorizationEndpoint;
namespace AGR = OAuth2Constants::AuthorizationGrantResponse;
namespace TE = OAuth2Constants::TokenEndpoint;
namespace DA = OAuth2Constants::DeviceAuthorization;

// =============================================================================================
// OAuth2ResponseMode
// =============================================================================================

OptString OAuth2ResponseModes::getParameterValue(OAuth2ResponseMode mode)
{
    switch (mode) {
        case OAuth2ResponseMode::Default: return std::nullopt;
        case OAuth2ResponseMode::Query: return AE::QueryResponseMode;
        case OAuth2ResponseMode::Fragment: return AE::FragmentResponseMode;
        case OAuth2ResponseMode::FormPost: return AE::FormPostResponseMode;
    }
    throw ArgumentOutOfRangeException(QStringLiteral("Unknown OAuth2 response mode. (Parameter 'mode')"));
}

bool OAuth2ResponseModes::tryParse(const OptString &value, OAuth2ResponseMode &mode)
{
    mode = OAuth2ResponseMode::Default;
    if (isNullOrWhiteSpace(value)) {
        return false;
    }
    if (equalsIgnoreCase(*value, AE::QueryResponseMode)) {
        mode = OAuth2ResponseMode::Query;
        return true;
    }
    if (equalsIgnoreCase(*value, AE::FragmentResponseMode)) {
        mode = OAuth2ResponseMode::Fragment;
        return true;
    }
    if (equalsIgnoreCase(*value, AE::FormPostResponseMode) || equalsIgnoreCase(*value, QStringLiteral("formpost"))) {
        mode = OAuth2ResponseMode::FormPost;
        return true;
    }
    return false;
}

// =============================================================================================
// OAuth2CryptographicCodeGenerator
// =============================================================================================

QString OAuth2CryptographicCodeGenerator::createNonce() { return QUuid::createUuid().toString(QUuid::Id128); }

QString OAuth2CryptographicCodeGenerator::createPkceCodeVerifier()
{
    QByteArray buf(96, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(buf.data()), buf.size() / int(sizeof(quint32)));
    return base64UrlEncode(buf);
}

QString OAuth2CryptographicCodeGenerator::createPkceCodeChallenge(OAuth2PkceChallengeMethod challengeMethod, const QString &codeVerifier)
{
    switch (challengeMethod) {
        case OAuth2PkceChallengeMethod::Plain:
            return codeVerifier;
        case OAuth2PkceChallengeMethod::Sha256:
            return base64UrlEncode(QCryptographicHash::hash(codeVerifier.toLatin1(), QCryptographicHash::Sha256));
    }
    throw ArgumentOutOfRangeException(QStringLiteral("Unknown PKCE code challenge method. (Parameter 'challengeMethod')"));
}

// =============================================================================================
// JSON
// =============================================================================================

namespace {

bool parseObject(const QString &json, QJsonObject &obj)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    obj = doc.object();
    return true;
}

/// PropertyNameCaseInsensitive lookup; Undefined when absent.
QJsonValue property(const QJsonObject &obj, const QString &name)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

/// A string property: absent/null => nullopt; returns false on a type mismatch.
bool readString(const QJsonObject &obj, const QString &name, OptString &out, bool required)
{
    const QJsonValue v = property(obj, name);
    out.reset();
    if (v.isUndefined()) {
        return !required;
    }
    if (v.isNull()) {
        return true;
    }
    if (!v.isString()) {
        return false;
    }
    out = v.toString();
    return true;
}

/// A numeric (integer) property: absent/null => nullopt; returns false on a type mismatch.
bool readInteger(const QJsonObject &obj, const QString &name, std::optional<qint64> &out)
{
    const QJsonValue v = property(obj, name);
    out.reset();
    if (v.isUndefined() || v.isNull()) {
        return true;
    }
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    if (d != double(qint64(d))) {
        return false;
    }
    out = qint64(d);
    return true;
}

/// HttpUtility.UrlEncode: unreserved [A-Za-z0-9-_.!*()], space as '+', other bytes as lower-case %xx.
QString urlEncode(const QString &s)
{
    QString out;
    for (const QByteArray utf8 = s.toUtf8(); const char c : utf8) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '!' || u == '*' || u == '(' || u == ')') {
            out += QLatin1Char(c);
        } else if (u == ' ') {
            out += QLatin1Char('+');
        } else {
            out += QStringLiteral("%%1").arg(u, 2, 16, QLatin1Char('0'));
        }
    }
    return out;
}

QString toQueryString(const StringDictionary &dict)
{
    QStringList parts;
    for (const auto &[key, value] : dict) {
        parts.append(urlEncode(key) + QLatin1Char('=') + urlEncode(value));
    }
    return parts.join(QLatin1Char('&'));
}

/// Waits `ms` milliseconds in a nested event loop; returns early (and throws) when `ct` is cancelled.
void delay(int ms, const CancellationToken &ct)
{
    ct.throwIfCancellationRequested();
    if (ms > 0) {
        QEventLoop loop;
        QPointer<QEventLoop> guard(&loop);
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        ct.onCancel([guard] {
            if (guard) {
                guard->quit();
            }
        });
        loop.exec();
    }
    ct.throwIfCancellationRequested();
}

} // namespace

bool OAuth2Json::tryParseTokenResponse(const QString &json, OAuth2TokenResult &result)
{
    QJsonObject obj;
    OptString accessToken, tokenType, refreshToken, scope;
    std::optional<qint64> expiresIn;
    if (!parseObject(json, obj) || !readString(obj, QStringLiteral("access_token"), accessToken, true) ||
        !readString(obj, QStringLiteral("token_type"), tokenType, true) ||
        !readInteger(obj, QStringLiteral("expires_in"), expiresIn) ||
        !readString(obj, QStringLiteral("refresh_token"), refreshToken, false) ||
        !readString(obj, QStringLiteral("scope"), scope, false)) {
        return false;
    }

    result = OAuth2TokenResult();
    result.accessToken = accessToken.value_or(QString());
    result.tokenType = tokenType.value_or(QString());
    result.refreshToken = refreshToken;
    if (expiresIn) {
        result.expiresInSecs = int(*expiresIn);
    }
    if (scope) {
        result.scopes = scope->split(QLatin1Char(' '));
    }
    return true;
}

bool OAuth2Json::tryParseDeviceCodeResponse(const QString &json, OAuth2DeviceCodeResult &result)
{
    QJsonObject obj;
    OptString deviceCode, userCode, verificationUri;
    std::optional<qint64> expiresIn, interval;
    if (!parseObject(json, obj) || !readString(obj, QStringLiteral("device_code"), deviceCode, true) ||
        !readString(obj, QStringLiteral("user_code"), userCode, true) ||
        !readString(obj, QStringLiteral("verification_uri"), verificationUri, true) ||
        !readInteger(obj, QStringLiteral("expires_in"), expiresIn) || !readInteger(obj, QStringLiteral("interval"), interval)) {
        return false;
    }

    result = OAuth2DeviceCodeResult();
    result.deviceCode = deviceCode.value_or(QString());
    result.userCode = userCode.value_or(QString());
    result.verificationUri = QUrl(verificationUri.value_or(QString()));
    if (interval && *interval > 0) {
        result.pollingIntervalSecs = int(*interval);
    }
    result.expiresInSecs = int(expiresIn.value_or(0));
    return true;
}

bool OAuth2Json::tryParseErrorResponse(const QString &json, QString &message)
{
    QJsonObject obj;
    OptString error, description, uriStr;
    if (!parseObject(json, obj) || !readString(obj, QStringLiteral("error"), error, true) ||
        !readString(obj, QStringLiteral("error_description"), description, false) ||
        !readString(obj, QStringLiteral("error_uri"), uriStr, false)) {
        return false;
    }

    message = error.value_or(QString());
    if (!isNullOrEmpty(description)) {
        message += QStringLiteral(": ") + *description;
    }
    if (uriStr) {
        const QUrl uri(*uriStr);
        if (!uri.isValid()) {
            return false;
        }
        message += QStringLiteral(" [%1]").arg(uri.isRelative() ? *uriStr : absoluteUri(uri));
    }
    return true;
}

// =============================================================================================
// OAuth2Client
// =============================================================================================

OAuth2Client::OAuth2Client(HttpClient httpClient, OAuth2ServerEndpoints endpoints, QString clientId,
                           std::optional<QUrl> redirectUri, OptString clientSecret, bool addAuthHeader,
                           OAuth2ResponseMode responseMode)
:   m_httpClient(std::move(httpClient)),
    m_endpoints(std::move(endpoints)),
    m_clientId(std::move(clientId)),
    m_redirectUri(std::move(redirectUri)),
    m_clientSecret(std::move(clientSecret)),
    m_addAuthHeader(addAuthHeader),
    m_responseMode(responseMode)
{
}

OAuth2Client::~OAuth2Client() = default;

IOAuth2CodeGenerator &OAuth2Client::codeGenerator()
{
    if (!m_codeGenerator) {
        m_codeGenerator = std::make_shared<OAuth2CryptographicCodeGenerator>();
    }
    return *m_codeGenerator;
}

void OAuth2Client::setCodeGenerator(std::shared_ptr<IOAuth2CodeGenerator> generator) { m_codeGenerator = std::move(generator); }

namespace {

struct ClientRequestBuilder
{
    const QString &clientId;
    const OptString &clientSecret;
    bool addAuthHeader;

    /// RFC 6749 2.2 / 3.2.1: client_id is identification and may be sent alongside HTTP Basic. Section 2.3: never
    /// use two authentication methods, so the secret only goes into the body when the Basic header is not used.
    void addClientParams(StringDictionary &data) const {
        data.set(OAuth2Constants::ClientIdParameter, clientId);
        if (!addAuthHeader && !isNullOrEmpty(clientSecret)) {
            data.set(OAuth2Constants::ClientSecretParameter, *clientSecret);
        }
    }

    HttpRequest createRequest(const QUrl &uri, const StringDictionary &form) const {
        HttpRequest request;
        request.method = Constants::Http::MethodPost;
        request.url = uri;
        request.setFormContent(form);
        request.headers.add(QStringLiteral("Accept"), Constants::Http::MimeTypeJson);
        // RFC 6749 2.3.1: servers must support HTTP Basic for clients issued a password.
        if (addAuthHeader && !isNullOrEmpty(clientSecret)) {
            request.setBasicAuthorization(clientId, *clientSecret);
        }
        return request;
    }
};

} // namespace

OAuth2AuthorizationCodeResult OAuth2Client::getAuthorizationCode(const QStringList &scopes, IOAuth2WebBrowser &browser,
                                                                 const StringDictionary &extraQueryParams,
                                                                 const CancellationToken &ct)
{
    Trace2::Scope region = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("get_authcode"));

    const QString state = codeGenerator().createNonce();
    const QString codeVerifier = codeGenerator().createPkceCodeVerifier();
    const QString codeChallenge = codeGenerator().createPkceCodeChallenge(OAuth2PkceChallengeMethod::Sha256, codeVerifier);

    StringDictionary queryParams {
        { AE::ResponseTypeParameter, AE::AuthorizationCodeResponseType },
        { OAuth2Constants::ClientIdParameter, m_clientId },
        { AE::StateParameter, state },
        { AE::PkceChallengeMethodParameter, AE::PkceChallengeMethodS256 },
        { AE::PkceChallengeParameter, codeChallenge },
    };

    // Only send the parameter when requesting a non-default mode to keep the request unchanged otherwise.
    if (m_responseMode != OAuth2ResponseMode::Default) {
        queryParams.set(AE::ResponseModeParameter, *OAuth2ResponseModes::getParameterValue(m_responseMode));
    }

    for (const auto &[key, value] : extraQueryParams) {
        if (queryParams.contains(key)) {
            throw ArgumentException(
                QStringLiteral("Extra query parameter '%1' would override required standard OAuth parameters. (Parameter 'extraQueryParams')").arg(key)
            );
        }
        queryParams.set(key, value);
    }

    std::optional<QUrl> redirectUri;
    if (m_redirectUri) {
        redirectUri = browser.updateRedirectUri(*m_redirectUri);
        // The original spelling: OAuth compares redirect URLs byte-for-byte, trailing slash included.
        queryParams.set(OAuth2Constants::RedirectUriParameter, redirectUri->toString(QUrl::FullyEncoded));
    }

    const QString scopesStr = scopes.join(QLatin1Char(' '));
    if (!isNullOrWhiteSpace(scopesStr)) {
        queryParams.set(OAuth2Constants::ScopeParameter, scopesStr);
    }

    QUrl authorizationUri = m_endpoints.authorizationEndpoint;
    authorizationUri.setQuery(toQueryString(queryParams), QUrl::StrictMode);
    if (authorizationUri.path().isEmpty()) {
        authorizationUri.setPath(QStringLiteral("/"));
    }

    // Open the browser at the request URI and intercept the response parameters delivered to the redirect URI.
    StringDictionary responseParams;
    {
        Trace2::Scope browserRegion = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("browser"));
        responseParams = browser.getAuthenticationResponse(authorizationUri, redirectUri.value_or(QUrl()), m_responseMode, ct);
    }

    // A missing or different state indicates a badly implemented server, or worse, a failed MITM or replay attack.
    QString replyState;
    if (!responseParams.tryGetValue(AGR::StateParameter, replyState)) {
        throw OAuth2Exception(QStringLiteral("Missing '%1' in response.").arg(AGR::StateParameter));
    }
    if (state != replyState) {
        throw OAuth2Exception(QStringLiteral("Invalid '%1' in response; does not match the request.").arg(AGR::StateParameter));
    }

    // Without an auth code authentication failed for some reason
    QString authCode;
    if (!responseParams.tryGetValue(AGR::AuthorizationCodeParameter, authCode)) {
        throw OAuth2Exception(QStringLiteral("Missing '%1' in response.").arg(AGR::AuthorizationCodeParameter));
    }

    return OAuth2AuthorizationCodeResult{authCode, redirectUri, codeVerifier};
}

OAuth2DeviceCodeResult OAuth2Client::getDeviceCode(const QStringList &scopes, const CancellationToken &ct)
{
    Trace2::Scope region = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("get_devicecode"));

    if (!m_endpoints.deviceAuthorizationEndpoint) {
        throw InvalidOperationException(QStringLiteral("No device authorization endpoint has been configured for this client."));
    }

    const ClientRequestBuilder builder{m_clientId, m_clientSecret, m_addAuthHeader};
    StringDictionary formData;
    builder.addClientParams(formData);
    const QString scopesStr = scopes.join(QLatin1Char(' '));
    if (!isNullOrWhiteSpace(scopesStr)) {
        formData.set(OAuth2Constants::ScopeParameter, scopesStr);
    }

    const HttpResponse response = m_httpClient.send(builder.createRequest(*m_endpoints.deviceAuthorizationEndpoint, formData), ct);
    const QString json = response.contentString();
    OAuth2DeviceCodeResult result;
    if (response.isSuccessStatusCode() && OAuth2Json::tryParseDeviceCodeResponse(json, result)) {
        return result;
    }
    throwExceptionFromResponse(json);
}

OAuth2TokenResult OAuth2Client::getTokenByAuthorizationCode(const OAuth2AuthorizationCodeResult &authorizationCodeResult, const CancellationToken &ct)
{
    Trace2::Scope region = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("token_by_authcode"));

    const ClientRequestBuilder builder{m_clientId, m_clientSecret, m_addAuthHeader};
    StringDictionary formData {
        { TE::GrantTypeParameter, TE::AuthorizationCodeGrantType },
        { TE::AuthorizationCodeParameter, authorizationCodeResult.code },
    };
    if (authorizationCodeResult.codeVerifier) {
        formData.set(TE::PkceVerifierParameter, *authorizationCodeResult.codeVerifier);
    }
    builder.addClientParams(formData);
    if (authorizationCodeResult.redirectUri) {
        formData.set(OAuth2Constants::RedirectUriParameter, authorizationCodeResult.redirectUri->toString(QUrl::FullyEncoded));
    }

    const HttpResponse response = m_httpClient.send(builder.createRequest(m_endpoints.tokenEndpoint, formData), ct);
    const QString json = response.contentString();
    OAuth2TokenResult result;
    if (response.isSuccessStatusCode() && tryCreateTokenEndpointResult(json, result)) {
        return result;
    }
    throwExceptionFromResponse(json);
}

OAuth2TokenResult OAuth2Client::getTokenByRefreshToken(const QString &refreshToken, const CancellationToken &ct)
{
    Trace2::Scope region = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("token_by_refresh"));

    const ClientRequestBuilder builder{m_clientId, m_clientSecret, m_addAuthHeader};
    StringDictionary formData {
        { TE::GrantTypeParameter, TE::RefreshTokenGrantType },
        { TE::RefreshTokenParameter, refreshToken },
    };
    builder.addClientParams(formData);

    const HttpResponse response = m_httpClient.send(builder.createRequest(m_endpoints.tokenEndpoint, formData), ct);
    const QString json = response.contentString();
    OAuth2TokenResult result;
    if (response.isSuccessStatusCode() && tryCreateTokenEndpointResult(json, result)) {
        return result;
    }
    throwExceptionFromResponse(json);
}

OAuth2TokenResult OAuth2Client::getTokenByDeviceCode(const OAuth2DeviceCodeResult &deviceCodeResult, const CancellationToken &ct)
{
    Trace2::Scope region = Trace2::startRegion(OAuth2Constants::Trace2Category, QStringLiteral("token_by_devicecode"));

    const ClientRequestBuilder builder{m_clientId, m_clientSecret, m_addAuthHeader};
    StringDictionary formData {
        { DA::GrantTypeParameter, DA::DeviceCodeGrantType },
        { DA::DeviceCodeParameter, deviceCodeResult.deviceCode },
    };
    builder.addClientParams(formData);

    qint64 retryIntervalMs = qint64(deviceCodeResult.pollingIntervalSecs) * 1000;
    while (true) {
        ct.throwIfCancellationRequested();
        try {
            const HttpResponse response = m_httpClient.send(builder.createRequest(m_endpoints.tokenEndpoint, formData), ct);
            const QString json = response.contentString();
            OAuth2TokenResult result;
            if (response.isSuccessStatusCode() && tryCreateTokenEndpointResult(json, result)) {
                return result;
            }

            QJsonObject obj;
            OptString error;
            if (parseObject(json, obj)) {
                readString(obj, QStringLiteral("error"), error, true);
            }
            if (error == DA::Errors::AuthorizationPending) {
                // Retry with the current polling interval value
            } else if (error == DA::Errors::SlowDown) {
                // We must increase the polling interval by 5 seconds
                retryIntervalMs += 5000;
            } else {
                // For all other errors do not retry
                throwExceptionFromResponse(json);
            }
        } catch (const TimeoutException &) {
            // Back-off exponentially
            retryIntervalMs += retryIntervalMs;
        }

        // Wait the polling interval before retrying
        delay(int(std::min<qint64>(retryIntervalMs, std::numeric_limits<int>::max())), ct);
    }
}

bool OAuth2Client::tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result)
{
    return OAuth2Json::tryParseTokenResponse(json, result);
}

bool OAuth2Client::tryCreateExceptionFromResponse(const QString &json, std::shared_ptr<OAuth2Exception> &exception)
{
    QString message;
    if (OAuth2Json::tryParseErrorResponse(json, message)) {
        exception = std::make_shared<OAuth2Exception>(message);
        return true;
    }
    exception.reset();
    return false;
}

void OAuth2Client::throwExceptionFromResponse(const QString &json)
{
    std::shared_ptr<OAuth2Exception> exception;
    if (tryCreateExceptionFromResponse(json, exception) && exception) {
        throw OAuth2Exception(*exception);
    }
    throw OAuth2Exception(QStringLiteral("Unknown OAuth error: %1").arg(json));
}

} // namespace gcm
