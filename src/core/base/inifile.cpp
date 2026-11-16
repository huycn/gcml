// Port of IniFile.cs
#include "core/git.h"
#include "core/strings.h"

#include <QRegularExpression>

namespace gcm {

bool IniSectionName::operator==(const IniSectionName &o) const
{
    // Section names are case-insensitive, sub-section names are case-sensitive. An empty sub-name is no sub-name.
    const OptString a = isNullOrEmpty(subName) ? std::nullopt : subName;
    const OptString b = isNullOrEmpty(o.subName) ? std::nullopt : o.subName;
    return equalsIgnoreCase(name, o.name) && a == b;
}

bool IniSection::tryGetProperty(const QString &name, QString &value) const
{
    QStringList values;
    if (tryGetMultiProperty(name, values)) {
        value = values.last();
        return true;
    }
    value.clear();
    return false;
}

bool IniSection::tryGetMultiProperty(const QString &name, QStringList &values) const
{
    values.clear();
    for (const IniProperty &prop : properties) {
        if (equalsIgnoreCase(prop.name, name)) {
            values.append(prop.value);
        }
    }
    return !values.isEmpty();
}

const IniSection *IniFile::tryGetSection(const QString &name, const OptString &subName) const
{
    const IniSectionName key{name, subName};
    for (const IniSection &section : sections) {
        if (section.name == key) {
            return &section;
        }
    }
    return nullptr;
}

IniSection &IniFile::section(const IniSectionName &name)
{
    for (IniSection &section : sections) {
        if (section.name == name) {
            return section;
        }
    }
    sections.append(IniSection{{name.name, isNullOrEmpty(name.subName) ? std::nullopt : name.subName}, {}});
    return sections.last();
}

namespace IniSerializer {

IniFile deserialize(const IFileSystem &fs, const QString &path)
{
    static const QRegularExpression sectionRegex(QStringLiteral(R"re(^\[[^\S#]*(?<name>[^\s#\]]*?)(?:\s+"(?<sub>.+)")?\s*\])re"));
    static const QRegularExpression propertyRegex(QStringLiteral(R"re(^[^\S#]*?(?<name>[^\s#]+)\s*=(?<value>.*)?$)re"));

    IniFile iniFile;
    // Index rather than pointer: appending sections may reallocate the list.
    qsizetype current = -1;

    const QStringList lines = fs.readAllLines(path);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();

        QRegularExpressionMatch match = sectionRegex.match(line);
        if (match.hasMatch()) {
            const QString mainName = match.captured(QStringLiteral("name"));
            const QString subName = match.captured(QStringLiteral("sub"));

            // Skip empty-named sections
            if (isNullOrWhiteSpace(mainName)) {
                continue;
            }

            IniSection &section = iniFile.section({mainName, subName});
            current = &section - iniFile.sections.constData();
            continue;
        }

        match = propertyRegex.match(line);
        if (match.hasMatch()) {
            if (current < 0) {
                throw Exception(QStringLiteral("Missing section header"));
            }

            const QString propName = match.captured(QStringLiteral("name"));
            QString propValue = match.captured(QStringLiteral("value")).trimmed();

            // Trim trailing comments, unless the '#' sits inside double quotes
            const qsizetype firstDQuote = propValue.indexOf(QLatin1Char('"'));
            const qsizetype lastDQuote = propValue.lastIndexOf(QLatin1Char('"'));
            const qsizetype commentIdx = propValue.lastIndexOf(QLatin1Char('#'));
            if (commentIdx > -1) {
                const bool insideDQuotes = firstDQuote > -1 && lastDQuote > -1 && (firstDQuote < commentIdx && commentIdx < lastDQuote);
                if (!insideDQuotes) {
                    propValue = propValue.left(commentIdx).trimmed();
                }
            }

            // Trim book-ending double quotes: "foo" => foo
            if (propValue.size() > 1 && propValue.front() == QLatin1Char('"') && propValue.back() == QLatin1Char('"')) {
                propValue = propValue.mid(1, propValue.size() - 2);
            }

            iniFile.sections[current].properties.append({propName, propValue});
        }
    }

    return iniFile;
}

} // namespace IniSerializer

} // namespace gcm
