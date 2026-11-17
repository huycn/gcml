// Ports of GitVersionTests, GitTests, GitConfigurationTests and ConfigurationServiceTests.
// Tests that run `git` use an isolated global configuration (IsolatedGitConfig) instead of the user's real one;
// upstream modifies (and then restores) the real global configuration.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/git.h"
#include "core/constants.h"
#include "infra/nulltrace.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"
#include "infra/testprocessmanager.h"
#include "infra/testutils.h"

#include <QDir>
#include <QUuid>

using namespace gcm;
using namespace gcm::test;
using namespace gcm::test::GitTestUtilities;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

/// A GitProcess on the real `git`, with its collaborators.
struct GitFixture
{
    explicit GitFixture(const QString &workingDirectory = QString())
        : git(trace, processManager, getGitPath(), workingDirectory)
    {
    }
    NullTrace trace;
    TestProcessManager processManager;
    GitProcess git;
};

QString randomName()
{
    return QUuid::createUuid().toString(QUuid::Id128) + QLatin1Char('.') + QUuid::createUuid().toString(QUuid::Id128);
}

using Entries = QList<std::pair<QString, QString>>;
} // namespace

// =============================================================================================

class GitVersionTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void gitVersion_compareTo_2_33_0_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("expectedCompare");
        QTest::newRow("null") << QString() << 1;
        QTest::newRow("2") << Q("2") << 1;
        QTest::newRow("3") << Q("3") << -1;
        QTest::newRow("2.33") << Q("2.33") << 0;
        QTest::newRow("2.32.0") << Q("2.32.0") << 1;
        QTest::newRow("2.33.0.windows.0.1") << Q("2.33.0.windows.0.1") << 0;
        QTest::newRow("2.33.0.2") << Q("2.33.0.2") << -1;
    }
    void gitVersion_compareTo_2_33_0()
    {
        QFETCH(QString, input);
        QFETCH(int, expectedCompare);
        const GitVersion baseline{2, 33, 0};
        const GitVersion actual(input);
        QCOMPARE(GitVersion::compare(baseline, actual), expectedCompare);
    }

    // Own tests
    void gitVersion_originalString()
    {
        QCOMPARE(GitVersion(Q("2.45.1.windows.1")).originalString(), Q("2.45.1.windows.1"));
        QCOMPARE(GitVersion(Q("2.45.1.windows.1")).toString(), Q("2.45.1"));
        QCOMPARE((GitVersion{2, 18}).originalString(), Q("2.18"));
        QCOMPARE(GitVersion(QString()).originalString(), QString());
    }
};

// =============================================================================================

class GitTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<IsolatedGitConfig> m_isolation;

    static void assertRemote(const QString &expectedName, const OptString &expectedFetchUrl,
                             const OptString &expectedPushUrl, const GitRemote &remote)
    {
        QCOMPARE(remote.name, expectedName);
        QCOMPARE(remote.fetchUrl, expectedFetchUrl);
        QCOMPARE(remote.pushUrl, expectedPushUrl);
    }

