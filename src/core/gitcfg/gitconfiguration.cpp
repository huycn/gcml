// Port of GitConfiguration.cs (GitProcessConfiguration, ConfigCache, GitConfigurationExtensions)
#include "core/git.h"

#include "core/strings.h"
#include "core/trace.h"
#include "core/trace2.h"

#include <QHash>

namespace gcm {

namespace {

QString levelName(GitConfigurationLevel level)
{
    switch (level) {
        case GitConfigurationLevel::All: return QStringLiteral("all");
        case GitConfigurationLevel::System: return QStringLiteral("system");
        case GitConfigurationLevel::Global: return QStringLiteral("global");
        case GitConfigurationLevel::Local: return QStringLiteral("local");
        case GitConfigurationLevel::Unknown: break;
    }
    return QStringLiteral("unknown");
}

QString typeName(GitConfigurationType type)
{
    switch (type) {
        case GitConfigurationType::Raw: return QStringLiteral("raw");
        case GitConfigurationType::Bool: return QStringLiteral("bool");
        case GitConfigurationType::Path: return QStringLiteral("path");
    }
    return QString();
}

/// GitConfigurationKeyComparer.TrySplit
bool trySplitKey(const QString &key, QString &section, OptString &scope, QString &property)
{
    section.clear();
    scope.reset();
    property.clear();
    if (isNullOrWhiteSpace(key)) {
        return false;
    }
    section = truncateFromIndexOf(key, QLatin1Char('.'));
    property = trimUntilLastIndexOf(key, QLatin1Char('.'));
    const qsizetype scopeLength = key.size() - (section.size() + property.size() + 2);
    if (scopeLength > 0) {
        scope = key.mid(section.size() + 1, scopeLength);
    }
    return true;
}

// --- config cache ('git config list --show-scope -z') ---------------------------------------

struct ConfigCacheEntry
{
    QString value;
    GitConfigurationLevel level;
};

GitConfigurationLevel parseScope(const QString &scope)
{
    if (scope == QLatin1String("system")) {
        return GitConfigurationLevel::System;
    }
    if (scope == QLatin1String("global")) {
        return GitConfigurationLevel::Global;
    }
    if (scope == QLatin1String("local") || scope == QLatin1String("worktree") || scope == QLatin1String("command")) {
        return GitConfigurationLevel::Local;
    }
    return GitConfigurationLevel::Unknown;
}

bool matchesLevel(GitConfigurationLevel filter, GitConfigurationLevel level)
{
    return filter == GitConfigurationLevel::All || filter == level;
}

class ConfigCache
{
public:
    bool isLoaded() const { return m_entries.has_value(); }

    void load(const QString &data, ITrace &trace)
    {
        // Keyed by normalized key; each entry remembers the key as Git printed it first.
        Dictionary<std::pair<QString, QList<ConfigCacheEntry>>> entries;
        qsizetype i = 0;
        const qsizetype n = data.size();
        while (i < n) {
            QString scope, key, value;

            // Read scope (NUL terminated)
            while (i < n && data[i] != QChar(0)) {
                scope += data[i++];
            }
            if (i >= n) {
                trace.writeLine(QStringLiteral("Invalid Git configuration output. Expected null terminator (\\0) after scope."));
                break;
            }
            ++i;

            // Read key (newline terminated)
            while (i < n && data[i] != QLatin1Char('\n')) {
                key += data[i++];
            }
            if (i >= n) {
                trace.writeLine(QStringLiteral("Invalid Git configuration output. Expected newline terminator (\\n) after key."));
                break;
            }
            ++i;

            // Read value (NUL terminated)
            while (i < n && data[i] != QChar(0)) {
                value += data[i++];
            }
            if (i >= n) {
                trace.writeLine(QStringLiteral("Invalid Git configuration output. Expected null terminator (\\0) after value."));
                break;
            }
            ++i;

            const QString norm = normalizeGitConfigKey(key);
            const bool existed = entries.contains(norm);
            auto &slot = entries[norm];
            if (!existed) {
                slot.first = key;
            }
            slot.second.append({value, parseScope(scope)});
        }
        m_entries = std::move(entries);
    }

    bool tryGet(const QString &name, GitConfigurationLevel level, QString &value) const
    {
        value.clear();
        if (!m_entries) {
            return false;
        }
        const auto *slot = m_entries->find(normalizeGitConfigKey(name));
        if (!slot) {
            return false;
        }
        // Last entry matching the level filter wins (system < global < local)
        const ConfigCacheEntry *lastMatch = nullptr;
        for (const ConfigCacheEntry &entry : slot->second) {
            if (matchesLevel(level, entry.level)) {
                lastMatch = &entry;
            }
        }
        if (!lastMatch) {
            return false;
        }
        value = lastMatch->value;
        return true;
    }

