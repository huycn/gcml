#include "core/trace.h"

#include <QDateTime>
#include <chrono>
#include <QFileInfo>

namespace gcm {

namespace {

// clazy:exclude-next-line=non-pod-global-static
const QString SecretMask = QStringLiteral("********");

/// Reduce a compiler function signature ("void gcm::Foo::bar(int) const") to its bare name ("bar").
QString memberName(const char *function)
{
    QString name = QString::fromUtf8(function);
    // Lambdas: "gcm::Foo::bar(int)::<lambda()>" -> keep the enclosing function.
    const qsizetype lambda = name.indexOf(QLatin1String("::<lambda"));
    if (lambda >= 0) {
        name.truncate(lambda);
    }
    // Cut the parameter list: find the '(' matching the last ')'.
    qsizetype depth = 0;
    qsizetype open = -1;
    for (qsizetype i = name.size() - 1; i >= 0; --i) {
        if (name[i] == QLatin1Char(')')) {
            ++depth;
        } else if (name[i] == QLatin1Char('(') && --depth == 0) {
            open = i;
            break;
        }
    }
    if (open >= 0) {
        name.truncate(open);
    }
    // Strip return type and qualifiers.
    const qsizetype space = name.lastIndexOf(QLatin1Char(' '));
    if (space >= 0) {
        name.remove(0, space + 1);
    }
    const qsizetype scope = name.lastIndexOf(QLatin1String("::"));
    if (scope >= 0) {
        name.remove(0, scope + 2);
    }
    return name;
}

QString formatText(const QString &message, const ITrace::Loc &loc)
{
    const int sourceColumnMaxWidth = 23;
    QString source =
        QStringLiteral("%1:%2").arg(QFileInfo(QString::fromUtf8(loc.file_name())).fileName()).arg(loc.line());
    if (source.size() > sourceColumnMaxWidth) {
        source = TraceUtils::formatSource(source, sourceColumnMaxWidth);
    }
    // Local time with microseconds, "HH:mm:ss.ffffff"
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const qint64 usec = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const QString time = QDateTime::fromSecsSinceEpoch(usec / 1000000).toString(QStringLiteral("HH:mm:ss")) +
                         QStringLiteral(".%1").arg(usec % 1000000, 6, 10, QLatin1Char('0'));
    return QStringLiteral("%1 %2 trace: [%3] %4")
        .arg(time, source.leftJustified(sourceColumnMaxWidth), memberName(loc.function_name()), message);
}

/// string.Format with positional `{n}` placeholders (`{{` and `}}` are literal braces).
QString formatPositional(const QString &format, const QStringList &args)
{
    QString out;
    for (qsizetype i = 0; i < format.size(); ++i) {
        const QChar c = format[i];
        if (c == QLatin1Char('{')) {
            if (i + 1 < format.size() && format[i + 1] == QLatin1Char('{')) {
                out += c;
                ++i;
                continue;
            }
            const qsizetype close = format.indexOf(QLatin1Char('}'), i);
            bool ok = false;
            const int index = close > i ? format.mid(i + 1, close - i - 1).toInt(&ok) : -1;
            if (ok && index >= 0 && index < args.size()) {
                out += args[index];
                i = close;
                continue;
            }
        } else if (c == QLatin1Char('}') && i + 1 < format.size() && format[i + 1] == QLatin1Char('}')) {
            ++i;
        }
        out += c;
    }
    return out;
}

} // namespace

struct Trace::Private
{
    QList<std::shared_ptr<TextWriter>> writers;
    bool secretTracing = false;
};

Trace::Trace() : d(std::make_unique<Private>()) {}

Trace::~Trace()
{
    try {
        flush();
    } catch (...) { /* squelch */
    }
}

bool Trace::hasListeners() const
{
    return !d->writers.isEmpty();
}

bool Trace::isSecretTracingEnabled() const
{
    return d->secretTracing;
}

void Trace::setSecretTracingEnabled(bool value)
{
    d->secretTracing = value;
}

void Trace::addListener(std::shared_ptr<TextWriter> listener)
{
    // Try not to add the same listener more than once
    if (listener && !d->writers.contains(listener)) {
        d->writers.append(std::move(listener));
    }
}

void Trace::flush()
{
    for (const auto &writer : std::as_const(d->writers)) {
        try {
            writer->flush();
        } catch (...) { /* squelch */
        }
    }
}

void Trace::writeException(const std::exception &ex, const Loc &loc)
{
    const auto *gcmEx = dynamic_cast<const Exception *>(&ex);
    writeLine(QStringLiteral("! error: '%1'.").arg(gcmEx ? gcmEx->message() : QString::fromUtf8(ex.what())), loc);
    for (auto inner = gcmEx ? gcmEx->inner() : nullptr; inner; inner = inner->inner()) {
        writeLine(QStringLiteral("       > '%1'.").arg(inner->message()), loc);
    }
}

void Trace::writeDictionary(const StringDictionary &dict, const Loc &loc)
{
    for (const auto &[key, value] : dict) {
        writeLine(QStringLiteral("\t%1=%2").arg(key, value), loc);
    }
}

void Trace::writeDictionarySecrets(const MultiDictionary &dict, const QStringList &secretKeys, Qt::CaseSensitivity keyCs,
                                   const Loc &loc)
{
    for (const auto &[key, values] : dict) {
        const bool isSecretEntry = secretKeys.contains(key, keyCs);
        const QString suffix = values.size() > 1 ? QStringLiteral("[]") : QString();
        for (const QString &value : values) {
            writeLine(QStringLiteral("\t%1%2=%3").arg(key, suffix, isSecretEntry && !d->secretTracing ? SecretMask : value), loc);
        }
    }
}

void Trace::writeLine(const QString &message, const Loc &loc)
{
    if (d->writers.isEmpty()) {
        return;
    }
    const QString text = formatText(message, loc) + QLatin1Char('\n');
    for (const auto &writer : std::as_const(d->writers)) {
        try {
            writer->write(text);
            writer->flush();
        } catch (...) { /* squelch */
        }
    }
}

void Trace::writeLineSecrets(const QString &format, const QStringList &secrets, const Loc &loc)
{
    QStringList args = secrets;
    if (!d->secretTracing) {
        for (QString &arg : args) {
            arg = SecretMask;
        }
    }
    writeLine(formatPositional(format, args), loc);
}

namespace TraceUtils {

QString formatSource(const QString &source, int sourceColumnMaxWidth)
{
    qsizetype idx = 0;
    const qsizetype maxlen = sourceColumnMaxWidth - 3;
    const qsizetype srclen = source.size();
    while (idx >= 0 && (srclen - idx) > maxlen) {
        idx = source.indexOf(QLatin1Char('\\'), idx + 1);
    }
    // If we cannot find a path separator which allows the path to be long enough, just truncate the file name
    if (idx < 0) {
        idx = srclen - maxlen;
    }
    return QStringLiteral("...") + source.mid(idx);
}

} // namespace TraceUtils

} // namespace gcm
