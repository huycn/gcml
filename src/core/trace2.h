// Git TRACE2 emission (GIT_TRACE2 / GIT_TRACE2_EVENT / GIT_TRACE2_PERF and trace2.* config).
// Port of Tracing/Trace2*.cs. The public surface is a process-wide static facade, exactly like upstream;
// every call is a cheap no-op when TRACE2 is not enabled.
#pragma once

#include "core/common.h"

#include <source_location>

namespace gcm {

enum class Trace2ProcessClass {
    None,
    UiHelper,
    Git,
    Other,
};

class Trace2
{
public:
    using Loc = std::source_location;

    /// RAII scope returned by startRegion/startThread; writes the matching "leave"/"thread_exit" on destruction.
    class Scope
    {
    public:
        Scope() = default;
        explicit Scope(std::function<void()> onExit) : m_onExit(std::move(onExit)) {}
        Scope(Scope &&other) noexcept : m_onExit(std::exchange(other.m_onExit, nullptr)) {}
        Scope &operator=(Scope &&other) noexcept
        {
            if (this != &other) {
                dispose();
                m_onExit = std::exchange(other.m_onExit, nullptr);
            }
            return *this;
        }
        ~Scope() { dispose(); }

        void dispose()
        {
            if (m_onExit) {
                std::exchange(m_onExit, nullptr)();
            }
        }

    private:
        std::function<void()> m_onExit;
    };

    /// Reads the TRACE2 environment variables (and, lazily, trace2.* Git config through `git config`),
    /// opens the targets and writes the version/start events.
    static void initialize(const QStringList &args, const Loc &loc = Loc::current());
    /// Writes the exit/atexit events and closes all targets.
    static void stop(int exitCode, const Loc &loc = Loc::current());
    static bool isEnabled();

    static void writeCommandName(const QString &name, const Loc &loc = Loc::current());
    static void writeError(const QString &message, const Loc &loc = Loc::current());
    static void writeError(const std::exception &ex, const Loc &loc = Loc::current());
    [[nodiscard]] static Scope startRegion(const QString &category, const QString &label, const Loc &loc = Loc::current());
    [[nodiscard]] static Scope startThread(const QString &threadName, const Loc &loc = Loc::current());
    static void writeData(const QString &category, const QString &key, const OptString &value, const Loc &loc = Loc::current());
    static void writeData(const QString &category, const QString &key, qint64 value, const Loc &loc = Loc::current());
    static void writeData(const QString &category, const QString &key, bool value, const Loc &loc = Loc::current());

    /// child_start / child_exit pairs; used by ChildProcess. `childId` comes from nextChildId(). Returns the start
    /// time (microseconds since the epoch) to pass to writeChildExit.
    static int nextChildId();
    static qint64 writeChildStart(int childId, Trace2ProcessClass processClass, bool useShell, const QString &appName,
                                  const QString &args, const Loc &loc = Loc::current());
    static void writeChildExit(int childId, qint64 startTime, int pid, int exitCode, const Loc &loc = Loc::current());

    /// Environment the child must inherit so nested Git/GCM processes chain their session ids.
    static QString sessionId();
    static constexpr const char *SidEnvar = "GIT_TRACE2_PARENT_SID";
    static constexpr const char *ParentNameEnvar = "GIT_TRACE2_PARENT_NAME";

    // Internals exposed for tests
    /// A new session id, appended to the parent's (GIT_TRACE2_PARENT_SID) when there is one.
    static QString createSid();
    /// Number of '/' in a session id: how deeply this process is nested under other Git processes.
    static int getProcessDepth(const QString &sid);
    /// The socket path of an "af_unix:[stream:|dgram:]<path>" target.
    static bool tryGetPipeName(const QString &eventTarget, QString &name);
};

} // namespace gcm