    QStringList getAll(const QString &name, GitConfigurationLevel level) const
    {
        QStringList results;
        if (!m_entries) {
            return results;
        }
        if (const auto *slot = m_entries->find(normalizeGitConfigKey(name))) {
            for (const ConfigCacheEntry &entry : slot->second) {
                if (matchesLevel(level, entry.level)) {
                    results.append(entry.value);
                }
            }
        }
        return results;
    }

    void enumerate(GitConfigurationLevel level, const GitConfigurationEnumerationCallback &cb) const
    {
        if (!m_entries) {
            return;
        }
        for (const auto &[norm, slot] : *m_entries) {
            for (const ConfigCacheEntry &entry : slot.second) {
                if (matchesLevel(level, entry.level) && !cb(GitConfigurationEntry{slot.first, entry.value})) {
                    return;
                }
            }
        }
    }

    void clear() { m_entries.reset(); }

private:
    std::optional<Dictionary<std::pair<QString, QList<ConfigCacheEntry>>>> m_entries;
};

// NOLINTBEGIN(non-pod-global-static)
const GitVersion TypeConfigMinVersion { 2, 18, 0 };
const GitVersion ConfigListTypeMinVersion { 2, 54, 0 };
const GitVersion ConfigListTypeMinVfsBase { 2, 53, 0 };
const GitVersion ConfigListTypeMinVfsSuffix { 0, 1 };
const GitVersion ConfigListRawMinVersion { 2, 26, 0 };
const GitVersion ConfigListSubcommandMinVersion { 2, 46, 0 };
// NOLINTEND(non-pod-global-static)

bool supportsConfigListType(GitProcess &git)
{
    const GitVersion version = git.version();
    if (version >= ConfigListTypeMinVersion) {
        return true;
    }

    // The microsoft/git fork fast-tracked the fix into 2.53.0.vfs.0.1. "2.53.0.vfs.0.1" parses as [2,53,0]
    // because parsing stops at "vfs", so look for the ".vfs." marker and parse the suffix.
    const QString versionStr = version.originalString();
    const qsizetype vfsIdx = versionStr.indexOf(QLatin1String(".vfs."));
    if (vfsIdx >= 0) {
        const GitVersion baseVersion(versionStr.left(vfsIdx));
        const GitVersion vfsSuffix(versionStr.mid(vfsIdx + 5));
        return baseVersion >= ConfigListTypeMinVfsBase && vfsSuffix >= ConfigListTypeMinVfsSuffix;
    }
    return false;
}

void ensureSpecificLevel(GitConfigurationLevel level)
{
    if (level == GitConfigurationLevel::All) {
        throw InvalidOperationException(QStringLiteral("Must have a specific configuration level filter to modify values."));
    }
}

QString getLevelFilterArg(GitConfigurationLevel level)
{
    switch (level) {
        case GitConfigurationLevel::System: return QStringLiteral("--system");
        case GitConfigurationLevel::Global: return QStringLiteral("--global");
        case GitConfigurationLevel::Local: return QStringLiteral("--local");
        default: return QString();
    }
}

} // namespace

// =============================================================================================
// GitProcessConfiguration
// =============================================================================================

struct GitProcessConfiguration::Private
{
    Private(ITrace &t, GitProcess &g) : trace(t), git(g) {}
    ITrace &trace;
    GitProcess &git;
    bool useConfigTypeCache = false;
    bool useConfigRawCache = false;
    QHash<int, ConfigCache> cache;

    bool useConfigCache(GitConfigurationType type) const
    {
        return type != GitConfigurationType::Raw ? useConfigTypeCache : useConfigRawCache;
    }

    QString getCanonicalizeTypeArg(GitConfigurationType type)
    {
        const bool modern = git.version() >= TypeConfigMinVersion;
        switch (type) {
            case GitConfigurationType::Bool: return modern ? QStringLiteral("--type=bool") : QStringLiteral("--bool");
            case GitConfigurationType::Path: return modern ? QStringLiteral("--type=path") : QStringLiteral("--path");
            default: return QString();
        }
    }

    /// Returns the loaded cache for `type`, or nullptr when the cache is not used or failed to load.
    ConfigCache *ensureCacheLoaded(GitConfigurationType type)
    {
        if (!useConfigCache(type)) {
            return nullptr;
        }
        ConfigCache &c = cache[int(type)];
        if (c.isLoaded()) {
            return &c;
        }

        Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("load_cache"));
        Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("type"), typeName(type));

