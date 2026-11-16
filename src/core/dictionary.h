// Insertion-ordered dictionary with configurable key case sensitivity.
// Mirrors how the .NET code uses Dictionary<string, T> with a StringComparer
// (and relies, in practice, on insertion-order enumeration).
#pragma once

#include "core/common.h"

#include <QHash>

namespace gcm {

template <typename V> class Dictionary
{
public:
    using Entry = std::pair<QString, V>;

    Dictionary() : m_cs(Qt::CaseSensitive) {}
    explicit Dictionary(Qt::CaseSensitivity cs) : m_cs(cs) {}

    Dictionary(std::initializer_list<Entry> init, Qt::CaseSensitivity cs = Qt::CaseSensitive) : m_cs(cs)
    {
        for (const Entry &e : init) {
            set(e.first, e.second);
        }
    }

    Qt::CaseSensitivity caseSensitivity() const { return m_cs; }
    qsizetype size() const { return m_entries.size(); }
    qsizetype count() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    bool contains(const QString &key) const { return m_index.contains(norm(key)); }

    /// Adds or replaces (keeping the original position on replace).
    void set(const QString &key, const V &value)
    {
        auto it = m_index.constFind(norm(key));
        if (it != m_index.constEnd()) {
            m_entries[*it].second = value;
        } else {
            m_index.insert(norm(key), m_entries.size());
            m_entries.append({key, value});
        }
    }

    /// Returns a reference to the value, default-inserting it when absent.
    V &operator[](const QString &key)
    {
        auto it = m_index.constFind(norm(key));
        if (it != m_index.constEnd()) {
            return m_entries[*it].second;
        }
        m_index.insert(norm(key), m_entries.size());
        m_entries.append({key, V()});
        return m_entries.last().second;
    }

    std::optional<V> tryGet(const QString &key) const
    {
        auto it = m_index.constFind(norm(key));
        if (it == m_index.constEnd()) {
            return std::nullopt;
        }
        return m_entries[*it].second;
    }

    bool tryGetValue(const QString &key, V &out) const
    {
        auto it = m_index.constFind(norm(key));
        if (it == m_index.constEnd()) {
            return false;
        }
        out = m_entries[*it].second;
        return true;
    }

    V value(const QString &key, const V &fallback = V()) const
    {
        auto it = m_index.constFind(norm(key));
        return it == m_index.constEnd() ? fallback : m_entries[*it].second;
    }

    const V *find(const QString &key) const
    {
        auto it = m_index.constFind(norm(key));
        return it == m_index.constEnd() ? nullptr : &m_entries[*it].second;
    }

    bool remove(const QString &key)
    {
        auto it = m_index.find(norm(key));
        if (it == m_index.end()) {
            return false;
        }

        const qsizetype pos = *it;
        m_index.erase(it);
        m_entries.removeAt(pos);

        for (auto &idx : m_index) {
            if (idx > pos) {
                --idx;
            }
        }
        return true;
    }

    void clear()
    {
        m_entries.clear();
        m_index.clear();
    }

    QStringList keys() const
    {
        QStringList k;
        for (const Entry &e : m_entries) {
            k.append(e.first);
        }
        return k;
    }

    const QList<Entry> &entries() const { return m_entries; }
    auto begin() const { return m_entries.cbegin(); }
    auto end() const { return m_entries.cend(); }

    bool operator==(const Dictionary &other) const { return m_entries == other.m_entries; }

private:
    QString norm(const QString &k) const { return m_cs == Qt::CaseSensitive ? k : k.toCaseFolded(); }

    Qt::CaseSensitivity m_cs;
    QList<Entry> m_entries;
    QHash<QString, qsizetype> m_index;
};

using StringDictionary = Dictionary<QString>;
using MultiDictionary = Dictionary<QStringList>;

} // namespace gcm
