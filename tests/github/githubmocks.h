// Hand-written stand-ins for the Moq mocks of IGitHubRestApi and IGitHubAuthentication used by the upstream tests.
// Each method calls the matching std::function (strict: an unset function fails the test) and records its call.
#pragma once

#include "github/github.h"

#include <QTest>

namespace gcm::github::test {

struct CreatePatCall
{
    QUrl targetUri;
    OptString userName;
    OptString password;
    OptString authenticationCode;
    QStringList scopes;
};

class MockGitHubRestApi : public IGitHubRestApi
{
public:
    std::function<AuthenticationResult(const CreatePatCall &)> CreatePersonalAccessToken;
    std::function<GitHubUserInfo(const QUrl &, const QString &)> GetUserInfo;
    std::function<GitHubMetaInfo(const QUrl &)> GetMetaInfo;

    QList<CreatePatCall> CreatePatCalls;
    QList<std::pair<QUrl, QString>> GetUserInfoCalls;
    int GetMetaInfoCalls = 0;

    AuthenticationResult createPersonalAccessToken(const QUrl &targetUri, const OptString &userName, const OptString &password,
                                                   const OptString &authenticationCode, const QStringList &scopes) override
    {
        const CreatePatCall call{targetUri, userName, password, authenticationCode, scopes};
        CreatePatCalls.append(call);
        if (!CreatePersonalAccessToken) {
            throw Exception(QStringLiteral("unexpected call: createPersonalAccessToken"));
        }
        return CreatePersonalAccessToken(call);
    }
    GitHubUserInfo getUserInfo(const QUrl &targetUri, const QString &accessToken) override
    {
        GetUserInfoCalls.append({targetUri, accessToken});
        if (!GetUserInfo) {
            throw Exception(QStringLiteral("unexpected call: getUserInfo"));
        }
        return GetUserInfo(targetUri, accessToken);
    }
    GitHubMetaInfo getMetaInfo(const QUrl &targetUri) override
    {
        ++GetMetaInfoCalls;
        if (!GetMetaInfo) {
            throw Exception(QStringLiteral("unexpected call: getMetaInfo"));
        }
        return GetMetaInfo(targetUri);
    }
};

struct GetAuthenticationCall
{
    QUrl targetUri;
    OptString userName;
    AuthenticationModes modes;
};

class MockGitHubAuthentication : public IGitHubAuthentication
{
public:
    std::function<OptString(const QUrl &, const QStringList &)> SelectAccount;
    std::function<AuthenticationPromptResult(const GetAuthenticationCall &)> GetAuthentication;
    std::function<QString(const QUrl &, bool)> GetTwoFactorCode;
    std::function<OAuth2TokenResult(const QUrl &, const QStringList &, const OptString &)> GetOAuthTokenViaBrowser;
    std::function<OAuth2TokenResult(const QUrl &, const QStringList &)> GetOAuthTokenViaDeviceCode;

    QList<std::pair<QUrl, QStringList>> SelectAccountCalls;
    QList<GetAuthenticationCall> GetAuthenticationCalls;
    QList<std::tuple<QUrl, QStringList, OptString>> BrowserCalls;

    OptString selectAccount(const QUrl &targetUri, const QStringList &accounts) override
    {
        SelectAccountCalls.append({targetUri, accounts});
        if (!SelectAccount) {
            throw Exception(QStringLiteral("unexpected call: selectAccount"));
        }
        return SelectAccount(targetUri, accounts);
    }
    AuthenticationPromptResult getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes) override
    {
        const GetAuthenticationCall call{targetUri, userName, modes};
        GetAuthenticationCalls.append(call);
        if (!GetAuthentication) {
            throw Exception(QStringLiteral("unexpected call: getAuthentication"));
        }
        return GetAuthentication(call);
    }
    QString getTwoFactorCode(const QUrl &targetUri, bool isSms) override
    {
        if (!GetTwoFactorCode) {
            throw Exception(QStringLiteral("unexpected call: getTwoFactorCode"));
        }
        return GetTwoFactorCode(targetUri, isSms);
    }
    OAuth2TokenResult getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes, const OptString &loginHint) override
    {
        BrowserCalls.append({targetUri, scopes, loginHint});
        if (!GetOAuthTokenViaBrowser) {
            throw Exception(QStringLiteral("unexpected call: getOAuthTokenViaBrowser"));
        }
        return GetOAuthTokenViaBrowser(targetUri, scopes, loginHint);
    }
    OAuth2TokenResult getOAuthTokenViaDeviceCode(const QUrl &targetUri, const QStringList &scopes) override
    {
        if (!GetOAuthTokenViaDeviceCode) {
            throw Exception(QStringLiteral("unexpected call: getOAuthTokenViaDeviceCode"));
        }
        return GetOAuthTokenViaDeviceCode(targetUri, scopes);
    }
};

inline OAuth2TokenResult tokenResult(const QString &accessToken, const QString &tokenType)
{
    OAuth2TokenResult r;
    r.accessToken = accessToken;
    r.tokenType = tokenType;
    return r;
}

} // namespace gcm::github::test