        QString typeArg;
        switch (type) {
            case GitConfigurationType::Raw: break;
            case GitConfigurationType::Path: typeArg = QStringLiteral("--type=path"); break;
            case GitConfigurationType::Bool: typeArg = QStringLiteral("--type=bool"); break;
        }

        const QString argList =
            git.version() >= ConfigListSubcommandMinVersion ? QStringLiteral("list") : QStringLiteral("--list");
        auto proc = git.createProcess(QStringLiteral("config %1 --show-scope -z %2").arg(argList, typeArg));
        proc->start();
        const QString data = QString::fromUtf8(proc->readAllStandardOutput());
        proc->waitForExit();

        if (proc->exitCode() == 0) {
            c.load(data, trace);
            return &c;
        }
        trace.writeLine(QStringLiteral("Failed to load config cache (exit=%1)").arg(proc->exitCode()));
        // Don't throw - fall back to individual commands
        return nullptr;
    }

    void invalidateCache()
    {
        for (ConfigCache &c : cache) {
            c.clear();
        }
    }

    /// Runs a modifying `git config` command; throws GitException unless the exit code is accepted.
    void runWrite(const QString &args, std::initializer_list<int> okCodes,
                  const std::function<QString(int exitCode)> &traceMessage, const QString &errorMessage)
    {
        auto proc = git.createProcess(args);
        proc->start();
        proc->waitForExit();
        const int exitCode = proc->exitCode();
        if (std::find(okCodes.begin(), okCodes.end(), exitCode) != okCodes.end()) {
            invalidateCache();
            return;
        }
        trace.writeLine(traceMessage(exitCode));
        throw GitProcess::createGitException(*proc, errorMessage);
    }
};

GitProcessConfiguration::GitProcessConfiguration(ITrace &trace, GitProcess &git, bool useCache)
    : d(std::make_unique<Private>(trace, git))
{
    // 'git config list --type=<X>' requires Git 2.54.0+, or microsoft/git fork 2.53.0.vfs.0.1+
    if (useCache && !supportsConfigListType(git)) {
        trace.writeLine(QStringLiteral("Git version %1 does not support 'git config --list --type'; config cache disabled").arg(git.version().originalString()));
        d->useConfigTypeCache = false;
    } else {
        d->useConfigTypeCache = useCache;
    }

    if (useCache && !(git.version() >= ConfigListRawMinVersion)) {
        trace.writeLine(QStringLiteral("Git version %1 does not support 'git config list --type'; config cache disabled").arg(git.version().originalString()));
        d->useConfigRawCache = false;
    } else {
        d->useConfigRawCache = useCache;
    }
}

GitProcessConfiguration::~GitProcessConfiguration() = default;

void GitProcessConfiguration::enumerate(GitConfigurationLevel level, const GitConfigurationEnumerationCallback &cb)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("enumerate"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));

    if (d->useConfigRawCache) {
        if (ConfigCache *cache = d->ensureCacheLoaded(GitConfigurationType::Raw)) {
            cache->enumerate(level, cb);
            return;
        }
    }

    // Fall back to the uncached implementation
    auto git = d->git.createProcess(QStringLiteral("config --null %1 --includes --list").arg(getLevelFilterArg(level)));
    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    git->waitForExit();
    if (git->exitCode() != 0) {
        d->trace.writeLine(QStringLiteral("Failed to enumerate config entries (exit=%1, level=%2)").arg(QString::number(git->exitCode()), levelName(level)));
        throw GitProcess::createGitException(*git, QStringLiteral("Failed to enumerate all Git configuration entries"));
    }

    qsizetype i = 0;
    const qsizetype n = data.size();
    while (i < n) {
        QString name, value;
        // Read key name (LF terminated)
        while (i < n && data[i] != QLatin1Char('\n')) {
            name += data[i++];
        }
        if (i >= n) {
            d->trace.writeLine(QStringLiteral("Invalid Git configuration output. Expected newline terminator (\\n) after key."));
            break;
        }
        ++i;
        // Read value (null terminated)
        while (i < n && data[i] != QChar(0)) {
            value += data[i++];
        }
        if (i >= n) {
            d->trace.writeLine(QStringLiteral("Invalid Git configuration output. Expected null terminator (\\0) after value."));
            break;
        }
        ++i;
        if (!cb(GitConfigurationEntry{name, value})) {
            break;
        }
    }
}

