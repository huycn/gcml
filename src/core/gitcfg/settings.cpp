// Port of Settings.cs (Settings, ProxyConfiguration), Interop/Linux/LinuxSettings.cs and LinuxConfigParser.cs
#include "core/settings.h"

#include "core/constants.h"
#include "core/git.h"
#include "core/platform.h"
#include "core/strings.h"
#include "core/trace.h"

#include <QRegularExpression>

namespace gcm {

namespace KnownEnvars = Constants::EnvironmentVariables;
namespace KnownGitCfg = Constants::GitConfiguration;
namespace GitCredCfg = Constants::GitConfiguration::Credential;
namespace GitHttpCfg = Constants::GitConfiguration::Http;

// =============================================================================================
// ProxyConfiguration
// =============================================================================================

QStringList ProxyConfiguration::convertToBypassRegexArray(const OptString &noProxy)
{
    QStringList result;
    if (isNullOrWhiteSpace(noProxy)) {
        return result;
    }

    static const QRegularExpression separators(QStringLiteral("[, ]"));
    const QStringList split = noProxy->split(separators, Qt::SkipEmptyParts);
    for (const QString &str : split) {
        QString normalized = str;
        // Strip leading subdomain wildcards: *.example.com => example.com
        if (normalized.size() > 1 && normalized[0] == QLatin1Char('*') && normalized[1] == QLatin1Char('.')) {
            normalized.remove(0, 2);
        }
        // Strip all leading dots: .example.com => example.com
        while (!normalized.isEmpty() && normalized[0] == QLatin1Char('.')) {
            normalized.remove(0, 1);
        }

        // Only match (sub-)domains, not partial domain names: "example.com" matches "http://example.com" and
        // "http://www.example.com" but not "http://notanexample.com". Then the escaped domain, anchored at the
        // end so only the specified port and TLD match. (Regex.Escape does not escape ':'.)
        QString escaped = QRegularExpression::escape(normalized);
        escaped.replace(QLatin1String("\\:"), QLatin1String(":")).replace(QLatin1String("\\-"), QLatin1String("-"));
        result.append(QStringLiteral("(\\.|\\:\\/\\/)") + escaped + QLatin1Char('$'));
    }
    return result;
}

// =============================================================================================
// Settings
// =============================================================================================

Settings::Settings(IEnvironment &environment, IGit &git)
:   m_environment(environment),
    m_git(git)
{
}

Settings::~Settings() = default;

bool Settings::tryGetSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value)
{
    const QStringList allValues = getSettingValues(envarName, section, property, false);
    value = allValues.isEmpty() ? QString() : allValues.first();
    return !allValues.isEmpty();
}

bool Settings::tryGetPathSetting(const OptString &envarName, const OptString &section, const OptString &property, QString &value)
{
    const QStringList allValues = getSettingValues(envarName, section, property, true);
    value = allValues.isEmpty() ? QString() : allValues.first();
    return !allValues.isEmpty();
}

