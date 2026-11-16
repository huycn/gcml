// Port of TestInfrastructure/Objects/TestGitConfiguration.cs: in-memory system/global/local configuration.
#pragma once

#include "core/git.h"

namespace gcm::test {

/// Multi-valued configuration entries of one level, keyed with Git configuration key semantics
/// (section and property case-insensitive, scope case-sensitive). Keeps the spelling of the first insertion.
class TestConfigLevel
{
public:
    using Entry = std::pair<QString, QStringList>;

    /// Default-inserts an empty list.
    QStringList &operator[](const QString &key);
    bool contains(const QString &key) const { return m_entries.contains(normalizeKey(key)); }
    bool tryGetValue(const QString &key, QStringList &values) const;
    QStringList value(const QString &key) const;
    bool remove(const QString &key) { return m_entries.remove(normalizeKey(key)); }
    void clear() { m_entries.clear(); }
    qsizetype count() const { return m_entries.count(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    QStringList keys() const;
    /// (original key, values) pairs in insertion order.
    QList<Entry> entries() const;

private:
    static QString normalizeKey(const QString &key);
    Dictionary<Entry> m_entries;
};

class TestGitConfiguration : public IGitConfiguration
{
public:
    static inline const QString CanonicalPathPrefix = QStringLiteral("/my/path/prefix");

    TestConfigLevel System;
    TestConfigLevel Global;
    TestConfigLevel Local;

    int SetCallCount = 0;
    int UnsetCallCount = 0;

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
    QStringList getRegex(GitConfigurationLevel level, GitConfigurationType type, const QString &nameRegex,
                         const OptString &valueRegex) override;
    void replaceAll(GitConfigurationLevel level, const QString &nameRegex, const OptString &valueRegex,
                    const QString &value) override;
    void unsetAll(GitConfigurationLevel level, const QString &name, const OptString &valueRegex) override;

private:
    TestConfigLevel &getDictionary(GitConfigurationLevel level);
    QList<TestConfigLevel *> getDictionaries(GitConfigurationLevel level);
};

} // namespace gcm::test
