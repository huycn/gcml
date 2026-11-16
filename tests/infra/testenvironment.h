// Port of TestInfrastructure/Objects/TestEnvironment.cs
#pragma once

#include "core/platform.h"
#include "infra/testfilesystem.h"

namespace gcm::test {

class TestEnvironment : public IEnvironment
{
public:
    /// Uses its own TestFileSystem when none is given.
    explicit TestEnvironment(IFileSystem *fileSystem = nullptr)
        : m_ownedFs(fileSystem ? nullptr : std::make_unique<TestFileSystem>()),
          m_fs(fileSystem ? fileSystem : m_ownedFs.get())
    {
    }

    StringDictionary Variables;
    StringDictionary Symlinks;

    QStringList path() const { return Variables.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts); }
    void setPath(const QStringList &value) { Variables.set(QStringLiteral("PATH"), value.join(QLatin1Char(':'))); }

    // IEnvironment
    const StringDictionary &variables() const override { return Variables; }
    bool isDirectoryOnPath(const QString &directoryPath) const override { return path().contains(directoryPath); }
    bool tryLocateExecutable(const QString &program, QString &result) const override
    {
        if (const QString *pathValue = Variables.find(QStringLiteral("PATH"))) {
            const QStringList paths = pathValue->split(QLatin1Char(':'));
            for (const QString &basePath : paths) {
                const QString candidate = program.startsWith(QLatin1Char('/'))
                    ? program
                    : (basePath.isEmpty() || basePath.endsWith(QLatin1Char('/')) ? basePath + program
                                                                                 : basePath + QLatin1Char('/') + program);
                if (m_fs->fileExists(candidate)) {
                    result = candidate;
                    return true;
                }
            }
        }
        result.clear();
        return false;
    }
    void setEnvironmentVariable(const QString &variable, const OptString &value) override
    {
        // Like upstream's fake: an existing variable is left alone. The process environment is not touched.
        if (Variables.contains(variable)) {
            return;
        }
        Variables.set(variable, value.value_or(QString()));
    }
    void refresh() override {}

private:
    std::unique_ptr<TestFileSystem> m_ownedFs;
    IFileSystem *m_fs;
};

} // namespace gcm::test
