#include "core/strings.h"

#include <QHostAddress>

namespace gcm {

// --- StringExtensions ---------------------------------------------------------------------

bool isNullOrWhiteSpace(const QString &s)
{
    for (QChar c : s) {
        if (!c.isSpace()) {
            return false;
        }
    }
    return true;
}

bool isNullOrWhiteSpace(const OptString &s)
{
    return !s || isNullOrWhiteSpace(*s);
}

bool isNullOrEmpty(const OptString &s)
{
    return !s || s->isEmpty();
}

bool equalsIgnoreCase(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

bool isTruthy(const OptString &s)
{
    if (!s) {
        return false;
    }
    return equalsIgnoreCase(*s, QStringLiteral("true")) || *s == QLatin1String("1")
        || equalsIgnoreCase(*s, QStringLiteral("on")) || equalsIgnoreCase(*s, QStringLiteral("yes"));
}

bool isFalsey(const OptString &s)
{
    if (!s) {
        return false;
    }
    return equalsIgnoreCase(*s, QStringLiteral("false")) || *s == QLatin1String("0")
        || equalsIgnoreCase(*s, QStringLiteral("off")) || equalsIgnoreCase(*s, QStringLiteral("no"));
}

std::optional<bool> toBooleany(const OptString &s)
{
    if (isTruthy(s)) {
        return true;
    }
    if (isFalsey(s)) {
        return false;
    }
    return std::nullopt;
}

bool toBooleanyOrDefault(const OptString &s, bool defaultValue)
{
    return toBooleany(s).value_or(defaultValue);
}

QString truncateFromIndexOf(const QString &s, QChar c)
{
    const qsizetype i = s.indexOf(c);
    return i > -1 ? s.left(i) : s;
}

QString truncateFromLastIndexOf(const QString &s, QChar c)
{
    const qsizetype i = s.lastIndexOf(c);
    return i > -1 ? s.left(i) : s;
}

QString trimUntilIndexOf(const QString &s, QChar c)
{
    const qsizetype i = s.indexOf(c);
    return i > -1 ? s.mid(i + 1) : s;
}

QString trimUntilIndexOf(const QString &s, const QString &value, Qt::CaseSensitivity cs)
{
    const qsizetype i = s.indexOf(value, 0, cs);
    return i > -1 ? s.mid(i + value.size()) : s;
}

QString trimUntilLastIndexOf(const QString &s, QChar c)
{
    const qsizetype i = s.lastIndexOf(c);
    return i > -1 ? s.mid(i + 1) : s;
}

QString trimUntilLastIndexOf(const QString &s, const QString &value, Qt::CaseSensitivity cs)
{
    const qsizetype i = s.lastIndexOf(value, -1, cs);
    return i > -1 ? s.mid(i + value.size()) : s;
}

QString trimMiddle(const QString &s, const QString &value, Qt::CaseSensitivity cs)
{
    const qsizetype i = s.indexOf(value, 0, cs);
    return i > -1 ? s.left(i) + s.mid(i + value.size()) : s;
}

QString wrapText(const QString &text, int width)
{
    QStringList out;
    for (const QStringList lines = text.split(QLatin1Char('\n')); const QString &line : lines) {
        QString current;
        qsizetype i = 0;
        while (i < line.size()) {
            const bool space = line[i].isSpace();
            qsizetype j = i;
            while (j < line.size() && line[j].isSpace() == space) {
                ++j;
            }
            QString segment = line.mid(i, j - i);
            i = j;
            if (space) {
                // Trailing white space that does not fit is dropped at the line break
                if (current.size() + segment.size() <= width) {
                    current += segment;
                } else if (!current.isEmpty()) {
                    out.append(current);
                    current.clear();
                }
                continue;
            }
            if (!current.isEmpty() && current.size() + segment.size() > width) {
                out.append(current);
                current.clear();
            }
            while (segment.size() > width) {
                out.append(segment.left(width));
                segment = segment.mid(width);
            }
            current += segment;
        }
        out.append(current);
    }
    return out.join(QLatin1Char('\n'));
}

QString toSnakeCase(const QString &s)
{
    const qsizetype len = s.size();
    QString out;
    out.reserve(2 * len);
    for (qsizetype i = 0; i < len; ++i) {
        if (i > 0 && s[i].isUpper() && (s[i - 1].isLower() || (i < len - 1 && s[i + 1].isLower()))) {
            out += QLatin1Char('_');
        }
        out += s[i].toLower();
    }
    return out;
}

// --- ConvertUtils -------------------------------------------------------------------------

bool tryToInt32(const OptString &s, int &value)
{
    // Convert.ToInt32((object)null) is 0.
    if (!s) {
        value = 0;
        return true;
    }
    bool ok = false;
    const int v = s->trimmed().toInt(&ok);
    value = ok ? v : 0;
    return ok;
}

// --- Uri helpers --------------------------------------------------------------------------

namespace {

int defaultPortForScheme(const QString &scheme)
{
    if (scheme == QLatin1String("http") || scheme == QLatin1String("ws")) {
        return 80;
    }
    if (scheme == QLatin1String("https") || scheme == QLatin1String("wss")) {
        return 443;
    }
    if (scheme == QLatin1String("ftp")) {
        return 21;
    }
    return -1;
}

/// HttpUtility.UrlDecode / WebUtility.UrlDecode: '+' is a space, then percent-decoding as UTF-8.
QString urlDecode(const QString &s)
{
    QByteArray raw = s.toUtf8();
    raw.replace('+', ' ');
    return QString::fromUtf8(QByteArray::fromPercentEncoding(raw));
}

} // namespace

QString absoluteUri(const QUrl &uri)
{
    QUrl u = uri;
    const QString scheme = u.scheme();
    if (u.port() != -1 && u.port() == defaultPortForScheme(scheme)) {
        u.setPort(-1);
    }
    if (!u.host().isEmpty() && u.path().isEmpty()) {
        u.setPath(QStringLiteral("/"));
    }
    return QString::fromLatin1(u.toEncoded());
}

std::optional<QUrl> tryCreateUri(const OptString &s)
{
    if (isNullOrWhiteSpace(s)) {
        return std::nullopt;
    }
    const QUrl url(s->trimmed());
    if (!url.isValid() || url.isRelative() || url.scheme().isEmpty()) {
        return std::nullopt;
    }
    return url;
}

bool isLoopback(const QUrl &uri)
{
    const QString host = uri.host();
    if (host.isEmpty() || equalsIgnoreCase(host, QStringLiteral("localhost"))) {
        return true;
    }
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

bool isDefaultPort(const QUrl &uri)
{
    return uri.port() == -1 || uri.port() == defaultPortForScheme(uri.scheme());
}

QString escapeDataString(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

StringDictionary parseQueryString(const QString &queryString)
{
    StringDictionary dict;
    const QStringList queryParts = queryString.split(QLatin1Char('&'));
    for (const QString &queryPart : queryParts) {
        if (isNullOrWhiteSpace(queryPart)) {
            continue;
        }
        const QStringList parts = queryPart.split(QLatin1Char('='));
        dict.set(urlDecode(parts[0]), parts.size() > 1 ? urlDecode(parts[1]) : QString());
    }
    return dict;
}

StringDictionary getQueryParameters(const QUrl &uri)
{
    return parseQueryString(uri.query(QUrl::FullyEncoded));
}

bool tryGetUserInfo(const QUrl &uri, OptString &userName, OptString &password)
{
    userName.reset();
    password.reset();
    const QString userInfo = uri.userInfo(QUrl::FullyEncoded);
    if (isNullOrWhiteSpace(userInfo)) {
        return false;
    }
    const qsizetype colon = userInfo.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        userName = urlDecode(userInfo);
    } else {
        userName = urlDecode(userInfo.left(colon));
        password = urlDecode(userInfo.mid(colon + 1));
    }
    return true;
}

OptString getUserName(const QUrl &uri)
{
    OptString userName, password;
    return tryGetUserInfo(uri, userName, password) ? userName : std::nullopt;
}

QUrl withoutUserInfo(const QUrl &uri)
{
    if (uri.userInfo().isEmpty()) {
        return uri;
    }
    QUrl u = uri;
    u.setUserInfo(QString());
    return u;
}

QStringList getGitConfigurationScopes(const QUrl &uri)
{
    QStringList scopes;
    const QString schemeAndDelim = uri.scheme() + QStringLiteral("://");
    QString host = uri.host(QUrl::FullyEncoded);
    while (host.endsWith(QLatin1Char('/'))) {
        host.chop(1);
    }
    const QString port = isDefaultPort(uri) ? QString() : QStringLiteral(":%1").arg(uri.port());

    QString path = uri.path(QUrl::FullyEncoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    // Unfold the path by component, right-to-left
    while (!isNullOrWhiteSpace(path)) {
        scopes.append(schemeAndDelim + host + port + QLatin1Char('/') + path);
        const QString next = truncateFromLastIndexOf(path, QLatin1Char('/'));
        if (next == path) {
            break;
        }
        path = next;
    }

    // Hosts without a dot (e.g. "localhost") are emitted once.
    if (!isNullOrWhiteSpace(host) && !host.contains(QLatin1Char('.'))) {
        scopes.append(schemeAndDelim + host + port);
        return scopes;
    }

    // Unfold the host by sub-domain, left-to-right
    while (!isNullOrWhiteSpace(host)) {
        if (host.contains(QLatin1Char('.'))) { // Do not emit just the TLD
            scopes.append(schemeAndDelim + host + port);
        }
        const QString next = trimUntilIndexOf(host, QLatin1Char('.'));
        if (next == host) {
            break;
        }
        host = next;
    }
    return scopes;
}

// --- misc ---------------------------------------------------------------------------------

QString quoteCmdArg(const QString &s)
{
    static const QString needsQuoteChars = QStringLiteral("\" \\\n\r\t");
    bool needsQuotes = false;
    for (QChar c : s) {
        if (needsQuoteChars.contains(c)) {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return s;
    }
    QString escaped = s;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QStringList splitCommandLine(const QString &args)
{
    // Same rules .NET applies to ProcessStartInfo.Arguments on Unix (the Windows CommandLineToArgvW rules):
    // 2n backslashes + quote => n backslashes and the quote toggles quoting; 2n+1 backslashes + quote => n
    // backslashes and a literal quote; "" inside a quoted section => a literal quote.
    QStringList result;
    const qsizetype n = args.size();
    qsizetype i = 0;
    while (i < n) {
        while (i < n && (args[i] == QLatin1Char(' ') || args[i] == QLatin1Char('\t'))) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        QString current;
        bool inQuotes = false;
        while (i < n && (inQuotes || (args[i] != QLatin1Char(' ') && args[i] != QLatin1Char('\t')))) {
            qsizetype backslashes = 0;
            while (i < n && args[i] == QLatin1Char('\\')) {
                ++i;
                ++backslashes;
            }
            if (i < n && args[i] == QLatin1Char('"')) {
                current += QString(backslashes / 2, QLatin1Char('\\'));
                if (backslashes % 2 == 1) {
                    current += QLatin1Char('"');
                } else if (inQuotes && i + 1 < n && args[i + 1] == QLatin1Char('"')) {
                    current += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = !inQuotes;
                }
                ++i;
            } else {
                current += QString(backslashes, QLatin1Char('\\'));
                if (i < n && (inQuotes || (args[i] != QLatin1Char(' ') && args[i] != QLatin1Char('\t')))) {
                    current += args[i++];
                }
            }
        }
        result.append(current);
    }
    return result;
}

QString base64UrlEncode(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray base64UrlDecode(const QString &s)
{
    return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool trySplitGitConfigKey(const QString &key, QString &section, OptString &scope, QString &property)
{
    section.clear();
    scope.reset();
    property.clear();
    const qsizetype first = key.indexOf(QLatin1Char('.'));
    const qsizetype last = key.lastIndexOf(QLatin1Char('.'));
    if (first < 0) {
        return false;
    }
    section = key.left(first);
    property = key.mid(last + 1);
    if (first != last) {
        scope = key.mid(first + 1, last - first - 1);
    }
    return true;
}

QString normalizeGitConfigKey(const QString &key)
{
    // Blank keys cannot be split into section/property and all compare equal upstream.
    if (isNullOrWhiteSpace(key)) {
        return QString();
    }
    const qsizetype first = key.indexOf(QLatin1Char('.'));
    if (first < 0) {
        return key.toLower();
    }
    const qsizetype last = key.lastIndexOf(QLatin1Char('.'));
    if (first == last) {
        return key.toLower();
    }
    return key.left(first).toLower() + key.mid(first, last - first + 1) + key.mid(last + 1).toLower();
}

bool gitConfigKeyEquals(const QString &a, const QString &b)
{
    return normalizeGitConfigKey(a) == normalizeGitConfigKey(b);
}

// =============================================================================================
// [Flags] enums
// =============================================================================================

QString formatFlagsEnum(int value, const QList<EnumName> &names)
{
    if (value == 0) {
        for (const EnumName &n : names) {
            if (n.value == 0) {
                return QString::fromLatin1(n.name);
            }
        }
        return QStringLiteral("0");
    }
    // .NET flags formatting: take the largest named values that fit, print them in ascending order
    QList<EnumName> sorted = names;
    std::stable_sort(sorted.begin(), sorted.end(), [](const EnumName &a, const EnumName &b) { return a.value > b.value; });
    int remaining = value;
    QStringList parts;
    for (const EnumName &n : std::as_const(sorted)) {
        if (n.value != 0 && (remaining & n.value) == n.value) {
            parts.prepend(QString::fromLatin1(n.name));
            remaining &= ~n.value;
        }
    }
    return remaining != 0 ? QString::number(value) : parts.join(QStringLiteral(", "));
}

bool tryParseFlagsEnum(const QString &value, const QList<EnumName> &names, int &result)
{
    result = 0;
    if (isNullOrWhiteSpace(value)) {
        return false;
    }
    int parsed = 0;
    const QStringList parts = value.split(QLatin1Char(','));
    for (const QString &rawPart : parts) {
        const QString part = rawPart.trimmed();
        bool ok = false;
        const int number = part.toInt(&ok);
        if (ok) {
            parsed |= number;
            continue;
        }
        const auto it = std::find_if(names.begin(), names.end(), [&](const EnumName &n) {
            return part.compare(QLatin1String(n.name), Qt::CaseInsensitive) == 0;
        });
        if (it == names.end()) {
            return false;
        }
        parsed |= it->value;
    }
    result = parsed;
    return true;
}

} // namespace gcm