private Q_SLOTS:
    void initTestCase() { m_isolation = std::make_unique<IsolatedGitConfig>(); }
    void cleanupTestCase() { m_isolation.reset(); }

    void git_getCurrentRepository_noLocalRepo_returnsNull()
    {
        GitFixture f(QDir::tempPath());
        QCOMPARE(f.git.getCurrentRepository(), OptString());
    }

    void git_getCurrentRepository_localRepo_returnsNotNull()
    {
        QString workDirPath;
        createRepository(&workDirPath);
        GitFixture f(workDirPath);
        QVERIFY(f.git.getCurrentRepository().has_value());
        QVERIFY(f.git.isInsideRepository());
    }

    void git_getRemotes_noLocalRepo_returnsEmpty()
    {
        GitFixture f(QDir::tempPath());
        QVERIFY(f.git.getRemotes().isEmpty());
    }

    void git_getRemotes_noRemotes_returnsEmpty()
    {
        QString workDirPath;
        createRepository(&workDirPath);
        GitFixture f(workDirPath);
        QVERIFY(f.git.getRemotes().isEmpty());
    }

    void git_getRemotes_oneRemote_returnsRemote()
    {
        const QString name = Q("origin"), url = Q("https://example.com");
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("remote add %1 %2").arg(name, url));

        GitFixture f(workDirPath);
        const QList<GitRemote> remotes = f.git.getRemotes();
        QCOMPARE(remotes.size(), 1);
        assertRemote(name, url, url, remotes[0]);
    }

    void git_getRemotes_oneRemoteFetchAndPull_returnsRemote()
    {
        const QString name = Q("origin"), fetchUrl = Q("https://fetch.example.com"), pushUrl = Q("https://push.example.com");
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("remote add %1 %2").arg(name, fetchUrl));
        execGit(repoPath, workDirPath, Q("remote set-url --push %1 %2").arg(name, pushUrl));

        GitFixture f(workDirPath);
        const QList<GitRemote> remotes = f.git.getRemotes();
        QCOMPARE(remotes.size(), 1);
        assertRemote(name, fetchUrl, pushUrl, remotes[0]);
    }

    void git_getRemotes_nonHttpRemote_returnsRemote_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("ssh") << Q("ssh://user@example.com/account/repo.git");
        QTest::newRow("scp") << Q("user@example.com:account/repo.git");
        QTest::newRow("git") << Q("git://example.com/path/to/repo.git");
        QTest::newRow("file") << Q("file:///path/to/repo.git");
        QTest::newRow("path") << Q("/path/to/repo.git");
    }
    void git_getRemotes_nonHttpRemote_returnsRemote()
    {
        QFETCH(QString, url);
        const QString name = Q("origin");
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("remote add %1 %2").arg(name, url));

        GitFixture f(workDirPath);
        const QList<GitRemote> remotes = f.git.getRemotes();
        QCOMPARE(remotes.size(), 1);
        assertRemote(name, url, url, remotes[0]);
    }

    void git_getRemotes_multipleRemotes_returnsAllRemotes()
    {
        const QStringList names{Q("origin"), Q("test"), Q("upstream")};
        const QStringList urls{Q("https://example.com/origin"), Q("https://example.com/test"),
                               Q("https://example.com/upstream")};
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        for (int i = 0; i < 3; ++i) {
            execGit(repoPath, workDirPath, Q("remote add %1 %2").arg(names[i], urls[i]));
        }

        GitFixture f(workDirPath);
        const QList<GitRemote> remotes = f.git.getRemotes();
        QCOMPARE(remotes.size(), 3);
        for (int i = 0; i < 3; ++i) {
            assertRemote(names[i], urls[i], urls[i], remotes[i]);
        }
    }

    void git_getRemotes_remoteNoFetchOnlyPull_returnsRemote()
    {
        const QString name = Q("origin"), pushUrl = Q("https://example.com");
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("remote add %1 %2").arg(name, pushUrl));
        execGit(repoPath, workDirPath, Q("remote set-url --push %1 %2").arg(name, pushUrl));
        execGit(repoPath, workDirPath, Q("config --unset --local remote.%1.url").arg(name));

        GitFixture f(workDirPath);
        const QList<GitRemote> remotes = f.git.getRemotes();
        QCOMPARE(remotes.size(), 1);
        assertRemote(name, std::nullopt, pushUrl, remotes[0]);
    }

    void git_version_returnsVersion()
    {
        GitFixture f(QDir::tempPath());
        QVERIFY(f.git.version() != GitVersion());
    }
};

// =============================================================================================

class GitConfigurationTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<IsolatedGitConfig> m_isolation;

    static Entries enumerateFoo(IGitConfiguration &config, const QString &prefix, int stopAfter = -1)
    {
        Entries visited;
        config.enumerate([&](const GitConfigurationEntry &entry) {
            if (entry.key.startsWith(prefix)) {
                visited.append({entry.key, entry.value});
            }
            return stopAfter < 0 || visited.size() < stopAfter;
        });
        return visited;
    }

