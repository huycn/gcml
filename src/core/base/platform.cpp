#include "core/platform.h"
#include "core/constants.h"
#include "core/git.h"
#include "core/strings.h"
#include "core/trace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSysInfo>

#include <sys/stat.h>
#include <unistd.h>

namespace gcm {

// =============================================================================================
// LinuxFileSystem
// =============================================================================================

QString LinuxFileSystem::userHomePath() const
{
    return QDir::homePath();
}

QString LinuxFileSystem::userDataDirectoryPath() const
{
    return QDir(userHomePath()).filePath(Constants::GcmDataDirectoryName);
}

bool LinuxFileSystem::isSamePath(const QString &a, const QString &b) const
{
    if (isNullOrWhiteSpace(a) || isNullOrWhiteSpace(b)) {
        return false;
    }

    // Normalize paths (resolve against the current directory, collapse "." and ".."), then resolve symbolic
    // links for anything that exists. Linux file systems are case-sensitive.
    auto resolve = [](const QString &path) {
        const QString full = QDir::cleanPath(QDir::current().absoluteFilePath(path));
        const QString canonical = QFileInfo(full).canonicalFilePath();
        return canonical.isEmpty() ? full : canonical;
    };
    return resolve(a) == resolve(b);
}

bool LinuxFileSystem::fileExists(const QString &path) const
{
    const QFileInfo info(path);
    return info.exists() && !info.isDir();
}

bool LinuxFileSystem::fileIsExecutable(const QString &path) const
{
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return false;
    }
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

bool LinuxFileSystem::directoryExists(const QString &path) const
{
    return QFileInfo(path).isDir();
}

QString LinuxFileSystem::getCurrentDirectory() const
{
    return QDir::currentPath();
}

void LinuxFileSystem::createDirectory(const QString &path)
{
    if (!QDir().mkpath(path)) {
        throw IOException(QStringLiteral("Could not create directory '%1'.").arg(path));
    }
}

void LinuxFileSystem::deleteFile(const QString &path)
{
    QFile file(path);
    if (file.exists() && !file.remove()) {
        throw IOException(QStringLiteral("Could not delete file '%1': %2").arg(path, file.errorString()));
    }
}

QStringList LinuxFileSystem::enumerateFiles(const QString &path, const QString &searchPattern) const
{
    const QDir dir(path);
    if (!dir.exists()) {
        throw IOException(QStringLiteral("Could not find a part of the path '%1'.").arg(path));
    }
    QStringList result;
    const QStringList names = dir.entryList({searchPattern}, QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QString &name : names) {
        result.append(dir.filePath(name));
    }
    return result;
}

QStringList LinuxFileSystem::enumerateDirectories(const QString &path) const
{
    const QDir dir(path);
    if (!dir.exists()) {
        throw IOException(QStringLiteral("Could not find a part of the path '%1'.").arg(path));
    }
    QStringList result;
    const QStringList names = dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        result.append(dir.filePath(name));
    }
    return result;
}

QByteArray LinuxFileSystem::readAllBytes(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw IOException(QStringLiteral("Could not read file '%1': %2").arg(path, file.errorString()));
    }
    return file.readAll();
}

QString LinuxFileSystem::readAllText(const QString &path) const
{
    QByteArray data = readAllBytes(path);
    if (data.startsWith("\xEF\xBB\xBF")) {
        data.remove(0, 3);
    }
    return QString::fromUtf8(data);
}

QStringList LinuxFileSystem::readAllLines(const QString &path) const
{
    QString text = readAllText(path);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text.isEmpty()) {
        return {};
    }
    if (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    return text.split(QLatin1Char('\n'));
}

void LinuxFileSystem::writeAllBytes(const QString &path, const QByteArray &data, bool ownerOnly)
{
    QFile file(path);
    const bool opened = ownerOnly
        ? file.open(QIODevice::WriteOnly | QIODevice::Truncate, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        : file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (!opened) {
        throw IOException(QStringLiteral("Could not write file '%1': %2").arg(path, file.errorString()));
    }
    if (ownerOnly) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    if (file.write(data) != data.size()) {
        throw IOException(QStringLiteral("Could not write file '%1': %2").arg(path, file.errorString()));
    }
}

void LinuxFileSystem::appendAllText(const QString &path, const QString &text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        throw IOException(QStringLiteral("Could not write file '%1': %2").arg(path, file.errorString()));
    }
    file.write(text.toUtf8());
}

