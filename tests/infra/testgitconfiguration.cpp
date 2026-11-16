#include "infra/testgitconfiguration.h"

#include "core/strings.h"

#include <QRegularExpression>

namespace gcm::test {

namespace {

bool isMatch(const QString &input, const OptString &pattern)
{
    // A missing value pattern matches every value.
    return !pattern || QRegularExpression(*pattern).match(input).hasMatch();
}

Exception multivarException() { return Exception(QStringLiteral("Configuration entry is a multivar")); }

} // namespace

// --- TestConfigLevel ------------------------------------------------------------------------

QString TestConfigLevel::normalizeKey(const QString &key) { return normalizeGitConfigKey(key); }

QStringList &TestConfigLevel::operator[](const QString &key)
{
    const QString norm = normalizeKey(key);
    const bool existed = m_entries.contains(norm);
    Entry &entry = m_entries[norm];
    if (!existed) {
        entry.first = key;
    }
    return entry.second;
}

bool TestConfigLevel::tryGetValue(const QString &key, QStringList &values) const
{
    if (const Entry *entry = m_entries.find(normalizeKey(key))) {
        values = entry->second;
        return true;
    }
    return false;
}

QStringList TestConfigLevel::value(const QString &key) const
{
    const Entry *entry = m_entries.find(normalizeKey(key));
    return entry ? entry->second : QStringList();
}

QStringList TestConfigLevel::keys() const
{
    QStringList result;
    for (const auto &[norm, entry] : m_entries) {
        result.append(entry.first);
    }
    return result;
}

QList<TestConfigLevel::Entry> TestConfigLevel::entries() const
{
    QList<Entry> result;
    for (const auto &[norm, entry] : m_entries) {
        result.append(entry);
    }
    return result;
}

// --- TestGitConfiguration -------------------------------------------------------------------

TestConfigLevel &TestGitConfiguration::getDictionary(GitConfigurationLevel level)
{
    switch (level) {
    case GitConfigurationLevel::System:
        return System;
    case GitConfigurationLevel::Global:
        return Global;
    case GitConfigurationLevel::Local:
        return Local;
    case GitConfigurationLevel::All:
        throw ArgumentException(QStringLiteral("Must specify a specific level"));
    default:
        throw ArgumentOutOfRangeException(QStringLiteral("Unsupported level"));
    }
}

QList<TestConfigLevel *> TestGitConfiguration::getDictionaries(GitConfigurationLevel level)
{
    switch (level) {
    case GitConfigurationLevel::System:
        return {&System};
    case GitConfigurationLevel::Global:
        return {&Global};
    case GitConfigurationLevel::Local:
        return {&Local};
    case GitConfigurationLevel::All:
        // From least to most specific
        return {&System, &Global, &Local};
    default:
        throw ArgumentOutOfRangeException(QStringLiteral("Unsupported level"));
    }
}

void TestGitConfiguration::enumerate(GitConfigurationLevel level, const GitConfigurationEnumerationCallback &cb)
{
    for (const QList<TestConfigLevel *> dicts = getDictionaries(level); TestConfigLevel *dict : dicts) {
        for (const auto entries = dict->entries(); const auto &[key, values] : entries) {
            for (const QString &value : values) {
                if (!cb(GitConfigurationEntry{key, value})) {
                    return;
                }
            }
        }
    }
}

bool TestGitConfiguration::tryGet(GitConfigurationLevel level, GitConfigurationType type, const QString &name,
                                  QString &value)
{
    OptString result;
    // Least to most specific level; the last value found wins.
    for (const QList<TestConfigLevel *> dicts = getDictionaries(level); TestConfigLevel *dict : dicts) {
        QStringList values;
        if (!dict->tryGetValue(name, values)) {
            continue;
        }
        if (values.size() > 1) {
            throw multivarException();
        }
        if (values.size() == 1) {
            result = values.first();
            if (type == GitConfigurationType::Path) {
                result->replace(QLatin1Char('~'), CanonicalPathPrefix); // a "fake" canonical path
            }
        }
    }
    value = result.value_or(QString());
    return result.has_value();
}

void TestGitConfiguration::set(GitConfigurationLevel level, const QString &name, const QString &value)
{
    ++SetCallCount;
    QStringList &values = getDictionary(level)[name];
    if (values.size() > 1) {
        throw multivarException();
    }
    if (values.size() == 1) {
        values[0] = value;
    } else {
        values.append(value);
    }
}

void TestGitConfiguration::add(GitConfigurationLevel level, const QString &name, const QString &value)
{
    getDictionary(level)[name].append(value);
}

void TestGitConfiguration::unset(GitConfigurationLevel level, const QString &name)
{
    ++UnsetCallCount;
    TestConfigLevel &dict = getDictionary(level);
    if (dict.value(name).size() > 1) {
        throw multivarException();
    }
    dict.remove(name);
}

QStringList TestGitConfiguration::getAll(GitConfigurationLevel level, GitConfigurationType, const QString &name)
{
    QStringList result;
    for (const QList<TestConfigLevel *> dicts = getDictionaries(level); TestConfigLevel *dict : dicts) {
        result.append(dict->value(name));
    }
    return result;
}

QStringList TestGitConfiguration::getRegex(GitConfigurationLevel level, GitConfigurationType, const QString &nameRegex,
                                           const OptString &valueRegex)
{
    const QRegularExpression nameRe(nameRegex);
    QStringList result;
    for (const QList<TestConfigLevel *> dicts = getDictionaries(level); TestConfigLevel *dict : dicts) {
        for (const auto entries = dict->entries(); const auto &[key, values] : entries) {
            if (nameRe.match(key).hasMatch()) {
                for (const QString &value : values) {
                    if (isMatch(value, valueRegex)) {
                        result.append(value);
                    }
                }
            }
        }
    }
    return result;
}

void TestGitConfiguration::replaceAll(GitConfigurationLevel level, const QString &nameRegex,
                                      const OptString &valueRegex, const QString &value)
{
    // Like upstream's fake, the name "regex" is used as a literal key.
    QStringList &values = getDictionary(level)[nameRegex];
    bool updated = false;
    for (QString &v : values) {
        if (isMatch(v, valueRegex)) {
            v = value;
            updated = true;
        }
    }
    if (!updated) {
        values.append(value);
    }
}

void TestGitConfiguration::unsetAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex)
{
    TestConfigLevel &dict = getDictionary(level);
    if (!dict.contains(name)) {
        return;
    }
    QStringList &values = dict[name];
    values.removeIf([&](const QString &v) { return isMatch(v, valueRegex); });
    if (values.isEmpty()) {
        dict.remove(name);
    }
}

} // namespace gcm::test
