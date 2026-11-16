// Ports of ConsoleServiceTests, Tty/AnsiEscapeParserTests, Tty/AnsiConsoleFactoryTests and
// Tty/RawModeExclusivityModeTests, plus own tests of the prompts with scripted input.
// Not ported: RunAsync / ReadKeyAsync cases (the port is synchronous).
#include "core/console.h"
#include "core/streams.h"
#include "core/tty.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

KeyInfo parse(const QByteArray &input)
{
    qsizetype pos = 0;
    auto next = [&]() -> int { return pos < input.size() ? static_cast<unsigned char>(input[pos++]) : -1; };
    const std::optional<KeyInfo> result = AnsiEscapeParser::readKey(next, next);
    if (!result) {
        return KeyInfo{QChar(0xFFFF), ConsoleKey::None, false, false, false};
    }
    return *result;
}

/// Scripted keyboard: keys are returned in order, then nullopt.
class ScriptedInput : public ITerminalInput
{
public:
    explicit ScriptedInput(QList<KeyInfo> keys) : m_keys(std::move(keys)) {}
    QStringList log;
    bool isKeyAvailable() override { return !m_keys.isEmpty(); }
    std::optional<KeyInfo> readKey() override
    {
        if (m_keys.isEmpty()) {
            return std::nullopt;
        }
        return m_keys.takeFirst();
    }
    std::unique_ptr<void, void (*)(void *)> beginRawModeSession() override
    {
        log.append(Q("session.begin"));
        return {this, [](void *p) { static_cast<ScriptedInput *>(p)->log.append(Q("session.dispose")); }};
    }

private:
    QList<KeyInfo> m_keys;
};

class RecordingOutput : public ITerminalOutput
{
public:
    explicit RecordingOutput(QString *text) : m_text(text) {}
    void write(const QString &text) override { *m_text += text; }
    bool isTerminal() const override { return true; }
    bool supportsAnsi() const override { return false; }
    int width() const override { return 80; }

private:
    QString *m_text;
};

QList<KeyInfo> typed(const QString &text)
{
    QList<KeyInfo> keys;
    for (const QChar c : text) {
        keys.append(KeyInfo{c, ConsoleKey::NoName, false, false, false});
    }
    keys.append(KeyInfo{QLatin1Char('\r'), ConsoleKey::Enter, false, false, false});
    return keys;
}

KeyInfo key(ConsoleKey k) { return KeyInfo{QChar(), k, false, false, false}; }
} // namespace

class AnsiEscapeParserTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void readKey_printableAscii_returnsCharAndKey()
    {
        const KeyInfo k = parse("a");
        QCOMPARE(k.keyChar, QLatin1Char('a'));
        QCOMPARE(k.key, ConsoleKey::A);
        QVERIFY(!k.shift);
    }
    void readKey_uppercaseAscii_hasShiftFlag()
    {
        const KeyInfo k = parse("A");
        QCOMPARE(k.keyChar, QLatin1Char('A'));
        QCOMPARE(k.key, ConsoleKey::A);
        QVERIFY(k.shift);
    }
    void readKey_digit_returnsDigitKey()
    {
        const KeyInfo k = parse("7");
        QCOMPARE(k.keyChar, QLatin1Char('7'));
        QCOMPARE(k.key, ConsoleKey::D7);
    }
    void readKey_space_returnsSpacebar() { QCOMPARE(parse(" ").key, ConsoleKey::Spacebar); }

    void readKey_controlChars_mapToNamedKeys_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<int>("expected");
        QTest::newRow("CR") << QByteArray("\r") << int(ConsoleKey::Enter);
        QTest::newRow("LF") << QByteArray("\n") << int(ConsoleKey::Enter);
        QTest::newRow("TAB") << QByteArray("\t") << int(ConsoleKey::Tab);
        QTest::newRow("DEL") << QByteArray("\x7F") << int(ConsoleKey::Backspace);
        QTest::newRow("BS") << QByteArray("\x08") << int(ConsoleKey::Backspace);
    }
    void readKey_controlChars_mapToNamedKeys()
    {
        QFETCH(QByteArray, input);
        QFETCH(int, expected);
        QCOMPARE(int(parse(input).key), expected);
    }

    void readKey_ctrlLetter_setsControlFlag()
    {
        const KeyInfo k = parse("\x01");
        QCOMPARE(k.key, ConsoleKey::A);
        QVERIFY(k.control);
    }

    void readKey_sequences_mapped_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<int>("expected");
        // CSI arrows and cursor
        QTest::newRow("up") << QByteArray("\x1b[A") << int(ConsoleKey::UpArrow);
        QTest::newRow("down") << QByteArray("\x1b[B") << int(ConsoleKey::DownArrow);
        QTest::newRow("right") << QByteArray("\x1b[C") << int(ConsoleKey::RightArrow);
        QTest::newRow("left") << QByteArray("\x1b[D") << int(ConsoleKey::LeftArrow);
        QTest::newRow("home") << QByteArray("\x1b[H") << int(ConsoleKey::Home);
        QTest::newRow("end") << QByteArray("\x1b[F") << int(ConsoleKey::End);
        // CSI ~ keys
        QTest::newRow("insert") << QByteArray("\x1b[2~") << int(ConsoleKey::Insert);
        QTest::newRow("delete") << QByteArray("\x1b[3~") << int(ConsoleKey::Delete);
        QTest::newRow("pgup") << QByteArray("\x1b[5~") << int(ConsoleKey::PageUp);
        QTest::newRow("pgdn") << QByteArray("\x1b[6~") << int(ConsoleKey::PageDown);
        QTest::newRow("home~") << QByteArray("\x1b[1~") << int(ConsoleKey::Home);
        QTest::newRow("end~") << QByteArray("\x1b[4~") << int(ConsoleKey::End);
        QTest::newRow("F5") << QByteArray("\x1b[15~") << int(ConsoleKey::F5);
        QTest::newRow("F12") << QByteArray("\x1b[24~") << int(ConsoleKey::F12);
        // SS3
        QTest::newRow("ss3 up") << QByteArray("\x1bOA") << int(ConsoleKey::UpArrow);
        QTest::newRow("ss3 down") << QByteArray("\x1bOB") << int(ConsoleKey::DownArrow);
        QTest::newRow("ss3 F1") << QByteArray("\x1bOP") << int(ConsoleKey::F1);
        QTest::newRow("ss3 F4") << QByteArray("\x1bOS") << int(ConsoleKey::F4);
    }
    void readKey_sequences_mapped()
    {
        QFETCH(QByteArray, input);
        QFETCH(int, expected);
        QCOMPARE(int(parse(input).key), expected);
    }

    void readKey_shiftArrow_setsShiftModifier()
    {
        const KeyInfo k = parse("\x1b[1;2A");
        QCOMPARE(k.key, ConsoleKey::UpArrow);
        QVERIFY(k.shift);
        QVERIFY(!k.control);
        QVERIFY(!k.alt);
    }
    void readKey_ctrlArrow_setsControlModifier()
    {
        const KeyInfo k = parse("\x1b[1;5A");
        QCOMPARE(k.key, ConsoleKey::UpArrow);
        QVERIFY(k.control);
        QVERIFY(!k.shift);
    }
    void readKey_shiftCtrlArrow_setsBothModifiers()
    {
        const KeyInfo k = parse("\x1b[1;6A");
        QCOMPARE(k.key, ConsoleKey::UpArrow);
        QVERIFY(k.shift);
        QVERIFY(k.control);
    }
    void readKey_backTab_returnsTabWithShift()
    {
        const KeyInfo k = parse("\x1b[Z");
        QCOMPARE(k.key, ConsoleKey::Tab);
        QVERIFY(k.shift);
    }
    void readKey_escapeAlone_timesOutAsEscape()
    {
        bool given = false;
        const std::optional<KeyInfo> result = AnsiEscapeParser::readKey(
            [&] {
                if (given) {
                    return -1;
                }
                given = true;
                return 0x1B;
            },
            [] { return -1; });
        QVERIFY(result);
        QCOMPARE(result->key, ConsoleKey::Escape);
    }
    void readKey_emptyStream_returnsNull()
    {
        QVERIFY(!AnsiEscapeParser::readKey([] { return -1; }, [] { return -1; }));
    }
    void readKey_utf8MultiByte_returnsDecodedChar() { QCOMPARE(parse(Q("é").toUtf8()).keyChar, QChar(0xE9)); }

    // Own test: Alt+<key>
    void readKey_altLetter_setsAltModifier()
    {
        const KeyInfo k = parse("\x1b" "b");
        QCOMPARE(k.key, ConsoleKey::B);
        QVERIFY(k.alt);
    }
};

