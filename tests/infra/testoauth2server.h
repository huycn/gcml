// Port of TestInfrastructure/Objects/TestOAuth2Server.cs and TestOAuth2WebBrowser.cs
#pragma once

#include "core/auth/oauth2.h"
#include "infra/testhttpmessagehandler.h"

namespace gcm::test {

class TestOAuth2ServerTokenGenerator
{
public:
    QStringList AuthCodes;
    QStringList DeviceCodes;
    QStringList UserCodes;
    QStringList AccessTokens;
    QStringList RefreshTokens;

    QString createAuthorizationCode() { return nextValueOrRandom(AuthCodes, m_authCodesIndex); }
    QString createDeviceCode() { return nextValueOrRandom(DeviceCodes, m_deviceCodesIndex); }
    QString createUserCode() { return nextValueOrRandom(UserCodes, m_userCodesIndex); }
    QString createAccessToken() { return nextValueOrRandom(AccessTokens, m_accessTokensIndex); }
    QString createRefreshToken() { return nextValueOrRandom(RefreshTokens, m_refreshTokensIndex); }

private:
    static QString nextValueOrRandom(const QStringList &values, int &index);
    int m_authCodesIndex = 0;
    int m_deviceCodesIndex = 0;
    int m_userCodesIndex = 0;
    int m_accessTokensIndex = 0;
    int m_refreshTokensIndex = 0;
};

/// Token endpoint response (TokenEndpointResponseJson).
struct TestTokenResponse
{
    QString tokenType;
    QString accessToken;
    QString refreshToken;
    QString scope;
    QByteArray toJson() const;
};

class OAuth2Application
{
public:
    struct AuthCodeGrant
    {
        QString code;
        std::optional<QStringList> scopes;
        OptString redirectUri;
        OptString codeChallenge;
        OAuth2PkceChallengeMethod codeChallengeMethod = OAuth2PkceChallengeMethod::Plain;
    };

    struct DeviceCodeGrant
    {
        QString userCode;
        QString deviceCode;
        std::optional<QStringList> scopes;
        bool approved = false;
    };

    explicit OAuth2Application(QString id) : Id(std::move(id)) {}

    QString Id;
    OptString Secret;
    /// Registered redirect URIs, compared by their original spelling.
    QStringList RedirectUris;
    QList<AuthCodeGrant> AuthGrants;
    QList<DeviceCodeGrant> DeviceGrants;
    /// access token -> refresh token
    StringDictionary AccessTokens;
    /// refresh token -> scopes
    Dictionary<std::optional<QStringList>> RefreshTokens;

    AuthCodeGrant createAuthorizationCodeGrant(TestOAuth2ServerTokenGenerator &generator,
                                               const std::optional<QStringList> &scopes, const OptString &redirectUri,
                                               const OptString &codeChallenge,
                                               OAuth2PkceChallengeMethod codeChallengeMethod);
    DeviceCodeGrant createDeviceCodeGrant(TestOAuth2ServerTokenGenerator &generator,
                                          const std::optional<QStringList> &scopes);
    bool ownsDeviceCodeGrant(const QString &userCode) const;
    void approveDeviceCodeGrant(const QString &userCode);
    bool isDeviceCodeGrantApproved(const QString &deviceCode) const;
    TestTokenResponse createTokenByAuthorizationGrant(TestOAuth2ServerTokenGenerator &generator, const QString &authCode,
                                                      const OptString &codeVerifier, const OptString &redirectUri);
    TestTokenResponse createTokenByRefreshTokenGrant(TestOAuth2ServerTokenGenerator &generator,
                                                     const QString &refreshToken);
    TestTokenResponse createTokenByDeviceCodeGrant(TestOAuth2ServerTokenGenerator &generator,
                                                   const QString &deviceCode);
    /// The redirect to use: the first registered one when `redirectUrl` is absent. Throws when not registered.
    QUrl validateRedirect(const OptString &redirectUrl) const;

private:
    bool isValidRedirect(const QString &url) const;
    TestTokenResponse issueTokens(TestOAuth2ServerTokenGenerator &generator, const std::optional<QStringList> &scopes);
};

class TestOAuth2Server
{
public:
    explicit TestOAuth2Server(OAuth2ServerEndpoints endpoints) : Endpoints(std::move(endpoints)) {}

    OAuth2ServerEndpoints Endpoints;
    TestOAuth2ServerTokenGenerator TokenGenerator;

    std::function<void(const HttpRequest &)> AuthorizationEndpointInvoked;
    std::function<void(const HttpRequest &)> DeviceAuthorizationEndpointInvoked;
    std::function<void(const HttpRequest &)> TokenEndpointInvoked;

    /// The application is shared so a test can inspect its grants and tokens afterwards.
    void registerApplication(std::shared_ptr<OAuth2Application> application) { m_apps.set(application->Id, application); }
    /// Installs the endpoint handlers. The server must outlive the handler's use.
    void bind(TestHttpMessageHandler &httpHandler);
    void signInDeviceWithUserCode(const QString &userCode);

    static const QUrl DeviceCodeVerificationUri;

private:
    HttpResponse onAuthorizationEndpoint(const HttpRequest &request);
    HttpResponse onDeviceAuthorizationEndpoint(const HttpRequest &request);
    HttpResponse onTokenEndpoint(const HttpRequest &request);
    OAuth2Application &findApplication(const OptString &clientId);

    Dictionary<std::shared_ptr<OAuth2Application>> m_apps;
};

/// Follows the authorization request through the handler and returns the query of the `Location` it answers with.
class TestOAuth2WebBrowser : public IOAuth2WebBrowser
{
public:
    explicit TestOAuth2WebBrowser(std::shared_ptr<IHttpMessageHandler> httpHandler) : m_handler(std::move(httpHandler)) {}

    QUrl updateRedirectUri(const QUrl &uri) override { return uri; }
    StringDictionary getAuthenticationResponse(const QUrl &authorizationUri, const QUrl &redirectUri,
                                               OAuth2ResponseMode responseMode, const CancellationToken &ct) override;

private:
    std::shared_ptr<IHttpMessageHandler> m_handler;
};

} // namespace gcm::test