// =============================================================================================
// Environment
// =============================================================================================

QString IEnvironment::locateExecutable(const QString &program) const
{
    QString path;
    if (tryLocateExecutable(program, path)) {
        return path;
    }
    throw Exception(QStringLiteral("Failed to locate '%1' executable on the path.").arg(program));
}

PosixEnvironment::PosixEnvironment(IFileSystem &fileSystem)
:   m_fs(fileSystem)
{
}

PosixEnvironment::PosixEnvironment(IFileSystem &fileSystem, StringDictionary variables)
:   m_fs(fileSystem),
    m_variables(std::move(variables)),
    m_fixed(true)
{
}

const StringDictionary &PosixEnvironment::variables() const
{
    if (!m_variables) {
        StringDictionary dict;
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QStringList keys = env.keys();
        for (const QString &key : keys) {
            dict.set(key, env.value(key));
        }
        m_variables = std::move(dict);
    }
    return *m_variables;
}

bool PosixEnvironment::isDirectoryOnPath(const QString &directoryPath) const
{
    if (const QString *pathValue = variables().find(QStringLiteral("PATH"))) {
        const QStringList paths = pathValue->split(QLatin1Char(':'));
        for (const QString &path : paths) {
            if (m_fs.isSamePath(path, directoryPath)) {
                return true;
            }
        }
    }
    return false;
}

bool PosixEnvironment::tryLocateExecutable(const QString &program, QString &path) const
{
    return tryLocateExecutable(program, QStringList(), path);
}

bool PosixEnvironment::tryLocateExecutable(const QString &program, const QStringList &pathsToIgnore, QString &path) const
{
    if (const QString *pathValue = variables().find(QStringLiteral("PATH"))) {
        const QStringList paths = pathValue->split(QLatin1Char(':'));
        for (const QString &basePath : paths) {
            // Path.Combine semantics: a rooted `program` replaces the base path.
            const QString candidatePath = program.startsWith(QLatin1Char('/'))
                ? program
                : (basePath.isEmpty() || basePath.endsWith(QLatin1Char('/')) ? basePath + program
                                                                             : basePath + QLatin1Char('/') + program);
            if (m_fs.fileExists(candidatePath) && !pathsToIgnore.contains(candidatePath, Qt::CaseInsensitive) && m_fs.fileIsExecutable(candidatePath)) {
                path = candidatePath;
                return true;
            }
        }
    }
    path.clear();
    return false;
}

void PosixEnvironment::setEnvironmentVariable(const QString &variable, const OptString &value)
{
    // Don't bother setting the variable if it already has the same value
    if (const QString *current = variables().find(variable); current && value && *current == *value) {
        return;
    }

    if (value) {
        qputenv(variable.toUtf8().constData(), value->toUtf8());
    } else {
        qunsetenv(variable.toUtf8().constData());
    }

    // Update the cached variables
    if (m_fixed) {
        if (value) {
            m_variables->set(variable, *value);
        } else {
            m_variables->remove(variable);
        }
    } else {
        refresh();
    }
}

void PosixEnvironment::refresh()
{
    if (!m_fixed) {
        m_variables.reset();
    }
}

// =============================================================================================
// Processes
// =============================================================================================

ChildProcess::ChildProcess(ProcessStartInfo startInfo, Trace2ProcessClass processClass)
:   m_startInfo(std::move(startInfo)),
    m_class(processClass),
    m_process(std::make_unique<QProcess>()),
    m_trace2Id(Trace2::nextChildId())
{
}

ChildProcess::~ChildProcess()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Do not kill a child we merely stopped waiting for; just detach from it quietly.
        m_process->closeWriteChannel();
        m_process->waitForFinished(-1);
    }
    traceExit();
}

