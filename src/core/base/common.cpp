#include "core/common.h"
#include "core/constants.h"
#include "core/platform.h"
#include "core/strings.h"

namespace gcm {

namespace {
QString withParam(const QString &message, const char *name)
{
    return QStringLiteral("%1 (Parameter '%2')").arg(message, QLatin1String(name));
}

} // namespace

namespace EnsureArgument {

void notNullOrEmpty(const OptString &arg, const char *name)
{
    if (!arg) {
        throw ArgumentNullException(withParam(QStringLiteral("Value cannot be null."), name));
    }
    notNullOrEmpty(*arg, name);
}

void notNullOrEmpty(const QString &arg, const char *name)
{
    if (arg.isEmpty()) {
        throw ArgumentException(withParam(QStringLiteral("Argument cannot be empty."), name));
    }
}

void notNullOrWhiteSpace(const OptString &arg, const char *name)
{
    if (!arg) {
        throw ArgumentNullException(withParam(QStringLiteral("Value cannot be null."), name));
    }
    notNullOrWhiteSpace(*arg, name);
}

void notNullOrWhiteSpace(const QString &arg, const char *name)
{
    if (isNullOrWhiteSpace(arg)) {
        throw ArgumentException(withParam(QStringLiteral("Argument cannot be empty or white space."), name));
    }
}

void absoluteUri(const QUrl &arg, const char *name)
{
    if (arg.isRelative() || !arg.isValid()) {
        throw ArgumentException(withParam(QStringLiteral("Argument must be an absolute URI."), name));
    }
}

void positiveOrZero(int arg, const char *name)
{
    if (arg < 0) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be positive or zero (non-negative)."), name));
    }
}

void positive(int arg, const char *name)
{
    if (arg <= 0) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be positive."), name));
    }
}

void negativeOrZero(int arg, const char *name)
{
    if (arg > 0) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be negative or zero (non-positive)."), name));
    }
}

void negative(int arg, const char *name)
{
    if (arg >= 0) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be negative."), name));
    }
}

void inRange(int arg, const char *name, int lower, int upper, bool lowerInclusive, bool upperInclusive)
{
    if (lowerInclusive && arg < lower) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be greater than or equal to %1.").arg(lower), name));
    }
    if (!lowerInclusive && arg <= lower) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be strictly greater than %1.").arg(lower), name));
    }
    if (upperInclusive && arg > upper) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be less than or equal to %1.").arg(upper), name));
    }
    if (!upperInclusive && arg >= upper) {
        throw ArgumentOutOfRangeException(withParam(QStringLiteral("Argument must be strictly less than %1.").arg(upper), name));
    }
}

} // namespace EnsureArgument

void CancellationToken::cancel() const
{
    if (m_state->cancelled) {
        return;
    }
    m_state->cancelled = true;
    const auto callbacks = std::exchange(m_state->callbacks, {});
    for (const auto &cb : callbacks) {
        cb();
    }
}

void CancellationToken::throwIfCancellationRequested() const
{
    if (m_state->cancelled) {
        throw OperationCanceledException(QStringLiteral("The operation was canceled."));
    }
}

void CancellationToken::onCancel(std::function<void()> cb) const
{
    if (m_state->cancelled) {
        cb();
    } else {
        m_state->callbacks.append(std::move(cb));
    }
}

namespace Constants {

QString gcmInformationalVersion()
{
    return QStringLiteral(GCM_VERSION_INFORMATIONAL);
}

QString gcmVersion()
{
    return QStringLiteral(GCM_VERSION_STRING);
}

QString getHttpUserAgent()
{
    const PlatformInformation info = PlatformUtils::getPlatformInformation();
    return QStringLiteral("Git-Credential-Manager/%1 (%2; %3) %4")
        .arg(gcmVersion(), info.operatingSystemType, info.cpuArchitecture, info.runtimeVersion);
}

} // namespace Constants

} // namespace gcm