QStringList Settings::getSettingValues(const OptString &envarName, const OptString &section, const OptString &property, bool isPath)
{
    QStringList result;
    QString value;

    if (envarName && m_environment.variables().tryGetValue(*envarName, value)) {
        result.append(value);
    }

    if (!section || !property) {
        return result;
    }

    IGitConfiguration &config = m_git.getConfiguration();

    // Take a local copy of all configuration entries once: the configuration is assumed not to change during a
    // single (short-lived) invocation. Path values still have to be asked from Git to be expanded correctly.
    if (!m_configEntries) {
        m_configEntries.emplace();
        config.enumerate([this](const GitConfigurationEntry &entry) {
            m_configEntries->set(normalizeGitConfigKey(entry.key), entry.value);
            return true; // Continue the enumeration
        });
    }

    auto lookup = [&](const QString &queryName, QString &out) {
        return (isPath && config.tryGet(queryName, true, out)) ||
               m_configEntries->tryGetValue(normalizeGitConfigKey(queryName), out);
    };

    if (m_remoteUri) {
        // URL scoped entries, most specific first (stopping before the TLD). Diverging from Git, each scope is also
        // looked up without the scheme ("example.com" besides "https://example.com"), so one entry can serve both
        // http and https. Section and property are case-insensitive, the scope is case-sensitive.
        const QStringList scopes = getGitConfigurationScopes(*m_remoteUri);
        for (const QString &scope : scopes) {
            if (lookup(QStringLiteral("%1.%2.%3").arg(*section, scope, *property), value)) {
                result.append(value);
            }

            const QString scopeWithoutScheme = trimUntilIndexOf(scope, QStringLiteral("://"));
            if (lookup(QStringLiteral("%1.%2.%3").arg(*section, scopeWithoutScheme, *property), value)) {
                result.append(value);
            }

            // An externally specified default value
            if (tryGetExternalDefault(*section, scope, *property, value)) {
                result.append(value);
            }
        }
    }

    // Un-scoped [section] property
    if (lookup(QStringLiteral("%1.%2").arg(*section, *property), value)) {
        result.append(value);
    }

    // An externally specified default value without a scope
    if (tryGetExternalDefault(*section, std::nullopt, *property, value)) {
        result.append(value);
    }

    return result;
}

bool Settings::tryGetExternalDefault(const QString &, const OptString &, const QString &, QString &value)
{
    value.clear();
    return false;
}

bool Settings::isDebuggingEnabled()
{
    QString str;
    return tryGetSetting(KnownEnvars::GcmDebug, GitCredCfg::SectionName, GitCredCfg::Debug, str) && isTruthy(str);
}

bool Settings::isTerminalPromptsEnabled()
{
    QString value;
    if (m_environment.variables().tryGetValue(KnownEnvars::GitTerminalPrompts, value)) {
        return toBooleanyOrDefault(value, true);
    }
    return true;
}

bool Settings::isGuiPromptsEnabled()
{
    const bool defaultValue = true;
    QString str;
    if (tryGetSetting(KnownEnvars::GcmGuiPromptsEnabled, GitCredCfg::SectionName, GitCredCfg::GuiPromptsEnabled, str)) {
        return toBooleanyOrDefault(str, defaultValue);
    }
    return defaultValue;
}

void Settings::setGuiPromptsEnabled(bool value)
{
    m_environment.setEnvironmentVariable(KnownEnvars::GcmGuiPromptsEnabled, value ? QStringLiteral("True") : QStringLiteral("False"));
}

bool Settings::isInteractionAllowed()
{
    const bool defaultValue = true;
    QString value;
    if (tryGetSetting(KnownEnvars::GcmInteractive, GitCredCfg::SectionName, GitCredCfg::Interactive, value)) {
        // COMPAT: the previous GCM accepted 'auto', 'never' and 'always'. 'always' (and 'force', 'true') now mean
        // "prompt if required", like 'auto'; 'never' and 'false' still fail when interaction is required.
        if (equalsIgnoreCase(value, QStringLiteral("never"))) {
            return false;
        }
        return toBooleanyOrDefault(value, defaultValue);
    }
    return defaultValue;
}

bool Settings::getTracingEnabled(QString &value)
{
    return tryGetSetting(KnownEnvars::GcmTrace, GitCredCfg::SectionName, GitCredCfg::Trace, value) && !isFalsey(value);
}

bool Settings::useSoftwareRendering()
{
    // Upstream defaults to software rendering on Windows ARM only.
    const bool defaultValue = false;
    QString str;
    return tryGetSetting(KnownEnvars::GcmGuiSoftwareRendering, GitCredCfg::SectionName, GitCredCfg::GuiSoftwareRendering, str) ? toBooleanyOrDefault(str, defaultValue) : defaultValue;
}