bool ChildProcess::start()
{
    const QStringList args = !m_startInfo.argumentList.isEmpty() ? m_startInfo.argumentList : splitCommandLine(m_startInfo.arguments);

    m_startTime = Trace2::writeChildStart(m_trace2Id, m_class, false, m_startInfo.fileName,
                                          !m_startInfo.argumentList.isEmpty() ? m_startInfo.argumentList.join(QLatin1Char(' '))
                                                                              : m_startInfo.arguments);

    if (!m_startInfo.workingDirectory.isEmpty()) {
        m_process->setWorkingDirectory(m_startInfo.workingDirectory);
    }

    if (!m_startInfo.environment.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const auto &[key, value] : m_startInfo.environment) {
            env.insert(key, value);
        }
        m_process->setProcessEnvironment(env);
    }

    // A stream that is not redirected is inherited from (forwarded to) our own standard stream.
    if (!m_startInfo.redirectStandardInput) {
        m_process->setInputChannelMode(QProcess::ForwardedInputChannel);
    }
    if (!m_startInfo.redirectStandardOutput && !m_startInfo.redirectStandardError) {
        m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    } else if (!m_startInfo.redirectStandardOutput) {
        m_process->setProcessChannelMode(QProcess::ForwardedOutputChannel);
    } else if (!m_startInfo.redirectStandardError) {
        m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    }

    m_process->setReadChannel(QProcess::StandardOutput);
    m_process->start(m_startInfo.fileName, args);
    if (!m_process->waitForStarted(-1)) {
        m_exitTraced = true; // nothing ran
        return false;
    }
    m_pid = m_process->processId();
    return true;
}

qint64 ChildProcess::id() const
{
    return m_pid;
}

void ChildProcess::writeStandardInput(const QByteArray &data)
{
    m_process->write(data);
    while (m_process->bytesToWrite() > 0 && m_process->state() == QProcess::Running) {
        if (!m_process->waitForBytesWritten(-1)) {
            break;
        }
    }
}

void ChildProcess::closeStandardInput()
{
    m_process->closeWriteChannel();
}

QByteArray ChildProcess::readAllStandardOutput()
{
    waitForExit();
    return m_process->readAllStandardOutput();
}

QByteArray ChildProcess::readAllStandardError()
{
    waitForExit();
    return m_process->readAllStandardError();
}

OptString ChildProcess::readStandardOutputLine()
{
    m_process->setReadChannel(QProcess::StandardOutput);
    while (!m_process->canReadLine()) {
        if (m_process->state() == QProcess::NotRunning) {
            break;
        }
        if (!m_process->waitForReadyRead(-1) && m_process->state() == QProcess::NotRunning) {
            break;
        }
    }
    if (!m_process->canReadLine() && m_process->bytesAvailable() == 0) {
        return std::nullopt;
    }
    QByteArray line = m_process->readLine();
    if (line.endsWith('\n')) {
        line.chop(1);
    }
    if (line.endsWith('\r')) {
        line.chop(1);
    }
    return QString::fromUtf8(line);
}

void ChildProcess::waitForExit()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->waitForFinished(-1);
    }
    traceExit();
}

bool ChildProcess::waitForExit(int timeoutMs)
{
    if (m_process->state() != QProcess::NotRunning && !m_process->waitForFinished(timeoutMs)) {
        return false;
    }
    traceExit();
    return true;
}

bool ChildProcess::hasExited() const
{
    return m_process->state() == QProcess::NotRunning;
}

int ChildProcess::exitCode() const
{
    if (m_process->exitStatus() == QProcess::CrashExit) {
        return -1;
    }
    return m_process->exitCode();
}

void ChildProcess::kill()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(-1);
    }
    traceExit();
}

void ChildProcess::traceExit()
{
    if (m_exitTraced || m_startTime == 0 || !m_process || m_process->state() != QProcess::NotRunning) {
        return;
    }
    m_exitTraced = true;
    Trace2::writeChildExit(m_trace2Id, m_startTime, int(m_pid), exitCode());
}

std::unique_ptr<ChildProcess> ProcessManager::createProcess(const QString &path, const QString &args, const QString &workingDirectory, Trace2ProcessClass processClass)
{
    ProcessStartInfo psi;
    psi.fileName = path;
    psi.arguments = args;
    psi.workingDirectory = workingDirectory;
    psi.redirectStandardInput = true;
    psi.redirectStandardOutput = true;
    psi.redirectStandardError = false; // Do not redirect stderr as tracing might be enabled
    return createProcess(psi, processClass);
}

std::unique_ptr<ChildProcess> ProcessManager::createProcess(const ProcessStartInfo &psi, Trace2ProcessClass processClass)
{
    return std::make_unique<ChildProcess>(psi, processClass);
}

