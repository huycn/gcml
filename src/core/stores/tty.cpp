// Port of Tty/AnsiEscapeParser.cs, Tty/RawModeExclusivityMode.cs, Tty/AnsiConsoleFactory.cs and
// Interop/Posix/PosixAnsiConsole{Input,Output}.cs, plus the prompt rendering Spectre.Console does upstream.
#include "core/tty.h"
#include "core/trace2.h"

#include <QFile>
#include <QScopeGuard>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace gcm {

// =============================================================================================
// AnsiEscapeParser
// =============================================================================================

namespace {

KeyInfo plain(QChar c, ConsoleKey key)
{
    return KeyInfo{c, key, false, false, false};
}

std::optional<KeyInfo> interpretCsi(const QByteArray &param, char final)
{
    // Parameters "n;mod"
    int n = 0, mod = 0;
    if (!param.isEmpty()) {
        const QList<QByteArray> parts = param.split(';');
        n = parts[0].toInt();
        if (parts.size() > 1) {
            mod = parts[1].toInt();
        }
    }
    // xterm modifier encoding: actual = mod - 1; bit 0 shift, bit 1 alt, bit 2 ctrl
    bool shift = false, alt = false, ctrl = false;
    if (mod > 1) {
        const int m = mod - 1;
        shift = m & 1;
        alt = m & 2;
        ctrl = m & 4;
    }

    std::optional<ConsoleKey> key;
    switch (final) {
        case 'A': key = ConsoleKey::UpArrow; break;
        case 'B': key = ConsoleKey::DownArrow; break;
        case 'C': key = ConsoleKey::RightArrow; break;
        case 'D': key = ConsoleKey::LeftArrow; break;
        case 'H': key = ConsoleKey::Home; break;
        case 'F': key = ConsoleKey::End; break;
        case 'Z': key = ConsoleKey::Tab; break; // back-tab: Shift+Tab
        case '~':
            switch (n) {
                case 1: case 7: key = ConsoleKey::Home; break;
                case 2: key = ConsoleKey::Insert; break;
                case 3: key = ConsoleKey::Delete; break;
                case 4: case 8: key = ConsoleKey::End; break;
                case 5: key = ConsoleKey::PageUp; break;
                case 6: key = ConsoleKey::PageDown; break;
                case 15: key = ConsoleKey::F5; break;
                case 17: key = ConsoleKey::F6; break;
                case 18: key = ConsoleKey::F7; break;
                case 19: key = ConsoleKey::F8; break;
                case 20: key = ConsoleKey::F9; break;
                case 21: key = ConsoleKey::F10; break;
                case 23: key = ConsoleKey::F11; break;
                case 24: key = ConsoleKey::F12; break;
                default: break;
            }
            break;
        default: break;
    }
    if (!key) {
        return KeyInfo();
    }
    if (final == 'Z') {
        shift = true;
    }
    return KeyInfo{QChar(), *key, shift, alt, ctrl};
}

std::optional<KeyInfo> readCsi(const std::function<int()> &readByte)
{
    QByteArray param;
    while (true) {
        const int b = readByte();
        if (b < 0) {
            return std::nullopt;
        }
        // Parameter bytes: digits and ';'
        if ((b >= '0' && b <= '9') || b == ';') {
            param.append(char(b));
            continue;
        }
        // Final byte
        return interpretCsi(param, char(b));
    }
}

std::optional<KeyInfo> readSs3(const std::function<int()> &readByte)
{
    const int b = readByte();
    if (b < 0) {
        return std::nullopt;
    }
    switch (b) {
        case 'A': return plain(QChar(), ConsoleKey::UpArrow);
        case 'B': return plain(QChar(), ConsoleKey::DownArrow);
        case 'C': return plain(QChar(), ConsoleKey::RightArrow);
        case 'D': return plain(QChar(), ConsoleKey::LeftArrow);
        case 'H': return plain(QChar(), ConsoleKey::Home);
        case 'F': return plain(QChar(), ConsoleKey::End);
        case 'P': return plain(QChar(), ConsoleKey::F1);
        case 'Q': return plain(QChar(), ConsoleKey::F2);
        case 'R': return plain(QChar(), ConsoleKey::F3);
        case 'S': return plain(QChar(), ConsoleKey::F4);
        default: return KeyInfo();
    }
}

KeyInfo readAscii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return KeyInfo{QChar::fromLatin1(c), ConsoleKey(int(c)), true, false, false};
    }
    if (c >= 'a' && c <= 'z') {
        return KeyInfo{QChar::fromLatin1(c), ConsoleKey(int(c) - 32), false, false, false};
    }
    if (c >= '0' && c <= '9') {
        return KeyInfo{QChar::fromLatin1(c), ConsoleKey(int(c)), false, false, false};
    }
    // Other printable punctuation
    return KeyInfo{QChar::fromLatin1(c), ConsoleKey::NoName, false, false, false};
}

