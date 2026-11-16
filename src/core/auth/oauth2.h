// OAuth 2.0 client machinery. Ports Authentication/OAuth/*.cs.
#pragma once

#include "core/constants.h"
#include "core/dictionary.h"
#include "core/http.h"
#include "core/platform.h"

namespace gcm {

namespace OAuth2Constants {

    GCM_STR(ClientIdParameter, "client_id");
    GCM_STR(ClientSecretParameter, "client_secret");
    GCM_STR(RedirectUriParameter, "redirect_uri");
    GCM_STR(ScopeParameter, "scope");
    GCM_STR(Trace2Category, "oauth2");

    namespace AuthorizationEndpoint {
        GCM_STR(StateParameter, "state");
        GCM_STR(AuthorizationCodeResponseType, "code");
        GCM_STR(ResponseTypeParameter, "response_type");
        GCM_STR(ResponseModeParameter, "response_mode");
        GCM_STR(QueryResponseMode, "query");
        GCM_STR(FragmentResponseMode, "fragment");
        GCM_STR(FormPostResponseMode, "form_post");
        GCM_STR(PkceChallengeParameter, "code_challenge");
        GCM_STR(PkceChallengeMethodParameter, "code_challenge_method");
        GCM_STR(PkceChallengeMethodPlain, "plain");
        GCM_STR(PkceChallengeMethodS256, "S256");
    } // namespace AuthorizationEndpoint

    namespace AuthorizationGrantResponse {
        GCM_STR(AuthorizationCodeParameter, "code");
        GCM_STR(ErrorCodeParameter, "error");
        GCM_STR(ErrorDescriptionParameter, "error_description");
        GCM_STR(ErrorUriParameter, "error_uri");
        GCM_STR(StateParameter, "state");
    } // namespace AuthorizationGrantResponse

    namespace TokenEndpoint {
        GCM_STR(GrantTypeParameter, "grant_type");
        GCM_STR(AuthorizationCodeGrantType, "authorization_code");
        GCM_STR(RefreshTokenGrantType, "refresh_token");
        GCM_STR(PkceVerifierParameter, "code_verifier");
        GCM_STR(AuthorizationCodeParameter, "code");
        GCM_STR(RefreshTokenParameter, "refresh_token");
    } // namespace TokenEndpoint

    namespace DeviceAuthorization {
        GCM_STR(GrantTypeParameter, "grant_type");
        GCM_STR(DeviceCodeParameter, "device_code");
        GCM_STR(DeviceCodeGrantType, "urn:ietf:params:oauth:grant-type:device_code");

        namespace Errors {
            GCM_STR(AuthorizationPending, "authorization_pending");
            GCM_STR(SlowDown, "slow_down");
        } // namespace Errors
    } // namespace DeviceAuthorization

} // namespace OAuth2Constants

GCM_DECLARE_EXCEPTION(OAuth2Exception, Exception)

enum class OAuth2ResponseMode {
    Default = 0,
    Query,
    Fragment,
    FormPost,
};

namespace OAuth2ResponseModes {
    /// nullopt for Default; otherwise "query" / "fragment" / "form_post".
    OptString getParameterValue(OAuth2ResponseMode mode);
    /// Case-insensitive; also accepts "formpost". False (mode = Default) for blank/unknown.
    bool tryParse(const OptString &value, OAuth2ResponseMode &mode);
} // namespace OAuth2ResponseModes

struct OAuth2ServerEndpoints
{
    QUrl authorizationEndpoint;
    QUrl tokenEndpoint;
    std::optional<QUrl> deviceAuthorizationEndpoint;

    OAuth2ServerEndpoints() = default;
    OAuth2ServerEndpoints(QUrl authorizationEndpoint, QUrl tokenEndpoint)
        : authorizationEndpoint(std::move(authorizationEndpoint)), tokenEndpoint(std::move(tokenEndpoint)) {}
};

struct OAuth2AuthorizationCodeResult
{
    QString code;
    std::optional<QUrl> redirectUri;
    OptString codeVerifier;
};

struct OAuth2DeviceCodeResult
{
    QString deviceCode;
    QString userCode;
    QUrl verificationUri;
    /// Seconds between polls. 5 when the server sent none (RFC 8628; upstream polls without delay then).
    int pollingIntervalSecs = 5;
    std::optional<int> expiresInSecs;
};

struct OAuth2TokenResult
{
    QString accessToken;
    QString tokenType;
    OptString refreshToken;
    std::optional<int> expiresInSecs;
    QStringList scopes;
};

/// The JSON bodies of the OAuth endpoints (Json/*.cs upstream). Property names match case-insensitively; parsing
/// fails when a required property is missing or has the wrong type.
namespace OAuth2Json {
    /// TokenEndpointResponseJson: access_token and token_type required; scope split on ' '.
    bool tryParseTokenResponse(const QString &json, OAuth2TokenResult &result);
    /// DeviceAuthorizationEndpointResponseJson: device_code, user_code and verification_uri required.
    bool tryParseDeviceCodeResponse(const QString &json, OAuth2DeviceCodeResult &result);
    /// ErrorResponseJson: "error" required; the message is "error[: description][ [uri]]".
    bool tryParseErrorResponse(const QString &json, QString &message);
} // namespace OAuth2Json

enum class OAuth2PkceChallengeMethod { Plain = 0, Sha256 = 1 };

class IOAuth2CodeGenerator
{
public:
    virtual ~IOAuth2CodeGenerator() = default;

