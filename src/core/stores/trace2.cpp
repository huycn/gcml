// Port of Tracing/Trace2.cs and the Trace2*Writer classes: the process-wide TRACE2 facade.
#include "core/trace2.h"

#include "core/constants.h"
#include "core/git.h"
#include "core/platform.h"
#include "core/strings.h"
#include "core/trace2message.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>
#include <atomic>
#include <cstdio>
#include <deque>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace gcm {

namespace {

// =============================================================================================
// Writers
// =============================================================================================

class Trace2Writer
{
public:
    explicit Trace2Writer(Trace2FormatTarget target) : m_target(target) {}
    virtual ~Trace2Writer() = default;
    bool failed() const { return m_failed; }
    virtual void write(const Trace2Message &message) = 0;

protected:
    QByteArray format(const Trace2Message &message) const
    {
        QString text;
        switch (m_target) {
        case Trace2FormatTarget::Event: text = message.toJson(); break;
        case Trace2FormatTarget::Normal: text = message.toNormalString(); break;
        case Trace2FormatTarget::Performance: text = message.toPerformanceString(); break;
        }
        return (text + QLatin1Char('\n')).toUtf8();
    }
    Trace2FormatTarget m_target;
    bool m_failed = false;
};

/// Standard error
class Trace2StderrWriter : public Trace2Writer
{
public:
    using Trace2Writer::Trace2Writer;
    void write(const Trace2Message &message) override
    {
        const QByteArray data = format(message);
        if (std::fwrite(data.constData(), 1, size_t(data.size()), stderr) != size_t(data.size())) {
            m_failed = true;
        }
        std::fflush(stderr);
    }
};

/// A file appended to for every message (errors are ignored, as upstream)
class Trace2FileWriter : public Trace2Writer
{
public:
    Trace2FileWriter(Trace2FormatTarget target, QString path) : Trace2Writer(target), m_path(std::move(path)) {}
    void write(const Trace2Message &message) override
    {
        QFile file(m_path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.write(format(message));
        }
    }

private:
    QString m_path;
};

/// A Unix domain socket (stream, falling back to datagram as Git does for a plain "af_unix:" target)
class Trace2SocketWriter : public Trace2Writer
{
public:
    Trace2SocketWriter(Trace2FormatTarget target, const QString &eventTarget, const QString &path) : Trace2Writer(target)
    {
        const bool dgramOnly = eventTarget.startsWith(QLatin1String("af_unix:dgram:"), Qt::CaseInsensitive);
        const bool streamOnly = eventTarget.startsWith(QLatin1String("af_unix:stream:"), Qt::CaseInsensitive);
        if (!dgramOnly) {
            m_fd = connectSocket(SOCK_STREAM, path);
        }
        if (m_fd < 0 && !streamOnly) {
            m_fd = connectSocket(SOCK_DGRAM, path);
        }
        if (m_fd < 0) {
            m_failed = true;
        }
    }
    ~Trace2SocketWriter() override
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    void write(const Trace2Message &message) override
    {
        const QByteArray data = format(message);
        if (::send(m_fd, data.constData(), size_t(data.size()), MSG_NOSIGNAL) != ssize_t(data.size())) {
            m_failed = true;
        }
    }

private:
    static int connectSocket(int type, const QString &path)
    {
        const QByteArray encoded = QFile::encodeName(path);
        sockaddr_un addr{};
        if (size_t(encoded.size()) >= sizeof(addr.sun_path)) {
            return -1;
        }
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, encoded.constData(), size_t(encoded.size()));
        const int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }
    int m_fd = -1;
};

// =============================================================================================
// State
// =============================================================================================

struct Context
{
    QString threadName;
    qint64 startTime = 0;
    int regionNesting = 0;
    std::optional<qint64> regionStartTime;
};

struct State
{
    bool initialized = false;
    qint64 applicationStartTime = 0;
    QString sid;
    int depth = 0;
    std::atomic<int> nextThreadId{0};
    std::vector<std::unique_ptr<Trace2Writer>> writers;
    std::deque<Context> contexts; // the main context, then any started threads (LIFO)
};

State &state()
{
    static State s;
    return s;
}

std::atomic<int> g_nextChildId{0};

Context &currentContext() { return state().contexts.back(); }

QString fileName(const Trace2::Loc &loc)
{
    return QFileInfo(QString::fromUtf8(loc.file_name())).fileName();
}

double secondsBetween(qint64 fromUsec, qint64 toUsec) { return double(toUsec - fromUsec) / 1e6; }