std::optional<KeyInfo> readUtf8Continuation(int firstByte, const std::function<int()> &readByte)
{
    const int extra = (firstByte & 0xE0) == 0xC0 ? 1 : (firstByte & 0xF0) == 0xE0 ? 2 : (firstByte & 0xF8) == 0xF0 ? 3 : 0;
    if (extra == 0) {
        return KeyInfo();
    }
    QByteArray bytes(1, char(firstByte));
    for (int i = 1; i <= extra; ++i) {
        const int b = readByte();
        if (b < 0 || (b & 0xC0) != 0x80) {
            return KeyInfo();
        }
        bytes.append(char(b));
    }
    const QString decoded = QString::fromUtf8(bytes);
    if (decoded.isEmpty()) {
        return KeyInfo();
    }
    // A surrogate pair yields its high surrogate only (as upstream)
    return KeyInfo{decoded[0], ConsoleKey::NoName, false, false, false};
}

} // namespace

std::optional<KeyInfo> AnsiEscapeParser::readKey(const std::function<int()> &readByte, const std::function<int()> &readByteWithTimeout)
{
    const int first = readByte();
    if (first < 0) {
        return std::nullopt;
    }

    switch (first) {
    case 0x1B: {
        const int next = readByteWithTimeout();
        if (next < 0) { // ESC alone
            return KeyInfo{QChar(0), ConsoleKey::Escape, false, false, false};
        }
        if (next == '[') {
            return readCsi(readByte);
        }
        if (next == 'O') {
            return readSs3(readByte);
        }
        // ESC + key is Alt+<key>; the base key with the Alt flag
        if (const std::optional<KeyInfo> k = readKey([next] { return next; }, readByteWithTimeout)) {
            return KeyInfo{k->keyChar, k->key, k->shift, true, k->control};
        }
        return KeyInfo{QChar(0), ConsoleKey::Escape, false, false, false};
    }
    case 0x7F:
    case 0x08: return plain(QLatin1Char('\b'), ConsoleKey::Backspace);
    case 0x0D:
    case 0x0A: return plain(QLatin1Char('\r'), ConsoleKey::Enter);
    case 0x09: return plain(QLatin1Char('\t'), ConsoleKey::Tab);
    case 0x20: return plain(QLatin1Char(' '), ConsoleKey::Spacebar);
    default: break;
    }
    if (first >= 0x01 && first <= 0x1A) { // Ctrl+A..Ctrl+Z
        return KeyInfo{QChar(first), ConsoleKey(int('A') + first - 1), false, false, true};
    }
    if (first < 0x80) {
        return readAscii(char(first));
    }
    return readUtf8Continuation(first, readByte);
}

// =============================================================================================
// RawModeExclusivityMode
// =============================================================================================

void RawModeExclusivityMode::run(const std::function<void()> &func)
{
    m_inner.run([&] {
        auto session = m_input.beginRawModeSession();
        func();
    });
}

// =============================================================================================
// Inputs and outputs
// =============================================================================================