// =============================================================================================
// Session
// =============================================================================================

void ISessionManager::openBrowser(const QString &url)
{
    const std::optional<QUrl> uri = tryCreateUri(url);
    if (!uri) {
        throw ArgumentException(QStringLiteral("Not a valid URI: '%1'").arg(url));
    }
    openBrowser(*uri);
}

namespace {
bool tryGetShellExecuteHandler(const IEnvironment &env, QString &shellExecPath)
{
    // One additional 'shell execute' utility we also attempt to use on Linux is `wslview`,
    // commonly found on WSL distributions, which opens the browser on the Windows host.
    const QStringList shellHandlers = {QStringLiteral("xdg-open"), QStringLiteral("gnome-open"), QStringLiteral("kfmclient"), WslUtils::WslViewShellHandlerName};
    for (const QString &shellExec : shellHandlers) {
        if (env.tryLocateExecutable(shellExec, shellExecPath)) {
            return true;
        }
    }
    shellExecPath.clear();
    return false;
}
} // namespace

LinuxSessionManager::LinuxSessionManager(ITrace &trace, IEnvironment &env, IFileSystem &fs)
    : m_trace(trace), m_env(env), m_fs(fs)
{
}

bool LinuxSessionManager::isDesktopSession() const
{
    return !isNullOrWhiteSpace(qEnvironmentVariable("DISPLAY")) || !isNullOrWhiteSpace(qEnvironmentVariable("WAYLAND_DISPLAY"));
}

bool LinuxSessionManager::isWebBrowserAvailable() const
{
    if (m_browserAvailable) {
        return *m_browserAvailable;
    }

    auto compute = [this]() {
        // We need a shell execute handler to be able to launch to browser
        QString handler;
        if (!tryGetShellExecuteHandler(m_env, handler)) {
            m_trace.writeLine(QStringLiteral("Could not locate a shell execute handler for Linux - browser is not available."));
            return false;
        }

        // If this is a Windows Subsystem for Linux distribution we may be able to launch the web browser
        // of the host Windows operating system -- unless we are in Windows session 0 (SSH into Windows).
        int wslVersion = 0;
        if (WslUtils::isWslDistribution(m_env, m_fs, wslVersion)) {
            if (WslUtils::getWindowsSessionId(m_fs) == 0) {
                m_trace.writeLine(QStringLiteral("This is a WSL distribution, but Windows session 0 was detected - browser is not available."));
                return false;
            }
            m_trace.writeLine(QStringLiteral("This is a WSL distribution - browser is available."));
            return true;
        }

        // We may be running in the VS Code integrated terminal connected to a remote: its BROWSER helper can
        // open URLs on the client for Remote SSH and Dev Containers, but not for Remote Tunnels.
        const StringDictionary &vars = m_env.variables();
        if (vars.contains(QStringLiteral("VSCODE_IPC_HOOK_CLI")) && vars.contains(QStringLiteral("BROWSER"))) {
            if (vars.contains(QStringLiteral("SSH_CONNECTION")) && !vars.contains(QStringLiteral("SSH_TTY"))) {
                m_trace.writeLine(QStringLiteral("VSCode (Remote SSH) detected - browser is available."));
                return true;
            }
            if (vars.contains(QStringLiteral("REMOTE_CONTAINERS"))) {
                m_trace.writeLine(QStringLiteral("VSCode (Dev Containers) detected - browser is available."));
                return true;
            }
            m_trace.writeLine(QStringLiteral("VSCode (Remote Tunnel) detected - browser is not available."));
            return false;
        }

        // We need a desktop session to be able to launch the browser in the general case
        return isDesktopSession();
    };

    m_browserAvailable = compute();
    return *m_browserAvailable;
}

void LinuxSessionManager::openBrowser(const QUrl &uri)
{
    const QString scheme = uri.scheme();
    if (!equalsIgnoreCase(scheme, QStringLiteral("http")) && !equalsIgnoreCase(scheme, QStringLiteral("https"))) {
        throw ArgumentException(QStringLiteral("Can only open HTTP/HTTPS URIs (Parameter 'uri')"));
    }

    openBrowserInternal(absoluteUri(uri));
}

