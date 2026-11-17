// Terminal input/output for interactive prompts. Ports Tty/AnsiEscapeParser.cs, Tty/RawModeExclusivityMode.cs,
// Tty/AnsiConsoleFactory.cs and Interop/Posix/PosixAnsiConsole{Input,Output}.cs. The prompt rendering upstream gets
// from Spectre.Console (text, secret and selection prompts) is implemented here in TerminalConsole.
#pragma once

#include "core/common.h"

namespace gcm {

class TextWriter;

/// System.ConsoleKey values used by the parser.
enum class ConsoleKey : int {
    None = 0,
    Backspace = 8,
    Tab = 9,
    Enter = 13,
    Escape = 27,
    Spacebar = 32,
    PageUp = 33,
    PageDown = 34,
    End = 35,
    Home = 36,
    LeftArrow = 37,
    UpArrow = 38,
    RightArrow = 39,
    DownArrow = 40,
    Insert = 45,
    Delete = 46,
    D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    F1 = 112, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    NoName = 252,
};

/// System.ConsoleKeyInfo
struct KeyInfo
{
    QChar keyChar;
    ConsoleKey key = ConsoleKey::None;
    bool shift = false;
    bool alt = false;
    bool control = false;
    bool operator==(const KeyInfo &) const = default;
};

namespace AnsiEscapeParser {
    /// Decodes one key press from a raw terminal byte stream. `readByte` blocks; `readByteWithTimeout` waits briefly
    /// (to tell a lone Escape from an escape sequence). Both return -1 at end of input. nullopt at end of input; a
    /// default KeyInfo for unknown sequences.
    std::optional<KeyInfo> readKey(const std::function<int()> &readByte, const std::function<int()> &readByteWithTimeout);
} // namespace AnsiEscapeParser

namespace Select {
    /// Port of Interop/Posix/Native/Select.WaitReadable: waits until `fd` is readable or `timeoutMs` elapses (0 polls
    /// without blocking). False on timeout or error.
    bool waitReadable(int fd, int timeoutMs);
} // namespace Select

/// Keyboard input of a terminal.
class ITerminalInput
{
public:
    virtual ~ITerminalInput() = default;

    virtual bool isKeyAvailable() = 0;
    /// nullopt when there is no input (no terminal / end of input).
    virtual std::optional<KeyInfo> readKey() = 0;
    /// Keeps the terminal in raw mode until the returned scope is destroyed (no echo of fast typing between reads).
    virtual std::unique_ptr<void, void (*)(void *)> beginRawModeSession() = 0;
};

/// Text output of a terminal.
class ITerminalOutput
{
public:
    virtual ~ITerminalOutput() = default;

    virtual void write(const QString &text) = 0;
    virtual bool isTerminal() const = 0;
    virtual bool supportsAnsi() const = 0;
    virtual int width() const = 0;
};

/// Spectre's IExclusivityMode: runs a prompt with exclusive access to the console.
class IExclusivityMode
{
public:
    virtual ~IExclusivityMode() = default;
    virtual void run(const std::function<void()> &func) = 0;
};

/// Holds a raw-mode session for the whole of an exclusive prompt, inside the inner exclusivity scope.
class RawModeExclusivityMode : public IExclusivityMode
{
public:
    RawModeExclusivityMode(IExclusivityMode &inner, ITerminalInput &input) : m_inner(inner), m_input(input) {}

    void run(const std::function<void()> &func) override;
    template <typename T> T run(const std::function<T()> &func)
    {
        std::optional<T> result;
        run(std::function<void()>([&] { result = func(); }));
        return std::move(*result);
    }

private:
    IExclusivityMode &m_inner;
    ITerminalInput &m_input;
};

/// A console that can prompt: output plus input (AnsiConsoleWithInput upstream).
class TerminalConsole
{
public:
    TerminalConsole(std::unique_ptr<ITerminalOutput> output, std::unique_ptr<ITerminalInput> input);
    ~TerminalConsole();

    ITerminalOutput &output() { return *m_output; }
    ITerminalInput &input() { return *m_input; }
    bool isInteractive() const;

    /// "<label>: " then a line; Enter accepts. Throws InvalidOperationException without input.
    QString promptText(const QString &label);
    /// As promptText without echo, or echoing `mask` per character.
    QString promptSecret(const QString &label, std::optional<QChar> mask = std::nullopt);
    /// Arrow keys / number keys move, Enter accepts, Escape throws OperationCanceledException.
    int promptSelection(const QString &title, const QStringList &choices);

private:
    QString readLine(const QString &label, bool secret, std::optional<QChar> mask);
    std::unique_ptr<ITerminalOutput> m_output;
    std::unique_ptr<ITerminalInput> m_input;
};

namespace AnsiConsoleFactory {
    /// The controlling terminal (/dev/tty) or, without one, a headless console.
    std::unique_ptr<TerminalConsole> createForTty();
    /// A console without input that discards its output.
    std::unique_ptr<TerminalConsole> createHeadless();
} // namespace AnsiConsoleFactory

} // namespace gcm
