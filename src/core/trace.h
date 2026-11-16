// Application tracing (GCM_TRACE). Port of Tracing/Trace.cs + TraceUtils.cs.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"
#include "core/streams.h"

#include <source_location>

namespace gcm {

class ITrace
{
public:
    using Loc = std::source_location;
    virtual ~ITrace() = default;

    virtual bool hasListeners() const = 0;
    virtual bool isSecretTracingEnabled() const = 0;
    virtual void setSecretTracingEnabled(bool value) = 0;
    /// The listener is shared; it is flushed when the trace is destroyed.
    virtual void addListener(std::shared_ptr<TextWriter> listener) = 0;
    virtual void flush() = 0;

    virtual void writeException(const std::exception &ex, const Loc &loc = Loc::current()) = 0;
    virtual void writeDictionary(const StringDictionary &dict, const Loc &loc = Loc::current()) = 0;
    /// Values whose key is in `secretKeys` are masked unless secret tracing is enabled.
    virtual void writeDictionarySecrets(const MultiDictionary &dict, const QStringList &secretKeys, Qt::CaseSensitivity keyCs = Qt::CaseSensitive, const Loc &loc = Loc::current()) = 0;
    virtual void writeLine(const QString &message, const Loc &loc = Loc::current()) = 0;
    /// `format` uses .NET style positional placeholders `{0}`, `{1}`...; each is replaced by the matching secret
    /// or by "********" when secret tracing is disabled.
    virtual void writeLineSecrets(const QString &format, const QStringList &secrets, const Loc &loc = Loc::current()) = 0;
};

class Trace : public ITrace
{
public:
    Trace();
    ~Trace() override;

    bool hasListeners() const override;
    bool isSecretTracingEnabled() const override;
    void setSecretTracingEnabled(bool value) override;
    void addListener(std::shared_ptr<TextWriter> listener) override;
    void flush() override;
    void writeException(const std::exception &ex, const Loc &loc = Loc::current()) override;
    void writeDictionary(const StringDictionary &dict, const Loc &loc = Loc::current()) override;
    void writeDictionarySecrets(const MultiDictionary &dict, const QStringList &secretKeys, Qt::CaseSensitivity keyCs = Qt::CaseSensitive, const Loc &loc = Loc::current()) override;
    void writeLine(const QString &message, const Loc &loc = Loc::current()) override;
    void writeLineSecrets(const QString &format, const QStringList &secrets, const Loc &loc = Loc::current()) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

namespace TraceUtils {
    /// Formats the `file:line` source column, truncating the path from the left ("...") to fit `sourceColumnMaxWidth`.
    QString formatSource(const QString &source, int sourceColumnMaxWidth);
} // namespace TraceUtils

} // namespace gcm