void LinuxSessionManager::openBrowserInternal(const QString &url)
{
    QString shellExecPath;
    if (!tryGetShellExecuteHandler(m_env, shellExecPath)) {
        throw Exception(QStringLiteral("Failed to locate a utility to launch the default web browser."));
    }

    m_trace.writeLine(QStringLiteral("Opening browser using '%1: %2").arg(shellExecPath, url));

    QProcess process;
    process.setProgram(shellExecPath);
    process.setArguments({url});
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.startDetached();
}

// =============================================================================================
// PlatformUtils
// =============================================================================================

namespace PlatformUtils {

namespace {

QString getLinuxDistroVersion()
{
    // Try /etc/os-release (or /usr/lib/os-release), which is required in systemd distributions.
    for (const QString &osReleasePath : {QStringLiteral("/etc/os-release"), QStringLiteral("/usr/lib/os-release")}) {
        QFile file(osReleasePath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        StringDictionary props(Qt::CaseInsensitive);
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            if (eq < 0) {
                continue;
            }
            QString value = line.mid(eq + 1);
            while (value.startsWith(QLatin1Char('"'))) {
                value.remove(0, 1);
            }
            while (value.endsWith(QLatin1Char('"'))) {
                value.chop(1);
            }
            props.set(line.left(eq), value);
        }

        if (const QString *prettyName = props.find(QStringLiteral("PRETTY_NAME"))) {
            return *prettyName;
        }

        const QString *distro = props.find(QStringLiteral("NAME"));
        if (!distro) {
            distro = props.find(QStringLiteral("ID"));
        }
        if (distro) {
            for (const QString &key : { QStringLiteral("VERSION"), QStringLiteral("VERSION_ID"), QStringLiteral("VERSION_CODENAME") }) {
                if (const QString *version = props.find(key)) {
                    return *distro + QLatin1Char(' ') + *version;
                }
            }
            return *distro;
        }
    }

    // Fall back to `uname -a`
    QProcess uname;
    uname.start(QStringLiteral("uname"), {QStringLiteral("-a")});
    if (uname.waitForFinished(5000) && uname.exitStatus() == QProcess::NormalExit && uname.exitCode() == 0) {
        return QString::fromUtf8(uname.readAllStandardOutput()).trimmed();
    }

    return QStringLiteral("Unknown-Linux");
}

QString getCpuArchitecture()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm")) {
        return QStringLiteral("ARM32");
    }
    if (arch == QLatin1String("arm64")) {
        return QStringLiteral("ARM64");
    }
    if (arch == QLatin1String("x86_64")) {
        return QStringLiteral("x86-64");
    }
    if (arch == QLatin1String("i386")) {
        return QStringLiteral("x86");
    }
    return arch;
}

} // namespace

PlatformInformation getPlatformInformation()
{
    static const QString distroVersion = getLinuxDistroVersion();
    return PlatformInformation {
        QStringLiteral("Linux"),
        distroVersion,
        getCpuArchitecture(),
        QStringLiteral("Qt/%1").arg(QString::fromLatin1(qVersion())),
    };
}

bool isElevatedUser()
{
    return ::geteuid() == 0;
}

QString getNativeEntryPath()
{
    // Try to extract our native argv[0] from the original cmdline: unlike /proc/self/exe this keeps the path
    // the user invoked us by (e.g. a symlink in /usr/local/bin), which is what `configure` should record.
    QString argv0;
    QFile cmdline(QStringLiteral("/proc/self/cmdline"));
    if (cmdline.open(QIODevice::ReadOnly)) {
        const QByteArray data = cmdline.readAll();
        argv0 = QFile::decodeName(data.left(data.indexOf('\0') < 0 ? data.size() : data.indexOf('\0')));
    }

    if (!argv0.isEmpty()) {
        // argv[0] is an absolute file path
        if (argv0.startsWith(QLatin1Char('/'))) {
            return argv0;
        }

        // argv[0] is relative to the current directory (./app or subdir/app). A simple file name ("app")
        // would have been resolved from $PATH instead.
        if (argv0.contains(QLatin1Char('/'))) {
            const QString path = QDir::cleanPath(QDir::current().absoluteFilePath(argv0));
            if (QFileInfo::exists(path)) {
                return path;
            }
        } else {
            const QStringList paths = qEnvironmentVariable("PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
            for (const QString &dir : paths) {
                const QString candidate = QDir(dir).filePath(argv0);
                const QFileInfo info(candidate);
                if (info.isFile() && info.isExecutable()) {
                    return candidate;
                }
            }
        }
    }

    const QString exe = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
    return exe.isEmpty() ? QCoreApplication::applicationFilePath() : exe;
}

} // namespace PlatformUtils

