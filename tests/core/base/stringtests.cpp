// Ports of StringExtensionsTests, UriExtensionsTests, EnsureArgumentTests, GitConfigurationKeyComparerTests
// plus tests for the helpers that only exist in this port.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/dictionary.h"
#include "core/strings.h"
#include "infra/testmain.h"

using namespace gcm;

namespace {
const OptString null = std::nullopt;
OptString S(const char *s) { return QString::fromUtf8(s); }
QString Q(const char *s) { return QString::fromUtf8(s); }
} // namespace

#define CHECK_ROW(cond, i) QVERIFY2(cond, qPrintable(QStringLiteral("row %1").arg(i)))

class StringExtensionsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void isTruthy()
    {
        const std::pair<OptString, bool> rows[] = {
            {S("true"), true}, {S("TRUE"), true}, {S("tRuE"), true}, {S("yes"), true}, {S("YES"), true},
            {S("yEs"), true},  {S("on"), true},   {S("ON"), true},   {S("oN"), true},  {S("1"), true},
            {S("false"), false}, {S("i am a random string"), false}, {S(""), false}, {S("     "), false},
            {S("\t"), false},  {null, false},
        };
        int i = 0;
        for (const auto &[input, expected] : rows) {
            CHECK_ROW(gcm::isTruthy(input) == expected, i++);
        }
    }

    void isFalsey()
    {
        const std::pair<OptString, bool> rows[] = {
            {S("false"), true}, {S("FALSE"), true}, {S("fAlSe"), true}, {S("no"), true}, {S("NO"), true},
            {S("nO"), true},    {S("off"), true},   {S("OFF"), true},   {S("oFf"), true}, {S("0"), true},
            {S("true"), false}, {S("i am a random string"), false}, {S(""), false}, {S("     "), false},
            {S("\t"), false},   {null, false},
        };
        int i = 0;
        for (const auto &[input, expected] : rows) {
            CHECK_ROW(gcm::isFalsey(input) == expected, i++);
        }
    }

    void toBooleany()
    {
        const std::optional<bool> T = true, F = false, N = std::nullopt;
        const std::pair<OptString, std::optional<bool>> rows[] = {
            {S("true"), T}, {S("TRUE"), T}, {S("tRuE"), T}, {S("yes"), T}, {S("YES"), T}, {S("yEs"), T},
            {S("on"), T},   {S("ON"), T},   {S("oN"), T},   {S("1"), T},
            {S("false"), F}, {S("FALSE"), F}, {S("fAlSe"), F}, {S("no"), F}, {S("NO"), F}, {S("nO"), F},
            {S("off"), F},  {S("OFF"), F},  {S("oFf"), F},  {S("0"), F},
            {S("i am a random string"), N}, {S(""), N}, {S("     "), N}, {S("\t"), N}, {null, N},
        };
        int i = 0;
        for (const auto &[input, expected] : rows) {
            CHECK_ROW(gcm::toBooleany(input) == expected, i++);
        }
    }

    void toBooleanyOrDefault()
    {
        for (const char *t : {"true", "TRUE", "tRuE", "yes", "YES", "yEs", "on", "ON", "oN", "1"}) {
            QVERIFY2(gcm::toBooleanyOrDefault(S(t), false), t);
        }
        for (const char *f : {"false", "FALSE", "fAlSe", "no", "NO", "nO", "off", "OFF", "oFf", "0"}) {
            QVERIFY2(!gcm::toBooleanyOrDefault(S(f), true), f);
        }
        for (const OptString &other : {S("i am a random string"), S(""), S("     "), S("\t"), null}) {
            QVERIFY(gcm::toBooleanyOrDefault(other, true));
            QVERIFY(!gcm::toBooleanyOrDefault(other, false));
        }
    }

    void truncateFromIndexOf()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"", ""}, {"/", ""}, {"foo", "foo"}, {"foo/", "foo"}, {"foo/bar", "foo"}, {"foo/bar/", "foo"}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::truncateFromIndexOf(Q(input), QLatin1Char('/')), Q(expected));
        }
    }

    void truncateFromLastIndexOf()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"", ""}, {"/", ""}, {"foo", "foo"}, {"foo/", "foo"}, {"foo/bar", "foo"}, {"foo/bar/", "foo/bar"}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::truncateFromLastIndexOf(Q(input), QLatin1Char('/')), Q(expected));
        }
    }

    void trimUntilIndexOf_character()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"", ""}, {"/", ""}, {"foo", "foo"}, {"foo/", ""}, {"foo/bar", "bar"}, {"foo/bar/", "bar/"}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::trimUntilIndexOf(Q(input), QLatin1Char('/')), Q(expected));
        }
    }

    void trimUntilIndexOf_string()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"", ""}, {"://", ""}, {"foo", "foo"}, {"foo://", ""}, {"foo://bar", "bar"}, {"foo://bar/", "bar/"}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::trimUntilIndexOf(Q(input), Q("://")), Q(expected));
        }
    }

    void trimUntilIndexOf_string_comparisonType()
    {
        const QString input = Q("fooTRIMbarTRIMsoup");
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("TRIM"), Qt::CaseSensitive), Q("barTRIMsoup"));
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("trim"), Qt::CaseSensitive), input);
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("tRiM"), Qt::CaseSensitive), input);
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("TRIM"), Qt::CaseInsensitive), Q("barTRIMsoup"));
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("trim"), Qt::CaseInsensitive), Q("barTRIMsoup"));
        QCOMPARE(gcm::trimUntilIndexOf(input, Q("tRiM"), Qt::CaseInsensitive), Q("barTRIMsoup"));
    }

    void trimUntilLastIndexOf_character()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"", ""}, {"/", ""}, {"foo", "foo"}, {"foo/", ""}, {"foo/bar", "bar"}, {"foo/bar/", ""}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::trimUntilLastIndexOf(Q(input), QLatin1Char('/')), Q(expected));
        }
    }

    void trimUntilLastIndexOf_string()
    {
        const std::tuple<const char *, const char *, const char *> rows[] = {
            {"", "://", ""},          {"://", "://", ""},          {"foo", "://", "foo"}, {"foo://", "://", ""},
            {"foo://bar", "://", "bar"}, {"foo://bar/", "://", "bar/"}, {"foo:/bar/baz", ":", "/bar/baz"}};
        for (const auto &[input, trim, expected] : rows) {
            QCOMPARE(gcm::trimUntilLastIndexOf(Q(input), Q(trim)), Q(expected));
        }
    }

    void trimUntilLastIndexOf_string_comparisonType()
    {
        const QString input = Q("fooTRIMbarTRIMsoup");
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("TRIM"), Qt::CaseSensitive), Q("soup"));
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("trim"), Qt::CaseSensitive), input);
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("tRiM"), Qt::CaseSensitive), input);
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("TRIM"), Qt::CaseInsensitive), Q("soup"));
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("trim"), Qt::CaseInsensitive), Q("soup"));
        QCOMPARE(gcm::trimUntilLastIndexOf(input, Q("tRiM"), Qt::CaseInsensitive), Q("soup"));
    }

    void trimMiddle()
    {
        const QString input = Q("fooTRIMbar");
        QCOMPARE(gcm::trimMiddle(input, Q("TRIM")), Q("foobar"));
        QCOMPARE(gcm::trimMiddle(input, Q("trim")), input);
        QCOMPARE(gcm::trimMiddle(input, Q("tRiM")), input);
        QCOMPARE(gcm::trimMiddle(input, Q("TRIM"), Qt::CaseInsensitive), Q("foobar"));
        QCOMPARE(gcm::trimMiddle(input, Q("trim"), Qt::CaseInsensitive), Q("foobar"));
        QCOMPARE(gcm::trimMiddle(input, Q("tRiM"), Qt::CaseInsensitive), Q("foobar"));
    }

    void toSnakeCase_convertsCorrectly()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"FooBar", "foo_bar"}, {"fooBar", "foo_bar"}, {"FBBaz", "fb_baz"}, {"Foo", "foo"}, {"Fo", "fo"},
            {"fO", "f_o"},         {"OO", "oo"},          {"F", "f"},          {"", ""}};
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::toSnakeCase(Q(input)), Q(expected));
        }
    }

    // --- helpers specific to this port ---

    void tryToInt32()
    {
        int value = -1;
        QVERIFY(gcm::tryToInt32(S("42"), value));
        QCOMPARE(value, 42);
        QVERIFY(gcm::tryToInt32(S(" -7 "), value));
        QCOMPARE(value, -7);
        QVERIFY(!gcm::tryToInt32(S("abc"), value));
        QVERIFY(!gcm::tryToInt32(S(""), value));
        QVERIFY(gcm::tryToInt32(null, value)); // Convert.ToInt32(null) == 0
        QCOMPARE(value, 0);
    }

    void quoteCmdArg()
    {
        QCOMPARE(gcm::quoteCmdArg(Q("plain")), Q("plain"));
        QCOMPARE(gcm::quoteCmdArg(Q("has space")), Q("\"has space\""));
        QCOMPARE(gcm::quoteCmdArg(Q("a\"b")), Q("\"a\\\"b\""));
        QCOMPARE(gcm::quoteCmdArg(Q("a\\b")), Q("\"a\\\\b\""));
    }

    void splitCommandLine()
    {
        QCOMPARE(gcm::splitCommandLine(Q("")), QStringList());
        QCOMPARE(gcm::splitCommandLine(Q("a b  c")), QStringList({Q("a"), Q("b"), Q("c")}));
        QCOMPARE(gcm::splitCommandLine(Q("config --get \"user name\"")), QStringList({Q("config"), Q("--get"), Q("user name")}));
        QCOMPARE(gcm::splitCommandLine(Q("a \"\" b")), QStringList({Q("a"), Q(""), Q("b")}));
        QCOMPARE(gcm::splitCommandLine(Q("\"a\\\"b\"")), QStringList({Q("a\"b")}));
        QCOMPARE(gcm::splitCommandLine(Q("\"a\\\\\" b")), QStringList({Q("a\\"), Q("b")}));
        QCOMPARE(gcm::splitCommandLine(Q("a\\b")), QStringList({Q("a\\b")}));
        QCOMPARE(gcm::splitCommandLine(Q("pre\"mid dle\"post")), QStringList({Q("premid dlepost")}));
    }

    void splitCommandLine_roundTripsQuoteCmdArg()
    {
        // (An argument containing backslashes does not round-trip: quoteCmdArg doubles them and the .NET
        // argument rules only halve backslashes that precede a quote. Same quirk as upstream.)
        for (const char *arg : {"plain", "has space", "quote\"inside", "tab\there", "https://example.com/a b"}) {
            const QString line = Q("cmd ") + gcm::quoteCmdArg(Q(arg)) + Q(" tail");
            QCOMPARE(gcm::splitCommandLine(line), QStringList({Q("cmd"), Q(arg), Q("tail")}));
        }
    }

    void base64Url()
    {
        const QByteArray data = QByteArray::fromHex("fbff3e00ff");
        const QString encoded = gcm::base64UrlEncode(data);
        QVERIFY(!encoded.contains(QLatin1Char('+')) && !encoded.contains(QLatin1Char('/')) && !encoded.contains(QLatin1Char('=')));
        QCOMPARE(gcm::base64UrlDecode(encoded), data);
        QCOMPARE(gcm::base64UrlEncode("hello"), Q("aGVsbG8"));
    }
};
GCM_REGISTER_TEST(StringExtensionsTests)

class UriExtensionsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void getQueryParameters()
    {
        const QUrl uri(Q("https://example.com/foo/bar?q1=value1&q2=value%20with%20spaces&key%20with%20spaces=value3"));
        const StringDictionary result = gcm::getQueryParameters(uri);
        QCOMPARE(result.count(), 3);
        QCOMPARE(result.tryGet(Q("q1")), S("value1"));
        QCOMPARE(result.tryGet(Q("q2")), S("value with spaces"));
        QCOMPARE(result.tryGet(Q("key with spaces")), S("value3"));
    }

    void getGitConfigurationScopes()
    {
        const std::pair<const char *, QStringList> rows[] = {
            {"http://hostname", {Q("http://hostname")}},
            {"http://example.com", {Q("http://example.com")}},
            {"http://hostname:7990", {Q("http://hostname:7990")}},
            {"http://foo.example.com", {Q("http://foo.example.com"), Q("http://example.com")}},
            {"http://example.com/foo", {Q("http://example.com/foo"), Q("http://example.com")}},
            {"http://example.com/foo?query=true#fragment", {Q("http://example.com/foo"), Q("http://example.com")}},
            {"http://buzz.foo.example.com/bar/baz",
             {Q("http://buzz.foo.example.com/bar/baz"), Q("http://buzz.foo.example.com/bar"),
              Q("http://buzz.foo.example.com"), Q("http://foo.example.com"), Q("http://example.com")}},
        };
        for (const auto &[start, expected] : rows) {
            QCOMPARE(gcm::getGitConfigurationScopes(QUrl(Q(start))), expected);
        }
    }

    void getUserInfo()
    {
        struct Row { const char *input; bool result; OptString user; OptString pass; };
        const Row rows[] = {
            {"http://example.com", false, null, null},
            {"http://john.doe:password123@example.com", true, S("john.doe"), S("password123")},
            {"http://john.doe@example.com", true, S("john.doe"), null},
            {"http://john.doe:@example.com", true, S("john.doe"), S("")},
            {"http://:password123@example.com", true, S(""), S("password123")},
            {"http://john.doe:::password123@example.com", true, S("john.doe"), S("::password123")},
            {"http://john%20doe:password%20123@example.com", true, S("john doe"), S("password 123")},
        };
        for (const Row &row : rows) {
            OptString user, pass;
            QVERIFY2(gcm::tryGetUserInfo(QUrl(Q(row.input)), user, pass) == row.result, row.input);
            QVERIFY2(user == row.user, row.input);
            QVERIFY2(pass == row.pass, row.input);
        }
    }

    void withoutUserInfo()
    {
        for (const char *input :
             {"http://example.com", "http://john.doe:password123@example.com", "http://john.doe@example.com",
              "http://john.doe:@example.com", "http://:password123@example.com",
              "http://john.doe:::password123@example.com", "http://john%20doe:password%20123@example.com"}) {
            QString result = gcm::absoluteUri(gcm::withoutUserInfo(QUrl(Q(input))));
            while (result.endsWith(QLatin1Char('/'))) {
                result.chop(1);
            }
            QCOMPARE(result, Q("http://example.com"));
              }
    }

    // --- helpers specific to this port (System.Uri behaviour) ---

    void absoluteUri_matchesSystemUri()
    {
        const std::pair<const char *, const char *> rows[] = {
            {"https://example.com", "https://example.com/"},
            {"HTTPS://Example.COM/Path", "https://example.com/Path"},
            {"https://example.com:443/foo", "https://example.com/foo"},
            {"http://example.com:80", "http://example.com/"},
            {"https://example.com:8443/", "https://example.com:8443/"},
            {"https://example.com/sign-in/a%20b?q=c%20d", "https://example.com/sign-in/a%20b?q=c%20d"},
            {"https://example.com/a b", "https://example.com/a%20b"},
        };
        for (const auto &[input, expected] : rows) {
            QCOMPARE(gcm::absoluteUri(QUrl(Q(input))), Q(expected));
        }
    }

    void tryCreateUri()
    {
        QVERIFY(gcm::tryCreateUri(S("https://example.com/path")).has_value());
        QVERIFY(gcm::tryCreateUri(S("http://127.0.0.1:8080")).has_value());
        QVERIFY(!gcm::tryCreateUri(S("example.com/path")).has_value());
        QVERIFY(!gcm::tryCreateUri(S("/relative/path")).has_value());
        QVERIFY(!gcm::tryCreateUri(S("")).has_value());
        QVERIFY(!gcm::tryCreateUri(S("   ")).has_value());
        QVERIFY(!gcm::tryCreateUri(null).has_value());
    }

    void isLoopback()
    {
        QVERIFY(gcm::isLoopback(QUrl(Q("http://localhost:1234/"))));
        QVERIFY(gcm::isLoopback(QUrl(Q("http://LOCALHOST/"))));
        QVERIFY(gcm::isLoopback(QUrl(Q("http://127.0.0.1/"))));
        QVERIFY(gcm::isLoopback(QUrl(Q("http://[::1]:80/"))));
        QVERIFY(!gcm::isLoopback(QUrl(Q("http://example.com/"))));
        QVERIFY(!gcm::isLoopback(QUrl(Q("http://192.168.1.1/"))));
    }

    void isDefaultPort()
    {
        QVERIFY(gcm::isDefaultPort(QUrl(Q("http://localhost/"))));
        QVERIFY(gcm::isDefaultPort(QUrl(Q("https://localhost:443/"))));
        QVERIFY(!gcm::isDefaultPort(QUrl(Q("http://localhost:443/"))));
        QVERIFY(!gcm::isDefaultPort(QUrl(Q("http://localhost:7990/"))));
    }

    void escapeDataString()
    {
        QCOMPARE(gcm::escapeDataString(Q("john doe@example.com")), Q("john%20doe%40example.com"));
        QCOMPARE(gcm::escapeDataString(Q("a-b_c.d~e")), Q("a-b_c.d~e"));
        QCOMPARE(gcm::escapeDataString(Q("a/b?c=d&e")), Q("a%2Fb%3Fc%3Dd%26e"));
    }

    void parseQueryString()
    {
        const StringDictionary dict = gcm::parseQueryString(Q("code=abc&state=x+y&&empty=&flag"));
        QCOMPARE(dict.count(), 4);
        QCOMPARE(dict.value(Q("code")), Q("abc"));
        QCOMPARE(dict.value(Q("state")), Q("x y"));
        QCOMPARE(dict.value(Q("empty")), Q(""));
        QVERIFY(dict.contains(Q("flag")));
    }
};
GCM_REGISTER_TEST(UriExtensionsTests)

class EnsureArgumentTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void inRange_doesNotThrow()
    {
        EnsureArgument::inRange(5, "x", 0, 10, true, true);
        EnsureArgument::inRange(0, "x", 0, 10, true, true);
        EnsureArgument::inRange(10, "x", 0, 10, true, true);
        EnsureArgument::inRange(0, "x", -10, 0, true, true);
        EnsureArgument::inRange(-10, "x", -10, 0, true, true);
    }

    void inRange_throwsException()
    {
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(-3, "x", 0, 10, true, true));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(13, "x", 0, 10, true, true));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(10, "x", 0, 10, true, false));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(0, "x", 0, 10, false, true));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(-10, "x", -10, 0, false, true));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::inRange(0, "x", -10, 0, true, false));
    }

    void stringChecks()
    {
        EnsureArgument::notNullOrEmpty(Q("x"), "arg");
        EnsureArgument::notNullOrWhiteSpace(Q("x"), "arg");
        QVERIFY_THROWS_EXCEPTION(ArgumentException, EnsureArgument::notNullOrEmpty(Q(""), "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentNullException, EnsureArgument::notNullOrEmpty(null, "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, EnsureArgument::notNullOrWhiteSpace(Q("  "), "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentNullException, EnsureArgument::notNullOrWhiteSpace(null, "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, EnsureArgument::absoluteUri(QUrl(Q("relative/path")), "arg"));
        EnsureArgument::absoluteUri(QUrl(Q("https://example.com")), "arg");
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::positive(0, "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::positiveOrZero(-1, "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::negative(0, "arg"));
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException, EnsureArgument::negativeOrZero(1, "arg"));
    }

    void messageNamesTheParameter()
    {
        try {
            EnsureArgument::notNullOrWhiteSpace(Q(" "), "resource");
            QFAIL("expected an exception");
        } catch (const ArgumentException &ex) {
            QCOMPARE(ex.message(), Q("Argument cannot be empty or white space. (Parameter 'resource')"));
        }
    }
};
GCM_REGISTER_TEST(EnsureArgumentTests)

class GitConfigurationKeyComparerTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void equals()
    {
        const std::tuple<const char *, const char *, bool> rows[] = {
            {"", "", true},
            {"   ", " ", true},
            {"foo", "foo", true},
            {"foo", "FOO", true},
            {"foo", "bar", false},
            {"foo.bar", "foo.bar", true},
            {"foo.bar", "foo.fish", false},
            {"fish.bar", "foo.bar", false},
            {"foo.bar", "FOO.BAR", true},
            {"foo.bar", "foo.BAR", true},
            {"foo.bar", "FOO.bar", true},
            {"foo.example.com.bar", "foo.example.com.bar", true},
            {"foo.example.com.bar", "foo.example.com.BAR", true},
            {"foo.example.com.bar", "FOO.example.com.BAR", true},
            {"foo.example.com.bar", "FOO.example.com.bar", true},
            {"foo.example.com.bar", "foo.EXAMPLE.COM.bar", false},
        };
        for (const auto &[x, y, expected] : rows) {
            QVERIFY2(gcm::gitConfigKeyEquals(Q(x), Q(y)) == expected, qPrintable(Q(x) + Q(" / ") + Q(y)));
        }
    }

    void normalize()
    {
        QCOMPARE(gcm::normalizeGitConfigKey(Q("Credential.https://Example.com/Path.UseHttpPath")),
                 Q("credential.https://Example.com/Path.usehttppath"));
        QCOMPARE(gcm::normalizeGitConfigKey(Q("Credential.Helper")), Q("credential.helper"));
        QCOMPARE(gcm::normalizeGitConfigKey(Q("NoDots")), Q("nodots"));
    }
};
GCM_REGISTER_TEST(GitConfigurationKeyComparerTests)

class DictionaryTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void keepsInsertionOrder()
    {
        StringDictionary dict;
        dict.set(Q("b"), Q("1"));
        dict.set(Q("a"), Q("2"));
        dict.set(Q("c"), Q("3"));
        dict.set(Q("a"), Q("4")); // replace keeps the position
        QCOMPARE(dict.keys(), QStringList({Q("b"), Q("a"), Q("c")}));
        QCOMPARE(dict.value(Q("a")), Q("4"));
    }

    void caseSensitivity()
    {
        StringDictionary sensitive;
        sensitive.set(Q("Key"), Q("1"));
        QVERIFY(!sensitive.contains(Q("key")));

        StringDictionary insensitive(Qt::CaseInsensitive);
        insensitive.set(Q("Key"), Q("1"));
        insensitive.set(Q("KEY"), Q("2"));
        QCOMPARE(insensitive.count(), 1);
        QCOMPARE(insensitive.value(Q("key")), Q("2"));
        QCOMPARE(insensitive.keys(), QStringList({Q("Key")}));
    }

    void removeKeepsIndexConsistent()
    {
        StringDictionary dict{{Q("a"), Q("1")}, {Q("b"), Q("2")}, {Q("c"), Q("3")}};
        QVERIFY(dict.remove(Q("a")));
        QVERIFY(!dict.remove(Q("a")));
        QCOMPARE(dict.value(Q("b")), Q("2"));
        QCOMPARE(dict.value(Q("c")), Q("3"));
        dict[Q("d")] = Q("4");
        QCOMPARE(dict.keys(), QStringList({Q("b"), Q("c"), Q("d")}));
        QCOMPARE(dict.tryGet(Q("a")), null);
        QString out;
        QVERIFY(dict.tryGetValue(Q("d"), out));
        QCOMPARE(out, Q("4"));
    }
};
GCM_REGISTER_TEST(DictionaryTests)

#include "stringtests.moc"
