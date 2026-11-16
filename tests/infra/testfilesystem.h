// Port of TestInfrastructure/Objects/TestFileSystem.cs: an in-memory IFileSystem.
#pragma once

#include "core/dictionary.h"
#include "core/platform.h"

#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace gcm::test {

class TestFileSystem : public IFileSystem
{
public:
    QString UserHomePath;
    QString UserDataDirectoryPath;
    /// path -> content, in insertion order.
    Dictionary<QByteArray> Files;
    QSet<QString> ExecutableFiles;
    QSet<QString> Directories;
    QString CurrentDirectory = QDir::tempPath();
    bool IsCaseSensitive = false;

    TestFileSystem()
    {
        const QString root =
            QDir::tempPath() + QStringLiteral("/gcmtest-") + QUuid::createUuid().toString(QUuid::Id128);
        UserHomePath = root + QStringLiteral("/HOME");
        UserDataDirectoryPath = UserHomePath + QStringLiteral("/.gcm");
    }

    void setExecutable(const QString &path, bool isExecutable = true)
    {
        if (!Files.contains(path)) {
            throw IOException(QStringLiteral("File not found"));
        }
        if (isExecutable) {
            ExecutableFiles.insert(path);
        } else {
            ExecutableFiles.remove(path);
        }
    }

    static QString trimSlash(const QString &path)
    {
        return path.endsWith(QLatin1Char('/')) ? path.left(path.size() - 1) : path;
    }

    // IFileSystem
    QString userHomePath() const override { return UserHomePath; }
    QString userDataDirectoryPath() const override { return UserDataDirectoryPath; }
    bool isSamePath(const QString &a, const QString &b) const override { return a.compare(b, cs()) == 0; }
    bool fileExists(const QString &path) const override { return Files.contains(path); }
    bool fileIsExecutable(const QString &path) const override
    {
        if (!Files.contains(path)) {
            throw IOException(QStringLiteral("File not found"));
        }
        return ExecutableFiles.contains(path);
    }
    bool directoryExists(const QString &path) const override { return Directories.contains(trimSlash(path)); }
    QString getCurrentDirectory() const override { return CurrentDirectory; }
    void createDirectory(const QString &path) override { Directories.insert(trimSlash(path)); }
    void deleteFile(const QString &path) override { Files.remove(path); }
    QStringList enumerateFiles(const QString &path, const QString &searchPattern) const override
    {
        QString regex = searchPattern;
        regex.replace(QLatin1Char('.'), QLatin1String("\\.")).replace(QLatin1Char('*'), QLatin1String(".*"));
        const QRegularExpression re(regex, IsCaseSensitive ? QRegularExpression::NoPatternOption
                                                           : QRegularExpression::CaseInsensitiveOption);
        QStringList result;
        for (const auto &[filePath, data] : Files) {
            if (filePath.startsWith(path, cs()) && re.match(filePath).hasMatch()) {
                result.append(filePath);
            }
        }
        return result;
    }
    QStringList enumerateDirectories(const QString &path) const override
    {
        QStringList result;
        for (const QString &dirPath : Directories) {
            if (dirPath.startsWith(path, cs())) {
                result.append(dirPath);
            }
        }
        result.sort();
        return result;
    }
    QByteArray readAllBytes(const QString &path) const override
    {
        if (const QByteArray *data = Files.find(path)) {
            return *data;
        }
        throw IOException(QStringLiteral("File not found"));
    }
    QString readAllText(const QString &path) const override { return QString::fromUtf8(readAllBytes(path)); }
    QStringList readAllLines(const QString &path) const override
    {
        QString text = readAllText(path);
        text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        return text.split(QLatin1Char('\n'));
    }
    void writeAllBytes(const QString &path, const QByteArray &data, bool = false) override { Files.set(path, data); }
    void appendAllText(const QString &path, const QString &text) override { Files[path].append(text.toUtf8()); }

private:
    Qt::CaseSensitivity cs() const { return IsCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive; }
};

} // namespace gcm::test
