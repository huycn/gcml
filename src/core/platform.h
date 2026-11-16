// OS abstractions: environment, file system, child processes, user session.
// Ports EnvironmentBase.cs/PosixEnvironment.cs, FileSystem.cs/LinuxFileSystem.cs, ProcessManager.cs/ChildProcess.cs,
// ISessionManager.cs/LinuxSessionManager.cs, PlatformUtils.cs, WslUtils.cs (Linux side only).
#pragma once

#include "core/common.h"
#include "core/dictionary.h"
#include "core/trace2.h"

class QProcess;

namespace gcm {

class ITrace;

// --- file system ----------------------------------------------------------------------------
class IFileSystem
{
public:
    virtual ~IFileSystem() = default;

    virtual QString userHomePath() const = 0;
    /// ~/.gcm
    virtual QString userDataDirectoryPath() const = 0;
    virtual bool isSamePath(const QString &a, const QString &b) const = 0;
    virtual bool fileExists(const QString &path) const = 0;
    virtual bool fileIsExecutable(const QString &path) const = 0;
    virtual bool directoryExists(const QString &path) const = 0;
    virtual QString getCurrentDirectory() const = 0;
    virtual void createDirectory(const QString &path) = 0;
    virtual void deleteFile(const QString &path) = 0;
    /// Full paths of files directly inside `path` matching the glob `searchPattern` (e.g. "*.gpg").
    virtual QStringList enumerateFiles(const QString &path, const QString &searchPattern) const = 0;
    virtual QStringList enumerateDirectories(const QString &path) const = 0;
    /// Throw IOException when the file cannot be read / written.
    virtual QByteArray readAllBytes(const QString &path) const = 0;
    virtual QString readAllText(const QString &path) const = 0;
    virtual QStringList readAllLines(const QString &path) const = 0;
    /// Creates or truncates; parent directories must exist. `ownerOnly` => mode 0600.
    virtual void writeAllBytes(const QString &path, const QByteArray &data, bool ownerOnly = false) = 0;
    virtual void appendAllText(const QString &path, const QString &text) = 0;
};

class LinuxFileSystem : public IFileSystem
{
public:
    QString userHomePath() const override;
    QString userDataDirectoryPath() const override;
    bool isSamePath(const QString &a, const QString &b) const override;
    bool fileExists(const QString &path) const override;
    bool fileIsExecutable(const QString &path) const override;
    bool directoryExists(const QString &path) const override;
    QString getCurrentDirectory() const override;
    void createDirectory(const QString &path) override;
    void deleteFile(const QString &path) override;
    QStringList enumerateFiles(const QString &path, const QString &searchPattern) const override;
    QStringList enumerateDirectories(const QString &path) const override;
    QByteArray readAllBytes(const QString &path) const override;
    QString readAllText(const QString &path) const override;
    QStringList readAllLines(const QString &path) const override;
    void writeAllBytes(const QString &path, const QByteArray &data, bool ownerOnly = false) override;
    void appendAllText(const QString &path, const QString &text) override;
};

// --- environment ----------------------------------------------------------------------------
class IEnvironment
{
public:
    virtual ~IEnvironment() = default;

    /// Case-sensitive on Linux.
    virtual const StringDictionary &variables() const = 0;
    virtual bool isDirectoryOnPath(const QString &directoryPath) const = 0;
    virtual bool tryLocateExecutable(const QString &program, QString &path) const = 0;
    /// Sets (or, with nullopt, unsets) a process environment variable and refreshes `variables()`.
    virtual void setEnvironmentVariable(const QString &variable, const OptString &value) = 0;
    virtual void refresh() = 0;

    // Extension helpers
    /// Throws Exception("Failed to locate '<program>' executable on the path.") when missing.
    QString locateExecutable(const QString &program) const;
    OptString getEnvironmentVariable(const QString &variable) const { return variables().tryGet(variable); }
};

class PosixEnvironment : public IEnvironment
{
public:
    explicit PosixEnvironment(IFileSystem &fileSystem);

