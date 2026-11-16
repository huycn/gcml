// User-facing console. Output-only messages go to standard error; interactive prompts use the controlling
// terminal (/dev/tty), never Git's stdin/stdout. Ports ConsoleService.cs, Tty/*, Interop/Posix/PosixAnsiConsole*.
// (Upstream renders prompts with Spectre.Console; here they are implemented directly on termios.)
#pragma once

#include "core/common.h"

namespace gcm {

class IStandardStreams;
class TextWriter;
class TerminalConsole;

class IConsoleService
{
public:
    virtual ~IConsoleService() = default;

    /// "info: ...", "warning: ...", "error: ...", "fatal: ..." -- coloured prefix when stderr is a terminal.
    virtual void writeInfo(const QString &message) = 0;
    virtual void writeWarning(const QString &message) = 0;
    virtual void writeError(const QString &message) = 0;
    virtual void writeFatal(const QString &message) = 0;
    virtual void writeLine(const QString &message) = 0;

    /// Prompts on the TTY as "<label>: " and returns the (possibly empty) line entered.
    /// Throws InvalidOperationException when no TTY is available and InterruptedException on Ctrl+C.
    virtual QString promptText(const QString &label) = 0;
    /// As promptText but without echo (optionally echoing `mask` per character).
    virtual QString promptSecret(const QString &label, std::optional<QChar> mask = std::nullopt) = 0;
    /// Shows a single-choice menu (arrow keys / number keys, Enter to accept) and returns the chosen index.
    /// Throws OperationCanceledException("User cancelled the prompt") on Escape, InterruptedException on Ctrl+C.
    virtual int promptSelection(const QString &title, const QStringList &choices) = 0;
};

class ConsoleService : public IConsoleService
{
public:
    explicit ConsoleService(IStandardStreams &streams);
    /// Test seam: the prompt console is created on first use; messages go to `errorWriter` (coloured unless redirected).
    ConsoleService(std::function<std::unique_ptr<TerminalConsole>()> ttyConsoleFactory, TextWriter &errorWriter, bool isErrorRedirected);
    ~ConsoleService() override;

    void writeInfo(const QString &message) override;
    void writeWarning(const QString &message) override;
    void writeError(const QString &message) override;
    void writeFatal(const QString &message) override;
    void writeLine(const QString &message) override;
    QString promptText(const QString &label) override;
    QString promptSecret(const QString &label, std::optional<QChar> mask = std::nullopt) override;
    int promptSelection(const QString &title, const QStringList &choices) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace gcm
