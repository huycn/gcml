#include "core/gitcapabilities.h"
#include "core/constants.h"
#include "core/strings.h"

namespace gcm {

namespace GitCapabilitiesUtils {

GitCapabilities parseName(const OptString &name)
{
    if (isNullOrWhiteSpace(name)) {
        return GitCapabilities::None;
    }
    const QString lower = name->toLower();
    if (lower == Constants::CredentialProtocol::StateKey) {
        return GitCapabilities::State;
    }
    if (lower == Constants::CredentialProtocol::AuthTypeKey) {
        return GitCapabilities::AuthType;
    }
    return GitCapabilities::None;
}

QString toProtocolName(GitCapabilities capability)
{
    switch (capability) {
    case GitCapabilities::State:
        return Constants::CredentialProtocol::StateKey;
    case GitCapabilities::AuthType:
        return Constants::CredentialProtocol::AuthTypeKey;
    case GitCapabilities::None:
        throw ArgumentException(QStringLiteral("Cannot render the None capability to a protocol name. (Parameter 'capability')"));
    }
    throw ArgumentOutOfRangeException(QStringLiteral("No protocol name mapping is defined for the given capability. (Parameter 'capability')"));
}

static const GitCapabilities allFlags[] = {GitCapabilities::State, GitCapabilities::AuthType};

QStringList toProtocolNames(GitCapabilities capabilities)
{
    QStringList names;
    for (GitCapabilities flag : allFlags) {
        if (hasFlag(capabilities, flag)) {
            names.append(toProtocolName(flag));
        }
    }
    return names;
}

QString toString(GitCapabilities capabilities)
{
    QStringList names;
    if (hasFlag(capabilities, GitCapabilities::State)) {
        names.append(QStringLiteral("State"));
    }
    if (hasFlag(capabilities, GitCapabilities::AuthType)) {
        names.append(QStringLiteral("AuthType"));
    }
    return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
}

} // namespace GitCapabilitiesUtils

namespace GitStateValidation {

namespace {
const QChar LF = QLatin1Char('\n');
const QChar NUL = QChar(0);
const QChar EQ = QLatin1Char('=');
} // namespace

bool isValidKey(const OptString &key)
{
    if (isNullOrWhiteSpace(key)) {
        return false;
    }
    for (QChar c : *key) {
        if (c == EQ || c == LF || c == NUL) {
            return false;
        }
    }
    // The "gcm." prefix is reserved and added automatically on emit.
    return !key->startsWith(Constants::CredentialProtocol::GcmStatePrefix);
}

bool isValidValue(const OptString &value)
{
    if (!value) {
        return false;
    }
    for (QChar c : *value) {
        if (c == LF || c == NUL) {
            return false;
        }
    }
    return true;
}

void validateKey(const OptString &key)
{
    if (isNullOrWhiteSpace(key)) {
        throw ArgumentException(QStringLiteral("State key cannot be null or whitespace. (Parameter 'key')"));
    }
    for (QChar c : *key) {
        if (c == EQ || c == LF || c == NUL) {
            throw ArgumentException(QStringLiteral("State key cannot contain '=', newline, or NUL characters. (Parameter 'key')"));
        }
    }
    if (key->startsWith(Constants::CredentialProtocol::GcmStatePrefix)) {
        throw ArgumentException(
            QStringLiteral("State key cannot start with '%1'; the prefix is reserved and added automatically when state is emitted. (Parameter 'key')").arg(Constants::CredentialProtocol::GcmStatePrefix)
        );
    }
}

void validateValue(const OptString &value)
{
    if (!value) {
        throw ArgumentNullException(QStringLiteral("State value cannot be null. (Parameter 'value')"));
    }
    for (QChar c : *value) {
        if (c == LF || c == NUL) {
            throw ArgumentException(QStringLiteral("State value cannot contain newline or NUL characters. (Parameter 'value')"));
        }
    }
}

} // namespace GitStateValidation

} // namespace gcm
