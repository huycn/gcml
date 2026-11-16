// Hand-written stand-ins for the Moq mocks of the upstream app tests.
#pragma once

#include "core/application.h"
#include "core/auth/authentication.h"

namespace gcm::test {

/// Mock<IHostProvider>: every member is a settable function; calls are counted.
class MockHostProvider : public IHostProvider
{
public:
    QString Id;
    QString Name;
    QStringList AuthorityIds;
    std::function<bool(const GitRequest &)> IsSupportedRequest = [](const GitRequest &) { return false; };
    std::function<bool(const HttpResponse *)> IsSupportedResponse = [](const HttpResponse *) { return false; };
    std::function<GitResponse(const GitRequest &)> GetCredentialFunc;
    QList<GitRequest> StoreCalls, EraseCalls;
    int GetCalls = 0;

    QString id() const override { return Id; }
    QString name() const override { return Name; }
    QStringList supportedAuthorityIds() const override { return AuthorityIds; }
    bool isSupported(const GitRequest &request) override { return IsSupportedRequest(request); }
    bool isSupported(const HttpResponse *response) override { return IsSupportedResponse(response); }
    GitResponse getCredential(const GitRequest &request) override
    {
        ++GetCalls;
        return GetCredentialFunc(request);
    }
    void storeCredential(const GitRequest &request) override { StoreCalls.append(request); }
    void eraseCredential(const GitRequest &request) override { EraseCalls.append(request); }
};

/// Mock<IConfigurationService>
class RecordingConfigurationService : public IConfigurationService
{
public:
    QList<ConfigurationTarget> Configured, Unconfigured;
    void addComponent(IConfigurableComponent *) override {}
    void configure(ConfigurationTarget target) override { Configured.append(target); }
    void unconfigure(ConfigurationTarget target) override { Unconfigured.append(target); }
};

/// Mock<IDiagnostic>
class MockDiagnostic : public IDiagnostic
{
public:
    explicit MockDiagnostic(QString name, QList<DiagnosticReport> reports = {})
        : m_name(std::move(name)), m_reports(std::move(reports))
    {
    }
    int RunCalls = 0;
    QString name() const override { return m_name; }
    bool canRun(QString &) override { return true; }
    DiagnosticResult run(const std::function<void(const QString &)> & = nullptr) override
    {
        ++RunCalls;
        return DiagnosticResult::create(m_reports, {});
    }

private:
    QString m_name;
    QList<DiagnosticReport> m_reports;
};

/// Mock<IBasicAuthentication>
class MockBasicAuthentication : public IBasicAuthentication
{
public:
    std::function<Credential(const QString &, const OptString &)> Func;
    int Calls = 0;
    QStringList Resources;
    Credential getCredentials(const QString &resource, const OptString &userName = std::nullopt) override
    {
        ++Calls;
        Resources.append(resource);
        return Func ? Func(resource, userName) : Credential();
    }
};

/// Mock<IWindowsIntegratedAuthentication>
class MockWindowsAuthentication : public IWindowsIntegratedAuthentication
{
public:
    WindowsAuthenticationTypes Types = WindowsAuthenticationTypes::None;
    NtlmSupport Ntlm = NtlmSupport::Disabled;
    int TypesCalls = 0, AskCalls = 0;
    NtlmSupport askEnableNtlm(const QUrl &) override
    {
        ++AskCalls;
        return Ntlm;
    }
    WindowsAuthenticationTypes getAuthenticationTypes(const QUrl &) override
    {
        ++TypesCalls;
        return Types;
    }
};

/// Mock<IOAuthAuthentication>
class MockOAuthAuthentication : public IOAuthAuthentication
{
public:
    OAuthAuthenticationModes Mode = OAuthAuthenticationModes::Browser;
    OAuth2TokenResult Token;
    QList<std::pair<QString, OAuthAuthenticationModes>> ModeCalls;
    QList<QStringList> BrowserCalls, DeviceCalls;

    OAuthAuthenticationModes getAuthenticationMode(const QString &resource, OAuthAuthenticationModes modes) override
    {
        ModeCalls.append({resource, modes});
        return Mode;
    }
    OAuth2TokenResult getTokenByBrowser(IOAuth2Client &, const QStringList &scopes) override
    {
        BrowserCalls.append(scopes);
        return Token;
    }
    OAuth2TokenResult getTokenByDeviceCode(IOAuth2Client &, const QStringList &scopes) override
    {
        DeviceCalls.append(scopes);
        return Token;
    }
};

} // namespace gcm::test
