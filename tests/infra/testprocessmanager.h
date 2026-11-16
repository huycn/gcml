// Process fakes (no upstream counterpart: upstream tests leave ICommandContext.ProcessManager null or use real processes).
#pragma once

#include "core/platform.h"
#include "core/strings.h"

namespace gcm::test {

/// A ChildProcess that never starts a real process: it records stdin and replays canned output and exit code.
class TestChildProcess : public ChildProcess
{
public:
    explicit TestChildProcess(ProcessStartInfo startInfo, Trace2ProcessClass processClass = Trace2ProcessClass::None)
        : ChildProcess(std::move(startInfo), processClass)
    {
    }

    QByteArray StandardOutput;
    QByteArray StandardError;
    int ExitCode = 0;
    bool StartSucceeds = true;
    /// Everything written to stdin.
    QByteArray StandardInput;
    bool Started = false;
    bool StandardInputClosed = false;
    bool Killed = false;
    /// Called when stdin is closed or output is first read, so a test can compute output from the input.
    std::function<void(TestChildProcess &)> OnRun;

    bool start() override { return Started = StartSucceeds; }
    qint64 id() const override { return 4242; }
    void writeStandardInput(const QByteArray &data) override { StandardInput += data; }
    void closeStandardInput() override
    {
        StandardInputClosed = true;
        run();
    }
    QByteArray readAllStandardOutput() override
    {
        run();
        QByteArray out = StandardOutput.mid(m_outPos);
        m_outPos = StandardOutput.size();
        return out;
    }
    QByteArray readAllStandardError() override
    {
        run();
        return StandardError;
    }
    OptString readStandardOutputLine() override
    {
        run();
        if (m_outPos >= StandardOutput.size()) {
            return std::nullopt;
        }
        qsizetype end = StandardOutput.indexOf('\n', m_outPos);
        if (end < 0) {
            end = StandardOutput.size();
        }
        QByteArray line = StandardOutput.mid(m_outPos, end - m_outPos);
        m_outPos = end + 1;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        return QString::fromUtf8(line);
    }
    void waitForExit() override { run(); }
    bool waitForExit(int) override
    {
        run();
        return true;
    }
    bool hasExited() const override { return m_ran; }
    int exitCode() const override { return ExitCode; }
    void kill() override { Killed = true; }

private:
    void run()
    {
        if (m_ran) {
            return;
        }
        m_ran = true;
        if (OnRun) {
            OnRun(*this);
        }
    }
    bool m_ran = false;
    qsizetype m_outPos = 0;
};

class TestProcessManager : public IProcessManager
{
public:
    /// Creates the child for a request; when unset, a real ChildProcess is created.
    std::function<std::unique_ptr<ChildProcess>(const ProcessStartInfo &, Trace2ProcessClass)> CreateProcessFunc;
    /// Start information of every process created, in order.
    QList<ProcessStartInfo> CreatedProcesses;

    std::unique_ptr<ChildProcess> createProcess(const QString &path, const QString &args, const QString &workingDirectory,
                                                Trace2ProcessClass processClass = Trace2ProcessClass::None) override
    {
        ProcessStartInfo psi;
        psi.fileName = path;
        psi.arguments = args;
        psi.workingDirectory = workingDirectory;
        psi.redirectStandardInput = true;
        psi.redirectStandardOutput = true;
        psi.redirectStandardError = true; // Ok to redirect stderr for testing (as upstream's Core.Tests fake)
        return createProcess(psi, processClass);
    }
    std::unique_ptr<ChildProcess> createProcess(const ProcessStartInfo &psi,
                                                Trace2ProcessClass processClass = Trace2ProcessClass::None) override
    {
        CreatedProcesses.append(psi);
        if (CreateProcessFunc) {
            return CreateProcessFunc(psi, processClass);
        }
        return std::make_unique<ChildProcess>(psi, processClass);
    }

    /// Convenience: every created process is a TestChildProcess with the given result.
    void setResult(const QByteArray &standardOutput, int exitCode = 0, const QByteArray &standardError = {})
    {
        CreateProcessFunc = [=](const ProcessStartInfo &psi, Trace2ProcessClass cls) {
            auto child = std::make_unique<TestChildProcess>(psi, cls);
            child->StandardOutput = standardOutput;
            child->StandardError = standardError;
            child->ExitCode = exitCode;
            return std::unique_ptr<ChildProcess>(std::move(child));
        };
    }
};

} // namespace gcm::test