    /// Test seam: fixed set of variables instead of the process environment.
    PosixEnvironment(IFileSystem &fileSystem, StringDictionary variables);
    const StringDictionary &variables() const override;
    bool isDirectoryOnPath(const QString &directoryPath) const override;
    bool tryLocateExecutable(const QString &program, QString &path) const override;
    /// As above but skipping the given candidate paths.
    bool tryLocateExecutable(const QString &program, const QStringList &pathsToIgnore, QString &path) const;
    void setEnvironmentVariable(const QString &variable, const OptString &value) override;
    void refresh() override;

private:
    IFileSystem &m_fs;
    mutable std::optional<StringDictionary> m_variables;
    bool m_fixed = false;
};

// --- processes ------------------------------------------------------------------------------
struct ProcessStartInfo
{
    QString fileName;
    /// Either a raw argument string (split with splitCommandLine) ...
    QString arguments;
    /// ... or an explicit argument list (takes precedence when non-empty).
    QStringList argumentList;
    QString workingDirectory;
    bool redirectStandardInput = true;
    bool redirectStandardOutput = true;
    bool redirectStandardError = true;
    /// Extra environment variables for the child (added on top of the current environment).
    StringDictionary environment;
};

/// A child process with synchronous, blocking I/O. Wraps QProcess; emits TRACE2 child_start/child_exit.
class ChildProcess
{
public:
    explicit ChildProcess(ProcessStartInfo startInfo, Trace2ProcessClass processClass = Trace2ProcessClass::None);
    virtual ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    const ProcessStartInfo &startInfo() const { return m_startInfo; }
    /// May be changed until start() is called.
    ProcessStartInfo &startInfo() { return m_startInfo; }
    /// Returns false if the process could not be started.
    virtual bool start();
    virtual qint64 id() const;
    virtual void writeStandardInput(const QByteArray &data);
    virtual void closeStandardInput();
    /// Blocks until the process exits, then returns everything it wrote.
    virtual QByteArray readAllStandardOutput();
    virtual QByteArray readAllStandardError();
    /// Reads one line (without terminator) from stdout, blocking; nullopt at EOF.
    virtual OptString readStandardOutputLine();
    virtual void waitForExit();
    /// Returns false on timeout.
    virtual bool waitForExit(int timeoutMs);
    virtual bool hasExited() const;
    virtual int exitCode() const;
    virtual void kill();
    QProcess *process() { return m_process.get(); }

protected:
    ProcessStartInfo m_startInfo;
    Trace2ProcessClass m_class;
    std::unique_ptr<QProcess> m_process;
    int m_trace2Id = 0;
    qint64 m_startTime = 0;
    bool m_exitTraced = false;
    void traceExit();
};

class IProcessManager
{
public:
    virtual ~IProcessManager() = default;
    virtual std::unique_ptr<ChildProcess> createProcess(const QString &path, const QString &args, const QString &workingDirectory, Trace2ProcessClass processClass = Trace2ProcessClass::None) = 0;
    virtual std::unique_ptr<ChildProcess> createProcess(const ProcessStartInfo &psi, Trace2ProcessClass processClass = Trace2ProcessClass::None) = 0;
};

class ProcessManager : public IProcessManager
{
public:
    std::unique_ptr<ChildProcess> createProcess(const QString &path, const QString &args, const QString &workingDirectory, Trace2ProcessClass processClass = Trace2ProcessClass::None) override;
    std::unique_ptr<ChildProcess> createProcess(const ProcessStartInfo &psi, Trace2ProcessClass processClass = Trace2ProcessClass::None) override;
};

// --- session --------------------------------------------------------------------------------
class ISessionManager
{
public:
    virtual ~ISessionManager() = default;
    /// True when DISPLAY or WAYLAND_DISPLAY is set.
    virtual bool isDesktopSession() const = 0;
    virtual bool isWebBrowserAvailable() const = 0;
    /// Throws ArgumentException unless the URI is http/https.
    virtual void openBrowser(const QUrl &uri) = 0;
    /// Extension: throws ArgumentException("Not a valid URI: '...'") when `url` is not absolute.
    void openBrowser(const QString &url);
};

class LinuxSessionManager : public ISessionManager
{
public:
    LinuxSessionManager(ITrace &trace, IEnvironment &env, IFileSystem &fs);
    bool isDesktopSession() const override;
    bool isWebBrowserAvailable() const override;
    void openBrowser(const QUrl &uri) override;
    using ISessionManager::openBrowser;

protected:
    /// Launches the system URL handler for an already validated, escaped http(s) URL.
    virtual void openBrowserInternal(const QString &url);

private:
    ITrace &m_trace;
    IEnvironment &m_env;
    IFileSystem &m_fs;
    mutable std::optional<bool> m_browserAvailable;
};

// --- platform information -------------------------------------------------------------------
struct PlatformInformation
{
    QString operatingSystemType; // "Linux"
    QString operatingSystemVersion;
    QString cpuArchitecture;
    QString runtimeVersion; // "Qt 6.x.y" (ClrVersion upstream)
};

namespace PlatformUtils {
PlatformInformation getPlatformInformation();
bool isElevatedUser();
/// Absolute path of the running executable.
QString getNativeEntryPath();
} // namespace PlatformUtils

namespace WslUtils {
inline const QString WslViewShellHandlerName = QStringLiteral("wslview");
/// Detects whether we run inside a WSL distribution; `wslVersion` is 1 or 2.
bool isWslDistribution(const IEnvironment &env, const IFileSystem &fs, int &wslVersion);
/// Session id of the Windows host session, or -1 when it cannot be determined.
int getWindowsSessionId(const IFileSystem &fs);
} // namespace WslUtils

} // namespace gcm
