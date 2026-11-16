// Git process + configuration access. Ports Git.cs, GitVersion.cs, GitConfiguration.cs,
// GitConfigurationEntry.cs, IniFile.cs, ConfigurationService.cs.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"
#include "core/platform.h"

namespace gcm {

class ITrace;
class ICommandContext;

class GitException : public Exception
{
public:
    GitException(QString message, QString gitErrorMessage, int exitCode)
        : Exception(std::move(message)), m_gitErrorMessage(std::move(gitErrorMessage)), m_exitCode(exitCode) {}

    const QString &gitErrorMessage() const { return m_gitErrorMessage; }
    int exitCode() const { return m_exitCode; }
    const char *typeName() const noexcept override { return "GitException"; }

private:
    QString m_gitErrorMessage;
    int m_exitCode;
};

class GitVersion
{
public:
    GitVersion() = default;
    /// Parses e.g. "2.45.1.windows.1" / "git version 2.45.1" style strings; non-numeric components end parsing.
    explicit GitVersion(const QString &versionString);
    GitVersion(std::initializer_list<int> components);

    QString toString() const;
    /// The string parsed, or toString() for a version built from components / a null string.
    QString originalString() const { return m_original.value_or(toString()); }

    static int compare(const GitVersion &left, const GitVersion &right);
    friend bool operator==(const GitVersion &l, const GitVersion &r) { return compare(l, r) == 0; }
    friend bool operator!=(const GitVersion &l, const GitVersion &r) { return compare(l, r) != 0; }
    friend bool operator<(const GitVersion &l, const GitVersion &r) { return compare(l, r) < 0; }
    friend bool operator>(const GitVersion &l, const GitVersion &r) { return compare(l, r) > 0; }
    friend bool operator<=(const GitVersion &l, const GitVersion &r) { return compare(l, r) <= 0; }
    friend bool operator>=(const GitVersion &l, const GitVersion &r) { return compare(l, r) >= 0; }

private:
    OptString m_original;
    QList<int> m_components;
};

struct GitRemote
{
    QString name;
    OptString fetchUrl;
    OptString pushUrl;
};

enum class GitConfigurationLevel { All, System, Global, Local, Unknown };
enum class GitConfigurationType { Raw, Bool, Path };

struct GitConfigurationEntry
{
    QString key;
    QString value;
};
/// Return false to stop enumerating.
using GitConfigurationEnumerationCallback = std::function<bool(const GitConfigurationEntry &)>;

class IGitConfiguration
{
public:
    virtual ~IGitConfiguration() = default;

    virtual void enumerate(GitConfigurationLevel level, const GitConfigurationEnumerationCallback &cb) = 0;
    virtual bool tryGet(GitConfigurationLevel level, GitConfigurationType type, const QString &name, QString &value) = 0;
    virtual void set(GitConfigurationLevel level, const QString &name, const QString &value) = 0;
    virtual void add(GitConfigurationLevel level, const QString &name, const QString &value) = 0;
    virtual void unset(GitConfigurationLevel level, const QString &name) = 0;
    virtual QStringList getAll(GitConfigurationLevel level, GitConfigurationType type, const QString &name) = 0;
    virtual QStringList getRegex(GitConfigurationLevel level, GitConfigurationType type, const QString &nameRegex, const OptString &valueRegex) = 0;
    virtual void replaceAll(GitConfigurationLevel level, const QString &nameRegex, const OptString &valueRegex, const QString &value) = 0;
    virtual void unsetAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex) = 0;

    // Extension helpers (GitConfigurationExtensions)
    void enumerate(const GitConfigurationEnumerationCallback &cb) {
        enumerate(GitConfigurationLevel::All, cb);
    }
    /// Enumerate entries of `section.[scope.]property` at the given level.
    void enumerate(GitConfigurationLevel level, const QString &section, const QString &property, const GitConfigurationEnumerationCallback &cb);
    void enumerate(const QString &section, const QString &property, const GitConfigurationEnumerationCallback &cb) { enumerate(GitConfigurationLevel::All, section, property, cb); }

    /// Throws KeyNotFoundException when the value is missing.
    QString get(GitConfigurationLevel level, const QString &name);
    QString get(const QString &name) { return get(GitConfigurationLevel::All, name); }
    bool tryGet(const QString &name, bool isPath, QString &value) { return tryGet(GitConfigurationLevel::All, isPath ? GitConfigurationType::Path : GitConfigurationType::Raw, name, value); }

    QStringList getAll(const QString &name) { return getAll(GitConfigurationLevel::All, GitConfigurationType::Raw, name); }
    QStringList getRegex(const QString &nameRegex, const OptString &valueRegex) { return getRegex(GitConfigurationLevel::All, GitConfigurationType::Raw, nameRegex, valueRegex); }
};

class IGit
{
public:
    virtual ~IGit() = default;

