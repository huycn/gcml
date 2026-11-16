// Port of TestInfrastructure/TestUtils.cs, GitTestUtilities.cs and RestTestUtilities.cs.
// The assert* helpers use Qt Test macros: a failure is recorded on the running test function.
#pragma once

#include "core/http.h"

namespace gcm::test {

namespace TestUtils {

/// Changes the process working directory until destroyed.
class ChangeDirectory
{
public:
    explicit ChangeDirectory(const QString &path);
    ~ChangeDirectory();
    ChangeDirectory(const ChangeDirectory &) = delete;
    ChangeDirectory &operator=(const ChangeDirectory &) = delete;

private:
    QString m_oldPath;
};

/// Returns the full path of the link.
QString createFileSymlink(const QString &baseDir, const QString &linkName, const QString &targetPath);
QString createDirectorySymlink(const QString &baseDir, const QString &linkName, const QString &targetPath);
/// Creates an empty file (and its parent directories) at baseDir/names...
QString createFile(const QString &baseDir, const QStringList &names);
QString createDirectory(const QString &baseDir, const QStringList &names);
/// A new unique directory under the system temp directory.
QString getTempDirectory(bool create = true);
/// A 32-character hex UUID, truncated to `length` when positive.
QString getUuid(int length = -1);
/// Runs a program and returns its stdout; throws InvalidOperationException on a non-zero exit.
QString runCommand(const QString &filePath, const QStringList &arguments, const QString &workingDirectory = QString());

} // namespace TestUtils

namespace GitTestUtilities {

struct GitResult
{
    int exitCode = -1;
    QString standardOutput;
    QString standardError;
    void assertSuccess() const;
};

/// Points Git's global configuration at an empty file in a temporary directory (GIT_CONFIG_GLOBAL) and disables the
/// system configuration (GIT_CONFIG_NOSYSTEM) for this process and its children, so tests never read or write the
/// user's real Git configuration. The previous environment is restored on destruction.
class IsolatedGitConfig
{
public:
    IsolatedGitConfig();
    ~IsolatedGitConfig();
    IsolatedGitConfig(const IsolatedGitConfig &) = delete;
    IsolatedGitConfig &operator=(const IsolatedGitConfig &) = delete;
    QString globalConfigPath() const { return m_globalPath; }

private:
    QString m_dir;
    QString m_globalPath;
    QByteArray m_oldGlobal, m_oldNoSystem;
    bool m_hadGlobal, m_hadNoSystem;
};

/// Path of `git` on PATH; throws when not found.
QString getGitPath();
/// `git init` in a new temp directory (removed when the process exits); returns the .git directory path.
QString createRepository(QString *workDirPath = nullptr);
/// Runs `git <command>` with GIT_DIR=repositoryPath in workingDirectory.
GitResult execGit(const QString &repositoryPath, const QString &workingDirectory, const QString &command);

} // namespace GitTestUtilities

namespace RestTestUtilities {
void assertBasicAuth(const HttpRequest &request, const QString &userName, const QString &password);
void assertBearerAuth(const HttpRequest &request, const QString &token);
} // namespace RestTestUtilities

} // namespace gcm::test
