// Port of CommandContext.cs (Linux wiring). Links once package stores provides ConsoleService and CredentialStore.
#include "core/commandcontext.h"

#include "core/constants.h"
#include "core/trace2.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace gcm {

struct CommandContext::Private
{
    QString applicationPath;
    QString installationDirectory;
    StandardStreams streams;
    Trace trace;
    std::unique_ptr<ConsoleService> console;
    LinuxFileSystem fileSystem;
    std::unique_ptr<PosixEnvironment> environment;
    std::unique_ptr<LinuxSessionManager> sessionManager;
    ProcessManager processManager;
    std::unique_ptr<GitProcess> git;
    std::unique_ptr<LinuxSettings> settings;
    std::unique_ptr<HttpClientFactory> httpClientFactory;
    std::unique_ptr<CredentialStore> credentialStore;
};

CommandContext::CommandContext() : d(std::make_unique<Private>())
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cmd_ctx"), QStringLiteral("create"));

    d->applicationPath = PlatformUtils::getNativeEntryPath();
    if (d->applicationPath.isEmpty()) {
        d->applicationPath = QCoreApplication::applicationFilePath();
    }
    d->installationDirectory = QFileInfo(d->applicationPath).absolutePath();

    d->console = std::make_unique<ConsoleService>(d->streams);
    d->environment = std::make_unique<PosixEnvironment>(d->fileSystem);
    d->sessionManager = std::make_unique<LinuxSessionManager>(d->trace, *d->environment, d->fileSystem);
    const QString gitPath = getGitPath(*d->environment, d->fileSystem, d->trace);
    d->git = std::make_unique<GitProcess>(d->trace, d->processManager, gitPath, d->fileSystem.getCurrentDirectory());
    d->settings = std::make_unique<LinuxSettings>(*d->environment, *d->git, d->trace, d->fileSystem);
    d->httpClientFactory = std::make_unique<HttpClientFactory>(d->fileSystem, d->trace, *d->settings, *d->console);
    d->credentialStore = std::make_unique<CredentialStore>(*this);
}

CommandContext::~CommandContext() = default;

QString CommandContext::getGitPath(IEnvironment &environment, IFileSystem &fileSystem, ITrace &trace)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cmd_ctx"), QStringLiteral("find_git"));
    const QString programName = QStringLiteral("git");

    // GIT_EXEC_PATH, if set
    if (const OptString gitExecPath = environment.getEnvironmentVariable(Constants::EnvironmentVariables::GitExecutablePath)) {
        const QString candidatePath = (gitExecPath->endsWith(QLatin1Char('/')) ? *gitExecPath : *gitExecPath + QLatin1Char('/')) + programName;
        if (fileSystem.fileExists(candidatePath)) {
            trace.writeLine(QStringLiteral("Using Git executable from GIT_EXEC_PATH: %1").arg(candidatePath));
            return candidatePath;
        }
    }

    // Otherwise git on the PATH
    const QString path = environment.locateExecutable(programName);
    trace.writeLine(QStringLiteral("Using PATH-located Git executable: %1").arg(path));
    return path;
}

QString CommandContext::applicationPath() const
{
    return d->applicationPath;
}

void CommandContext::setApplicationPath(const QString &path)
{
    d->applicationPath = path;
}

QString CommandContext::installationDirectory() const
{
    return d->installationDirectory;
}

ISettings &CommandContext::settings()
{
    return *d->settings;
}

IStandardStreams &CommandContext::streams()
{
    return d->streams;
}

IConsoleService &CommandContext::console()
{
    return *d->console;
}

ISessionManager &CommandContext::sessionManager()
{
    return *d->sessionManager;
}

ITrace &CommandContext::trace()
{
    return d->trace;
}

IFileSystem &CommandContext::fileSystem()
{
    return d->fileSystem;
}

ICredentialStore &CommandContext::credentialStore()
{
    return *d->credentialStore;
}

IHttpClientFactory &CommandContext::httpClientFactory()
{
    return *d->httpClientFactory;
}

IGit &CommandContext::git()
{
    return *d->git;
}

IEnvironment &CommandContext::environment()
{
    return *d->environment;
}

IProcessManager &CommandContext::processManager()
{
    return d->processManager;
}

} // namespace gcm
