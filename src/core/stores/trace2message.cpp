// Port of Tracing/Trace2Message.cs
#include "core/trace2message.h"

#include "core/strings.h"
#include "core/trace.h"

#include <QDateTime>
#include <QTimeZone>
#include <charconv>
#include <chrono>

namespace gcm {

// =============================================================================================
// Time and numbers
// =============================================================================================

qint64 Trace2Time::now()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {
void splitUsec(qint64 usec, qint64 &secs, int &micros)
{
    secs = usec / 1000000;
    qint64 rem = usec % 1000000;
    if (rem < 0) {
        rem += 1000000;
        --secs;
    }
    micros = int(rem);
}
} // namespace

QString Trace2Time::toJson(qint64 usec)
{
    qint64 secs;
    int micros;
    splitUsec(usec, secs, micros);
    QString text = QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    if (micros > 0) {
        QString fraction = QStringLiteral("%1").arg(micros, 6, 10, QLatin1Char('0'));
        while (fraction.endsWith(QLatin1Char('0'))) {
            fraction.chop(1);
        }
        text += QLatin1Char('.') + fraction;
    }
    return text + QStringLiteral("+00:00");
}

QString Trace2Time::toLocalTimeOfDay(qint64 usec)
{
    qint64 secs;
    int micros;
    splitUsec(usec, secs, micros);
    return QDateTime::fromSecsSinceEpoch(secs).toLocalTime().toString(QStringLiteral("HH:mm:ss")) +
           QStringLiteral(".%1").arg(micros, 6, 10, QLatin1Char('0'));
}

QString formatDouble(double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    QString text = QString::fromLatin1(buffer, result.ptr - buffer);
    text.replace(QLatin1Char('e'), QLatin1Char('E'));
    return text;
}

QString jsonString(const QString &value)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : value) {
        const ushort u = c.unicode();
        switch (u) {
        case '"': out += QLatin1String("\\\""); break;
        case '\\': out += QLatin1String("\\\\"); break;
        case '\b': out += QLatin1String("\\b"); break;
        case '\f': out += QLatin1String("\\f"); break;
        case '\n': out += QLatin1String("\\n"); break;
        case '\r': out += QLatin1String("\\r"); break;
        case '\t': out += QLatin1String("\\t"); break;
        default:
            // JavaScriptEncoder.Default: controls, HTML-sensitive characters and non-ASCII are escaped
            if (u < 0x20 || u >= 0x7F || u == '<' || u == '>' || u == '&' || u == '\'' || u == '+' || u == '`') {
                out += QLatin1String("\\u");
                out += QString::number(u, 16).rightJustified(4, QLatin1Char('0')).toUpper();
            } else {
                out += c;
            }
        }
    }
    return out + QLatin1Char('"');
}

// =============================================================================================
// PerformanceFormatFields
// =============================================================================================

namespace {

QString getSpan(QString data, int beginPadding, int endPadding, int size)
{
    const int paddingTotal = beginPadding + endPadding;
    const int dataLimit = size - paddingTotal;
    const int sizeDifference = dataLimit - int(data.size());
    if (sizeDifference <= 0) {
        bool isNumber = false;
        data.toDouble(&isNumber);
        if (isNumber) {
            // No padding for values taking the entire span; less leading padding for large values that don't
            if (std::abs(sizeDifference) >= paddingTotal) {
                beginPadding = 0;
                endPadding = 0;
            } else {
                beginPadding += sizeDifference;
            }
        } else {
            data = data.left(dataLimit);
        }
    }
    if (data.size() < dataLimit) {
        endPadding += sizeDifference;
    }
    return QString(std::max(0, beginPadding), QLatin1Char(' ')) + data + QString(std::max(0, endPadding), QLatin1Char(' '));
}

} // namespace

QString PerformanceFormatFields::getRepoSpan(int repo) { return getSpan(QStringLiteral("r%1").arg(repo), 1, 2, 5); }
QString PerformanceFormatFields::getTimeSpan(double time) { return getSpan(QString::number(time, 'f', 6), 2, 1, 11); }
QString PerformanceFormatFields::getCategorySpan(const QString &category) { return getSpan(category, 1, 1, 13); }

QString PerformanceFormatFields::toString() const
{
    if (empty) {
        return QStringLiteral("|     |           |           |             ");
    }
    QString s = QStringLiteral("|");
    s += repo ? getRepoSpan(*repo) : QStringLiteral("     ");
    s += QLatin1Char('|');
    s += elapsedTime ? getTimeSpan(*elapsedTime) : QStringLiteral("           ");
    s += QLatin1Char('|');
    s += relativeTime ? getTimeSpan(*relativeTime) : QStringLiteral("           ");
    s += QLatin1Char('|');
    s += category ? getCategorySpan(*category) : QStringLiteral("             ");
    return s;
}

// =============================================================================================
// Trace2Message
// =============================================================================================

