// Port of StringExtensions.cs, ConvertUtils.cs, UriExtensions.cs and small helpers.
#pragma once

#include "core/common.h"
#include "core/dictionary.h"

namespace gcm {

// --- StringExtensions ---------------------------------------------------------------------
bool isNullOrWhiteSpace(const QString &s);
bool isNullOrWhiteSpace(const OptString &s);
bool isNullOrEmpty(const OptString &s);
bool equalsIgnoreCase(const QString &a, const QString &b);

/// "1", "true", "yes", "on" (case-insensitive) => true.
bool isTruthy(const OptString &s);
/// "0", "false", "no", "off" (case-insensitive) => true.
bool isFalsey(const OptString &s);
std::optional<bool> toBooleany(const OptString &s);
bool toBooleanyOrDefault(const OptString &s, bool defaultValue);

QString truncateFromIndexOf(const QString &s, QChar c);
QString truncateFromLastIndexOf(const QString &s, QChar c);
QString trimUntilIndexOf(const QString &s, QChar c);
QString trimUntilIndexOf(const QString &s, const QString &value, Qt::CaseSensitivity cs = Qt::CaseSensitive);
QString trimUntilLastIndexOf(const QString &s, QChar c);
QString trimUntilLastIndexOf(const QString &s, const QString &value, Qt::CaseSensitivity cs = Qt::CaseSensitive);
QString trimMiddle(const QString &s, const QString &value, Qt::CaseSensitivity cs = Qt::CaseSensitive);
/// Wraps `text` to `width` columns the way Spectre.Console renders plain text: words move to the next line when they
/// don't fit, white space stays at the end of the line it follows, words longer than a line are split, explicit line
/// breaks are kept.
QString wrapText(const QString &text, int width);
QString toSnakeCase(const QString &s);

// --- ConvertUtils -------------------------------------------------------------------------
bool tryToInt32(const OptString &s, int &value);

// --- UriExtensions ------------------------------------------------------------------------
/// Render like System.Uri.AbsoluteUri / ToString(): scheme+host lower-cased, an empty path becomes "/",
/// default ports omitted. Use this (never QUrl::toString directly) wherever upstream prints or stores a Uri.
QString absoluteUri(const QUrl &uri);
/// Parse like `new Uri(str)` / Uri.TryCreate(str, UriKind.Absolute): returns nullopt unless absolute with a scheme.
std::optional<QUrl> tryCreateUri(const OptString &s);
/// System.Uri.IsLoopback
bool isLoopback(const QUrl &uri);
/// System.Uri.IsDefaultPort (no explicit port, or the scheme default)
bool isDefaultPort(const QUrl &uri);
/// Uri.EscapeDataString
QString escapeDataString(const QString &s);

StringDictionary parseQueryString(const QString &queryString);
StringDictionary getQueryParameters(const QUrl &uri);
bool tryGetUserInfo(const QUrl &uri, OptString &userName, OptString &password);
OptString getUserName(const QUrl &uri);
QUrl withoutUserInfo(const QUrl &uri);
/// Most-specific to least-specific Git configuration URL scopes for the URI
/// (see comment in Settings::getSettingValues).
QStringList getGitConfigurationScopes(const QUrl &uri);

// --- [Flags] enums ------------------------------------------------------------------------
/// A named value of a .NET [Flags] enum, listed in declaration order.
struct EnumName
{
    int value;
    const char *name;
};
/// Enum.ToString of a flags value: "None", "Basic, Pat", "Pat, OAuth" (largest named values that fit, printed in
/// ascending order); the number when the bits cannot be expressed with the names.
QString formatFlagsEnum(int value, const QList<EnumName> &names);
/// Enum.TryParse(value, ignoreCase: true): comma-separated names or numbers, OR-ed together.
bool tryParseFlagsEnum(const QString &value, const QList<EnumName> &names, int &result);

// --- misc ---------------------------------------------------------------------------------
/// AuthenticationBase.QuoteCmdArg
QString quoteCmdArg(const QString &s);
/// Split a command line into arguments honouring double quotes and backslash escapes (inverse of quoteCmdArg).
QStringList splitCommandLine(const QString &args);
/// Base64Url without padding (RFC 7636 appendix A).
QString base64UrlEncode(const QByteArray &data);
QByteArray base64UrlDecode(const QString &s);

/// Comparer for Git configuration keys: section and property are case-insensitive, the scope
/// (middle, possibly containing dots) is case-sensitive. Returns the normalised key usable as a hash key.
QString normalizeGitConfigKey(const QString &key);
bool gitConfigKeyEquals(const QString &a, const QString &b);
/// GitConfigurationKeyComparer.TrySplit: "section[.scope].property" into its parts (the scope may contain dots).
bool trySplitGitConfigKey(const QString &key, QString &section, OptString &scope, QString &property);

} // namespace gcm