namespace {

std::unique_ptr<void, void (*)(void *)> noSession()
{
    return {nullptr, [](void *) {}};
}

class NullTerminalInput : public ITerminalInput
{
public:
    bool isKeyAvailable() override { return false; }
    std::optional<KeyInfo> readKey() override { return std::nullopt; }
    std::unique_ptr<void, void (*)(void *)> beginRawModeSession() override { return noSession(); }
};

class NullTerminalOutput : public ITerminalOutput
{
public:
    void write(const QString &) override {}
    bool isTerminal() const override { return false; }
    bool supportsAnsi() const override { return false; }
    int width() const override { return 80; }
};

class TtyOutput : public ITerminalOutput
{
public:
    TtyOutput() : m_fd(::open("/dev/tty", O_WRONLY | O_CLOEXEC)) {
        if (m_fd < 0) {
            throw IOException(QStringLiteral("Failed to open /dev/tty for writing."));
        }
    }

    ~TtyOutput() override {
        ::close(m_fd);
    }

    void write(const QString &text) override {
        const QByteArray data = text.toUtf8();
        qsizetype off = 0;
        while (off < data.size()) {
            const ssize_t n = ::write(m_fd, data.constData() + off, size_t(data.size() - off));
            if (n <= 0) {
                break;
            }
            off += n;
        }
    }

    bool isTerminal() const override {
        return true;
    }

    bool supportsAnsi() const override {
        return true;
    }

    int width() const override {
        winsize ws{};
        return ::ioctl(m_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
    }

private:
    int m_fd;
};

// --- raw mode on /dev/tty, restored on fatal signals -------------------------------------------

std::atomic<int> g_rawFd{-1};
termios g_savedTermios{};
struct sigaction g_previousActions[4];
const int g_signals[4] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP};

extern "C" void restoreTerminalOnSignal(int sig)
{
    // Restore the terminal if a read holds raw mode, then die as the signal intended.
    const int fd = g_rawFd.exchange(-1);
    if (fd >= 0) {
        ::tcsetattr(fd, TCSANOW, &g_savedTermios);
        static const char showCursor[] = "\x1b[?25h\n";
        (void)!::write(fd, showCursor, sizeof(showCursor) - 1);
    }
    for (int i = 0; i < 4; ++i) {
        if (g_signals[i] == sig) {
            ::sigaction(sig, &g_previousActions[i], nullptr);
        }
    }
    ::raise(sig);
}

class TtyInput : public ITerminalInput
{
public:
    TtyInput() : m_fd(::open("/dev/tty", O_RDWR | O_CLOEXEC)) {
        if (m_fd < 0 || ::tcgetattr(m_fd, &m_saved) != 0) {
            if (m_fd >= 0) {
                ::close(m_fd);
            }
            throw IOException(QStringLiteral("Failed to open /dev/tty for reading."));
        }
        // Restore the terminal if the process is killed while a read holds raw mode. Outside reads the terminal
        // stays cooked, so Ctrl+C keeps working while GCM does other work.
        struct sigaction action{};
        action.sa_handler = restoreTerminalOnSignal;
        sigemptyset(&action.sa_mask);
        for (int i = 0; i < 4; ++i) {
            ::sigaction(g_signals[i], &action, &g_previousActions[i]);
        }
    }

    ~TtyInput() override {
        restoreTerminal(false);
        for (int i = 0; i < 4; ++i) {
            ::sigaction(g_signals[i], &g_previousActions[i], nullptr);
        }
        ::close(m_fd);
    }

    bool isKeyAvailable() override {
        return waitReadable(0);
    }

    std::optional<KeyInfo> readKey() override {
        // A raw-mode hold for this read (nested inside a session it changes nothing)
        acquireRawMode();
        const auto release = qScopeGuard([this] { releaseRawMode(); });
        return AnsiEscapeParser::readKey([this] { return readOneBlocking(); }, [this] { return readOneWithTimeout(); });
    }

    std::unique_ptr<void, void (*)(void *)> beginRawModeSession() override {
        acquireRawMode();
        return {this, [](void *p) { static_cast<TtyInput *>(p)->releaseRawMode(); }};
    }

private:
    static constexpr int EscapeDisambiguationTimeoutMs = 50;
    static constexpr int CtrlC = 0x03;

