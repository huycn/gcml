// The TRACE2 event messages and their three renderings (JSON event format, normal and performance text formats).
// Port of Tracing/Trace2Message.cs; exposed for tests.
#pragma once

#include "core/common.h"
#include "core/trace2.h"

namespace gcm {

enum class Trace2Event {
    Version,
    Start,
    Exit,
    ChildStart,
    ChildExit,
    Error,
    RegionEnter,
    RegionLeave,
    ThreadStart,
    ThreadExit,
    Data,
    DataJson,
    CommandName,
};

enum class Trace2FormatTarget {
    Event,
    Normal,
    Performance,
};

/// Microseconds since the Unix epoch, UTC. MinValue is DateTimeOffset.MinValue (0001-01-01T00:00:00+00:00).
namespace Trace2Time {
    inline constexpr qint64 MinValue = -62135596800LL * 1000000LL;
    qint64 now();
    /// System.Text.Json rendering of a UTC DateTimeOffset, e.g. "2026-11-15T10:20:30.123456+00:00".
    QString toJson(qint64 usec);
    /// Local time "HH:mm:ss.ffffff" of the normal and performance formats.
    QString toLocalTimeOfDay(qint64 usec);
} // namespace Trace2Time

/// The fixed-width columns of the performance format.
struct PerformanceFormatFields
{
    bool empty = true;
    std::optional<int> repo;
    std::optional<double> elapsedTime;
    std::optional<double> relativeTime;
    OptString category;

    QString toString() const;
    static QString getRepoSpan(int repo);
    static QString getTimeSpan(double time);
    static QString getCategorySpan(const QString &category);
};

/// Shortest round-trip rendering of a double, like .NET's double.ToString() and JSON writer ("0.05", "1E-05").
QString formatDouble(double value);

class Trace2Message
{
public:
    explicit Trace2Message(Trace2Event event) : event(event) {}
    virtual ~Trace2Message() = default;

    Trace2Event event;
    QString sid;
    QString thread;
    qint64 time = Trace2Time::MinValue;
    QString file;
    int line = 0;
    int depth = 0;

    QString toJson() const;
    QString toNormalString() const;
    QString toPerformanceString() const;

protected:
    class JsonWriter;
    virtual QString eventMessage(Trace2FormatTarget target) const = 0;
    virtual PerformanceFormatFields performanceFields() const { return {}; }
    virtual void writeJsonFields(JsonWriter &) const {}

private:
    QString source() const;
};

class Trace2Message::JsonWriter
{
public:
    void string(const char *name, const QString &value);
    void integer(const char *name, qint64 value);
    void number(const char *name, double value);
    void boolean(const char *name, bool value);
    void raw(const char *name, const QString &json);
    void stringArray(const char *name, const QStringList &values);
    QString result() const { return m_json + QLatin1Char('}'); }

private:
    void key(const char *name);
    QString m_json = QStringLiteral("{");
};

/// JSON string literal with System.Text.Json's default escaping (non-ASCII and HTML-sensitive characters as \uXXXX).
QString jsonString(const QString &value);

class VersionMessage : public Trace2Message
{
public:
    QString evt;
    QString exe;

    VersionMessage() : Trace2Message(Trace2Event::Version) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return exe.toLower(); }
    void writeJsonFields(JsonWriter &w) const override;
};

class StartMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    QStringList argv;

    StartMessage() : Trace2Message(Trace2Event::Start) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return argv.join(QLatin1Char(' ')); }
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class ExitMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    int code = 0;

    ExitMessage() : Trace2Message(Trace2Event::Exit) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override;
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class ChildStartMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    QStringList argv;
    qint64 id = 0;
    Trace2ProcessClass classification = Trace2ProcessClass::None;
    bool useShell = false;

    ChildStartMessage() : Trace2Message(Trace2Event::ChildStart) {}

protected:
    QString eventMessage(Trace2FormatTarget target) const override;
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class ChildExitMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    double relativeTime = 0;
    qint64 id = 0;
    int pid = 0;
    int code = 0;

    ChildExitMessage() : Trace2Message(Trace2Event::ChildExit) {}

protected:
    QString eventMessage(Trace2FormatTarget target) const override;
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class CommandNameMessage : public Trace2Message
{
public:
    QString name;
    OptString hierarchy;

    CommandNameMessage() : Trace2Message(Trace2Event::CommandName) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class ThreadStartMessage : public Trace2Message
{
public:
    ThreadStartMessage() : Trace2Message(Trace2Event::ThreadStart) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return thread; }
};

class ThreadExitMessage : public Trace2Message
{
public:
    double relativeTime = 0;

    ThreadExitMessage() : Trace2Message(Trace2Event::ThreadExit) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override;
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class ErrorMessage : public Trace2Message
{
public:
    QString message;
    QString parameterizedMessage;

    ErrorMessage() : Trace2Message(Trace2Event::Error) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return message; }
    void writeJsonFields(JsonWriter &w) const override;
};

class RegionMessage : public Trace2Message
{
public:
    using Trace2Message::Trace2Message;
    double elapsedTime = 0;
    int repo = 1; // as Git
    int nesting = 1;
    QString category;
    QString label;
    QString message;

protected:
    QString eventMessage(Trace2FormatTarget) const override { return message; }
    void writeJsonFields(JsonWriter &w) const override;
};

class RegionEnterMessage : public RegionMessage
{
public:
    RegionEnterMessage() : RegionMessage(Trace2Event::RegionEnter) {}

protected:
    PerformanceFormatFields performanceFields() const override;
};

class RegionLeaveMessage : public RegionMessage
{
public:
    double relativeTime = 0;
    RegionLeaveMessage() : RegionMessage(Trace2Event::RegionLeave) {}

protected:
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class DataMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    double relativeTime = 0;
    int repo = 1;
    int nesting = 0;
    QString category;
    QString key;
    QString value;

    DataMessage() : Trace2Message(Trace2Event::Data) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return key + QLatin1Char(':') + value; }
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

class DataJsonMessage : public Trace2Message
{
public:
    double elapsedTime = 0;
    double relativeTime = 0;
    int repo = 1;
    int nesting = 0;
    QString category;
    QString key;
    /// Raw JSON text of the value.
    QString value;

    DataJsonMessage() : Trace2Message(Trace2Event::DataJson) {}

protected:
    QString eventMessage(Trace2FormatTarget) const override { return key + QLatin1Char(':') + value; }
    PerformanceFormatFields performanceFields() const override;
    void writeJsonFields(JsonWriter &w) const override;
};

} // namespace gcm