bool Settings::allowUnsafeRemotes()
{
    QString str;
    return tryGetSetting(KnownEnvars::GcmAllowUnsafeRemotes, GitCredCfg::SectionName, GitCredCfg::AllowUnsafeRemotes, str) && toBooleanyOrDefault(str, false);
}

bool Settings::isSecretTracingEnabled()
{
    QString str;
    return tryGetSetting(KnownEnvars::GcmTraceSecrets, GitCredCfg::SectionName, GitCredCfg::TraceSecrets, str) && isTruthy(str);
}

bool Settings::isMsalTracingEnabled()
{
    QString str;
    return tryGetSetting(KnownEnvars::GcmTraceMsAuth, GitCredCfg::SectionName, GitCredCfg::TraceMsAuth, str) && isTruthy(str);
}

OptString Settings::providerOverride()
{
    QString providerId;
    if (tryGetSetting(KnownEnvars::GcmProvider, GitCredCfg::SectionName, GitCredCfg::Provider, providerId)) {
        return providerId;
    }
    return std::nullopt;
}

OptString Settings::legacyAuthorityOverride()
{
    QString authority;
    if (tryGetSetting(KnownEnvars::GcmAuthority, GitCredCfg::SectionName, GitCredCfg::Authority, authority)) {
        return authority;
    }
    return std::nullopt;
}

bool Settings::isWindowsIntegratedAuthenticationEnabled()
{
    QString value;
    return !tryGetSetting(KnownEnvars::GcmAllowWia, GitCredCfg::SectionName, GitCredCfg::AllowWia, value) || toBooleanyOrDefault(value, true);
}

bool Settings::isCertificateVerificationEnabled()
{
    // Prefer environment variable
    QString envarValue;
    if (m_environment.variables().tryGetValue(KnownEnvars::GitSslNoVerify, envarValue)) {
        return !toBooleanyOrDefault(envarValue, false);
    }

    // Next try the equivalent Git configuration option
    QString cfgValue;
    if (tryGetSetting(std::nullopt, GitHttpCfg::SectionName, GitHttpCfg::SslVerify, cfgValue)) {
        return toBooleanyOrDefault(cfgValue, true);
    }

    // Safe default
    return true;
}

bool Settings::automaticallyUseClientCertificates()
{
    QString value;
    return tryGetSetting(std::nullopt, GitHttpCfg::SectionName, GitHttpCfg::SslAutoClientCert, value) &&
           toBooleanyOrDefault(value, false);
}

OptString Settings::customCertificateBundlePath()
{
    QString value;
    if (tryGetPathSetting(KnownEnvars::GitSslCaInfo, GitHttpCfg::SectionName, GitHttpCfg::SslCaInfo, value)) {
        return value;
    }
    return std::nullopt;
}

OptString Settings::customCookieFilePath()
{
    QString value;
    if (tryGetPathSetting(std::nullopt, GitHttpCfg::SectionName, GitHttpCfg::CookieFile, value)) {
        return value;
    }
    return std::nullopt;
}

int Settings::autoDetectProviderTimeout()
{
    QString valueStr;
    int value = 0;
    if (tryGetSetting(KnownEnvars::GcmAutoDetectTimeout, GitCredCfg::SectionName, GitCredCfg::AutoDetectTimeout, valueStr) && tryToInt32(valueStr, value)) {
        return value;
    }
    return Constants::DefaultAutoDetectProviderTimeoutMs;
}

