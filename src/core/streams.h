// Text streams (TextReader/TextWriter), standard streams and the Git key=value dictionary wire format.
// Ports StandardStreams.cs, GitStreamReader.cs, StreamExtensions.cs.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"

class QIODevice;

namespace gcm {

class TextReader
{
public:
    virtual ~TextReader() = default;

    /// Returns nullopt at end of input. Lines end at LF; a CR immediately before the LF is dropped
    /// (a lone CR is content) -- GitStreamReader semantics.
    virtual OptString readLine() = 0;
    virtual QString readToEnd() = 0;
};

class TextWriter
{
public:
    virtual ~TextWriter() = default;

    virtual void write(const QString &text) = 0;
    virtual void flush() {}
    /// Always terminates with a single LF.
    void writeLine(const QString &text = QString()) { write(text + QLatin1Char('\n')); }
};

/// Reads UTF-8 text from a QIODevice (not owned) with GitStreamReader line semantics.
class DeviceTextReader : public TextReader
{
public:
    explicit DeviceTextReader(QIODevice *device) : m_device(device) {}

    OptString readLine() override;
    QString readToEnd() override;

private:
    QIODevice *m_device;
};

/// Writes UTF-8 (no BOM) to a QIODevice (not owned).
class DeviceTextWriter : public TextWriter
{
public:
    explicit DeviceTextWriter(QIODevice *device, bool autoFlush = true) : m_device(device), m_autoFlush(autoFlush) {}

    void write(const QString &text) override;
    void flush() override;

private:
    QIODevice *m_device;
    bool m_autoFlush;
};

/// In-memory reader/writer (StringReader / StringWriter); used by tests and for building child stdin.
class StringReader : public TextReader
{
public:
    explicit StringReader(QString text) : m_text(std::move(text)) {}

    OptString readLine() override;
    QString readToEnd() override;

private:
    QString m_text;
    qsizetype m_pos = 0;
};

class StringWriter : public TextWriter
{
public:
    void write(const QString &text) override { m_text += text; }
    const QString &toString() const { return m_text; }
    void clear() { m_text.clear(); }

private:
    QString m_text;
};

class IStandardStreams
{
public:
    virtual ~IStandardStreams() = default;
    virtual TextReader &in() = 0;
    virtual TextWriter &out() = 0;
    virtual TextWriter &error() = 0;
    virtual bool isInputRedirected() const = 0;
    virtual bool isOutputRedirected() const = 0;
    virtual bool isErrorRedirected() const = 0;
};

/// Process stdin/stdout/stderr, UTF-8 without BOM, LF newlines, auto-flush.
class StandardStreams : public IStandardStreams
{
public:
    StandardStreams();
    ~StandardStreams() override;

    TextReader &in() override;
    TextWriter &out() override;
    TextWriter &error() override;
    bool isInputRedirected() const override;
    bool isOutputRedirected() const override;
    bool isErrorRedirected() const override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// --- StreamExtensions -----------------------------------------------------------------------
/// Reads `key=value` lines until a blank line or EOF. Later keys overwrite earlier ones.
StringDictionary readDictionary(TextReader &reader, Qt::CaseSensitivity cs = Qt::CaseSensitive);
/// As above but understands `key[]=value` multi-values (an empty value resets the list).
MultiDictionary readMultiDictionary(TextReader &reader, Qt::CaseSensitivity cs = Qt::CaseSensitive);
/// Writes `key=value` lines followed by a blank line.
void writeDictionary(TextWriter &writer, const StringDictionary &dict);
/// Multi-value form: single values as `key=value`, several as `key[]=value`.
void writeDictionary(TextWriter &writer, const MultiDictionary &dict);

} // namespace gcm
