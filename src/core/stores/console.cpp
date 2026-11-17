// Port of ConsoleService.cs
#include "core/console.h"

#include "core/streams.h"
#include "core/strings.h"
#include "core/tty.h"

#include <sys/ioctl.h>
#include <unistd.h>

namespace gcm {

namespace {

int terminalWidth()
{
    winsize ws{};
    return ::ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
}

} // namespace

struct ConsoleService::Private
{
    std::function<std::unique_ptr<TerminalConsole>()> ttyFactory;
    std::unique_ptr<TerminalConsole> tty;
    TextWriter *error = nullptr;
    bool isErrorRedirected = true;

    TerminalConsole &ttyConsole()
    {
        if (!tty) {
            tty = ttyFactory();
        }
        return *tty;
    }

    int width() const { return isErrorRedirected ? 80 : terminalWidth(); }

    /// "[color]prefix:[/] message", wrapped to the console width (the colour codes take no columns)
    void write(const char *color, const char *prefix, const QString &message)
    {
        const QString label = QString::fromLatin1(prefix);
        QString text = wrapText(label + QLatin1Char(' ') + message, width());
        if (!isErrorRedirected) {
            text = QStringLiteral("\x1b[%1m%2\x1b[0m").arg(QLatin1String(color), label) + text.mid(label.size());
        }
        error->writeLine(text);
    }
};

ConsoleService::ConsoleService(IStandardStreams &streams)
    : ConsoleService(AnsiConsoleFactory::createForTty, streams.error(), streams.isErrorRedirected())
{
}

ConsoleService::ConsoleService(std::function<std::unique_ptr<TerminalConsole>()> ttyConsoleFactory, TextWriter &errorWriter,
                               bool isErrorRedirected)
    : d(std::make_unique<Private>())
{
    d->ttyFactory = std::move(ttyConsoleFactory);
    d->error = &errorWriter;
    d->isErrorRedirected = isErrorRedirected;
}

ConsoleService::~ConsoleService() = default;

void ConsoleService::writeInfo(const QString &message)
{
    d->write("94", "info:", message);
}

void ConsoleService::writeWarning(const QString &message)
{
    d->write("93", "warning:", message);
}

void ConsoleService::writeError(const QString &message)
{
    d->write("91", "error:", message);
}

void ConsoleService::writeFatal(const QString &message)
{
    d->write("91", "fatal:", message);
}

void ConsoleService::writeLine(const QString &message)
{
    d->error->writeLine(wrapText(message, d->width()));
}

QString ConsoleService::promptText(const QString &label)
{
    return d->ttyConsole().promptText(label);
}

QString ConsoleService::promptSecret(const QString &label, std::optional<QChar> mask)
{
    return d->ttyConsole().promptSecret(label, mask);
}

int ConsoleService::promptSelection(const QString &title, const QStringList &choices)
{
    return d->ttyConsole().promptSelection(title, choices);
}

} // namespace gcm