std::optional<ProxyConfiguration> Settings::getProxyConfiguration()
{
    auto tryGetUriSetting = [this](const OptString &envarName, const OptString &section, const OptString &property, QUrl &uri) {
        const QStringList allValues = getSettingValues(envarName, section, property, false);
        for (const QString &value : allValues) {
            if (const std::optional<QUrl> parsed = tryCreateUri(value)) {
                uri = *parsed;
                return true;
            }
            if (isNullOrWhiteSpace(value)) {
                return false; // An empty string value means "no proxy"
            }
        }
        return false;
    };

    auto createConfiguration = [this](const QUrl &uri, bool isLegacy = false) {
        ProxyConfiguration config;
        // Keep only the scheme, host, port and path
        config.address = uri.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
        // Extract the user name and password from the URI if present
        tryGetUserInfo(uri, config.userName, config.password);
        // Proxy bypass host names: lowercase "no_proxy" preferred over "NO_PROXY"
        config.noProxyRaw = m_environment.variables().tryGet(KnownEnvars::CurlNoProxy);
        if (!config.noProxyRaw) {
            config.noProxyRaw = m_environment.variables().tryGet(KnownEnvars::CurlNoProxyUpper);
        }
        config.isDeprecatedSource = isLegacy;
        return config;
    };

    /*
     * Supported proxy configuration, in order of preference:
     *   1. GCM proxy Git configuration (deprecated): credential.httpsProxy, credential.httpProxy
     *   2. Standard Git configuration: http.proxy
     *   3. cURL environment variables: https_proxy, HTTPS_PROXY, http_proxy (libcurl ignores uppercase HTTP_PROXY),
     *      all_proxy, ALL_PROXY
     *   4. GCM proxy environment variable (deprecated): GCM_HTTP_PROXY
     * HTTPS remotes check the HTTPS variants first and fall back to the HTTP ones; HTTP remotes only check the
     * HTTP variants. cURL's "no_proxy" / "NO_PROXY" host list applies to all of them.
     */
    const bool isHttps = m_remoteUri && equalsIgnoreCase(m_remoteUri->scheme(), QStringLiteral("https"));

    QUrl proxyUri;

    // 1. GCM proxy Git configuration (deprecated)
    if ((isHttps && tryGetUriSetting(std::nullopt, GitCredCfg::SectionName, GitCredCfg::HttpsProxy, proxyUri)) ||
        tryGetUriSetting(std::nullopt, GitCredCfg::SectionName, GitCredCfg::HttpProxy, proxyUri)) {
        return createConfiguration(proxyUri, true);
        }

    // 2. Standard Git configuration
    if (tryGetUriSetting(std::nullopt, GitHttpCfg::SectionName, GitHttpCfg::Proxy, proxyUri)) {
        return createConfiguration(proxyUri);
    }

    // 3. cURL environment variables (lowercase variants preferred as these are quasi-standard)
    if ((isHttps && tryGetUriSetting(KnownEnvars::CurlHttpsProxy, std::nullopt, std::nullopt, proxyUri)) ||
        (isHttps && tryGetUriSetting(KnownEnvars::CurlHttpsProxyUpper, std::nullopt, std::nullopt, proxyUri)) ||
        tryGetUriSetting(KnownEnvars::CurlHttpProxy, std::nullopt, std::nullopt, proxyUri) ||
        tryGetUriSetting(KnownEnvars::CurlAllProxy, std::nullopt, std::nullopt, proxyUri) ||
        tryGetUriSetting(KnownEnvars::CurlAllProxyUpper, std::nullopt, std::nullopt, proxyUri)) {
        return createConfiguration(proxyUri);
    }

    // 4. GCM proxy environment variable (deprecated)
    if (tryGetUriSetting(KnownEnvars::GcmHttpProxy, std::nullopt, std::nullopt, proxyUri)) {
        return createConfiguration(proxyUri, true);
    }

    return std::nullopt;
}

OptString Settings::parentWindowId()
{
    return m_environment.variables().tryGet(KnownEnvars::GcmParentWindow);
}

QString Settings::credentialNamespace()
{
    QString ns;
    if (tryGetSetting(KnownEnvars::GcmCredNamespace, GitCredCfg::SectionName, GitCredCfg::CredNamespace, ns)) {
        return ns;
    }
    return Constants::DefaultCredentialNamespace;
}