    virtual GitVersion version() = 0;
    virtual std::unique_ptr<ChildProcess> createProcess(const QString &args) = 0;
    virtual bool isInsideRepository() = 0;
    /// Absolute path of the current repository's git directory, or nullopt when not in one.
    virtual OptString getCurrentRepository() = 0;
    virtual QList<GitRemote> getRemotes() = 0;
    virtual IGitConfiguration &getConfiguration() = 0;
    /// Runs `git <args>` as a credential-helper style child: writes `standardInput` as a dictionary to its stdin,
    /// reads a dictionary from its stdout. Throws on non-zero exit (message from the "error" key or "Unknown").
    virtual StringDictionary invokeHelper(const QString &args, const StringDictionary *standardInput = nullptr) = 0;
};

class GitProcess : public IGit
{
public:
    GitProcess(ITrace &trace, IProcessManager &processManager, QString gitPath, QString workingDirectory = QString());
    ~GitProcess() override;

    GitVersion version() override;
    std::unique_ptr<ChildProcess> createProcess(const QString &args) override;
    bool isInsideRepository() override;
    OptString getCurrentRepository() override;
    QList<GitRemote> getRemotes() override;
    IGitConfiguration &getConfiguration() override;
    StringDictionary invokeHelper(const QString &args, const StringDictionary *standardInput = nullptr) override;

    const QString &gitPath() const { return m_gitPath; }
    /// Builds a GitException from a finished git child (reads its stderr).
    static GitException createGitException(ChildProcess &git, const QString &message);

private:
    ITrace &m_trace;
    IProcessManager &m_processManager;
    QString m_gitPath;
    QString m_workingDirectory;
    std::optional<GitVersion> m_version;
    /// A fresh configuration object per getConfiguration() call (as upstream); kept alive for the references handed out.
    QList<std::shared_ptr<IGitConfiguration>> m_configs;
};

/// IGitConfiguration backed by `git config` child processes (with a raw-value cache for reads).
class GitProcessConfiguration : public IGitConfiguration
{
public:
    GitProcessConfiguration(ITrace &trace, GitProcess &git, bool useCache = true);
    ~GitProcessConfiguration() override;

    using IGitConfiguration::enumerate;
    using IGitConfiguration::getAll;
    using IGitConfiguration::getRegex;
    using IGitConfiguration::tryGet;
    void enumerate(GitConfigurationLevel level, const GitConfigurationEnumerationCallback &cb) override;
    bool tryGet(GitConfigurationLevel level, GitConfigurationType type, const QString &name, QString &value) override;
    void set(GitConfigurationLevel level, const QString &name, const QString &value) override;
    void add(GitConfigurationLevel level, const QString &name, const QString &value) override;
    void unset(GitConfigurationLevel level, const QString &name) override;
    QStringList getAll(GitConfigurationLevel level, GitConfigurationType type, const QString &name) override;
    QStringList getRegex(GitConfigurationLevel level, GitConfigurationType type, const QString &nameRegex, const OptString &valueRegex) override;
    void replaceAll(GitConfigurationLevel level, const QString &nameRegex, const OptString &valueRegex, const QString &value) override;
    void unsetAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex) override;

    /// Quote an argument for the `git config` command line (GitProcessConfiguration.QuoteCmdArg).
    static QString quoteCmdArg(const QString &str);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// --- INI files (used for /etc/git-credential-manager/config.d and similar) -------------------------
struct IniProperty
{
    QString name;
    QString value;
};
struct IniSectionName
{
    QString name;      // case-insensitive
    OptString subName; // case-sensitive
    bool operator==(const IniSectionName &o) const;
};
struct IniSection
{
    IniSectionName name;
    QList<IniProperty> properties;
    /// Last value wins. Property names are case-insensitive.
    bool tryGetProperty(const QString &name, QString &value) const;
    bool tryGetMultiProperty(const QString &name, QStringList &values) const;
};
class IniFile
{
public:
    QList<IniSection> sections;
    const IniSection *tryGetSection(const QString &name, const OptString &subName = std::nullopt) const;
    /// Finds or appends.
    IniSection &section(const IniSectionName &name);
};
namespace IniSerializer {
IniFile deserialize(const IFileSystem &fs, const QString &path);
} // namespace IniSerializer

// --- `configure` / `unconfigure` -----------------------------------------------------------------
enum class ConfigurationTarget { User, System };

class IConfigurableComponent
{
public:
    virtual ~IConfigurableComponent() = default;
    virtual QString componentName() const = 0;
    virtual void configure(ConfigurationTarget target) = 0;
    virtual void unconfigure(ConfigurationTarget target) = 0;
};

class IConfigurationService
{
public:
    virtual ~IConfigurationService() = default;
    /// Components are not owned.
    virtual void addComponent(IConfigurableComponent *component) = 0;
    virtual void configure(ConfigurationTarget target) = 0;
    virtual void unconfigure(ConfigurationTarget target) = 0;
};

class ConfigurationService : public IConfigurationService
{
public:
    explicit ConfigurationService(ICommandContext &context);
    void addComponent(IConfigurableComponent *component) override;
    void configure(ConfigurationTarget target) override;
    void unconfigure(ConfigurationTarget target) override;

private:
    ICommandContext &m_context;
    QList<IConfigurableComponent *> m_components;
};

} // namespace gcm
