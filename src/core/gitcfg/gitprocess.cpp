// Port of GitVersion.cs and Git.cs
#include "core/git.h"

#include "core/streams.h"
#include "core/strings.h"
#include "core/trace.h"

#include <QRegularExpression>

namespace gcm {

// =============================================================================================
// GitVersion
// =============================================================================================

GitVersion::GitVersion(const QString &versionString)
{
    if (versionString.isNull()) {
        return;
    }

    m_original = versionString;
    const QStringList parts = versionString.split(QLatin1Char('.'));
    for (const QString &part : parts) {
        // Int32.TryParse semantics: surrounding white space and a sign are allowed.
        bool ok = false;
        const int component = part.trimmed().toInt(&ok);
        if (!ok) {
            break; // Exit at the first non-integer component
        }
        m_components.append(component);
    }
}

GitVersion::GitVersion(std::initializer_list<int> components) : m_components(components) {}

QString GitVersion::toString() const
{
    QStringList parts;
    for (int c : m_components) {
        parts.append(QString::number(c));
    }
    return parts.join(QLatin1Char('.'));
}

int GitVersion::compare(const GitVersion &left, const GitVersion &right)
{
    // Compare for as many components as the two versions have in common. A missing component is assumed to be 0.
    const qsizetype leftCount = left.m_components.size(), rightCount = right.m_components.size();
    for (qsizetype i = 0; i < std::max(leftCount, rightCount); ++i) {
        const int l = i < leftCount ? left.m_components[i] : 0;
        const int r = i < rightCount ? right.m_components[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return 0;
}

// =============================================================================================
// GitProcess
// =============================================================================================

GitProcess::GitProcess(ITrace &trace, IProcessManager &processManager, QString gitPath, QString workingDirectory)
:   m_trace(trace),
    m_processManager(processManager),
    m_gitPath(std::move(gitPath)),
    m_workingDirectory(std::move(workingDirectory))
{
    EnsureArgument::notNullOrWhiteSpace(m_gitPath, "gitPath");
}

GitProcess::~GitProcess() = default;

GitVersion GitProcess::version()
{
    if (!m_version) {
        auto git = createProcess(QStringLiteral("version"));
        git->start();
        const QString data = QString::fromUtf8(git->readAllStandardOutput());
        git->waitForExit();

        static const QRegularExpression re(QStringLiteral("^git version (?<value>.*)"));
        const QRegularExpressionMatch match = re.match(data);
        m_version = match.hasMatch() ? GitVersion(match.captured(QStringLiteral("value"))) : GitVersion();
    }
    return *m_version;
}

IGitConfiguration &GitProcess::getConfiguration()
{
    m_configs.append(std::make_shared<GitProcessConfiguration>(m_trace, *this));
    return *m_configs.last();
}

namespace {

QString trimEnd(const QString &s)
{
    qsizetype end = s.size();
    while (end > 0 && s[end - 1].isSpace()) {
        --end;
    }
    return s.left(end);
}

OptString getCurrentRepositoryInternal(GitProcess &self, ITrace &trace, bool suppressStreams)
{
    auto git = self.createProcess(QStringLiteral("rev-parse --absolute-git-dir"));
    // Capture any error messages so they are not exposed on the user's console. QProcess buffers stderr itself,
    // so the pipe cannot fill up and deadlock the child.
    if (suppressStreams) {
        git->startInfo().redirectStandardError = true;
    }

    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    git->waitForExit();

    switch (git->exitCode()) {
        case 0: // OK
            return trimEnd(data);
        case 128: // Not inside a Git repository
            return std::nullopt;
        default: {
            const QString message = QStringLiteral("Failed to get current Git repository");
            trace.writeLine(QStringLiteral("%1 (exit=%2)").arg(message).arg(git->exitCode()));
            throw GitProcess::createGitException(*git, message);
        }
    }
}

} // namespace

bool GitProcess::isInsideRepository()
{
    return !isNullOrWhiteSpace(getCurrentRepositoryInternal(*this, m_trace, true));
}

OptString GitProcess::getCurrentRepository() { return getCurrentRepositoryInternal(*this, m_trace, false); }

QList<GitRemote> GitProcess::getRemotes()
{
    auto git = createProcess(QStringLiteral("remote -v show"));
    // Redirect stderr so we can check for 'not a git repository' errors
    git->startInfo().redirectStandardError = true;
    git->start();
    const QString data = QString::fromUtf8(git->readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(git->readAllStandardError());
    git->waitForExit();

    switch (git->exitCode()) {
        case 0: // OK
            break;
        case 128:
            if (stderrText.contains(QLatin1String("not a git repository"))) {
                return {}; // Not inside a Git repository
            }
            [[fallthrough]];
        default: {
            const QString message = QStringLiteral("Failed to enumerate Git remotes");
            m_trace.writeLine(QStringLiteral("%1 (exit=%2)").arg(message).arg(git->exitCode()));
            throw GitException(message, stderrText, git->exitCode());
        }
    }

    // string.Split() with no separators splits on any white space, keeping empty entries.
    static const QRegularExpression ws(QStringLiteral("\\s"));
    const QStringList lines = data.split(QLatin1Char('\n'));
    QList<GitRemote> remotes;
    // Remotes are always output in groups of two (fetch and push)
    for (qsizetype i = 0; i + 1 < lines.size(); i += 2) {
        // The fetch URL is written first, followed by the push URL
        const QStringList fetchLine = lines[i].split(ws);
        const QStringList pushLine = lines[i + 1].split(ws);

        GitRemote remote;
        // Remote name is always first (and should match between fetch/push)
        remote.name = fetchLine[0];
        // The next part, if present, is the URL
        if (fetchLine.size() > 1 && !isNullOrWhiteSpace(fetchLine[1])) {
            remote.fetchUrl = trimEnd(fetchLine[1]);
        }
        if (pushLine.size() > 1 && !isNullOrWhiteSpace(pushLine[1])) {
            remote.pushUrl = trimEnd(pushLine[1]);
        }
        remotes.append(remote);
    }
    return remotes;
}

std::unique_ptr<ChildProcess> GitProcess::createProcess(const QString &args)
{
    return m_processManager.createProcess(m_gitPath, args, m_workingDirectory, Trace2ProcessClass::Git);
}

StringDictionary GitProcess::invokeHelper(const QString &args, const StringDictionary *standardInput)
{
    ProcessStartInfo psi;
    psi.fileName = m_gitPath;
    psi.arguments = args;
    psi.redirectStandardInput = true;
    psi.redirectStandardOutput = true;
    psi.redirectStandardError = false; // Do not redirect stderr as tracing might be enabled

    auto process = m_processManager.createProcess(psi, Trace2ProcessClass::Git);
    if (!process->start()) {
        throw Exception(QStringLiteral("Failed to start Git helper '%1'").arg(args));
    }

    if (standardInput) {
        StringWriter writer;
        writeDictionary(writer, *standardInput);
        process->writeStandardInput(writer.toString().toUtf8());
        // some helpers won't continue until they see EOF; cf git-credential-cache
        process->closeStandardInput();
    }

    StringDictionary resultDict(Qt::CaseInsensitive);
    // Same rules as readDictionary: stop at a blank line or EOF.
    while (const OptString line = process->readStandardOutputLine()) {
        if (isNullOrWhiteSpace(*line)) {
            break;
        }
        const qsizetype eq = line->indexOf(QLatin1Char('='));
        if (eq > 0) {
            resultDict.set(line->left(eq), line->mid(eq + 1));
        }
    }

    process->waitForExit();
    const int exitCode = process->exitCode();
    if (exitCode != 0) {
        const QString errorMessage = resultDict.value(QStringLiteral("error"), QStringLiteral("Unknown"));
        throw Exception(QStringLiteral("helper error (%1): %2").arg(exitCode).arg(errorMessage));
    }
    return resultDict;
}

GitException GitProcess::createGitException(ChildProcess &git, const QString &message)
{
    const QString gitMessage = git.startInfo().redirectStandardError ? QString::fromUtf8(git.readAllStandardError()) : QString();
    return GitException(message, gitMessage, git.exitCode());
}

} // namespace gcm