    void acquireRawMode() {
        if (m_rawDepth++ == 0) {
            termios raw = m_saved;
            raw.c_lflag &= ~tcflag_t(ICANON | ECHO | ISIG | IEXTEN);
            raw.c_iflag &= ~tcflag_t(IXON | ICRNL);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            g_savedTermios = m_saved;
            ::tcsetattr(m_fd, TCSANOW, &raw);
            g_rawFd = m_fd;
        }
    }

    void releaseRawMode() {
        if (m_rawDepth > 0 && --m_rawDepth == 0) {
            restoreTerminal(false);
        }
    }

    /// Back to cooked mode, dropping all holds; optionally re-show the cursor (after an abort).
    void restoreTerminal(bool showCursor) {
        if (g_rawFd.exchange(-1) < 0 && m_rawDepth == 0) {
            return;
        }
        ::tcsetattr(m_fd, TCSANOW, &m_saved);
        m_rawDepth = 0;
        if (showCursor) {
            static const char seq[] = "\x1b[?25h\n";
            (void)!::write(m_fd, seq, sizeof(seq) - 1);
        }
    }

    bool waitReadable(int timeoutMs) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(m_fd, &set);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        return ::select(m_fd + 1, &set, nullptr, nullptr, &tv) > 0;
    }

    int readOneBlocking() {
        unsigned char c = 0;
        if (::read(m_fd, &c, 1) != 1) {
            return -1;
        }
        if (c == CtrlC) {
            // Ctrl+C arrives as a byte (ISIG is off): restore the terminal and abort the operation.
            restoreTerminal(true);
            throw InterruptedException(QStringLiteral("The operation was interrupted by the user."));
        }
        return c;
    }

    int readOneWithTimeout() {
        return waitReadable(EscapeDisambiguationTimeoutMs) ? readOneBlocking() : -1;
    }

    int m_fd;
    termios m_saved{};
    int m_rawDepth = 0;
};

} // namespace

// =============================================================================================
// TerminalConsole
// =============================================================================================

TerminalConsole::TerminalConsole(std::unique_ptr<ITerminalOutput> output, std::unique_ptr<ITerminalInput> input)
    : m_output(std::move(output)), m_input(std::move(input))
{
}

TerminalConsole::~TerminalConsole() = default;

bool TerminalConsole::isInteractive() const
{
    return m_output->isTerminal();
}

QString TerminalConsole::promptText(const QString &label)
{
    return readLine(label, false, std::nullopt);
}

QString TerminalConsole::promptSecret(const QString &label, std::optional<QChar> mask)
{
    return readLine(label, true, mask);
}

QString TerminalConsole::readLine(const QString &label, bool secret, std::optional<QChar> mask)
{
    if (!isInteractive()) {
        throw InvalidOperationException(QStringLiteral("Cannot show prompt since the current terminal isn't interactive."));
    }

    auto session = m_input->beginRawModeSession();
    m_output->write(label + QStringLiteral(": "));
    QString text;
    while (true) {
        const std::optional<KeyInfo> key = m_input->readKey();
        if (!key) {
            throw InvalidOperationException(QStringLiteral("Cannot read input from the terminal."));
        }
        if (key->key == ConsoleKey::Enter) {
            m_output->write(QStringLiteral("\n"));
            return text;
        }
        if (key->key == ConsoleKey::Backspace) {
            if (!text.isEmpty()) {
                text.chop(1);
                if (!secret || mask) {
                    m_output->write(QStringLiteral("\b \b"));
                }
            }
            continue;
        }
        if (key->keyChar.isNull() || key->control || key->keyChar.category() == QChar::Other_Control) {
            continue;
        }
        text += key->keyChar;
        if (!secret) {
            m_output->write(QString(key->keyChar));
        } else if (mask) {
            m_output->write(QString(*mask));
        }
    }
}

