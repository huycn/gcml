// The generic host provider: basic authentication, generic OAuth, and WIA detection. Ports GenericHostProvider.cs
// and GenericOAuthConfig.cs.
#pragma once

#include "core/auth/authentication.h"
#include "core/hostprovider.h"

namespace gcm {

class GenericOAuthConfig
{
public:
    struct WellKnown
    {
        static inline const QString GiteaClientId = QStringLiteral("e90ee53c-94e2-48ac-9358-a874fb9e0662");
        static inline const QString GiteaAuthzEndpoint = QStringLiteral("login/oauth/authorize");
        static inline const QString GiteaTokenEndpoint = QStringLiteral("login/oauth/access_token");
        static inline const QUrl LocalIPv4RedirectUri = QUrl(QStringLiteral("http://127.0.0.1"));
    };

    /// False (and a trace line) when the authorize or token endpoint is missing or invalid.
    static bool tryGet(ITrace &trace, ISettings &settings, const GitRequest &request, GenericOAuthConfig &config);

    OAuth2ServerEndpoints endpoints;
    OptString clientId;
    OptString clientSecret;
    QUrl redirectUri;
    QStringList scopes;
    bool useAuthHeader = true;
    OAuth2ResponseMode responseMode = OAuth2ResponseMode::Default;
    QString defaultUserName;

    bool supportsDeviceCode() const {
        return endpoints.deviceAuthorizationEndpoint.has_value();
    }
};

class GenericHostProvider : public IHostProvider
{
public:
    explicit GenericHostProvider(ICommandContext &context);
    GenericHostProvider(ICommandContext &context,
                        std::shared_ptr<IBasicAuthentication> basicAuth,
                        std::shared_ptr<IWindowsIntegratedAuthentication> winAuth,
                        std::shared_ptr<IOAuthAuthentication> oauth);

    QString id() const override { return QStringLiteral("generic"); }
    QString name() const override { return QStringLiteral("Generic"); }
    QStringList supportedAuthorityIds() const override;
    bool isSupported(const GitRequest &request) override;
    bool isSupported(const HttpResponse *) override { return false; }
    QString getServiceName(const GitRequest &request) const;
    GitResponse getCredential(const GitRequest &request) override;
    void storeCredential(const GitRequest &request) override;
    void eraseCredential(const GitRequest &request) override;
    GitResponse generateCredential(const GitRequest &request);

private:
    Credential getOAuthAccessToken(const QUrl &remoteUri, const OptString &userName, const GenericOAuthConfig &config);
    bool isWindowsAuthAllowed();

    ICommandContext &m_context;
    std::shared_ptr<IBasicAuthentication> m_basicAuth;
    std::shared_ptr<IWindowsIntegratedAuthentication> m_winAuth;
    std::shared_ptr<IOAuthAuthentication> m_oauth;
    std::optional<HttpClient> m_httpClient;
};

} // namespace gcm
