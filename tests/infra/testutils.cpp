#include "infra/testutils.h"

#include "core/strings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>

namespace gcm::test {

namespace TestUtils {

ChangeDirectory::ChangeDirectory(const QString &path) : m_oldPath(QDir::currentPath()) { QDir::setCurrent(path); }
ChangeDirectory::~ChangeDirectory() { QDir::setCurrent(m_oldPath); }

namespace {
QString joinPaths(const QString &basePath, const QStringList &names)
{
    QString path = basePath;
    for (const QString &name : names) {
        path = QDir(path).filePath(name);
    }
    return path;
}

QString createSymlink(const QString &baseDir, const QString &linkName, const QString &targetPath)
{
    const QString linkPath = QDir(baseDir).filePath(linkName);
    if (!QFile::link(targetPath, linkPath)) {
        throw IOException(QStringLiteral("Failed to create symlink '%1' -> '%2'").arg(linkPath, targetPath));
    }
    return QFileInfo(linkPath).absoluteFilePath();
}
} // namespace

QString createFileSymlink(const QString &baseDir, const QString &linkName, const QString &targetPath)
{
    return createSymlink(baseDir, linkName, targetPath);
}

QString createDirectorySymlink(const QString &baseDir, const QString &linkName, const QString &targetPath)
{
    return createSymlink(baseDir, linkName, targetPath);
}

QString createFile(const QString &baseDir, const QStringList &names)
{
    const QString path = joinPaths(baseDir, names);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw IOException(QStringLiteral("Failed to create file '%1'").arg(path));
    }
    return path;
}

QString createDirectory(const QString &baseDir, const QStringList &names)
{
    const QString path = joinPaths(baseDir, names);
    QDir().mkpath(path);
    return path;
}

QString getTempDirectory(bool create)
{
    const QString path = QDir(QDir::tempPath()).filePath(getUuid(8));
    if (create) {
        QDir().mkpath(path);
    }
    return path;
}

QString getUuid(int length)
{
    const QString uuid = QUuid::createUuid().toString(QUuid::Id128);
    if (length <= 0 || length > uuid.size()) {
        return uuid;
    }
    return uuid.left(length);
}

QString runCommand(const QString &filePath, const QStringList &arguments, const QString &workingDirectory)
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory.isEmpty() ? QDir::currentPath() : workingDirectory);
    process.start(filePath, arguments);
    process.waitForFinished(-1);
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QString error = QString::fromUtf8(process.readAllStandardError());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        throw InvalidOperationException(QStringLiteral("Command `%1 %2` failed with exit code %3.\nOutput: %4\nError: %5")
                                            .arg(filePath, arguments.join(QLatin1Char(' ')))
                                            .arg(process.exitCode())
                                            .arg(output, error));
    }
    return output;
}

} // namespace TestUtils

namespace GitTestUtilities {

void GitResult::assertSuccess() const
{
    const QByteArray err = standardError.toUtf8();
    QTest::qCompare(exitCode, 0, err.isEmpty() ? "exitCode" : err.constData(), "0", __FILE__, __LINE__);
}

IsolatedGitConfig::IsolatedGitConfig()
    : m_dir(TestUtils::getTempDirectory()), m_globalPath(QDir(m_dir).filePath(QStringLiteral("gitconfig"))),
      m_oldGlobal(qgetenv("GIT_CONFIG_GLOBAL")), m_oldNoSystem(qgetenv("GIT_CONFIG_NOSYSTEM")),
      m_hadGlobal(qEnvironmentVariableIsSet("GIT_CONFIG_GLOBAL")),
      m_hadNoSystem(qEnvironmentVariableIsSet("GIT_CONFIG_NOSYSTEM"))
{
    TestUtils::createFile(m_dir, {QStringLiteral("gitconfig")});
    qputenv("GIT_CONFIG_GLOBAL", m_globalPath.toUtf8());
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
}

IsolatedGitConfig::~IsolatedGitConfig()
{
    if (m_hadGlobal) {
        qputenv("GIT_CONFIG_GLOBAL", m_oldGlobal);
    } else {
        qunsetenv("GIT_CONFIG_GLOBAL");
    }
    if (m_hadNoSystem) {
        qputenv("GIT_CONFIG_NOSYSTEM", m_oldNoSystem);
    } else {
        qunsetenv("GIT_CONFIG_NOSYSTEM");
    }
    QDir(m_dir).removeRecursively();
}

QString getGitPath()
{
    const QString path = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (path.isEmpty()) {
        throw Exception(QStringLiteral("Failed to locate Git on the PATH"));
    }
    return path;
}

namespace {
/// Removes the repositories created by createRepository when the test process exits.
struct CreatedRepositories
{
    Q_DISABLE_COPY_MOVE(CreatedRepositories)
    CreatedRepositories() = default;
    QStringList paths;
    ~CreatedRepositories()
    {
        for (const QString &path : std::as_const(paths)) {
            QDir(path).removeRecursively();
        }
    }
};
CreatedRepositories &createdRepositories()
{
    static CreatedRepositories instance;
    return instance;
}
} // namespace

QString createRepository(QString *workDirPath)
{
    const QString workDir = QDir(QDir::tempPath()).filePath(QStringLiteral("repo-") + TestUtils::getUuid(8));
    const QString gitDir = QDir(workDir).filePath(QStringLiteral(".git"));
    QDir(workDir).removeRecursively();
    QDir().mkpath(workDir);
    createdRepositories().paths.append(workDir);

    const GitResult result = execGit(gitDir, workDir, QStringLiteral("init"));
    if (result.exitCode != 0) {
        throw Exception(QStringLiteral("git init failed: %1").arg(result.standardError));
    }

    if (workDirPath) {
        *workDirPath = workDir;
    }
    return gitDir;
}

GitResult execGit(const QString &repositoryPath, const QString &workingDirectory, const QString &command)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_DIR"), repositoryPath);
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(workingDirectory);
    process.start(QStringLiteral("git"), splitCommandLine(command));
    if (!process.waitForStarted(-1)) {
        throw Exception(QStringLiteral("Failed to start Git process"));
    }
    process.closeWriteChannel();
    process.waitForFinished(-1);

    GitResult result;
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.standardOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.standardError = QString::fromUtf8(process.readAllStandardError());
    return result;
}

} // namespace GitTestUtilities

namespace RestTestUtilities {

namespace {
void assertAuth(const HttpRequest &request, const QString &scheme, const QString &parameter)
{
    const OptString header = request.headers.value(QStringLiteral("Authorization"));
    QVERIFY2(header.has_value(), "Authorization header missing");
    const qsizetype space = header->indexOf(QLatin1Char(' '));
    QCOMPARE(header->left(space), scheme);
    QCOMPARE(space < 0 ? QString() : header->mid(space + 1), parameter);
}
} // namespace

void assertBasicAuth(const HttpRequest &request, const QString &userName, const QString &password)
{
    const QString expected = QString::fromLatin1((userName + QLatin1Char(':') + password).toUtf8().toBase64());
    assertAuth(request, QStringLiteral("Basic"), expected);
}

void assertBearerAuth(const HttpRequest &request, const QString &token)
{
    assertAuth(request, QStringLiteral("Bearer"), token);
}

} // namespace RestTestUtilities

} // namespace gcm::test