OptString Settings::credentialBackingStore()
{
    QString credStore;
    if (tryGetSetting(KnownEnvars::GcmCredentialStore, GitCredCfg::SectionName, GitCredCfg::CredentialStore, credStore)) {
        return credStore;
    }
    return std::nullopt;
}

std::optional<bool> Settings::useMsAuthDefaultAccount()
{
    QString str;
    if (tryGetSetting(KnownEnvars::MsAuthUseDefaultAccount, GitCredCfg::SectionName, GitCredCfg::MsAuthUseDefaultAccount, str)) {
        return toBooleany(str);
    }
    return std::nullopt;
}

// =============================================================================================
// LinuxSettings
// =============================================================================================

LinuxSettings::LinuxSettings(IEnvironment &environment, IGit &git, ITrace &trace, IFileSystem &fs)
:   Settings(environment, git),
    m_trace(trace),
    m_fs(fs)
{
}

bool LinuxSettings::tryGetExternalDefault(const QString &section, const OptString &scope, const QString &property, QString &value)
{
    value.clear();

    if (!m_extConfigCache) {
        // Read the system-wide config files in /etc/git-credential-manager/config.d, concatenated in alphabetical
        // order to form a single configuration. Reading defaults is not critical: failures are only traced.
        try {
            const QString configDir = Constants::LinuxAppDefaultsDirectoryPath;
            if (m_fs.directoryExists(configDir)) {
                QStringList files = m_fs.enumerateFiles(configDir, QStringLiteral("*"));
                files.sort();
                QString combined;
                for (const QString &file : std::as_const(files)) {
                    combined += m_fs.readAllText(file) + QLatin1Char('\n');
                }

                StringDictionary normalized;
                for (const auto &[key, v] : LinuxConfigParser(m_trace).parse(combined)) {
                    normalized.set(normalizeGitConfigKey(key), v);
                }
                m_extConfigCache = std::move(normalized);
            }
        } catch (const std::exception &ex) {
            m_trace.writeLine(QStringLiteral("Failed to read default setting from app configuration directory."));
            m_trace.writeException(ex);
        }
    }

    if (!m_extConfigCache) {
        return false; // No external config found (or failed to read)
    }

    const QString name = isNullOrWhiteSpace(scope) ? QStringLiteral("%1.%2").arg(section, property)
                                                   : QStringLiteral("%1.%2.%3").arg(section, *scope, property);
    if (!m_extConfigCache->tryGetValue(normalizeGitConfigKey(name), value)) {
        return false;
    }

    m_trace.writeLine(QStringLiteral("Default setting found in app configuration directory: %1=%2").arg(name, value));
    return true;
}

// =============================================================================================
// LinuxConfigParser
// =============================================================================================

StringDictionary LinuxConfigParser::parse(const QString &content)
{
    static const QRegularExpression lineRegex(QStringLiteral("^\\s*(?<key>[a-zA-Z0-9\\.-]+)\\s*=\\s*(?<value>.+?)\\s*(?:#.*)?$"));

    // Keys compare with Git configuration key semantics; the first spelling of a key is kept.
    StringDictionary result;
    QHash<QString, QString> spelling;

    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // Ignore empty lines or full-line comments
        const QString trimmedLine = line.trimmed();
        if (trimmedLine.isEmpty() || trimmedLine.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const QRegularExpressionMatch match = lineRegex.match(trimmedLine);
        if (!match.hasMatch()) {
            m_trace.writeLine(QStringLiteral("Invalid config line format: %1").arg(line));
            continue;
        }

        const QString key = match.captured(QStringLiteral("key"));
        QString value = match.captured(QStringLiteral("value"));

        // Remove enclosing quotes from the value, if any
        if (value.size() >= 2 &&
            ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
             (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
            value = value.mid(1, value.size() - 2);
        }

        const QString norm = normalizeGitConfigKey(key);
        const QString storedKey = spelling.value(norm, key);
        spelling.insert(norm, storedKey);
        result.set(storedKey, value);
    }
    return result;
}

} // namespace gcm