template <typename M> M message(const Trace2::Loc &loc, qint64 time)
{
    M m;
    m.sid = state().sid;
    m.time = time;
    m.thread = currentContext().threadName;
    m.file = fileName(loc);
    m.line = int(loc.line());
    m.depth = state().depth;
    return m;
}

void writeMessage(const Trace2Message &m)
{
    State &s = state();
    if (!s.initialized) {
        return;
    }
    for (const auto &writer : s.writers) {
        if (!writer->failed()) {
            writer->write(m);
        }
    }
}

/// trace2.* entries of `git config` (only read when an environment variable does not decide)
StringDictionary readGitConfig()
{
    const QString programName = QStringLiteral("git");
    const QString gitExecPath = qEnvironmentVariable("GIT_EXEC_PATH");
    const QString candidate = gitExecPath.isEmpty() ? QString() : QDir(gitExecPath).filePath(programName);
    const QString gitPath = !candidate.isEmpty() && QFile::exists(candidate) ? candidate : programName;

    StringDictionary dict(Qt::CaseInsensitive);
    QProcess process;
    process.start(gitPath, {QStringLiteral("config"), QStringLiteral("-z"), QStringLiteral("--get-regexp"), QStringLiteral("trace2\\..*")});
    if (!process.waitForStarted() || !process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return dict;
        }
    const QStringList kvps = QString::fromUtf8(process.readAllStandardOutput()).split(QChar(0), Qt::SkipEmptyParts);
    for (const QString &kvp : kvps) {
        const qsizetype nl = kvp.indexOf(QLatin1Char('\n'));
        if (nl >= 0) {
            dict.set(kvp.left(nl).trimmed(), kvp.mid(nl + 1).trimmed());
        }
    }
    return dict;
}

void initializeWriters()
{
    std::optional<StringDictionary> gitConfig;
    const std::pair<Trace2FormatTarget, std::pair<QString, QString>> targets[] = {
        { Trace2FormatTarget::Event, {Constants::EnvironmentVariables::GitTrace2Event, Constants::GitConfiguration::Trace2::EventTarget} },
        { Trace2FormatTarget::Normal, {Constants::EnvironmentVariables::GitTrace2Normal, Constants::GitConfiguration::Trace2::NormalTarget} },
        { Trace2FormatTarget::Performance, {Constants::EnvironmentVariables::GitTrace2Performance, Constants::GitConfiguration::Trace2::PerformanceTarget} },
    };
    for (const auto &[format, names] : targets) {
        OptString value;
        if (qEnvironmentVariableIsSet(names.first.toLatin1().constData())) {
            value = qEnvironmentVariable(names.first.toLatin1().constData());
        } else {
            if (!gitConfig) {
                gitConfig = readGitConfig();
            }
            value = gitConfig->tryGet(Constants::GitConfiguration::Trace2::SectionName + QLatin1Char('.') + names.second);
        }
        if (!value) {
            continue;
        }

        QString pipeName;
        if (Trace2::tryGetPipeName(*value, pipeName)) {
            // A Unix domain socket
            state().writers.push_back(std::make_unique<Trace2SocketWriter>(format, *value, pipeName));
        } else if (isTruthy(value)) {
            // Standard error
            state().writers.push_back(std::make_unique<Trace2StderrWriter>(format));
        } else if (value->startsWith(QLatin1Char('/'))) {
            // A file, or (as Git) a directory holding one file per process named after the session id
            QString path = *value;
            if (QFileInfo(path).isDir()) {
                path = QDir(path).filePath(state().sid.section(QLatin1Char('/'), -1));
            }
            state().writers.push_back(std::make_unique<Trace2FileWriter>(format, path));
        }
    }
}

} // namespace

// =============================================================================================
// Trace2
// =============================================================================================

QString Trace2::createSid()
{
    QString parent = qEnvironmentVariable(SidEnvar);
    while (parent.startsWith(QLatin1Char('/'))) {
        parent.remove(0, 1);
    }
    while (parent.endsWith(QLatin1Char('/'))) {
        parent.chop(1);
    }
    const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return parent.isEmpty() ? guid : parent + QLatin1Char('/') + guid;
}

int Trace2::getProcessDepth(const QString &sid)
{
    return int(sid.count(QLatin1Char('/')));
}