private Q_SLOTS:
    void initTestCase() { m_isolation = std::make_unique<IsolatedGitConfig>(); }
    void cleanupTestCase() { m_isolation.reset(); }

    void gitConfiguration_quoteCmdArg_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("null") << QString() << Q("\"\"");
        QTest::newRow("empty") << Q("") << Q("\"\"");
        QTest::newRow("hello") << Q("hello") << Q("hello");
        QTest::newRow("hello world") << Q("hello world") << Q("\"hello world\"");
        QTest::newRow("C:\\app.exe") << Q("C:\\app.exe") << Q("C:\\app.exe");
        QTest::newRow("path with space") << Q("C:\\path with space\\app.exe") << Q("\"C:\\path with space\\app.exe\"");
        QTest::newRow("''") << Q("''") << Q("\"''\"");
        QTest::newRow("'hello'") << Q("'hello'") << Q("\"'hello'\"");
        QTest::newRow("'hello world'") << Q("'hello world'") << Q("\"'hello world'\"");
        QTest::newRow("'C:\\app.exe'") << Q("'C:\\app.exe'") << Q("\"'C:\\app.exe'\"");
        QTest::newRow("'path with space'") << Q("'C:\\path with space\\app.exe'") << Q("\"'C:\\path with space\\app.exe'\"");
        QTest::newRow("\"\"") << Q("\"\"") << Q("\"\\\"\\\"\"");
        QTest::newRow("\"hello\"") << Q("\"hello\"") << Q("\"\\\"hello\\\"\"");
        QTest::newRow("\"hello world\"") << Q("\"hello world\"") << Q("\"\\\"hello world\\\"\"");
        QTest::newRow("\"C:\\app.exe\"") << Q("\"C:\\app.exe\"") << Q("\"\\\"C:\\app.exe\\\"\"");
        QTest::newRow("\"path with space\"") << Q("\"C:\\path with space\\app.exe\"") << Q("\"\\\"C:\\path with space\\app.exe\\\"\"");
        QTest::newRow("\\") << Q("\\") << Q("\\");
        QTest::newRow("\\\\") << Q("\\\\") << Q("\\\\");
        QTest::newRow("\\\\\\") << Q("\\\\\\") << Q("\\\\\\");
        QTest::newRow("\"") << Q("\"") << Q("\"\\\"\"");
        QTest::newRow("\\\"") << Q("\\\"") << Q("\"\\\\\\\"\"");
        QTest::newRow("\\\\\"") << Q("\\\\\"") << Q("\"\\\\\\\\\\\"\"");
        QTest::newRow("\"\\") << Q("\"\\") << Q("\"\\\"\\\\\"");
        QTest::newRow("\"\\\\") << Q("\"\\\\") << Q("\"\\\"\\\\\\\\\"");
        QTest::newRow("ab\\") << Q("ab\\") << Q("ab\\");
        QTest::newRow("a b\\") << Q("a b\\") << Q("\"a b\\\\\"");
    }
    void gitConfiguration_quoteCmdArg()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(GitProcessConfiguration::quoteCmdArg(input), expected);
    }

    // Own test: whatever quoteCmdArg produces must split back into the original argument.
    void gitConfiguration_quoteCmdArg_roundTripsThroughSplitCommandLine_data() { gitConfiguration_quoteCmdArg_data(); }
    void gitConfiguration_quoteCmdArg_roundTripsThroughSplitCommandLine()
    {
        QFETCH(QString, input);
        const QStringList args = splitCommandLine(Q("config ") + GitProcessConfiguration::quoteCmdArg(input));
        QCOMPARE(args.size(), 2);
        QCOMPARE(args[1], input.isNull() ? Q("") : input);
    }

    void gitProcess_getConfiguration_returnsConfiguration()
    {
        GitFixture f;
        IGitConfiguration &config = f.git.getConfiguration();
        Q_UNUSED(config);
    }

    void gitConfiguration_enumerate_callbackReturnsTrue_invokesCallbackForEachEntry()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local foo.name lancelot")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.quest seek-holy-grail")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.favcolor blue")).assertSuccess();
        const Entries expected{{Q("foo.name"), Q("lancelot")}, {Q("foo.quest"), Q("seek-holy-grail")}, {Q("foo.favcolor"), Q("blue")}};

        GitFixture f(repoPath);
        QCOMPARE(enumerateFoo(f.git.getConfiguration(), Q("foo.")), expected);
    }

    void gitConfiguration_enumerate_callbackReturnsFalse_invokesCallbackForEachEntryUntilReturnsFalse()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local foo.name lancelot")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.quest seek-holy-grail")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.favcolor blue")).assertSuccess();
        const Entries expected{{Q("foo.name"), Q("lancelot")}, {Q("foo.quest"), Q("seek-holy-grail")}};

        GitFixture f(repoPath);
        // Stop enumeration after 2 'foo' entries
        QCOMPARE(enumerateFoo(f.git.getConfiguration(), Q("foo."), 2), expected);
    }

    // Own test: the uncached `git config --list` fallback behaves the same.
    void gitConfiguration_enumerate_noCache_invokesCallbackForEachEntryUntilReturnsFalse()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local foo.name lancelot")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.quest seek-holy-grail")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local foo.favcolor blue")).assertSuccess();

        GitFixture f(repoPath);
        GitProcessConfiguration config(f.trace, f.git, false);
        QCOMPARE(enumerateFoo(config, Q("foo."), 2), (Entries{{Q("foo.name"), Q("lancelot")}, {Q("foo.quest"), Q("seek-holy-grail")}}));
        QString value;
        QVERIFY(config.tryGet(Q("foo.quest"), false, value));
        QCOMPARE(value, Q("seek-holy-grail"));
        QVERIFY(!config.tryGet(randomName(), false, value));
    }

    void gitConfiguration_tryGet_name_exists_returnsTrueOutString()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local user.name john.doe")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(Q("user.name"), false, value));
        QCOMPARE(value, Q("john.doe"));
    }

    void gitConfiguration_tryGet_name_doesNotExists_returnsFalse()
    {
        const QString repoPath = createRepository();
        GitFixture f(repoPath);
        QString value = Q("unchanged");
        QVERIFY(!f.git.getConfiguration().tryGet(randomName(), false, value));
        QVERIFY(value.isEmpty());
    }

    void gitConfiguration_tryGet_isPath_true_returnsCanonicalPath()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local example.path ~/test")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(Q("example.path"), true, value));
        QCOMPARE(value, QDir::cleanPath(QDir::homePath() + Q("/test")));
    }

    void gitConfiguration_tryGet_isPath_false_returnsRawConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local example.path ~/test")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(Q("example.path"), false, value));
        QCOMPARE(value, Q("~/test"));
    }

    void gitConfiguration_tryGet_boolType_returnsCanonicalBool()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local example.bool fAlSe")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(GitConfigurationLevel::Local, GitConfigurationType::Bool, Q("example.bool"), value));
        QCOMPARE(value, Q("false"));
    }

    void gitConfiguration_tryGet_boolWithoutType_returnsRawConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local example.bool fAlSe")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(GitConfigurationLevel::Local, GitConfigurationType::Raw, Q("example.bool"), value));
        QCOMPARE(value, Q("fAlSe"));
    }

    void gitConfiguration_get_name_exists_returnsString()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local user.name john.doe")).assertSuccess();

        GitFixture f(repoPath);
        QCOMPARE(f.git.getConfiguration().get(Q("user.name")), Q("john.doe"));
    }

    void gitConfiguration_get_name_doesNotExists_throwsException()
    {
        const QString repoPath = createRepository();
        GitFixture f(repoPath);
        QVERIFY_THROWS_EXCEPTION(KeyNotFoundException, f.git.getConfiguration().get(randomName()));
    }

    void gitConfiguration_set_local_setsLocalConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        GitFixture f(repoPath);
        f.git.getConfiguration().set(GitConfigurationLevel::Local, Q("core.foobar"), Q("foo123"));

        const GitResult localResult = execGit(repoPath, workDirPath, Q("config --local core.foobar"));
        QCOMPARE(localResult.standardOutput.trimmed(), Q("foo123"));
    }

    void gitConfiguration_set_all_throwsException()
    {
        const QString repoPath = createRepository();
        GitFixture f(repoPath);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException,
                                 f.git.getConfiguration().set(GitConfigurationLevel::All, Q("core.foobar"), Q("test123")));
    }

    void gitConfiguration_unset_global_unsetsGlobalConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --global core.foobar alice")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local core.foobar bob")).assertSuccess();

        GitFixture f(repoPath);
        f.git.getConfiguration().unset(GitConfigurationLevel::Global, Q("core.foobar"));

        QCOMPARE(execGit(repoPath, workDirPath, Q("config --global core.foobar")).standardOutput.trimmed(), QString());
        QCOMPARE(execGit(repoPath, workDirPath, Q("config --local core.foobar")).standardOutput.trimmed(), Q("bob"));
        execGit(repoPath, workDirPath, Q("config --global --unset core.foobar"));
    }

    void gitConfiguration_unset_local_unsetsLocalConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --global core.foobar alice")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local core.foobar bob")).assertSuccess();

        GitFixture f(repoPath);
        f.git.getConfiguration().unset(GitConfigurationLevel::Local, Q("core.foobar"));

        QCOMPARE(execGit(repoPath, workDirPath, Q("config --global core.foobar")).standardOutput.trimmed(), Q("alice"));
        QCOMPARE(execGit(repoPath, workDirPath, Q("config --local core.foobar")).standardOutput.trimmed(), QString());
        execGit(repoPath, workDirPath, Q("config --global --unset core.foobar"));
    }

    void gitConfiguration_unset_all_throwsException()
    {
        const QString repoPath = createRepository();
        GitFixture f(repoPath);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException,
                                 f.git.getConfiguration().unset(GitConfigurationLevel::All, Q("core.foobar")));
    }

    void gitConfiguration_unsetAll_unsetsAllConfig()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local --add core.foobar foo1")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local --add core.foobar foo2")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local --add core.foobar bar1")).assertSuccess();

        GitFixture f(repoPath);
        f.git.getConfiguration().unsetAll(GitConfigurationLevel::Local, Q("core.foobar"), Q("foo*"));

        QCOMPARE(execGit(repoPath, workDirPath, Q("config --local --get-all core.foobar")).standardOutput.trimmed(), Q("bar1"));
    }

    void gitConfiguration_unsetAll_all_throwsException()
    {
        const QString repoPath = createRepository();
        GitFixture f(repoPath);
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException,
                                 f.git.getConfiguration().unsetAll(GitConfigurationLevel::All, Q("core.foobar"),
                                                                   Constants::RegexPatterns::Any));
    }

    // Own tests for the write and regex operations upstream only exercises indirectly.
    void gitConfiguration_addGetAllGetRegexReplaceAll()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        config.add(GitConfigurationLevel::Local, Q("credential.helper"), QString());
        config.add(GitConfigurationLevel::Local, Q("credential.helper"), Q("/usr/bin/gcm with space"));
        QCOMPARE(config.getAll(Q("credential.helper")), (QStringList{QString(), Q("/usr/bin/gcm with space")}));
        QCOMPARE(config.getRegex(Q("credential\\.help.*"), Q("gcm")), QStringList{Q("/usr/bin/gcm with space")});
        QCOMPARE(config.getRegex(GitConfigurationLevel::Local, GitConfigurationType::Raw, Q("credential\\.helper"), std::nullopt),
                 (QStringList{QString(), Q("/usr/bin/gcm with space")}));
        config.replaceAll(GitConfigurationLevel::Local, Q("credential.helper"), Q("gcm"), Q("other"));
        QCOMPARE(config.getAll(GitConfigurationLevel::Local, GitConfigurationType::Raw, Q("credential.helper")),
                 (QStringList{QString(), Q("other")}));
        // Removing a value that does not exist is not an error
        config.unsetAll(GitConfigurationLevel::Local, Q("credential.missing"), std::nullopt);

        GitProcessConfiguration uncached(f.trace, f.git, false);
        QCOMPARE(uncached.getAll(Q("credential.helper")), (QStringList{QString(), Q("other")}));
        QCOMPARE(uncached.getAll(randomName()), QStringList());
    }

    void gitConfiguration_enumerateSectionProperty_filtersEntries()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local credential.https://example.com.provider generic")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local credential.provider github")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local credential.helper manager")).assertSuccess();

        GitFixture f(repoPath);
        QStringList values;
        f.git.getConfiguration().enumerate(Q("CREDENTIAL"), Q("Provider"), [&](const GitConfigurationEntry &e) {
            values.append(e.value);
            return true;
        });
        QCOMPARE(values, (QStringList{Q("generic"), Q("github")}));
    }

    void gitConfiguration_cacheTryGet_returnsValueFromCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local user.name john.doe")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local user.email john@example.com")).assertSuccess();

        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        QString value1, value2;
        // First access loads the cache, the second uses it
        QVERIFY(config.tryGet(Q("user.name"), false, value1));
        QCOMPARE(value1, Q("john.doe"));
        QVERIFY(config.tryGet(Q("user.email"), false, value2));
        QCOMPARE(value2, Q("john@example.com"));
    }

    void gitConfiguration_cacheGetAll_returnsAllValuesFromCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local --add test.multi value1")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local --add test.multi value2")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local --add test.multi value3")).assertSuccess();

        GitFixture f(repoPath);
        QCOMPARE(f.git.getConfiguration().getAll(Q("test.multi")), (QStringList{Q("value1"), Q("value2"), Q("value3")}));
    }

    void gitConfiguration_cacheEnumerate_enumeratesFromCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local cache.name test-value")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local cache.enabled true")).assertSuccess();

        GitFixture f(repoPath);
        const Entries entries = enumerateFoo(f.git.getConfiguration(), Q("cache."));
        QCOMPARE(entries.size(), 2);
        QVERIFY(entries.contains(std::pair(Q("cache.name"), Q("test-value"))));
        QVERIFY(entries.contains(std::pair(Q("cache.enabled"), Q("true"))));
    }

    void gitConfiguration_cacheInvalidation_setInvalidatesCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local test.value initial")).assertSuccess();

        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        QString value;
        QVERIFY(config.tryGet(Q("test.value"), false, value));
        QCOMPARE(value, Q("initial"));
        config.set(GitConfigurationLevel::Local, Q("test.value"), Q("updated"));
        QVERIFY(config.tryGet(Q("test.value"), false, value));
        QCOMPARE(value, Q("updated"));
    }

    void gitConfiguration_cacheInvalidation_addInvalidatesCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local test.multi first")).assertSuccess();

        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        QCOMPARE(config.getAll(Q("test.multi")), QStringList{Q("first")});
        config.add(GitConfigurationLevel::Local, Q("test.multi"), Q("second"));
        QCOMPARE(config.getAll(Q("test.multi")), (QStringList{Q("first"), Q("second")}));
    }

    void gitConfiguration_cacheInvalidation_unsetInvalidatesCache()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local test.value exists")).assertSuccess();

        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        QString value;
        QVERIFY(config.tryGet(Q("test.value"), false, value));
        QCOMPARE(value, Q("exists"));
        config.unset(GitConfigurationLevel::Local, Q("test.value"));
        QVERIFY(!config.tryGet(Q("test.value"), false, value));
        QVERIFY(value.isEmpty());
    }

    void gitConfiguration_cacheLevelFilter_returnsOnlyLocalValues()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --global test.level global-value")).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local test.level local-value")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(GitConfigurationLevel::Local, GitConfigurationType::Raw, Q("test.level"), value));
        QCOMPARE(value, Q("local-value"));
        QVERIFY(f.git.getConfiguration().tryGet(GitConfigurationLevel::Global, GitConfigurationType::Raw, Q("test.level"), value));
        QCOMPARE(value, Q("global-value"));
        execGit(repoPath, workDirPath, Q("config --global --unset test.level"));
    }

    void gitConfiguration_typedQuery_canonicalizesValues()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        execGit(repoPath, workDirPath, Q("config --local test.path ~/example")).assertSuccess();

        GitFixture f(repoPath);
        QString value;
        QVERIFY(f.git.getConfiguration().tryGet(GitConfigurationLevel::Local, GitConfigurationType::Path, Q("test.path"), value));
        QVERIFY(!value.isEmpty());
        QVERIFY(value != Q("~/example"));
    }

    void gitConfiguration_scopedConfig_processesIncludes()
    {
        const QString configKey = Q("test.value");
        const QString configTrigger = Q("remote.*.url:http://localhost/**");
        const QString configValue = Q("foo123");
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        const QByteArray oldGlobal = qgetenv("GIT_CONFIG_GLOBAL");
        qputenv("GIT_CONFIG_GLOBAL", (repoPath + Q("/global.config")).toUtf8());
        execGit(repoPath, workDirPath, Q("config --file %1/git.config.glob %2 %3").arg(workDirPath, configKey, configValue)).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --global includeif.hasconfig:%1.path %2/git.config.glob").arg(configTrigger, workDirPath)).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --file %1/git.config.loc %2 %3").arg(workDirPath, configKey, configValue)).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local include.path %1/git.config.loc").arg(workDirPath)).assertSuccess();
        execGit(repoPath, workDirPath, Q("config --local remote.test.url http://localhost/my/repo")).assertSuccess();

        GitFixture f(repoPath);
        IGitConfiguration &config = f.git.getConfiguration();
        // The value of an included file is reported at the level of the including file
        for (GitConfigurationLevel scope : {GitConfigurationLevel::Global, GitConfigurationLevel::Local}) {
            QString value;
            const bool found = config.tryGet(scope, GitConfigurationType::Raw, configKey, value);
            if (!found || value != configValue) {
                qputenv("GIT_CONFIG_GLOBAL", oldGlobal);
            }
            QVERIFY(found);
            QCOMPARE(value, configValue);
        }
        qputenv("GIT_CONFIG_GLOBAL", oldGlobal);
    }

    // Own test: helper protocol round trip through `git credential-store` style helper invocation.
    void gitProcess_invokeHelper_returnsDictionary()
    {
        QString workDirPath;
        const QString repoPath = createRepository(&workDirPath);
        GitFixture f(repoPath);
        const StringDictionary input{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                     {Q("username"), Q("john")}, {Q("password"), Q("letmein")}};
        // `git credential fill` with a helper that always answers from the request: use the request itself.
        const StringDictionary result = f.git.invokeHelper(
            Q("-c credential.helper= -c credential.interactive=never credential fill"), &input);
        QCOMPARE(result.value(Q("USERNAME")), Q("john"));
        QCOMPARE(result.value(Q("password")), Q("letmein"));
    }
};

