// Common vocabulary types shared by every module of the C++/Qt port of Git Credential Manager.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace gcm {

/// A string that may be absent. Used wherever the .NET code distinguishes `null` from `""`.
using OptString = std::optional<QString>;

inline QString valueOr(const OptString &s, const QString &fallback = QString()) { return s ? *s : fallback; }

// ---------------------------------------------------------------------------------------------
// Exceptions (mirror of the .NET exception types the original code relies on)
// ---------------------------------------------------------------------------------------------

class Exception : public std::exception
{
public:
    explicit Exception(QString message, std::shared_ptr<const Exception> inner = nullptr)
        : m_message(std::move(message)), m_utf8(m_message.toUtf8()), m_inner(std::move(inner)) {}

    const QString &message() const noexcept { return m_message; }
    const char *what() const noexcept override { return m_utf8.constData(); }
    const std::shared_ptr<const Exception> &inner() const noexcept { return m_inner; }

    /// Short type name used by tracing / TRACE2 error events.
    virtual const char *typeName() const noexcept { return "Exception"; }

private:
    QString m_message;
    QByteArray m_utf8;
    std::shared_ptr<const Exception> m_inner;
};

#define GCM_DECLARE_EXCEPTION(Name, Base)                                                          \
    class Name : public Base                                                                       \
    {                                                                                              \
    public:                                                                                        \
        using Base::Base;                                                                          \
        const char *typeName() const noexcept override { return #Name; }                           \
    };

GCM_DECLARE_EXCEPTION(ArgumentException, Exception)
GCM_DECLARE_EXCEPTION(ArgumentNullException, ArgumentException)
GCM_DECLARE_EXCEPTION(ArgumentOutOfRangeException, ArgumentException)
GCM_DECLARE_EXCEPTION(InvalidOperationException, Exception)
GCM_DECLARE_EXCEPTION(NotSupportedException, Exception)
GCM_DECLARE_EXCEPTION(IOException, Exception)
GCM_DECLARE_EXCEPTION(KeyNotFoundException, Exception)
GCM_DECLARE_EXCEPTION(TimeoutException, Exception)
/// The operation was cancelled (e.g. the user dismissed a dialog). `get` answers this with `quit=1`.
GCM_DECLARE_EXCEPTION(OperationCanceledException, Exception)
/// The user pressed Ctrl+C during an interactive terminal prompt. Exit code 130, no "fatal:" banner.
GCM_DECLARE_EXCEPTION(InterruptedException, Exception)
/// A network level failure (DNS, TLS, connection refused, ...). HTTP error statuses do NOT throw.
GCM_DECLARE_EXCEPTION(HttpRequestException, Exception)

/// Failure of a native/system call, carrying an error code (printed as `[0x..]`).
class InteropException : public Exception
{
public:
    InteropException(QString message, int errorCode, std::shared_ptr<const Exception> inner = nullptr)
        : Exception(std::move(message), std::move(inner)), m_errorCode(errorCode) {}
    int errorCode() const noexcept { return m_errorCode; }
    const char *typeName() const noexcept override { return "InteropException"; }

private:
    int m_errorCode;
};

// ---------------------------------------------------------------------------------------------
// Argument validation (EnsureArgument in .NET)
// ---------------------------------------------------------------------------------------------
namespace EnsureArgument {
    void notNullOrEmpty(const OptString &arg, const char *name);
    void notNullOrEmpty(const QString &arg, const char *name);
    void notNullOrWhiteSpace(const OptString &arg, const char *name);
    void notNullOrWhiteSpace(const QString &arg, const char *name);
    void absoluteUri(const QUrl &arg, const char *name);
    void positiveOrZero(int arg, const char *name);
    void positive(int arg, const char *name);
    void negativeOrZero(int arg, const char *name);
    void negative(int arg, const char *name);
    void inRange(int arg, const char *name, int lower, int upper, bool lowerInclusive = true, bool upperInclusive = true);
    template <typename T> void notNull(const T &ptr, const char *name)
    {
        if (!ptr) {
            throw ArgumentNullException(QStringLiteral("Value cannot be null. (Parameter '%1')").arg(QLatin1String(name)));
        }
    }
} // namespace EnsureArgument

/// Minimal cooperative cancellation primitive (CancellationToken in .NET).
/// Copies share state. A default constructed token can never be cancelled unless `cancel()` is called on a copy.
class CancellationToken
{
public:
    CancellationToken() : m_state(std::make_shared<State>()) {}
    void cancel() const;
    bool isCancellationRequested() const { return m_state->cancelled; }
    /// Throws OperationCanceledException if cancellation was requested.
    void throwIfCancellationRequested() const;
    /// Registers a callback run (once) on cancel(); runs immediately if already cancelled.
    void onCancel(std::function<void()> cb) const;

private:
    struct State
    {
        bool cancelled = false;
        QList<std::function<void()>> callbacks;
    };
    std::shared_ptr<State> m_state;
};

} // namespace gcm