bool GitProcessConfiguration::tryGet(GitConfigurationLevel level, GitConfigurationType type, const QString &name,
                                     QString &value)
{
    // A loaded cache answers the query whether the entry is found or not.
    if (ConfigCache *cache = d->ensureCacheLoaded(type)) {
        return cache->tryGet(name, level, value);
    }

    // Fall back to an individual git config command
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("get"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("type"), typeName(type));

    auto git = d->git.createProcess(QStringLiteral("config --null %1 --includes %2 %3").arg(getLevelFilterArg(level), d->getCanonicalizeTypeArg(type), quoteCmdArg(name)));
    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    git->waitForExit();

    value.clear();
    switch (git->exitCode()) {
        case 0: // OK
            break;
        case 1: // Not found
            return false;
        default: // Error
            d->trace.writeLine(QStringLiteral("Failed to read Git configuration entry '%1'. (exit=%2, level=%3)").arg(name, QString::number(git->exitCode()), levelName(level)));
            return false;
    }

    value = data.section(QChar(0), 0, 0);
    return true;
}

void GitProcessConfiguration::set(GitConfigurationLevel level, const QString &name, const QString &value)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("set"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    ensureSpecificLevel(level);
    d->runWrite(
        QStringLiteral("config %1 %2 %3").arg(getLevelFilterArg(level), quoteCmdArg(name), quoteCmdArg(value)),
        {0},
        [&](int e) { return QStringLiteral("Failed to set config entry '%1' to value '%2' (exit=%3, level=%4)").arg(name, value, QString::number(e), levelName(level)); },
        QStringLiteral("Failed to set Git configuration entry '%1'").arg(name)
    );
}

void GitProcessConfiguration::add(GitConfigurationLevel level, const QString &name, const QString &value)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("add"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    ensureSpecificLevel(level);
    d->runWrite(
        QStringLiteral("config %1 --add %2 %3").arg(getLevelFilterArg(level), quoteCmdArg(name), quoteCmdArg(value)),
        {0},
        [&](int e) { return QStringLiteral("Failed to add config entry '%1' with value '%2' (exit=%3, level=%4)").arg(name, value, QString::number(e), levelName(level)); },
        QStringLiteral("Failed to add Git configuration entry '%1'").arg(name)
    );
}

void GitProcessConfiguration::unset(GitConfigurationLevel level, const QString &name)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("unset"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    ensureSpecificLevel(level);
    // 5: trying to unset a value that does not exist
    d->runWrite(
        QStringLiteral("config %1 --unset %2").arg(getLevelFilterArg(level), quoteCmdArg(name)),
        {0, 5},
        [&](int e) { return QStringLiteral("Failed to unset config entry '%1' (exit=%2, level=%3)").arg(name, QString::number(e), levelName(level)); },
        QStringLiteral("Failed to unset Git configuration entry '%1'").arg(name)
    );
}

QStringList GitProcessConfiguration::getAll(GitConfigurationLevel level, GitConfigurationType type, const QString &name)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("get_all"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("type"), typeName(type));

    if (ConfigCache *cache = d->ensureCacheLoaded(type)) {
        return cache->getAll(name, level);
    }

    // Fall back to an individual git config command
    auto git = d->git.createProcess(QStringLiteral("config --null %1 --includes %2 --get-all %3")
                                        .arg(getLevelFilterArg(level), d->getCanonicalizeTypeArg(type), quoteCmdArg(name)));
    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    git->waitForExit();

    switch (git->exitCode()) {
    case 0: { // OK
        QStringList entries = data.split(QChar(0));
        // Each value ends with \0, which leaves one bogus blank entry at the end
        entries.removeLast();
        return entries;
    }
    case 1: // No results
        return {};
    default:
        d->trace.writeLine(QStringLiteral("Failed to get all config entries '%1' (exit=%2, level=%3)")
                               .arg(name, QString::number(git->exitCode()), levelName(level)));
        throw GitProcess::createGitException(*git, QStringLiteral("Failed to get all Git configuration entries '%1'").arg(name));
    }
}

QStringList GitProcessConfiguration::getRegex(GitConfigurationLevel level, GitConfigurationType type,
                                              const QString &nameRegex, const OptString &valueRegex)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("get_regex"));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("scope"), levelName(level));
    Trace2::writeData(QStringLiteral("git_config"), QStringLiteral("type"), typeName(type));

    QString gitArgs = QStringLiteral("config --null %1 --includes %2 --get-regex %3")
                          .arg(getLevelFilterArg(level), d->getCanonicalizeTypeArg(type), quoteCmdArg(nameRegex));
    if (valueRegex) {
        gitArgs += QLatin1Char(' ') + quoteCmdArg(*valueRegex);
    }

    auto git = d->git.createProcess(gitArgs);
    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    git->waitForExit();

    switch (git->exitCode()) {
    case 0: // OK
    case 1: // No results
        break;
    default:
        d->trace.writeLine(QStringLiteral("Failed to get all multivar regex '%1' and value regex '%2' (exit=%3, level=%4)")
                               .arg(nameRegex, valueRegex.value_or(QString()), QString::number(git->exitCode()), levelName(level)));
        throw GitProcess::createGitException(
            *git, QStringLiteral("Failed to get Git configuration multi-valued entries with name regex '%1'").arg(nameRegex));
    }

    QStringList result;
    const QStringList entries = data.split(QChar(0));
    for (const QString &entry : entries) {
        const qsizetype nl = entry.indexOf(QLatin1Char('\n'));
        if (nl >= 0) {
            result.append(entry.mid(nl + 1));
        }
    }
    return result;
}