// =============================================================================================

class AnsiConsoleFactoryTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void create_returnsNonNullConsole() { QVERIFY(AnsiConsoleFactory::createForTty()); }
    void createHeadless_readKey_returnsNull() { QVERIFY(!AnsiConsoleFactory::createHeadless()->input().readKey()); }
    void createHeadless_isKeyAvailable_returnsFalse() { QVERIFY(!AnsiConsoleFactory::createHeadless()->input().isKeyAvailable()); }
    void createHeadless_output_isNotTerminal() { QVERIFY(!AnsiConsoleFactory::createHeadless()->output().isTerminal()); }
    void createHeadless_output_isNoColor() { QVERIFY(!AnsiConsoleFactory::createHeadless()->output().supportsAnsi()); }
    void createHeadless_output_write_doesNotThrow() { AnsiConsoleFactory::createHeadless()->output().write(Q("error in headless mode\n")); }
    void testCommandContext_exposesConsole()
    {
        TestCommandContext context;
        ICommandContext &ctx = context;
        ctx.console().writeLine(Q("x"));
    }
    // Own test: prompting on a headless console fails instead of hanging.
    void createHeadless_prompts_throw()
    {
        auto console = AnsiConsoleFactory::createHeadless();
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, console->promptText(Q("Username")));
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, console->promptSelection(Q("Pick"), {Q("a")}));
    }
};

// =============================================================================================

class RawModeExclusivityModeTests : public QObject
{
    Q_OBJECT

    class RecordingExclusivityMode : public IExclusivityMode
    {
    public:
        explicit RecordingExclusivityMode(QStringList &log) : m_log(log) {}
        void run(const std::function<void()> &func) override
        {
            m_log.append(Q("inner.Run"));
            func();
        }

    private:
        QStringList &m_log;
    };

private Q_SLOTS:
    void run_beginsRawModeSessionInsideExclusivity_andDisposesAfterFunc()
    {
        QStringList log;
        RecordingExclusivityMode inner(log);
        ScriptedInput input({});
        RawModeExclusivityMode mode(inner, input);
        const int result = mode.run<int>([&] {
            log.append(input.log);
            input.log.clear();
            log.append(Q("func"));
            return 42;
        });
        log.append(input.log);
        QCOMPARE(result, 42);
        QCOMPARE(log, (QStringList{Q("inner.Run"), Q("session.begin"), Q("func"), Q("session.dispose")}));
    }

    void run_disposesRawModeSession_whenFuncThrows()
    {
        QStringList log;
        RecordingExclusivityMode inner(log);
        ScriptedInput input({});
        RawModeExclusivityMode mode(inner, input);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, mode.run<int>([&]() -> int {
            log.append(input.log);
            input.log.clear();
            throw InvalidOperationException(Q("x"));
        }));
        log.append(input.log);
        QCOMPARE(log, (QStringList{Q("inner.Run"), Q("session.begin"), Q("session.dispose")}));
    }
};

// =============================================================================================

class ConsoleServiceTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void consoleService_writeMethods_routeToErrorConsoleWriter()
    {
        StringWriter err;
        ConsoleService console(AnsiConsoleFactory::createHeadless, err, true);
        console.writeInfo(Q("info-[marker]"));
        console.writeWarning(Q("warn-[marker]"));
        console.writeError(Q("error-[marker]"));
        console.writeFatal(Q("fatal-[marker]"));
        console.writeLine(Q("line-[marker]"));
        QCOMPARE(err.toString(), Q("info: info-[marker]\nwarning: warn-[marker]\nerror: error-[marker]\nfatal: fatal-[marker]\nline-[marker]\n"));
    }

    void consoleService_writeFatal_toTerminal_isColoured()
    {
        StringWriter err;
        ConsoleService console(AnsiConsoleFactory::createHeadless, err, false);
        console.writeFatal(Q("fatal-marker"));
        QCOMPARE(err.toString(), Q("\x1b[91mfatal:\x1b[0m fatal-marker\n"));
    }

    // Own tests of the prompts with scripted keys
    void prompts_textSecretSelection()
    {
        QString screen;
        auto *input = new ScriptedInput(typed(Q("john")) + typed(Q("s3cretX")) +
                                        QList<KeyInfo>{key(ConsoleKey::DownArrow), key(ConsoleKey::DownArrow),
                                                       key(ConsoleKey::UpArrow), key(ConsoleKey::Enter)});
        StringWriter err;
        ConsoleService console(
            [&] { return std::make_unique<TerminalConsole>(std::make_unique<RecordingOutput>(&screen), std::unique_ptr<ITerminalInput>(input)); },
            err, true);
        QCOMPARE(console.promptText(Q("Username")), Q("john"));
        QCOMPARE(screen, Q("Username: john\n"));
        screen.clear();
        QCOMPARE(console.promptSecret(Q("Password")), Q("s3cretX"));
        QCOMPARE(screen, Q("Password: \n"));
        screen.clear();
        QCOMPARE(console.promptSelection(Q("Select an authentication method"), {Q("Web browser"), Q("Device code"), Q("Other")}), 1);
        QVERIFY(screen.startsWith(Q("Select an authentication method\n> Web browser\n  Device code\n  Other\n")));
    }

    void prompts_backspaceMaskAndCancel()
    {
        QString screen;
        QList<KeyInfo> keys = typed(Q("abc"));
        keys.insert(2, KeyInfo{QLatin1Char('\b'), ConsoleKey::Backspace, false, false, false});
        keys.append(key(ConsoleKey::Escape));
        TerminalConsole console(std::make_unique<RecordingOutput>(&screen), std::make_unique<ScriptedInput>(keys));
        QCOMPARE(console.promptSecret(Q("Token"), QLatin1Char('*')), Q("ac"));
        QCOMPARE(screen, Q("Token: **\b \b*\n"));
        try {
            console.promptSelection(Q("Pick"), {Q("a"), Q("b")});
            QFAIL("no exception");
        } catch (const OperationCanceledException &ex) {
            QCOMPARE(ex.message(), Q("User cancelled the prompt"));
        }
    }

    void prompts_numberKeySelects()
    {
        QString screen;
        TerminalConsole console(std::make_unique<RecordingOutput>(&screen),
                                std::make_unique<ScriptedInput>(QList<KeyInfo>{
                                    KeyInfo{QLatin1Char('3'), ConsoleKey::D3, false, false, false}, key(ConsoleKey::Enter)}));
        QCOMPARE(console.promptSelection(Q("Pick"), {Q("a"), Q("b"), Q("c")}), 2);
    }
};

GCM_REGISTER_TEST(AnsiEscapeParserTests)
GCM_REGISTER_TEST(AnsiConsoleFactoryTests)
GCM_REGISTER_TEST(RawModeExclusivityModeTests)
GCM_REGISTER_TEST(ConsoleServiceTests)
#include "consoletests.moc"