bool Trace2::tryGetPipeName(const QString &eventTarget, QString &name)
{
    if (eventTarget.startsWith(QLatin1String("af_unix:"), Qt::CaseInsensitive)) {
        name = eventTarget;
        name.replace(QLatin1String("af_unix:dgram:"), QString())
            .replace(QLatin1String("af_unix:stream:"), QString())
            .replace(QLatin1String("af_unix:"), QString());
        return true;
    }
    name.clear();
    return false;
}

void Trace2::initialize(const QStringList &args, const Loc &loc)
{
    State &s = state();
    if (s.initialized) {
        return;
    }

    s.applicationStartTime = Trace2Time::now();
    s.sid = createSid();
    qputenv(SidEnvar, s.sid.toUtf8());
    s.depth = getProcessDepth(s.sid);
    s.contexts.push_back(Context{QStringLiteral("main"), s.applicationStartTime, 0, std::nullopt});
    initializeWriters();
    s.initialized = true;

    // version and start events
    auto version = message<VersionMessage>(loc, Trace2Time::now());
    version.evt = QStringLiteral("3");
    version.exe = Constants::gcmInformationalVersion();
    version.depth = 0;
    writeMessage(version);

    QString appPath = PlatformUtils::getNativeEntryPath();
    auto start = message<StartMessage>(loc, Trace2Time::now());
    start.depth = 0;
    start.argv = QStringList{QFileInfo(appPath).fileName()} + args;
    start.elapsedTime = secondsBetween(s.applicationStartTime, start.time);
    writeMessage(start);
}

void Trace2::stop(int exitCode, const Loc &loc)
{
    State &s = state();
    if (!s.initialized) {
        return;
    }
    auto exit = message<ExitMessage>(loc, Trace2Time::now());
    exit.depth = 0;
    exit.code = exitCode;
    exit.elapsedTime = secondsBetween(s.applicationStartTime, exit.time);
    writeMessage(exit);
    while (!s.writers.empty()) {
        s.writers.pop_back();
    }
}

bool Trace2::isEnabled()
{
    return state().initialized && !state().writers.empty();
}

QString Trace2::sessionId()
{
    return state().sid;
}

void Trace2::writeCommandName(const QString &name, const Loc &loc)
{
    if (!state().initialized) {
        return;
    }
    EnsureArgument::notNullOrWhiteSpace(name, "name");
    const QString parentName = qEnvironmentVariable(ParentNameEnvar);
    const QString hierarchy = parentName.isEmpty() ? name : parentName + QLatin1Char('/') + name;
    qputenv(ParentNameEnvar, hierarchy.toUtf8());

    auto m = message<CommandNameMessage>(loc, Trace2Time::now());
    m.name = name;
    m.hierarchy = hierarchy;
    writeMessage(m);
}

void Trace2::writeError(const QString &errorMessage, const Loc &loc)
{
    if (!state().initialized) {
        return;
    }
    auto m = message<ErrorMessage>(loc, Trace2Time::now());
    m.message = errorMessage;
    m.parameterizedMessage = errorMessage;
    writeMessage(m);
}

void Trace2::writeError(const std::exception &ex, const Loc &loc)
{
    if (!state().initialized) {
        return;
    }
    auto m = message<ErrorMessage>(loc, Trace2Time::now());
    if (const auto *gitEx = dynamic_cast<const GitException *>(&ex)) {
        m.parameterizedMessage = QStringLiteral("message: '%1' error code: '%2' git message: '{0}'")
                                     .arg(gitEx->message(), QString::number(gitEx->exitCode()));
        m.message = QString(m.parameterizedMessage).replace(QLatin1String("{0}"), gitEx->gitErrorMessage());
    } else if (const auto *interopEx = dynamic_cast<const InteropException *>(&ex)) {
        m.parameterizedMessage = QStringLiteral("message: {0} error code: {1}");
        m.message = QStringLiteral("message: %1 error code: %2").arg(interopEx->message(), QString::number(interopEx->errorCode()));
    } else {
        const auto *gcmEx = dynamic_cast<const Exception *>(&ex);
        m.message = gcmEx ? gcmEx->message() : QString::fromUtf8(ex.what());
        m.parameterizedMessage = m.message;
    }
    writeMessage(m);
}