    virtual QString createNonce() = 0;
    virtual QString createPkceCodeVerifier() = 0;
    virtual QString createPkceCodeChallenge(OAuth2PkceChallengeMethod challengeMethod, const QString &codeVerifier) = 0;
};

class OAuth2CryptographicCodeGenerator : public IOAuth2CodeGenerator
{
public:
    QString createNonce() override;
    QString createPkceCodeVerifier() override;
    QString createPkceCodeChallenge(OAuth2PkceChallengeMethod challengeMethod, const QString &codeVerifier) override;
};

class IOAuth2WebBrowser
{
public:
    virtual ~IOAuth2WebBrowser() = default;

    /// Gives a loopback redirect URI without an explicit port a free TCP port. Throws for non-loopback URIs.
    virtual QUrl updateRedirectUri(const QUrl &uri) = 0;
    /// Opens `authorizationUri` in the user's browser and waits for the redirect to `redirectUri`;
    /// returns the parameters delivered by query, fragment or form_post.
    virtual StringDictionary getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri, OAuth2ResponseMode responseMode, const CancellationToken &ct) = 0;
};

struct OAuth2WebBrowserOptions
{
    OptString successResponseHtml;
    /// May contain .NET style placeholders {0} error, {1} description, {2} uri.
    OptString failureResponseHtmlFormat;
    std::optional<QUrl> successRedirect;
    std::optional<QUrl> failureRedirectFormat;
};

/// Uses the system browser plus a loopback HTTP listener (QTcpServer) to intercept the redirect.
class OAuth2SystemWebBrowser : public IOAuth2WebBrowser
{
public:
    OAuth2SystemWebBrowser(ISessionManager &sessionManager, OAuth2WebBrowserOptions options);

    QUrl updateRedirectUri(const QUrl &uri) override;
    StringDictionary getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri, OAuth2ResponseMode responseMode, const CancellationToken &ct) override;

    static bool isFormUrlEncoded(const OptString &contentType);
    static const QString DefaultSuccessHtml;
    static const QString DefaultFailureHtmlFormat;

private:
    ISessionManager &m_sessionManager;
    OAuth2WebBrowserOptions m_options;
};

class IOAuth2Client
{
public:
    virtual ~IOAuth2Client() = default;

    virtual OAuth2AuthorizationCodeResult getAuthorizationCode(const QStringList &scopes, IOAuth2WebBrowser &browser, const StringDictionary &extraQueryParams, const CancellationToken &ct) = 0;
    virtual OAuth2DeviceCodeResult getDeviceCode(const QStringList &scopes, const CancellationToken &ct) = 0;
    virtual OAuth2TokenResult getTokenByAuthorizationCode(const OAuth2AuthorizationCodeResult &authorizationCodeResult, const CancellationToken &ct) = 0;
    virtual OAuth2TokenResult getTokenByRefreshToken(const QString &refreshToken, const CancellationToken &ct) = 0;
    /// Polls the token endpoint until the user completed the device flow, it expired, or `ct` is cancelled.
    virtual OAuth2TokenResult getTokenByDeviceCode(const OAuth2DeviceCodeResult &deviceCodeResult, const CancellationToken &ct) = 0;

    /// Extension: no extra query parameters.
    OAuth2AuthorizationCodeResult getAuthorizationCode(const QStringList &scopes, IOAuth2WebBrowser &browser, const CancellationToken &ct)
    {
        return getAuthorizationCode(scopes, browser, StringDictionary(), ct);
    }
};

class OAuth2Client : public IOAuth2Client
{
public:
    OAuth2Client(HttpClient httpClient, OAuth2ServerEndpoints endpoints, QString clientId,
                 std::optional<QUrl> redirectUri = std::nullopt, OptString clientSecret = std::nullopt,
                 bool addAuthHeader = true, OAuth2ResponseMode responseMode = OAuth2ResponseMode::Default);
    ~OAuth2Client() override;

    IOAuth2CodeGenerator &codeGenerator();
    void setCodeGenerator(std::shared_ptr<IOAuth2CodeGenerator> generator);

    using IOAuth2Client::getAuthorizationCode;
    OAuth2AuthorizationCodeResult getAuthorizationCode(const QStringList &scopes, IOAuth2WebBrowser &browser, const StringDictionary &extraQueryParams, const CancellationToken &ct) override;
    OAuth2DeviceCodeResult getDeviceCode(const QStringList &scopes, const CancellationToken &ct) override;
    OAuth2TokenResult getTokenByAuthorizationCode(const OAuth2AuthorizationCodeResult &authorizationCodeResult, const CancellationToken &ct) override;
    OAuth2TokenResult getTokenByRefreshToken(const QString &refreshToken, const CancellationToken &ct) override;
    OAuth2TokenResult getTokenByDeviceCode(const OAuth2DeviceCodeResult &deviceCodeResult, const CancellationToken &ct) override;

protected:
    /// Hooks for providers whose token endpoints deviate from the RFC (e.g. Bitbucket's "scopes" field).
    virtual bool tryCreateTokenEndpointResult(const QString &json, OAuth2TokenResult &result);
    virtual bool tryCreateExceptionFromResponse(const QString &json, std::shared_ptr<OAuth2Exception> &exception);
    /// Never returns normally: throws the OAuth2Exception built from the response (or a generic one).
    [[noreturn]] void throwExceptionFromResponse(const QString &json);

    HttpClient m_httpClient;
    OAuth2ServerEndpoints m_endpoints;
    QString m_clientId;
    std::optional<QUrl> m_redirectUri;
    OptString m_clientSecret;
    bool m_addAuthHeader;
    OAuth2ResponseMode m_responseMode;
    std::shared_ptr<IOAuth2CodeGenerator> m_codeGenerator;
};

} // namespace gcm
