// Port of GitCapabilities.cs
#pragma once

#include "core/common.h"

namespace gcm {

enum class GitCapabilities : int {
    None = 0,
    State = 1 << 0,
    AuthType = 1 << 1,
};

constexpr GitCapabilities operator|(GitCapabilities a, GitCapabilities b) { return GitCapabilities(int(a) | int(b)); }
constexpr GitCapabilities operator&(GitCapabilities a, GitCapabilities b) { return GitCapabilities(int(a) & int(b)); }
constexpr GitCapabilities &operator|=(GitCapabilities &a, GitCapabilities b) { return a = a | b; }
constexpr bool hasFlag(GitCapabilities value, GitCapabilities flag) { return (int(value) & int(flag)) == int(flag) && flag != GitCapabilities::None; }

namespace GitCapabilitiesUtils {
    /// Unknown / blank names map to None.
    GitCapabilities parseName(const OptString &name);
    /// Throws ArgumentException for None, ArgumentOutOfRangeException for combined / unknown values.
    QString toProtocolName(GitCapabilities capability);
    QStringList toProtocolNames(GitCapabilities capabilities);
    /// Human readable rendering for trace output, like .NET's flags enum ToString(): "None", "State", "State, AuthType".
    QString toString(GitCapabilities capabilities);
} // namespace GitCapabilitiesUtils

namespace GitStateValidation {
    bool isValidKey(const OptString &key);
    bool isValidValue(const OptString &value);
    /// Throw ArgumentException / ArgumentNullException on invalid input.
    void validateKey(const OptString &key);
    void validateValue(const OptString &value);
} // namespace GitStateValidation

} // namespace gcm