// =============================================================================================

namespace {
class RecordingComponent : public IConfigurableComponent
{
public:
    explicit RecordingComponent(QString name) : m_name(std::move(name)) {}
    QString componentName() const override { return m_name; }
    void configure(ConfigurationTarget target) override { configured.append(target); }
    void unconfigure(ConfigurationTarget target) override { unconfigured.append(target); }
    QList<ConfigurationTarget> configured;
    QList<ConfigurationTarget> unconfigured;

private:
    QString m_name;
};
} // namespace

class ConfigurationServiceTests : public QObject
{
    Q_OBJECT

    static void run(bool configure, ConfigurationTarget target)
    {
        TestCommandContext context;
        ConfigurationService service(context);
        RecordingComponent c1(Q("one")), c2(Q("two")), c3(Q("three"));
        service.addComponent(&c1);
        service.addComponent(&c2);
        service.addComponent(&c3);

        if (configure) {
            service.configure(target);
        } else {
            service.unconfigure(target);
        }

        for (RecordingComponent *c : {&c1, &c2, &c3}) {
            QCOMPARE(configure ? c->configured : c->unconfigured, QList<ConfigurationTarget>{target});
            QVERIFY((configure ? c->unconfigured : c->configured).isEmpty());
        }
        const QString verb = configure ? Q("Configuring") : Q("Unconfiguring");
        QCOMPARE(context.Console.WrittenMessages,
                 (QStringList{Q("%1 component 'one'...").arg(verb), Q("%1 component 'two'...").arg(verb),
                              Q("%1 component 'three'...").arg(verb)}));
    }

private Q_SLOTS:
    void configurationService_configure_system_componentsAreConfiguredWithSystem() { run(true, ConfigurationTarget::System); }
    void configurationService_configure_user_componentsAreConfiguredWithUser() { run(true, ConfigurationTarget::User); }
    void configurationService_unconfigure_system_componentsAreUnconfiguredWithSystem() { run(false, ConfigurationTarget::System); }
    void configurationService_unconfigure_user_componentsAreUnconfiguredWithUser() { run(false, ConfigurationTarget::User); }
};

GCM_REGISTER_TEST(GitVersionTests)
GCM_REGISTER_TEST(GitTests)
GCM_REGISTER_TEST(GitConfigurationTests)
GCM_REGISTER_TEST(ConfigurationServiceTests)
#include "gittests.moc"