// =============================================================================================
// WslUtils
// =============================================================================================

namespace WslUtils {

namespace {

const QString WslInteropEnvar = QStringLiteral("WSL_INTEROP");
const QString WslConfFilePath = QStringLiteral("/etc/wsl.conf");
const QString DefaultWslMountPrefix = QStringLiteral("/mnt");
const QString DefaultWslSysDriveMountName = QStringLiteral("c");

int g_windowsSessionId = -1;
int g_wslVersion = -1;

int getWslVersion(const IEnvironment &env, const IFileSystem &fs)
{
    // The WSL_INTEROP variable is set in WSL2 distributions
    if (env.variables().contains(WslInteropEnvar)) {
        return 2;
    }

    const QString procVersionPath = QStringLiteral("/proc/version");
    if (fs.fileExists(procVersionPath)) {
        // Both WSL1 and WSL2 distributions include "[Mm]icrosoft" in the version string
        const QString procVersion = fs.readAllText(procVersionPath);
        static const QRegularExpression microsoftRegex(QStringLiteral("[Mm]icrosoft"));
        if (!procVersion.contains(microsoftRegex)) {
            return 0;
        }
        // If the version string contains "wsl2" then this is a WSL2 distribution
        if (procVersion.contains(QLatin1String("wsl2"), Qt::CaseInsensitive)) {
            return 2;
        }
        return 1;
    }
    return 0;
}

QString getSystemDriveMountPath(const IFileSystem &fs)
{
    static OptString sysDriveMountPath;
    if (sysDriveMountPath) {
        return *sysDriveMountPath;
    }

    QString mountPrefix = DefaultWslMountPrefix;

    // If the wsl.conf file exists in this distribution the user may have changed the Windows volume mount point
    if (fs.fileExists(WslConfFilePath)) {
        const IniFile wslConf = IniSerializer::deserialize(fs, WslConfFilePath);
        QString value;
        if (const IniSection *automount = wslConf.tryGetSection(QStringLiteral("automount"));
            automount && automount->tryGetProperty(QStringLiteral("root"), value)) {
            mountPrefix = value;
        }
    }

    // Try to locate the system volume by looking for the Windows\System32 directory
    if (fs.directoryExists(mountPrefix)) {
        const QStringList mountPoints = fs.enumerateDirectories(mountPrefix);
        for (const QString &mountPoint : mountPoints) {
            if (fs.directoryExists(QDir(mountPoint).filePath(QStringLiteral("Windows/System32")))) {
                sysDriveMountPath = mountPoint;
                return mountPoint;
            }
        }
    }

    // Fallback to assuming the system drive is C: mounted at the default prefix
    sysDriveMountPath = QDir(mountPrefix).filePath(DefaultWslSysDriveMountName);
    return *sysDriveMountPath;
}

} // namespace

bool isWslDistribution(const IEnvironment &env, const IFileSystem &fs, int &wslVersion)
{
    if (g_wslVersion < 0) {
        g_wslVersion = getWslVersion(env, fs);
    }
    wslVersion = g_wslVersion;
    return g_wslVersion > 0;
}

int getWindowsSessionId(const IFileSystem &fs)
{
    if (g_windowsSessionId < 0) {
        const QString script = QStringLiteral("(Get-Process -ID $PID).SessionId");
        const QString psStreamSetup = QStringLiteral("[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
                                                     "[Console]::InputEncoding = [System.Text.Encoding]::UTF8; ");
        const QString launcher = QDir(getSystemDriveMountPath(fs))
                                     .filePath(QStringLiteral("Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));

        QProcess proc;
        proc.start(launcher, {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            psStreamSetup + QLatin1Char(' ') + script,
        });
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(30000)) {
            proc.kill();
            return -1;
        }
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            bool ok = false;
            const int sessionId = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toInt(&ok);
            if (ok) {
                g_windowsSessionId = sessionId;
            }
        }
    }
    return g_windowsSessionId;
}

} // namespace WslUtils

} // namespace gcm
