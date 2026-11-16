// Port of TestInfrastructure/Objects/TestSettings.cs
#pragma once

#include "core/constants.h"
#include "core/settings.h"
#include "infra/testenvironment.h"
#include "infra/testgitconfiguration.h"

namespace gcm::test {

class TestSettings : public ISettings
{
public:
    /// Not owned; either may be null.
    TestEnvironment *Environment = nullptr;
    TestGitConfiguration *GitConfiguration = nullptr;

    bool IsDebuggingEnabled = false;
    bool IsTerminalPromptsEnabled = true;
    bool IsGuiPromptsEnabled = true;
    bool IsInteractionAllowed = true;
    OptString Trace;
    bool IsSecretTracingEnabled = false;
    bool IsMsalTracingEnabled = false;
    OptString ProviderOverride;
    OptString LegacyAuthorityOverride;
    bool IsWindowsIntegratedAuthenticationEnabled = true;
    bool IsCertificateVerificationEnabled = true;
    bool AutomaticallyUseClientCertificates = false;
    std::optional<gcm::ProxyConfiguration> ProxyConfiguration;
    OptString ParentWindowId;
    QString CredentialNamespace = QStringLiteral("git-test");
    OptString CredentialBackingStore;
    OptString CustomCertificateBundlePath;
    OptString CustomCookieFilePath;
    int AutoDetectProviderTimeout = Constants::DefaultAutoDetectProviderTimeoutMs;
    std::optional<bool> UseMsAuthDefaultAccount;
    bool AllowUnsafeRemotes = false;
    std::optional<QUrl> RemoteUri;

    // ISettings
    bool tryGetSetting(const OptString &envarName, const OptString &section, const OptString &property,
                       QString &value) override;
    bool tryGetPathSetting(const OptString &envarName, const OptString &section, const OptString &property,
                           QString &value) override
    {
        return tryGetSetting(envarName, section, property, value);
    }
    QStringList getSettingValues(const OptString &envarName, const OptString &section, const OptString &property,
                                 bool isPath) override;
    std::optional<QUrl> remoteUri() const override { return RemoteUri; }
    void setRemoteUri(const std::optional<QUrl> &uri) override { RemoteUri = uri; }
    bool isDebuggingEnabled() override { return IsDebuggingEnabled; }
    bool isTerminalPromptsEnabled() override { return IsTerminalPromptsEnabled; }
    bool isGuiPromptsEnabled() override { return IsGuiPromptsEnabled; }
    void setGuiPromptsEnabled(bool value) override { IsGuiPromptsEnabled = value; }
    bool isInteractionAllowed() override { return IsInteractionAllowed; }
    bool getTracingEnabled(QString &value) override
    {
        value = Trace.value_or(QString());
        return Trace.has_value();
    }
    bool isSecretTracingEnabled() override { return IsSecretTracingEnabled; }
    bool isMsalTracingEnabled() override { return IsMsalTracingEnabled; }
    OptString providerOverride() override { return ProviderOverride; }
    OptString legacyAuthorityOverride() override { return LegacyAuthorityOverride; }
    bool isWindowsIntegratedAuthenticationEnabled() override { return IsWindowsIntegratedAuthenticationEnabled; }
    bool isCertificateVerificationEnabled() override { return IsCertificateVerificationEnabled; }
    bool automaticallyUseClientCertificates() override { return AutomaticallyUseClientCertificates; }
    std::optional<gcm::ProxyConfiguration> getProxyConfiguration() override { return ProxyConfiguration; }
    OptString parentWindowId() override { return ParentWindowId; }
    QString credentialNamespace() override { return CredentialNamespace; }
    OptString credentialBackingStore() override { return CredentialBackingStore; }
    OptString customCertificateBundlePath() override { return CustomCertificateBundlePath; }
    OptString customCookieFilePath() override { return CustomCookieFilePath; }
    int autoDetectProviderTimeout() override { return AutoDetectProviderTimeout; }
    std::optional<bool> useMsAuthDefaultAccount() override { return UseMsAuthDefaultAccount; }
    bool useSoftwareRendering() override { return false; }
    bool allowUnsafeRemotes() override { return AllowUnsafeRemotes; }
};

} // namespace gcm::test