int TerminalConsole::promptSelection(const QString &title, const QStringList &choices)
{
    if (!isInteractive()) {
        throw InvalidOperationException(QStringLiteral("Cannot show selection prompt since the current terminal isn't interactive."));
    }
    if (choices.isEmpty()) {
        throw InvalidOperationException(QStringLiteral("Cannot show an empty selection prompt. Please call the AddChoice() method to configure the prompt."));
    }

    auto session = m_input->beginRawModeSession();
    const bool ansi = m_output->supportsAnsi();
    int index = 0;
    const int lines = int(choices.size()) + 1;
    bool drawn = false;

    auto render = [&] {
        QString out;
        if (drawn && ansi) {
            out += QStringLiteral("\x1b[%1A").arg(lines);
        }
        auto line = [&](const QString &text) { out += (ansi ? QStringLiteral("\r\x1b[2K") : QString()) + text + QLatin1Char('\n'); };
        line(title);
        for (int i = 0; i < choices.size(); ++i) {
            if (i == index) {
                line(ansi ? QStringLiteral("\x1b[94m> %1\x1b[0m").arg(choices[i]) : QStringLiteral("> %1").arg(choices[i]));
            } else {
                line(QStringLiteral("  %1").arg(choices[i]));
            }
        }
        m_output->write(out);
        drawn = true;
    };
    auto clear = [&] {
        if (!ansi) {
            return;
        }
        QString out = QStringLiteral("\x1b[%1A").arg(lines);
        for (int i = 0; i < lines; ++i) {
            out += QStringLiteral("\r\x1b[2K\n");
        }
        out += QStringLiteral("\x1b[%1A\x1b[?25h").arg(lines);
        m_output->write(out);
    };

    if (ansi) {
        m_output->write(QStringLiteral("\x1b[?25l"));
    }
    render();
    while (true) {
        std::optional<KeyInfo> key;
        try {
            key = m_input->readKey();
        } catch (...) {
            if (ansi) {
                m_output->write(QStringLiteral("\x1b[?25h"));
            }
            throw;
        }
        if (!key) {
            clear();
            throw InvalidOperationException(QStringLiteral("Cannot read input from the terminal."));
        }

        switch (key->key) {
            case ConsoleKey::UpArrow: index = std::max(0, index - 1); break;
            case ConsoleKey::DownArrow: index = std::min(int(choices.size()) - 1, index + 1); break;
            case ConsoleKey::Home:
            case ConsoleKey::PageUp: index = 0; break;
            case ConsoleKey::End:
            case ConsoleKey::PageDown: index = int(choices.size()) - 1; break;
            case ConsoleKey::Enter:
            case ConsoleKey::Spacebar:
                clear();
                return index;
            case ConsoleKey::Escape:
                clear();
                throw OperationCanceledException(QStringLiteral("User cancelled the prompt"));
            default:
                if (key->key >= ConsoleKey::D1 && key->key <= ConsoleKey::D9) {
                    const int n = int(key->key) - int(ConsoleKey::D1);
                    if (n < choices.size()) {
                        index = n;
                    }
                }
                break;
        }
        render();
    }
}

// =============================================================================================
// AnsiConsoleFactory
// =============================================================================================

std::unique_ptr<TerminalConsole> AnsiConsoleFactory::createForTty()
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("ansi_console"), QStringLiteral("create_tty"));
    std::unique_ptr<ITerminalOutput> output;
    try {
        output = std::make_unique<TtyOutput>();
    } catch (const IOException &) {
        // No controlling TTY: headless
        return createHeadless();
    }
    std::unique_ptr<ITerminalInput> input;
    try {
        input = std::make_unique<TtyInput>();
    } catch (const IOException &) {
        input = std::make_unique<NullTerminalInput>();
    }
    return std::make_unique<TerminalConsole>(std::move(output), std::move(input));
}

std::unique_ptr<TerminalConsole> AnsiConsoleFactory::createHeadless()
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("ansi_console"), QStringLiteral("create_headless"));
    return std::make_unique<TerminalConsole>(std::make_unique<NullTerminalOutput>(), std::make_unique<NullTerminalInput>());
}

} // namespace gcm