void GitProcessConfiguration::replaceAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex,
                                         const QString &value)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("replace_all"));
    ensureSpecificLevel(level);
    QString gitArgs = QStringLiteral("config %1 --replace-all %2 %3")
                          .arg(getLevelFilterArg(level), quoteCmdArg(name), quoteCmdArg(value));
    if (valueRegex) {
        gitArgs += QLatin1Char(' ') + quoteCmdArg(*valueRegex);
    }
    d->runWrite(gitArgs, {0},
                [&](int e) { return QStringLiteral("Failed to replace all multivar '%1' and value regex '%2' with new value '%3' (exit=%4, level=%5)").arg(name, valueRegex.value_or(QString()), value, QString::number(e), levelName(level)); },
                QStringLiteral("Failed to replace all Git configuration multi-valued entries '%1'").arg(name));
}

void GitProcessConfiguration::unsetAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_config"), QStringLiteral("unset_all"));
    ensureSpecificLevel(level);
    QString gitArgs = QStringLiteral("config %1 --unset-all %2").arg(getLevelFilterArg(level), quoteCmdArg(name));
    if (valueRegex) {
        gitArgs += QLatin1Char(' ') + quoteCmdArg(*valueRegex);
    }
    d->runWrite(gitArgs, {0, 5},
                [&](int e) { return QStringLiteral("Failed to unset all multivar '%1' with value regex '%2' (exit=%3, level=%4)").arg(name, valueRegex.value_or(QString()), QString::number(e), levelName(level)); },
                QStringLiteral("Failed to unset all Git configuration multi-valued entries '%1'").arg(name));
}

QString GitProcessConfiguration::quoteCmdArg(const QString &str)
{
    bool needsQuotes = str.isEmpty();
    QString result;
    const qsizetype n = str.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = str[i];
        switch (c.unicode()) {
        case '"':
            result += QLatin1String("\\\"");
            needsQuotes = true;
            break;
        case ' ': case '{': case '*': case '?': case '\r': case '\n': case '\t': case '\'':
            result += c;
            needsQuotes = true;
            break;
        case '\\': {
            qsizetype end = i;
            // Copy all the '\'s in this run.
            while (end < n && str[end] == QLatin1Char('\\')) {
                result += QLatin1Char('\\');
                ++end;
            }
            // A run ending in '"' needs its '\'s doubled (the '"' is escaped on the next pass). At the end of a
            // string that will be quoted, double them too so the closing quote is not escaped.
            if ((end < n && str[end] == QLatin1Char('"')) || (end == n && needsQuotes)) {
                result += QString(end - i, QLatin1Char('\\'));
            }
            // Back-off one character
            if (end > i) {
                --end;
            }
            i = end;
            break;
        }
        default:
            result += c;
            break;
        }
    }
    if (needsQuotes) {
        result = QLatin1Char('"') + result + QLatin1Char('"');
    }
    return result;
}

// =============================================================================================
// GitConfigurationExtensions
// =============================================================================================

void IGitConfiguration::enumerate(GitConfigurationLevel level, const QString &section, const QString &property,
                                  const GitConfigurationEnumerationCallback &cb)
{
    enumerate(level, [&](const GitConfigurationEntry &entry) {
        QString entrySection, entryProperty;
        OptString entryScope;
        if (trySplitKey(entry.key, entrySection, entryScope, entryProperty) &&
            (section.isNull() || equalsIgnoreCase(section, entrySection)) &&
            (property.isNull() || equalsIgnoreCase(property, entryProperty))) {
            return cb(entry);
        }
        return true;
    });
}

QString IGitConfiguration::get(GitConfigurationLevel level, const QString &name)
{
    QString value;
    if (!tryGet(level, GitConfigurationType::Raw, name, value)) {
        throw KeyNotFoundException(QStringLiteral("Git configuration entry with the name '%1' was not found.").arg(name));
    }
    return value;
}

} // namespace gcm
