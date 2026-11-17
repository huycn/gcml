// Hand-written stand-ins for the Moq mocks of the Bitbucket tests (strict: an unexpected call throws, which the code
// under test sees like a failing request -- as Moq's MockBehavior.Strict does).
#pragma once

#include "bitbucket/bitbucket.h"

namespace gcm::bitbucket::test {

struct GetUserInfoCall
{
    OptString userName;
    QString password;
    bool isBearerToken;
    bool operator==(const GetUserInfoCall &) const = default;
};

class MockBitbucketRestApi : public IBitbucketRestApi
{
public:
    /// Results by (userName, password, isBearerToken).
    QList<std::pair<GetUserInfoCall, UserInfoResult>> UserInfoResults;
    std::optional<QList<AuthenticationMethod>> AuthenticationMethods;
    std::optional<bool> OAuthInstalled;
    QList<GetUserInfoCall> UserInfoCalls;

    void setupUserInfo(const OptString &userName, const QString &password, bool isBearerToken, UserInfoResult result)
    {
        UserInfoResults.append({GetUserInfoCall{userName, password, isBearerToken}, std::move(result)});
    }
    int userInfoCalls(const OptString &userName, const QString &password, bool isBearerToken) const
    {
        return int(UserInfoCalls.count(GetUserInfoCall{userName, password, isBearerToken}));
    }
    int basicUserInfoCalls() const
    {
        return int(std::count_if(UserInfoCalls.begin(), UserInfoCalls.end(), [](const GetUserInfoCall &c) { return !c.isBearerToken; }));
    }

    UserInfoResult getUserInformation(const OptString &userName, const QString &password, bool isBearerToken) override
    {
        const GetUserInfoCall call{userName, password, isBearerToken};
        UserInfoCalls.append(call);
        for (const auto &[setup, result] : std::as_const(UserInfoResults)) {
            if (setup == call) {
                return result;
            }
        }
        throw Exception(QStringLiteral("unexpected call: getUserInformation"));
    }
    bool isOAuthInstalled() override
    {
        if (!OAuthInstalled) {
            throw Exception(QStringLiteral("unexpected call: isOAuthInstalled"));
        }
        return *OAuthInstalled;
    }
    QList<AuthenticationMethod> getAuthenticationMethods() override
    {
        if (!AuthenticationMethods) {
            throw Exception(QStringLiteral("unexpected call: getAuthenticationMethods"));
        }
        return *AuthenticationMethods;
    }
};

struct GetCredentialsCall
{
    QUrl targetUri;
    OptString userName;
    AuthenticationModes modes;
};

class MockBitbucketAuthentication : public IBitbucketAuthentication
{
public:
    std::optional<CredentialsPromptResult> PromptResult;
    std::optional<OAuth2TokenResult> CreateResult;
    /// Results by refresh token.
    QList<std::pair<QString, OAuth2TokenResult>> RefreshResults;

    QList<GetCredentialsCall> GetCredentialsCalls;
    int CreateCalls = 0;
    QStringList RefreshCalls;

    CredentialsPromptResult getCredentials(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override
    {
        GetCredentialsCalls.append({targetUri, userName, modes});
        if (!PromptResult) {
            throw Exception(QStringLiteral("unexpected call: getCredentials"));
        }
        return *PromptResult;
    }
    OAuth2TokenResult createOAuthCredentials(const GitRequest &) override
    {
        ++CreateCalls;
        if (!CreateResult) {
            throw Exception(QStringLiteral("unexpected call: createOAuthCredentials"));
        }
        return *CreateResult;
    }
    OAuth2TokenResult refreshOAuthCredentials(const GitRequest &, const QString &refreshToken) override
    {
        RefreshCalls.append(refreshToken);
        for (const auto &[token, result] : std::as_const(RefreshResults)) {
            if (token == refreshToken) {
                return result;
            }
        }
        throw Exception(QStringLiteral("unexpected call: refreshOAuthCredentials"));
    }
    QString getRefreshTokenServiceName(const GitRequest &request) override
    {
        return BitbucketHostProvider::getRefreshTokenServiceName(request.getRemoteUri().value_or(QUrl()));
    }
};

/// IRegistry<IBitbucketRestApi> that always returns the same API.
class MockRestApiRegistry : public IRegistry<IBitbucketRestApi>
{
public:
    explicit MockRestApiRegistry(std::shared_ptr<IBitbucketRestApi> api) : m_api(std::move(api)) {}
    std::shared_ptr<IBitbucketRestApi> get(const GitRequest &) override { return m_api; }

private:
    std::shared_ptr<IBitbucketRestApi> m_api;
};

inline OAuth2TokenResult tokenResult(const QString &accessToken, const OptString &refreshToken)
{
    OAuth2TokenResult r;
    r.accessToken = accessToken;
    r.tokenType = QStringLiteral("access_token");
    r.refreshToken = refreshToken;
    return r;
}

} // namespace gcm::bitbucket::test