Trace2::Scope Trace2::startRegion(const QString &category, const QString &label, const Loc &loc)
{
    State &s = state();
    if (!s.initialized) {
        return Scope();
    }

    Context &context = currentContext();
    const int nesting = context.regionNesting + 1;
    context.regionNesting = nesting;
    const std::optional<qint64> previousRegionStart = context.regionStartTime;

    auto enter = message<RegionEnterMessage>(loc, Trace2Time::now());
    enter.category = category;
    enter.label = label;
    enter.message = label;
    enter.nesting = nesting;
    enter.elapsedTime = secondsBetween(s.applicationStartTime, enter.time);
    writeMessage(enter);
    const qint64 startTime = enter.time;
    context.regionStartTime = startTime;

    Context *ctx = &context;
    return Scope([ctx, category, label, loc, nesting, previousRegionStart, startTime] {
        State &st = state();
        if (st.initialized) {
            auto leave = message<RegionLeaveMessage>(loc, Trace2Time::now());
            leave.thread = ctx->threadName;
            leave.category = category;
            leave.label = label;
            leave.message = label;
            leave.nesting = nesting;
            leave.elapsedTime = secondsBetween(st.applicationStartTime, leave.time);
            leave.relativeTime = secondsBetween(startTime, leave.time);
            writeMessage(leave);
        }
        ctx->regionNesting = std::max(0, nesting - 1);
        ctx->regionStartTime = previousRegionStart;
    });
}

Trace2::Scope Trace2::startThread(const QString &threadName, const Loc &loc)
{
    State &s = state();
    if (!s.initialized) {
        return Scope();
    }

    const int id = ++s.nextThreadId;
    const QString fullName = QStringLiteral("th%1:%2").arg(id).arg(threadName.isEmpty() ? QStringLiteral("unknown") : threadName);
    const qint64 startTime = Trace2Time::now();
    s.contexts.push_back(Context{fullName, startTime, 0, std::nullopt});

    auto start = message<ThreadStartMessage>(loc, startTime);
    writeMessage(start);

    return Scope([loc, startTime] {
        State &st = state();
        auto exit = message<ThreadExitMessage>(loc, Trace2Time::now());
        exit.relativeTime = secondsBetween(startTime, exit.time);
        writeMessage(exit);
        if (st.contexts.size() > 1) {
            st.contexts.pop_back();
        }
    });
}

void Trace2::writeData(const QString &category, const QString &key, const OptString &value, const Loc &loc)
{
    State &s = state();
    if (!s.initialized) {
        return;
    }
    EnsureArgument::notNullOrWhiteSpace(category, "category");
    EnsureArgument::notNullOrWhiteSpace(key, "key");

    const Context &context = currentContext();
    auto m = message<DataMessage>(loc, Trace2Time::now());
    m.elapsedTime = secondsBetween(s.applicationStartTime, m.time);
    m.relativeTime = secondsBetween(context.regionStartTime.value_or(context.startTime), m.time);
    m.nesting = context.regionNesting + 1;
    m.category = category;
    m.key = key;
    m.value = value.value_or(QString());
    writeMessage(m);
}

void Trace2::writeData(const QString &category, const QString &key, qint64 value, const Loc &loc)
{
    writeData(category, key, OptString(QString::number(value)), loc);
}

void Trace2::writeData(const QString &category, const QString &key, bool value, const Loc &loc)
{
    writeData(category, key, OptString(value ? QStringLiteral("True") : QStringLiteral("False")), loc);
}

int Trace2::nextChildId() { return ++g_nextChildId; }

qint64 Trace2::writeChildStart(int childId, Trace2ProcessClass processClass, bool useShell, const QString &appName,
                               const QString &args, const Loc &loc)
{
    const qint64 startTime = Trace2Time::now();
    State &s = state();
    if (!s.initialized) {
        return startTime;
    }

    auto m = message<ChildStartMessage>(loc, startTime);
    m.id = childId;
    m.classification = processClass;
    m.useShell = useShell;
    m.argv = QStringList{QFileInfo(appName).fileName()};
    if (!args.isEmpty()) {
        m.argv.append(args.split(QLatin1Char(' ')));
    }
    m.elapsedTime = secondsBetween(s.applicationStartTime, Trace2Time::now());
    writeMessage(m);
    return startTime;
}

void Trace2::writeChildExit(int childId, qint64 startTime, int pid, int exitCode, const Loc &loc)
{
    State &s = state();
    if (!s.initialized) {
        return;
    }
    auto m = message<ChildExitMessage>(loc, Trace2Time::now());
    m.id = childId;
    m.pid = pid;
    m.code = exitCode;
    m.elapsedTime = secondsBetween(s.applicationStartTime, m.time);
    m.relativeTime = secondsBetween(startTime, m.time);
    writeMessage(m);
}

} // namespace gcm
