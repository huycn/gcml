// Ports of StreamExtensionsTests, GitStreamReaderTests, GitCapabilitiesTests, GitStateValidationTests,
// TraceTests, TraceUtilsTests.
// Not ported: the CRLF TextWriter variants of StreamExtensionsTests (writers in this port always use LF).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/constants.h"
#include "core/gitcapabilities.h"
#include "core/streams.h"
#include "core/trace.h"
#include "infra/testmain.h"

#include <QBuffer>
#include <QRegularExpression>

using namespace gcm;

namespace {
const OptString null = std::nullopt;
OptString S(const char *s) { return QString::fromUtf8(s); }
QString Q(const char *s) { return QString::fromUtf8(s); }
QStringList L(std::initializer_list<const char *> items)
{
    QStringList list;
    for (const char *item : items) {
        list.append(QString::fromUtf8(item));
    }
    return list;
}
} // namespace

class StreamExtensionsTests : public QObject
{
    Q_OBJECT

    static StringDictionary read(const char *input, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    {
        StringReader reader(Q(input));
        return gcm::readDictionary(reader, cs);
    }
    static MultiDictionary readMulti(const char *input, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    {
        StringReader reader(Q(input));
        return gcm::readMultiDictionary(reader, cs);
    }
    template <typename T> static QString write(const T &dict)
    {
        StringWriter writer;
        gcm::writeDictionary(writer, dict);
        return writer.toString();
    }

private Q_SLOTS:
    void readDictionary_emptyString_returnsEmptyDictionary() { QVERIFY(read("").isEmpty()); }

    void readDictionary_terminatedLF_returnsDictionary()
    {
        const StringDictionary output = read("a=1\nb=2\nc=3\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.tryGet(Q("a")), S("1"));
        QCOMPARE(output.tryGet(Q("b")), S("2"));
        QCOMPARE(output.tryGet(Q("c")), S("3"));
    }

    void readDictionary_terminatedCRLF_returnsDictionary()
    {
        const StringDictionary output = read("a=1\r\nb=2\r\nc=3\r\n\r\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.tryGet(Q("a")), S("1"));
        QCOMPARE(output.tryGet(Q("b")), S("2"));
        QCOMPARE(output.tryGet(Q("c")), S("3"));
    }

    void readDictionary_caseSensitive_returnsDictionaryWithMultipleEntries()
    {
        const StringDictionary output = read("a=1\nA=2\n\n", Qt::CaseSensitive);
        QCOMPARE(output.count(), 2);
        QCOMPARE(output.tryGet(Q("a")), S("1"));
        QCOMPARE(output.tryGet(Q("A")), S("2"));
    }

    void readDictionary_caseInsensitive_returnsDictionaryWithLastValue()
    {
        const StringDictionary output = read("a=1\nA=2\n\n", Qt::CaseInsensitive);
        QCOMPARE(output.count(), 1);
        QCOMPARE(output.tryGet(Q("a")), S("2"));
    }

    void readDictionary_spaces_returnsCorrectKeysAndValues()
    {
        const StringDictionary output = read("key a=value 1\n  key b  = 2 \nkey\tc\t=\t3\t\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.tryGet(Q("key a")), S("value 1"));
        QCOMPARE(output.tryGet(Q("  key b  ")), S(" 2 "));
        QCOMPARE(output.tryGet(Q("key\tc\t")), S("\t3\t"));
    }

    void readDictionary_equalsInValues_returnsCorrectKeysAndValues()
    {
        const StringDictionary output = read("a=value=1\nb=value=2\nc=value=3\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.tryGet(Q("a")), S("value=1"));
        QCOMPARE(output.tryGet(Q("b")), S("value=2"));
        QCOMPARE(output.tryGet(Q("c")), S("value=3"));
    }

    void readDictionary_stopsAtBlankLine()
    {
        StringReader reader(Q("a=1\n\nb=2\n\n"));
        QCOMPARE(gcm::readDictionary(reader).keys(), L({"a"}));
        QCOMPARE(gcm::readDictionary(reader).keys(), L({"b"}));
    }

    void writeDictionary_emptyDictionary_writesLineLF() { QCOMPARE(write(StringDictionary()), Q("\n")); }

    void writeDictionary_entries_writesKVPsAndLF()
    {
        const StringDictionary input{{Q("a"), Q("1")}, {Q("b"), Q("2")}, {Q("c"), Q("3")}};
        QCOMPARE(write(input), Q("a=1\nb=2\nc=3\n\n"));
    }

    void writeDictionary_entriesWithSpaces_writesKVPsAndLF()
    {
        const StringDictionary input{{Q("key a"), Q("value 1")}, {Q("  key b  "), Q(" value 2 ")}, {Q("\tvalue\tc\t"), Q("\t3\t")}};
        QCOMPARE(write(input), Q("key a=value 1\n  key b  = value 2 \n\tvalue\tc\t=\t3\t\n\n"));
    }

    void readMultiDictionary_emptyString_returnsEmptyDictionary() { QVERIFY(readMulti("").isEmpty()); }

    void readMultiDictionary_terminatedLF_returnsDictionary()
    {
        const MultiDictionary output = readMulti("a=1\nb=2\nc=3\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.value(Q("a")), L({"1"}));
        QCOMPARE(output.value(Q("b")), L({"2"}));
        QCOMPARE(output.value(Q("c")), L({"3"}));
    }

    void readMultiDictionary_terminatedCRLF_returnsDictionary()
    {
        const MultiDictionary output = readMulti("a=1\r\nb=2\r\nc=3\r\n\r\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.value(Q("a")), L({"1"}));
        QCOMPARE(output.value(Q("b")), L({"2"}));
        QCOMPARE(output.value(Q("c")), L({"3"}));
    }

    void readMultiDictionary_caseSensitive_returnsDictionaryWithMultipleEntries()
    {
        const MultiDictionary output = readMulti("a=1\nA=2\n\n", Qt::CaseSensitive);
        QCOMPARE(output.count(), 2);
        QCOMPARE(output.value(Q("a")), L({"1"}));
        QCOMPARE(output.value(Q("A")), L({"2"}));
    }

    void readMultiDictionary_caseInsensitive_returnsDictionaryWithLastValue()
    {
        const MultiDictionary output = readMulti("a=1\nA=2\n\n", Qt::CaseInsensitive);
        QCOMPARE(output.count(), 1);
        QCOMPARE(output.value(Q("a")), L({"2"}));
    }

    void readMultiDictionary_emptyString_returnsKeyWithEmptyStringValue()
    {
        const MultiDictionary output = readMulti("a=\n\n");
        QCOMPARE(output.count(), 1);
        QCOMPARE(output.value(Q("a")), L({""}));
    }

    void readMultiDictionary_spaces_returnsCorrectKeysAndValues()
    {
        const MultiDictionary output = readMulti("key a=value 1\n  key b  = 2 \nkey\tc\t=\t3\t\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.value(Q("key a")), L({"value 1"}));
        QCOMPARE(output.value(Q("  key b  ")), L({" 2 "}));
        QCOMPARE(output.value(Q("key\tc\t")), L({"\t3\t"}));
    }

    void readMultiDictionary_equalsInValues_returnsCorrectKeysAndValues()
    {
        const MultiDictionary output = readMulti("a=value=1\nb=value=2\nc=value=3\n\n");
        QCOMPARE(output.count(), 3);
        QCOMPARE(output.value(Q("a")), L({"value=1"}));
        QCOMPARE(output.value(Q("b")), L({"value=2"}));
        QCOMPARE(output.value(Q("c")), L({"value=3"}));
    }

    void readMultiDictionary_multiValue_returnsDictionary()
    {
        const MultiDictionary output = readMulti("odd[]=1\neven[]=2\neven[]=4\nodd[]=3\n\n");
        QCOMPARE(output.count(), 2);
        QCOMPARE(output.value(Q("odd")), L({"1", "3"}));
        QCOMPARE(output.value(Q("even")), L({"2", "4"}));
    }

    void readMultiDictionary_emptyMultiValue_resetsList()
    {
        const MultiDictionary output = readMulti("wwwauth[]=a\nwwwauth[]=b\nwwwauth[]=\nwwwauth[]=c\n\n");
        QCOMPARE(output.value(Q("wwwauth")), L({"c"}));
    }

    void writeDictionary_emptyMultiDictionary_writesLineLF() { QCOMPARE(write(MultiDictionary()), Q("\n")); }

    void writeDictionary_multiEntries_writesKVPListsAndLF()
    {
        const MultiDictionary input{{Q("a"), L({"1", "2", "3"})}, {Q("b"), L({"4", "5"})}, {Q("c"), L({"6"})}};
        QCOMPARE(write(input), Q("a[]=1\na[]=2\na[]=3\nb[]=4\nb[]=5\nc=6\n\n"));
    }

    void writeDictionary_noMultiEntries_writesKVPsAndLF()
    {
        const MultiDictionary input{{Q("a"), L({"1"})}, {Q("b"), L({"2"})}, {Q("c"), L({"3"})}};
        QCOMPARE(write(input), Q("a=1\nb=2\nc=3\n\n"));
    }

    void writeDictionary_multiEntriesWithEmpty_writesKVPListsAndLF()
    {
        const MultiDictionary input{{Q("a"), L({"1", "2", "", "3", "4"})},
                                    {Q("b"), L({"5"})},
                                    {Q("c"), L({"6", "7", ""})},
                                    {Q("d"), L({"8", "", "9"})}};
        QCOMPARE(write(input), Q("a[]=3\na[]=4\nb=5\nd=9\n\n"));
    }
};
GCM_REGISTER_TEST(StreamExtensionsTests)

class GitStreamReaderTests : public QObject
{
    Q_OBJECT

    // Upstream tests ReadLine and ReadLineAsync separately; here both the device-backed and the
    // string-backed reader are exercised with the same expectations.
    static QList<OptString> readAll(const QByteArray &bytes, bool viaDevice)
    {
        QList<OptString> lines;
        QBuffer buffer;
        buffer.setData(bytes);
        (void)buffer.open(QIODevice::ReadOnly);
        DeviceTextReader deviceReader(&buffer);
        StringReader stringReader(QString::fromUtf8(bytes));
        TextReader &reader = viaDevice ? static_cast<TextReader &>(deviceReader) : stringReader;
        for (;;) {
            lines.append(reader.readLine());
            if (!lines.last()) {
                break;
            }
        }
        return lines;
    }

    static void check(const QByteArray &bytes, const QList<OptString> &expected)
    {
        QCOMPARE(readAll(bytes, true), expected);
        QCOMPARE(readAll(bytes, false), expected);
    }

private Q_SLOTS:
    void readLine_LF() { check("hello\nworld\n", {S("hello"), S("world"), null}); }
    void readLine_CR() { check("hello\rworld\r", {S("hello\rworld\r"), null}); }
    void readLine_CRLF() { check("hello\r\nworld\r\n", {S("hello"), S("world"), null}); }
    void readLine_mixed()
    {
        check("hello\r\nworld\rthis\nis\na\n\rmixed\rnewline\r\n\nstring\n",
              {S("hello"), S("world\rthis"), S("is"), S("a"), S("\rmixed\rnewline"), S(""), S("string"), null});
    }
    void readLine_noTrailingNewline() { check("hello\nworld", {S("hello"), S("world"), null}); }
    void readLine_utf8() { check("usern\xC3\xA4me=j\xC3\xB6rg\n", {S("usern\xC3\xA4me=j\xC3\xB6rg"), null}); }

    void deviceWriter_writesUtf8WithLF()
    {
        QBuffer buffer;
        (void)buffer.open(QIODevice::WriteOnly);
        DeviceTextWriter writer(&buffer);
        writer.writeLine(Q("username=j\xC3\xB6rg"));
        writer.writeLine();
        QCOMPARE(buffer.data(), QByteArray("username=j\xC3\xB6rg\n\n"));
    }
};
GCM_REGISTER_TEST(GitStreamReaderTests)

class GitCapabilitiesTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void advertised_includesState() { QVERIFY(hasFlag(Constants::SupportedCapabilities, GitCapabilities::State)); }

    void parseName_nullOrWhitespace_returnsNone()
    {
        QCOMPARE(GitCapabilitiesUtils::parseName(null), GitCapabilities::None);
        QCOMPARE(GitCapabilitiesUtils::parseName(S("")), GitCapabilities::None);
        QCOMPARE(GitCapabilitiesUtils::parseName(S("   ")), GitCapabilities::None);
    }

    void parseName_unknownName_returnsNone()
    {
        QCOMPARE(GitCapabilitiesUtils::parseName(S("totally-unknown")), GitCapabilities::None);
    }

    void parseName_state_returnsStateFlag()
    {
        QCOMPARE(GitCapabilitiesUtils::parseName(S("state")), GitCapabilities::State);
        QCOMPARE(GitCapabilitiesUtils::parseName(S("STATE")), GitCapabilities::State);
        QCOMPARE(GitCapabilitiesUtils::parseName(S("State")), GitCapabilities::State);
    }

    void parseName_authType_returnsAuthTypeFlag()
    {
        QCOMPARE(GitCapabilitiesUtils::parseName(S("authtype")), GitCapabilities::AuthType);
    }

    void toProtocolName_none_throws()
    {
        QVERIFY_THROWS_EXCEPTION(ArgumentException, GitCapabilitiesUtils::toProtocolName(GitCapabilities::None));
    }

    void toProtocolName_combined_throws()
    {
        QVERIFY_THROWS_EXCEPTION(ArgumentOutOfRangeException,
                                 GitCapabilitiesUtils::toProtocolName(GitCapabilities::State | GitCapabilities::AuthType));
    }

    void toProtocolName_state_returnsStateString()
    {
        QCOMPARE(GitCapabilitiesUtils::toProtocolName(GitCapabilities::State), Q("state"));
    }

    void toProtocolNames_none_returnsEmpty() { QVERIFY(GitCapabilitiesUtils::toProtocolNames(GitCapabilities::None).isEmpty()); }

    void toProtocolNames_state_returnsStateOnly()
    {
        QCOMPARE(GitCapabilitiesUtils::toProtocolNames(GitCapabilities::State), L({"state"}));
    }

    void toProtocolNames_all_returnsInFlagOrder()
    {
        QCOMPARE(GitCapabilitiesUtils::toProtocolNames(Constants::SupportedCapabilities), L({"state", "authtype"}));
    }

    void toString_rendersLikeFlagsEnum()
    {
        QCOMPARE(GitCapabilitiesUtils::toString(GitCapabilities::None), Q("None"));
        QCOMPARE(GitCapabilitiesUtils::toString(GitCapabilities::State), Q("State"));
        QCOMPARE(GitCapabilitiesUtils::toString(GitCapabilities::State | GitCapabilities::AuthType), Q("State, AuthType"));
    }
};
GCM_REGISTER_TEST(GitCapabilitiesTests)

class GitStateValidationTests : public QObject
{
    Q_OBJECT
    static OptString withNul(const char *before, const char *after)
    {
        return QString::fromUtf8(before) + QChar(0) + QString::fromUtf8(after);
    }

private Q_SLOTS:
    void isValidKey_validKeys_returnsTrue()
    {
        for (const char *key : {"github.account", "plain", "a.b.c.d", "with-dashes", "with_under", "UPPER"}) {
            QVERIFY2(GitStateValidation::isValidKey(S(key)), key);
        }
    }

    void isValidKey_invalidKeys_returnsFalse()
    {
        for (const OptString &key : {null, S(""), S("has=equals"), S("has\nnewline"), withNul("has", "null"), S("gcm.alreadyprefixed")}) {
            QVERIFY(!GitStateValidation::isValidKey(key));
        }
    }

    void isValidValue_validValues_returnsTrue()
    {
        for (const char *value : {"", "alice", "value with spaces", "value=with=equals"}) {
            QVERIFY2(GitStateValidation::isValidValue(S(value)), value);
        }
    }

    void isValidValue_invalidValues_returnsFalse()
    {
        for (const OptString &value : {null, S("has\nnewline"), withNul("has", "null")}) {
            QVERIFY(!GitStateValidation::isValidValue(value));
        }
    }

    void validateKey_nullOrEmpty_throwsArgumentException()
    {
        QVERIFY_THROWS_EXCEPTION(ArgumentException, GitStateValidation::validateKey(null));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, GitStateValidation::validateKey(S("")));
    }

    void validateKey_reservedPrefix_throwsArgumentException()
    {
        try {
            GitStateValidation::validateKey(S("gcm.foo"));
            QFAIL("expected an exception");
        } catch (const ArgumentException &ex) {
            QVERIFY(ex.message().contains(QLatin1String("gcm.")));
        }
    }

    void validateKey_bannedCharacters_throwsArgumentException()
    {
        for (const OptString &key : {S("a=b"), S("a\nb"), withNul("a", "b")}) {
            QVERIFY_THROWS_EXCEPTION(ArgumentException, GitStateValidation::validateKey(key));
        }
    }

    void validateValue_null_throwsArgumentNullException()
    {
        QVERIFY_THROWS_EXCEPTION(ArgumentNullException, GitStateValidation::validateValue(null));
    }

    void validateValue_bannedCharacters_throwsArgumentException()
    {
        for (const OptString &value : {S("a\nb"), withNul("a", "b")}) {
            QVERIFY_THROWS_EXCEPTION(ArgumentException, GitStateValidation::validateValue(value));
        }
    }
};
GCM_REGISTER_TEST(GitStateValidationTests)

class TraceTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void writeLineSecrets_secretTracingEnabled_writesSecretValues()
    {
        auto listener = std::make_shared<StringWriter>();
        Trace trace;
        trace.addListener(listener);
        trace.setSecretTracingEnabled(true);
        trace.writeLineSecrets(Q("Secrets: {0} {1} {2}"), L({"foo", "bar", "test"}));
        QVERIFY2(listener->toString().endsWith(Q("Secrets: foo bar test\n")), qPrintable(listener->toString()));
    }

    void writeLineSecrets_secretTracingDisabled_writesMaskedValues()
    {
        auto listener = std::make_shared<StringWriter>();
        Trace trace;
        trace.addListener(listener);
        trace.setSecretTracingEnabled(false);
        trace.writeLineSecrets(Q("Secrets: {0} {1} {2}"), L({"foo", "bar", "test"}));
        QVERIFY2(listener->toString().endsWith(Q("Secrets: ******** ******** ********\n")), qPrintable(listener->toString()));
    }

    void writeLine_noListeners_isNoOp()
    {
        Trace trace;
        QVERIFY(!trace.hasListeners());
        trace.writeLine(Q("nobody listens"));
    }

    void writeLine_formatsTimestampSourceAndMember()
    {
        auto listener = std::make_shared<StringWriter>();
        Trace trace;
        trace.addListener(listener);
        trace.addListener(listener); // the same listener is only added once
        trace.writeLine(Q("hello"));
        const QString text = listener->toString();
        QCOMPARE(text.count(QLatin1Char('\n')), 1);
        static const QRegularExpression re(
            QStringLiteral(R"(^\d{2}:\d{2}:\d{2}\.\d{6} .{23} trace: \[writeLine_formatsTimestampSourceAndMember\] hello\n$)"));
        QVERIFY2(re.match(text).hasMatch(), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("streamtests.cpp:")), qPrintable(text));
    }

    void writeDictionarySecrets_masksSecretKeys()
    {
        auto listener = std::make_shared<StringWriter>();
        Trace trace;
        trace.addListener(listener);
        const MultiDictionary dict{{Q("protocol"), L({"https"})}, {Q("Password"), L({"hunter2"})}, {Q("wwwauth"), L({"a", "b"})}};
        trace.writeDictionarySecrets(dict, L({"password"}), Qt::CaseInsensitive);
        const QString text = listener->toString();
        QVERIFY(text.contains(QLatin1String("\tprotocol=https\n")));
        QVERIFY(text.contains(QLatin1String("\tPassword=********\n")));
        QVERIFY(!text.contains(QLatin1String("hunter2")));
        QVERIFY(text.contains(QLatin1String("\twwwauth[]=a\n")));
        QVERIFY(text.contains(QLatin1String("\twwwauth[]=b\n")));
    }

    void writeException_writesInnerExceptions()
    {
        auto listener = std::make_shared<StringWriter>();
        Trace trace;
        trace.addListener(listener);
        const Exception ex(Q("outer"), std::make_shared<Exception>(Q("inner")));
        trace.writeException(ex);
        const QString text = listener->toString();
        QVERIFY2(text.contains(QLatin1String("! error: 'outer'.\n")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("       > 'inner'.\n")), qPrintable(text));
    }
};
GCM_REGISTER_TEST(TraceTests)

class TraceUtilsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void formatSource_returnsExpectedSourceValues()
    {
        QCOMPARE(TraceUtils::formatSource(Q("/foo/bar/baz/boo"), 10), Q("...baz/boo"));
        QCOMPARE(TraceUtils::formatSource(Q("thisfileshouldbetruncated"), 12), Q("...truncated"));
    }
};
GCM_REGISTER_TEST(TraceUtilsTests)

#include "streamtests.moc"