namespace {

QString eventJsonName(Trace2Event e)
{
    switch (e) {
        case Trace2Event::Version: return QStringLiteral("version");
        case Trace2Event::Start: return QStringLiteral("start");
        case Trace2Event::Exit: return QStringLiteral("exit");
        case Trace2Event::ChildStart: return QStringLiteral("child_start");
        case Trace2Event::ChildExit: return QStringLiteral("child_exit");
        case Trace2Event::Error: return QStringLiteral("error");
        case Trace2Event::RegionEnter: return QStringLiteral("region_enter");
        case Trace2Event::RegionLeave: return QStringLiteral("region_leave");
        case Trace2Event::ThreadStart: return QStringLiteral("thread_start");
        case Trace2Event::ThreadExit: return QStringLiteral("thread_exit");
        case Trace2Event::Data: return QStringLiteral("data");
        case Trace2Event::DataJson: return QStringLiteral("data_json");
        case Trace2Event::CommandName: return QStringLiteral("cmd_name");
    }
    return QString();
}

/// Event.ToString().ToSnakeCase() of the text formats (note "command_name", unlike the JSON "cmd_name").
QString eventTextName(Trace2Event e)
{
    return e == Trace2Event::CommandName ? QStringLiteral("command_name") : eventJsonName(e);
}

QString processClassName(Trace2ProcessClass c)
{
    switch (c) {
    case Trace2ProcessClass::None: return QStringLiteral("none");
    case Trace2ProcessClass::UiHelper: return QStringLiteral("ui_helper");
    case Trace2ProcessClass::Git: return QStringLiteral("git");
    case Trace2ProcessClass::Other: return QStringLiteral("other");
    }
    return QString();
}

} // namespace

void Trace2Message::JsonWriter::key(const char *name)
{
    if (m_json.size() > 1) {
        m_json += QLatin1Char(',');
    }
    m_json += QLatin1Char('"') + QLatin1String(name) + QStringLiteral("\":");
}
void Trace2Message::JsonWriter::string(const char *name, const QString &value)
{
    key(name);
    m_json += jsonString(value);
}
void Trace2Message::JsonWriter::integer(const char *name, qint64 value)
{
    key(name);
    m_json += QString::number(value);
}
void Trace2Message::JsonWriter::number(const char *name, double value)
{
    key(name);
    m_json += formatDouble(value);
}
void Trace2Message::JsonWriter::boolean(const char *name, bool value)
{
    key(name);
    m_json += value ? QStringLiteral("true") : QStringLiteral("false");
}
void Trace2Message::JsonWriter::raw(const char *name, const QString &json)
{
    key(name);
    m_json += json;
}
void Trace2Message::JsonWriter::stringArray(const char *name, const QStringList &values)
{
    key(name);
    QStringList items;
    for (const QString &v : values) {
        items.append(jsonString(v));
    }
    m_json += QLatin1Char('[') + items.join(QLatin1Char(',')) + QLatin1Char(']');
}

QString Trace2Message::toJson() const
{
    JsonWriter w;
    w.string("event", eventJsonName(event));
    w.string("sid", sid);
    w.string("thread", thread);
    // Dates are written unescaped by System.Text.Json (no "\u002B" for the offset sign)
    w.raw("time", QLatin1Char('"') + Trace2Time::toJson(time) + QLatin1Char('"'));
    w.string("file", file);
    w.integer("line", line);
    w.integer("depth", depth);
    writeJsonFields(w);
    return w.result();
}

QString Trace2Message::source() const
{
    // file:line, truncated from the left to the column width
    constexpr int SourceColumnMaxWidth = 23;
    const QString s = QStringLiteral("%1:%2").arg(file).arg(line);
    return s.size() > SourceColumnMaxWidth ? TraceUtils::formatSource(s, SourceColumnMaxWidth) : s;
}

QString Trace2Message::toNormalString() const
{
    // [<time> SP <filename>:<line> SP+] <event-name> [[SP] <event-message>] LF
    return QStringLiteral("%1 %2 %3 %4")
        .arg(Trace2Time::toLocalTimeOfDay(time), source().leftJustified(33), eventTextName(event), eventMessage(Trace2FormatTarget::Normal));
}

QString Trace2Message::toPerformanceString() const
{
    // [<time> SP <filename>:<line> SP+ BAR SP] d<depth> SP BAR SP <thread-name> SP+ BAR SP <event-name> SP+
    //     BAR SP [r<repo-id>] SP+ BAR SP [<t_abs>] SP+ BAR SP [<t_rel>] SP+ BAR SP [<category>] SP+
    //     BAR SP DOTS* <perf-event-message> LF
    return QStringLiteral("%1 %2| d%3 | %4 | %5 %6 | %7")
        .arg(Trace2Time::toLocalTimeOfDay(time), source().leftJustified(29), QString::number(depth), thread.leftJustified(24),
             eventTextName(event).leftJustified(12), performanceFields().toString(), eventMessage(Trace2FormatTarget::Performance));
}

