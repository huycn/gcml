#include "core/streams.h"
#include "core/strings.h"

#include <QFile>
#include <QIODevice>

#include <unistd.h>

namespace gcm {

// --- DeviceTextReader -----------------------------------------------------------------------

OptString DeviceTextReader::readLine()
{
    // GitStreamReader semantics: lines end at LF; a CR directly before the LF is dropped,
    // any other CR is content. Read byte-by-byte so nothing beyond the line is consumed.
    QByteArray line;
    char c = 0;
    while (m_device->getChar(&c)) {
        if (c == '\n') {
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            return QString::fromUtf8(line);
        }
        line.append(c);
    }
    if (line.isEmpty()) {
        return std::nullopt;
    }
    return QString::fromUtf8(line);
}

QString DeviceTextReader::readToEnd()
{
    return QString::fromUtf8(m_device->readAll());
}

// --- DeviceTextWriter -----------------------------------------------------------------------

void DeviceTextWriter::write(const QString &text)
{
    m_device->write(text.toUtf8());
    if (m_autoFlush) {
        flush();
    }
}

void DeviceTextWriter::flush()
{
    if (auto *file = qobject_cast<QFile *>(m_device)) {
        file->flush();
    }
}

// --- StringReader ---------------------------------------------------------------------------

OptString StringReader::readLine()
{
    if (m_pos >= m_text.size()) {
        return std::nullopt;
    }
    const qsizetype lf = m_text.indexOf(QLatin1Char('\n'), m_pos);
    QString line;
    if (lf < 0) {
        line = m_text.mid(m_pos);
        m_pos = m_text.size();
    } else {
        line = m_text.mid(m_pos, lf - m_pos);
        m_pos = lf + 1;
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
    }
    return line;
}

QString StringReader::readToEnd()
{
    const QString rest = m_text.mid(m_pos);
    m_pos = m_text.size();
    return rest;
}

// --- StandardStreams ------------------------------------------------------------------------

struct StandardStreams::Private
{
    QFile in, out, err;
    std::unique_ptr<DeviceTextReader> reader;
    std::unique_ptr<DeviceTextWriter> outWriter;
    std::unique_ptr<DeviceTextWriter> errWriter;
};

StandardStreams::StandardStreams() : d(std::make_unique<Private>()) {}
StandardStreams::~StandardStreams() = default;

TextReader &StandardStreams::in()
{
    if (!d->reader) {
        (void)d->in.open(STDIN_FILENO, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::DontCloseHandle);
        d->reader = std::make_unique<DeviceTextReader>(&d->in);
    }
    return *d->reader;
}

TextWriter &StandardStreams::out()
{
    if (!d->outWriter) {
        (void)d->out.open(STDOUT_FILENO, QIODevice::WriteOnly | QIODevice::Unbuffered, QFileDevice::DontCloseHandle);
        d->outWriter = std::make_unique<DeviceTextWriter>(&d->out);
    }
    return *d->outWriter;
}

TextWriter &StandardStreams::error()
{
    if (!d->errWriter) {
        (void)d->err.open(STDERR_FILENO, QIODevice::WriteOnly | QIODevice::Unbuffered, QFileDevice::DontCloseHandle);
        d->errWriter = std::make_unique<DeviceTextWriter>(&d->err);
    }
    return *d->errWriter;
}

bool StandardStreams::isInputRedirected() const
{
    return !isatty(STDIN_FILENO);
}

bool StandardStreams::isOutputRedirected() const
{
    return !isatty(STDOUT_FILENO);
}

bool StandardStreams::isErrorRedirected() const
{
    return !isatty(STDERR_FILENO);
}

// --- dictionary wire format -----------------------------------------------------------------

StringDictionary readDictionary(TextReader &reader, Qt::CaseSensitivity cs)
{
    StringDictionary dict(cs);
    OptString line;
    while ((line = reader.readLine()) && !isNullOrWhiteSpace(*line)) {
        const qsizetype split = line->indexOf(QLatin1Char('='));
        if (split > 0) {
            dict.set(line->left(split), line->mid(split + 1));
        }
    }
    return dict;
}

MultiDictionary readMultiDictionary(TextReader &reader, Qt::CaseSensitivity cs)
{
    MultiDictionary dict(cs);
    OptString line;
    while ((line = reader.readLine()) && !isNullOrWhiteSpace(*line)) {
        const qsizetype split = line->indexOf(QLatin1Char('='));
        if (split <= 0) {
            continue;
        }
        QString key = line->left(split);
        const QString value = line->mid(split + 1);

        const bool multi = key.endsWith(QLatin1String("[]"));
        if (multi) {
            key.chop(2);
        }

        QStringList &list = dict[key];

        // Only allow one value for non-multi/array entries; an empty value resets the list.
        const bool emptyValue = value.isEmpty();
        if (!multi || emptyValue) {
            list.clear();
        }
        if (multi && emptyValue) {
            continue;
        }
        list.append(value);
    }
    return dict;
}

void writeDictionary(TextWriter &writer, const StringDictionary &dict)
{
    for (const auto &[key, value] : dict) {
        writer.writeLine(key + QLatin1Char('=') + value);
    }
    writer.writeLine();
}

void writeDictionary(TextWriter &writer, const MultiDictionary &dict)
{
    for (const auto &[key, rawValues] : dict) {
        // An empty/blank value resets everything before it.
        QStringList values;
        for (const QString &value : rawValues) {
            if (isNullOrWhiteSpace(value)) {
                values.clear();
            } else {
                values.append(value);
            }
        }
        if (values.size() == 1) {
            writer.writeLine(key + QLatin1Char('=') + values[0]);
        } else {
            for (const QString &value : std::as_const(values)) {
                writer.writeLine(key + QStringLiteral("[]=") + value);
            }
        }
    }
    writer.writeLine();
}

} // namespace gcm
