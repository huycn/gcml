// Port of Settings.cs + Interop/Linux/LinuxSettings.cs + LinuxConfigParser.cs.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"

namespace gcm {

class IEnvironment;
class IGit;
class ITrace;
class IFileSystem;

struct ProxyConfiguration
{
    QUrl address; // without userinfo/query/fragment
    OptString userName;
    OptString password;
    OptString noProxyRaw;
    bool isDeprecatedSource = false;

    /// Converts a cURL `no_proxy` list into bypass regular expressions (same output as upstream).
    static QStringList convertToBypassRegexArray(const OptString &noProxy);
};

class ISettings
{
public:
    virtual ~ISettings() = default;

    /// Looks up a setting: environment variable first, then Git configuration scoped to remoteUri()
    /// (most specific scope first), then unscoped, then external defaults. Pass nullopt to skip a source.
    virtual bool tryGetSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value) = 0;
    /// As tryGetSetting but Git config values are canonicalised as paths (`git config --type=path`).
    virtual bool tryGetPathSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value) = 0;
    /// All candidate values in precedence order.
    virtual QStringList getSettingValues(const OptString &envarName, const OptString &section, const OptString &property, bool isPath) = 0;

    virtual std::optional<QUrl> remoteUri() const = 0;
    virtual void setRemoteUri(const std::optional<QUrl> &uri) = 0;

    virtual bool isDebuggingEnabled() = 0;
    virtual bool isTerminalPromptsEnabled() = 0;
    virtual bool isGuiPromptsEnabled() = 0;
    virtual void setGuiPromptsEnabled(bool value) = 0;
    virtual bool isInteractionAllowed() = 0;
    /// Returns true when tracing is on; `value` receives the raw setting ("1"/"true" => stderr, else a file path).
    virtual bool getTracingEnabled(QString &value) = 0;
    virtual bool isSecretTracingEnabled() = 0;
    virtual bool isMsalTracingEnabled() = 0;
    virtual OptString providerOverride() = 0;
    virtual OptString legacyAuthorityOverride() = 0;
    virtual bool isWindowsIntegratedAuthenticationEnabled() = 0;
    virtual bool isCertificateVerificationEnabled() = 0;
    virtual bool automaticallyUseClientCertificates() = 0;
    virtual std::optional<ProxyConfiguration> getProxyConfiguration() = 0;
    virtual OptString parentWindowId() = 0;
    virtual QString credentialNamespace() = 0;
    virtual OptString credentialBackingStore() = 0;
    virtual OptString customCertificateBundlePath() = 0;
    virtual OptString customCookieFilePath() = 0;
    virtual int autoDetectProviderTimeout() = 0;
    virtual std::optional<bool> useMsAuthDefaultAccount() = 0;
    virtual bool useSoftwareRendering() = 0;
    virtual bool allowUnsafeRemotes() = 0;
};

class Settings : public ISettings
{
public:
    Settings(IEnvironment &environment, IGit &git);
    ~Settings() override;

    bool tryGetSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value) override;
    bool tryGetPathSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value) override;
    QStringList getSettingValues(const OptString &envarName, const OptString &section, const OptString &property, bool isPath) override;
    std::optional<QUrl> remoteUri() const override { return m_remoteUri; }
    void setRemoteUri(const std::optional<QUrl> &uri) override { m_remoteUri = uri; }
    bool isDebuggingEnabled() override;
    bool isTerminalPromptsEnabled() override;
    bool isGuiPromptsEnabled() override;
    void setGuiPromptsEnabled(bool value) override;
    bool isInteractionAllowed() override;
    bool getTracingEnabled(QString &value) override;
    bool isSecretTracingEnabled() override;
    bool isMsalTracingEnabled() override;
    OptString providerOverride() override;
    OptString legacyAuthorityOverride() override;
    bool isWindowsIntegratedAuthenticationEnabled() override;
    bool isCertificateVerificationEnabled() override;
    bool automaticallyUseClientCertificates() override;
    std::optional<ProxyConfiguration> getProxyConfiguration() override;
    OptString parentWindowId() override;
    QString credentialNamespace() override;
    OptString credentialBackingStore() override;
    OptString customCertificateBundlePath() override;
    OptString customCookieFilePath() override;
    int autoDetectProviderTimeout() override;
    std::optional<bool> useMsAuthDefaultAccount() override;
    bool useSoftwareRendering() override;
    bool allowUnsafeRemotes() override;

    /// Hook for OS-level defaults (LinuxSettings reads /etc/git-credential-manager/config.d/*).
    /// Public for tests (`protected internal` upstream).
    virtual bool tryGetExternalDefault(const QString &section, const OptString &scope, const QString &property, QString &value);

protected:
    IEnvironment &m_environment;
    IGit &m_git;

private:
    std::optional<QUrl> m_remoteUri;
    std::optional<StringDictionary> m_configEntries; // keyed by normalizeGitConfigKey
};

/// Adds system-wide defaults read from INI-like files in Constants::LinuxAppDefaultsDirectoryPath.
class LinuxSettings : public Settings
{
public:
    LinuxSettings(IEnvironment &environment, IGit &git, ITrace &trace, IFileSystem &fs);

    bool tryGetExternalDefault(const QString &section, const OptString &scope, const QString &property, QString &value) override;

private:
    ITrace &m_trace;
    IFileSystem &m_fs;
    /// Keyed by normalizeGitConfigKey.
    std::optional<StringDictionary> m_extConfigCache;
};

/// Parser of the `key=value` Linux app-defaults files (LinuxConfigParser upstream).
class LinuxConfigParser
{
public:
    explicit LinuxConfigParser(ITrace &trace) : m_trace(trace) {}

    /// Keys are compared with Git config key semantics; later entries overwrite earlier ones.
    StringDictionary parse(const QString &content);

private:
    ITrace &m_trace;
};

} // namespace gcm