// =============================================================================================
// Messages
// =============================================================================================

namespace {

PerformanceFormatFields fields(std::optional<int> repo, std::optional<double> elapsed, std::optional<double> relative, OptString category)
{
    return PerformanceFormatFields{false, repo, elapsed, relative, std::move(category)};
}
} // namespace

void VersionMessage::writeJsonFields(JsonWriter &w) const
{
    w.string("evt", evt);
    w.string("exe", exe);
}

PerformanceFormatFields StartMessage::performanceFields() const
{
    return fields(std::nullopt, elapsedTime, std::nullopt, std::nullopt);
}

void StartMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.stringArray("argv", argv);
}

QString ExitMessage::eventMessage(Trace2FormatTarget) const
{
    return QStringLiteral("elapsed:%1 code:%2").arg(formatDouble(elapsedTime)).arg(code);
}

PerformanceFormatFields ExitMessage::performanceFields() const
{
    return fields(std::nullopt, elapsedTime, std::nullopt, std::nullopt);
}

void ExitMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.integer("code", code);
}

QString ChildStartMessage::eventMessage(Trace2FormatTarget target) const
{
    return (target == Trace2FormatTarget::Performance ? QStringLiteral("[ch%1]") : QStringLiteral("[%1]")).arg(id) +
           QLatin1Char(' ') + argv.join(QLatin1Char(' '));
}

PerformanceFormatFields ChildStartMessage::performanceFields() const
{
    return fields(std::nullopt, elapsedTime, std::nullopt, std::nullopt);
}

void ChildStartMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.stringArray("argv", argv);
    w.integer("child_id", id);
    w.string("child_class", processClassName(classification));
    w.boolean("use_shell", useShell);
}

QString ChildExitMessage::eventMessage(Trace2FormatTarget target) const
{
    return (target == Trace2FormatTarget::Performance ? QStringLiteral("[ch%1]") : QStringLiteral("[%1]")).arg(id) +
           QStringLiteral(" pid:%1 code:%2 elapsed:%3").arg(pid).arg(code).arg(formatDouble(relativeTime));
}

PerformanceFormatFields ChildExitMessage::performanceFields() const
{
    return fields(std::nullopt, elapsedTime, relativeTime, std::nullopt);
}

void ChildExitMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.number("t_rel", relativeTime);
    w.integer("child_id", id);
    w.integer("pid", pid);
    w.integer("code", code);
}

QString CommandNameMessage::eventMessage(Trace2FormatTarget) const
{
    return isNullOrEmpty(hierarchy) ? name : QStringLiteral("%1 (%2)").arg(name, *hierarchy);
}
void CommandNameMessage::writeJsonFields(JsonWriter &w) const
{
    w.string("name", name);
    if (hierarchy) {
        w.string("hierarchy", *hierarchy);
    }
}

QString ThreadExitMessage::eventMessage(Trace2FormatTarget) const
{
    return QStringLiteral("elapsed:%1").arg(formatDouble(relativeTime));
}

PerformanceFormatFields ThreadExitMessage::performanceFields() const
{
    return fields(std::nullopt, std::nullopt, relativeTime, std::nullopt);
}

void ThreadExitMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_rel", relativeTime);
}

void ErrorMessage::writeJsonFields(JsonWriter &w) const
{
    w.string("msg", message);
    w.string("fmt", parameterizedMessage);
}

void RegionMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.integer("repo", repo);
    w.integer("nesting", nesting);
    w.string("category", category);
    w.string("label", label);
    w.string("msg", message);
}

PerformanceFormatFields RegionEnterMessage::performanceFields() const
{
    return fields(repo, elapsedTime, std::nullopt, category);
}

PerformanceFormatFields RegionLeaveMessage::performanceFields() const
{
    return fields(repo, elapsedTime, relativeTime, category);
}

void RegionLeaveMessage::writeJsonFields(JsonWriter &w) const
{
    RegionMessage::writeJsonFields(w);
    w.number("t_rel", relativeTime);
}

PerformanceFormatFields DataMessage::performanceFields() const
{
    return fields(repo, elapsedTime, relativeTime, category);
}

void DataMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.number("t_rel", relativeTime);
    w.integer("repo", repo);
    w.integer("nesting", nesting);
    w.string("category", category);
    w.string("key", key);
    w.string("value", value);
}

PerformanceFormatFields DataJsonMessage::performanceFields() const
{
    return fields(repo, elapsedTime, relativeTime, category);
}

void DataJsonMessage::writeJsonFields(JsonWriter &w) const
{
    w.number("t_abs", elapsedTime);
    w.number("t_rel", relativeTime);
    w.integer("repo", repo);
    w.integer("nesting", nesting);
    w.string("category", category);
    w.string("key", key);
    w.raw("value", value);
}

} // namespace gcm
