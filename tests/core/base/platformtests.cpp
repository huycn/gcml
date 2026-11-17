// Ports of EnvironmentTests (POSIX cases), IniFileTests, SessionManagerTests, Interop/Linux/LinuxFileSystemTests,
// Interop/Posix/PosixFileSystemTests, plus tests for ChildProcess and the real file system of this port.
// Not ported: Windows/macOS environment cases; WslUtilsTests (IsWslPath / ConvertToDistroPath / CreateWslProcess
// serve GCM running on Windows and are not part of this port).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/git.h"
#include "core/platform.h"
#include "core/strings.h"
#include "infra/nulltrace.h"
#include "infra/testenvironment.h"
#include "infra/testfilesystem.h"
#include "infra/testmain.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

QString createFile(const QString &dir, const QString &name, const QByteArray &content = {})
{
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qFatal("cannot create %s", qPrintable(path));
    }
    file.write(content);
    return path;
}
} // namespace

class EnvironmentTests : public QObject
{
    Q_OBJECT
    const QString PosixPathVar = Q("/home/john.doe/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    const QString PosixExecName = Q("foo");

private Q_SLOTS:
    void posixEnvironment_tryLocateExecutable_notExists_returnFalse()
    {
        TestFileSystem fs;
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QString actualPath = Q("unchanged");
        QVERIFY(!env.tryLocateExecutable(PosixExecName, actualPath));
        QVERIFY(actualPath.isEmpty());
    }

    void posixEnvironment_tryLocateExecutable_exists_returnTrueAndPath()
    {
        const QString expectedPath = Q("/usr/local/bin/foo");
        TestFileSystem fs;
        fs.Files.set(expectedPath, {});
        fs.setExecutable(expectedPath);
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QString actualPath;
        QVERIFY(env.tryLocateExecutable(PosixExecName, actualPath));
        QCOMPARE(actualPath, expectedPath);
    }

    void posixEnvironment_tryLocateExecutable_existsMultiple_returnTrueAndFirstPath()
    {
        const QString expectedPath = Q("/home/john.doe/bin/foo");
        TestFileSystem fs;
        for (const QString &path : {expectedPath, Q("/usr/local/bin/foo"), Q("/bin/foo")}) {
            fs.Files.set(path, {});
            fs.setExecutable(path);
        }
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QString actualPath;
        QVERIFY(env.tryLocateExecutable(PosixExecName, actualPath));
        QCOMPARE(actualPath, expectedPath);
    }

    void posixEnvironment_tryLocateExecutable_notExecutable_skipsToNextMatch()
    {
        const QString nonExecPath = Q("/home/john.doe/bin/foo");
        const QString expectedPath = Q("/usr/local/bin/foo");
        TestFileSystem fs;
        fs.Files.set(nonExecPath, {});
        fs.Files.set(expectedPath, {});
        fs.setExecutable(expectedPath);
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QString actualPath;
        QVERIFY(env.tryLocateExecutable(PosixExecName, actualPath));
        QCOMPARE(actualPath, expectedPath);
    }

    void posixEnvironment_tryLocateExecutable_pathsAreIgnored()
    {
        const QString ignored = Q("/home/john.doe/bin/foo");
        const QString expectedPath = Q("/usr/local/bin/foo");
        TestFileSystem fs;
        for (const QString &path : {ignored, expectedPath}) {
            fs.Files.set(path, {});
            fs.setExecutable(path);
        }
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QString actualPath;
        QVERIFY(env.tryLocateExecutable(PosixExecName, QStringList{ignored}, actualPath));
        QCOMPARE(actualPath, expectedPath);
    }

    void posixEnvironment_setEnvironmentVariable_setsExpectedValue()
    {
        TestFileSystem fs;
        PosixEnvironment env(fs, StringDictionary());
        env.setEnvironmentVariable(Q("GCM_TEST_FOO_BAR"), Q("baz"));
        QCOMPARE(env.variables().tryGet(Q("GCM_TEST_FOO_BAR")), OptString(Q("baz")));
        QCOMPARE(qEnvironmentVariable("GCM_TEST_FOO_BAR"), Q("baz"));
        env.setEnvironmentVariable(Q("GCM_TEST_FOO_BAR"), std::nullopt);
        QVERIFY(!env.variables().contains(Q("GCM_TEST_FOO_BAR")));
        QVERIFY(!qEnvironmentVariableIsSet("GCM_TEST_FOO_BAR"));
    }

    void posixEnvironment_readsProcessEnvironment()
    {
        qputenv("GCM_TEST_REAL_ENV", "value");
        LinuxFileSystem fs;
        PosixEnvironment env(fs);
        QCOMPARE(env.getEnvironmentVariable(Q("GCM_TEST_REAL_ENV")), OptString(Q("value")));
        QVERIFY(!env.getEnvironmentVariable(Q("gcm_test_real_env"))); // case-sensitive
        qputenv("GCM_TEST_REAL_ENV", "changed");
        env.refresh();
        QCOMPARE(env.getEnvironmentVariable(Q("GCM_TEST_REAL_ENV")), OptString(Q("changed")));
        qunsetenv("GCM_TEST_REAL_ENV");
    }

    void locateExecutable_missing_throws()
    {
        TestFileSystem fs;
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        try {
            env.locateExecutable(Q("nope"));
            QFAIL("expected an exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("Failed to locate 'nope' executable on the path."));
        }
    }

    void isDirectoryOnPath()
    {
        TestFileSystem fs;
        fs.IsCaseSensitive = true;
        PosixEnvironment env(fs, StringDictionary{{Q("PATH"), PosixPathVar}});
        QVERIFY(env.isDirectoryOnPath(Q("/usr/local/bin")));
        QVERIFY(!env.isDirectoryOnPath(Q("/opt/bin")));
    }
};
GCM_REGISTER_TEST(EnvironmentTests)

class IniFileTests : public QObject
{
    Q_OBJECT

    static const IniSection *section(const IniFile &file, const char *name, const OptString &subName = std::nullopt)
    {
        const IniSection *s = file.tryGetSection(Q(name), subName);
        if (!s || s->name.name != Q(name) || s->name.subName != subName) {
            return nullptr;
        }
        return s;
    }
    static OptString property(const IniSection &section, const char *name)
    {
        QString value;
        return section.tryGetProperty(Q(name), value) ? OptString(value) : std::nullopt;
    }

private Q_SLOTS:
    void iniSectionName_equality()
    {
        QVERIFY((IniSectionName{Q("foo"), std::nullopt} == IniSectionName{Q("foo"), std::nullopt}));
        QVERIFY((IniSectionName{Q("foo"), std::nullopt} == IniSectionName{Q("FOO"), std::nullopt}));
        QVERIFY(!(IniSectionName{Q("foo"), Q("bar")} == IniSectionName{Q("foo"), Q("BAR")}));
        QVERIFY((IniSectionName{Q("foo"), Q("bar")} == IniSectionName{Q("FOO"), Q("bar")}));
        QVERIFY((IniSectionName{Q("foo"), Q("")} == IniSectionName{Q("foo"), std::nullopt}));
    }

    void iniSerializer_deserialize()
    {
        const QString path = Q("/tmp/test.ini");
        const char *iniText = R"ini(
[one]
    foo = 123
  [two]
    foo   =   abc
# comment
[two "subsection name"] # comment [section]
    foo = this is different # comment prop = val
#[notasection]
    [
[bad #section]
recovery tests]
[]
    ]
    [three]
    bar = a
    bar = b
    # comment
    bar = c
    empty =
[TWO]
    foo = hello
    widget = "Hello, World!"
[four]
[five]
    prop1 = "this hash # is inside quotes"
    prop2 = "this hash # is inside quotes" # this line has two hashes
    prop3 = "   this dquoted string has three spaces around   "
    #prop4 = this property has been commented-out
)ini";
        TestFileSystem fs;
        fs.Files.set(path, iniText);

        const IniFile ini = IniSerializer::deserialize(fs, path);
        QCOMPARE(ini.sections.size(), 6);

        const IniSection *one = section(ini, "one");
        QVERIFY(one);
        QCOMPARE(one->properties.size(), 1);
        QCOMPARE(property(*one, "foo"), OptString(Q("123")));

        const IniSection *twoA = section(ini, "two");
        QVERIFY(twoA);
        QCOMPARE(twoA->properties.size(), 3);
        QCOMPARE(property(*twoA, "foo"), OptString(Q("hello")));
        QCOMPARE(property(*twoA, "widget"), OptString(Q("Hello, World!")));

        const IniSection *twoB = section(ini, "two", Q("subsection name"));
        QVERIFY(twoB);
        QCOMPARE(twoB->properties.size(), 1);
        QCOMPARE(property(*twoB, "foo"), OptString(Q("this is different")));

        const IniSection *three = section(ini, "three");
        QVERIFY(three);
        QCOMPARE(three->properties.size(), 4);
        QStringList bars;
        QVERIFY(three->tryGetMultiProperty(Q("bar"), bars));
        QCOMPARE(bars, QStringList({Q("a"), Q("b"), Q("c")}));
        QCOMPARE(property(*three, "empty"), OptString(Q("")));

        const IniSection *four = section(ini, "four");
        QVERIFY(four);
        QVERIFY(four->properties.isEmpty());

        const IniSection *five = section(ini, "five");
        QVERIFY(five);
        QCOMPARE(five->properties.size(), 3);
        QCOMPARE(property(*five, "prop1"), OptString(Q("this hash # is inside quotes")));
        QCOMPARE(property(*five, "prop2"), OptString(Q("this hash # is inside quotes")));
        QCOMPARE(property(*five, "prop3"), OptString(Q("   this dquoted string has three spaces around   ")));
    }

    void iniSerializer_propertyBeforeSection_throws()
    {
        TestFileSystem fs;
        fs.Files.set(Q("/tmp/bad.ini"), "foo = bar\n");
        QVERIFY_THROWS_EXCEPTION(Exception, IniSerializer::deserialize(fs, Q("/tmp/bad.ini")));
    }
};
GCM_REGISTER_TEST(IniFileTests)

class SessionManagerTests : public QObject
{
    Q_OBJECT

    class CapturingSessionManager : public LinuxSessionManager
    {
    public:
        CapturingSessionManager(ITrace &trace, IEnvironment &env, IFileSystem &fs) : LinuxSessionManager(trace, env, fs) {}
        bool isDesktopSession() const override { return true; }
        QString openedUrl;

    protected:
        void openBrowserInternal(const QString &url) override { openedUrl = url; }
    };

    struct Fixture
    {
        NullTrace trace;
        TestFileSystem fs;
        TestEnvironment env{&fs};
        CapturingSessionManager manager{trace, env, fs};
    };

private Q_SLOTS:
    void openBrowser_escapedUri_preservesEscaping()
    {
        const QString expectedUrl = Q("https://example.com/sign-in/a%20b?q=c%20d");
        Fixture f;
        f.manager.openBrowser(QUrl(expectedUrl));
        QCOMPARE(f.manager.openedUrl, expectedUrl);
    }

    void openBrowser_string_parsesUri()
    {
        Fixture f;
        f.manager.openBrowser(Q("https://example.com/path"));
        QCOMPARE(f.manager.openedUrl, Q("https://example.com/path"));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.openBrowser(Q("not a uri")));
    }

    void openBrowser_nonHttpScheme_throws()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.openBrowser(QUrl(Q("file:///etc/passwd"))));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.openBrowser(QUrl(Q("ftp://example.com/"))));
        QVERIFY(f.manager.openedUrl.isEmpty());
    }

    void isWebBrowserAvailable_noShellHandler_returnsFalse()
    {
        Fixture f;
        QVERIFY(!f.manager.isWebBrowserAvailable());
    }

    void isWebBrowserAvailable_vscodeRemote()
    {
        auto available = [](const QStringList &vars) {
            Fixture f;
            f.fs.Files.set(Q("/usr/bin/xdg-open"), {});
            f.env.Variables.set(Q("PATH"), Q("/usr/bin"));
            for (const QString &name : vars) {
                f.env.Variables.set(name, Q("1"));
            }
            return f.manager.isWebBrowserAvailable();
        };
        const QStringList vscode = {Q("VSCODE_IPC_HOOK_CLI"), Q("BROWSER")};
        QVERIFY(available(vscode + QStringList{Q("SSH_CONNECTION")}));                  // Remote SSH
        QVERIFY(available(vscode + QStringList{Q("REMOTE_CONTAINERS")}));               // Dev Containers
        QVERIFY(!available(vscode));                                                    // Remote Tunnel
        QVERIFY(!available(vscode + QStringList{Q("SSH_CONNECTION"), Q("SSH_TTY")}));   // plain SSH terminal
        QVERIFY(available({})); // falls back to isDesktopSession() (true in the fixture)
    }
};
GCM_REGISTER_TEST(SessionManagerTests)

class LinuxFileSystemTests : public QObject
{
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString baseDir() const { return QDir(m_tmp.path()).canonicalPath(); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_tmp.isValid()); }

    void isSamePath_samePath_returnsTrue()
    {
        LinuxFileSystem fs;
        const QString fileA = createFile(baseDir(), Q("a.file"));
        QVERIFY(fs.isSamePath(fileA, fileA));
    }

    void isSamePath_differentFile_returnsFalse()
    {
        LinuxFileSystem fs;
        const QString fileA = createFile(baseDir(), Q("a.file"));
        const QString fileB = createFile(baseDir(), Q("b.file"));
        QVERIFY(!fs.isSamePath(fileA, fileB));
        QVERIFY(!fs.isSamePath(fileB, fileA));
    }

    void isSamePath_sameFileDifferentCase_returnsFalse()
    {
        LinuxFileSystem fs;
        const QString fileA1 = createFile(baseDir(), Q("a.file"));
        const QString fileA2 = QDir(baseDir()).filePath(Q("A.file"));
        QVERIFY(!fs.isSamePath(fileA1, fileA2));
        QVERIFY(!fs.isSamePath(fileA2, fileA1));
    }

    void isSamePath_sameFileDifferentPathNormalization_returnsTrue()
    {
        LinuxFileSystem fs;
        QVERIFY(QDir(baseDir()).mkpath(Q("subDir1/subDir2")));
        const QString fileA1 = createFile(baseDir(), Q("a.file"));
        const QString fileA2 = baseDir() + Q("/subDir1/subDir2/../../a.file");
        QVERIFY(fs.isSamePath(fileA1, fileA2));
        QVERIFY(fs.isSamePath(fileA2, fileA1));
    }

    void isSamePath_sameFileViaSymlink_returnsTrue()
    {
        LinuxFileSystem fs;
        const QString fileA1 = createFile(baseDir(), Q("a.file"));
        const QString fileA2 = QDir(baseDir()).filePath(Q("a.link"));
        QFile::remove(fileA2);
        QVERIFY(QFile::link(fileA1, fileA2));
        QVERIFY(fs.isSamePath(fileA1, fileA2));
        QVERIFY(fs.isSamePath(fileA2, fileA1));
    }

    void isSamePath_sameFileRelativePath_returnsTrue()
    {
        LinuxFileSystem fs;
        const QString fileA1 = createFile(baseDir(), Q("a.file"));
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(baseDir()));
        const bool ab = fs.isSamePath(fileA1, Q("./a.file"));
        const bool ba = fs.isSamePath(Q("./a.file"), fileA1);
        QDir::setCurrent(previous);
        QVERIFY(ab);
        QVERIFY(ba);
    }

    void isSamePath_blank_returnsFalse()
    {
        LinuxFileSystem fs;
        QVERIFY(!fs.isSamePath(Q(""), Q("")));
        QVERIFY(!fs.isSamePath(Q(" "), Q("/tmp")));
    }

    // PosixFileSystemTests: symbolic links are resolved for files and for directories on the way.
    void resolveSymlinks_fileLinks()
    {
        LinuxFileSystem fs;
        const QString realPath = createFile(baseDir(), Q("realFile.txt"));
        const QString linkPath = QDir(baseDir()).filePath(Q("linkFile.txt"));
        QFile::remove(linkPath);
        QVERIFY(QFile::link(realPath, linkPath));
        QVERIFY(fs.isSamePath(linkPath, realPath));
    }

    void resolveSymlinks_directoryLinks()
    {
        LinuxFileSystem fs;
        QVERIFY(QDir(baseDir()).mkpath(Q("realDir")));
        const QString realDir = QDir(baseDir()).filePath(Q("realDir"));
        const QString linkDir = QDir(baseDir()).filePath(Q("linkDir"));
        QFile::remove(linkDir);
        QVERIFY(QFile::link(realDir, linkDir));
        const QString filePath = createFile(linkDir, Q("file.txt"));
        QVERIFY(fs.isSamePath(filePath, QDir(realDir).filePath(Q("file.txt"))));
    }

    void fileQueries()
    {
        LinuxFileSystem fs;
        const QString file = createFile(baseDir(), Q("plain.txt"), "x");
        QVERIFY(fs.fileExists(file));
        QVERIFY(!fs.fileExists(baseDir()));
        QVERIFY(fs.directoryExists(baseDir()));
        QVERIFY(!fs.directoryExists(file));
        QVERIFY(!fs.fileIsExecutable(file));
        QVERIFY(QFile::setPermissions(file, QFile::permissions(file) | QFileDevice::ExeOwner));
        QVERIFY(fs.fileIsExecutable(file));
        QVERIFY(fs.fileIsExecutable(Q("/bin/sh")));
        QCOMPARE(fs.userDataDirectoryPath(), fs.userHomePath() + Q("/.gcm"));
    }

    void readWriteRoundTrip()
    {
        LinuxFileSystem fs;
        const QString dir = QDir(baseDir()).filePath(Q("nested/deeper"));
        fs.createDirectory(dir);
        fs.createDirectory(dir); // idempotent
        QVERIFY(fs.directoryExists(dir));

        const QString file = QDir(dir).filePath(Q("data.txt"));
        fs.writeAllBytes(file, "line1\r\nline2\nline3\n", true);
        QCOMPARE(QFile::permissions(file) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QCOMPARE(fs.readAllText(file), Q("line1\r\nline2\nline3\n"));
        QCOMPARE(fs.readAllLines(file), QStringList({Q("line1"), Q("line2"), Q("line3")}));
        fs.appendAllText(file, Q("line4"));
        QCOMPARE(fs.readAllLines(file), QStringList({Q("line1"), Q("line2"), Q("line3"), Q("line4")}));

        fs.writeAllBytes(file, "\xEF\xBB\xBFwith bom");
        QCOMPARE(fs.readAllText(file), Q("with bom"));

        fs.deleteFile(file);
        QVERIFY(!fs.fileExists(file));
        fs.deleteFile(file); // deleting a missing file is not an error
        QVERIFY_THROWS_EXCEPTION(IOException, fs.readAllText(file));
    }

    void enumerate()
    {
        LinuxFileSystem fs;
        const QString dir = QDir(baseDir()).filePath(Q("enum"));
        fs.createDirectory(dir + Q("/sub1"));
        fs.createDirectory(dir + Q("/sub2"));
        createFile(dir, Q("a.gpg"));
        createFile(dir, Q("b.gpg"));
        createFile(dir, Q("c.txt"));
        createFile(dir, Q(".gpg-id"));
        QCOMPARE(fs.enumerateFiles(dir, Q("*.gpg")), QStringList({dir + Q("/a.gpg"), dir + Q("/b.gpg")}));
        QCOMPARE(fs.enumerateFiles(dir, Q("*")).size(), 4);
        QCOMPARE(fs.enumerateDirectories(dir), QStringList({dir + Q("/sub1"), dir + Q("/sub2")}));
        QVERIFY_THROWS_EXCEPTION(IOException, fs.enumerateFiles(dir + Q("/missing"), Q("*")));
    }
};
GCM_REGISTER_TEST(LinuxFileSystemTests)

class ChildProcessTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void echo_capturesStandardOutput()
    {
        ProcessManager pm;
        auto process = pm.createProcess(Q("/bin/sh"), Q("-c \"echo hello; echo world\""), QString());
        QVERIFY(process->start());
        QVERIFY(process->id() > 0);
        QCOMPARE(process->readAllStandardOutput(), QByteArray("hello\nworld\n"));
        QVERIFY(process->hasExited());
        QCOMPARE(process->exitCode(), 0);
        QVERIFY(process->id() > 0); // still known after the exit (TRACE2 child_exit reports it)
    }

    void standardInput_isForwardedAndLinesAreRead()
    {
        ProcessStartInfo psi;
        psi.fileName = Q("/bin/cat");
        ChildProcess process(psi);
        QVERIFY(process.start());
        process.writeStandardInput("first\nsecond\r\nthird");
        process.closeStandardInput();
        QCOMPARE(process.readStandardOutputLine(), OptString(Q("first")));
        QCOMPARE(process.readStandardOutputLine(), OptString(Q("second")));
        QCOMPARE(process.readStandardOutputLine(), OptString(Q("third")));
        QCOMPARE(process.readStandardOutputLine(), OptString());
        process.waitForExit();
        QCOMPARE(process.exitCode(), 0);
    }

    void exitCodeAndStandardError()
    {
        ProcessStartInfo psi;
        psi.fileName = Q("/bin/sh");
        psi.argumentList = {Q("-c"), Q("echo oops >&2; exit 3")};
        ChildProcess process(psi);
        QVERIFY(process.start());
        process.waitForExit();
        QCOMPARE(process.exitCode(), 3);
        QCOMPARE(process.readAllStandardError(), QByteArray("oops\n"));
    }

    void environmentAndWorkingDirectory()
    {
        QTemporaryDir tmp;
        ProcessStartInfo psi;
        psi.fileName = Q("/bin/sh");
        psi.argumentList = {Q("-c"), Q("echo \"$GCM_TEST_CHILD_VAR\"; pwd")};
        psi.workingDirectory = tmp.path();
        psi.environment.set(Q("GCM_TEST_CHILD_VAR"), Q("from parent"));
        ChildProcess process(psi);
        QVERIFY(process.start());
        const QList<QByteArray> lines = process.readAllStandardOutput().trimmed().split('\n');
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], QByteArray("from parent"));
        QCOMPARE(QDir(QString::fromUtf8(lines[1])).canonicalPath(), QDir(tmp.path()).canonicalPath());
    }

    void start_missingProgram_returnsFalse()
    {
        ProcessStartInfo psi;
        psi.fileName = Q("/nonexistent/program");
        ChildProcess process(psi);
        QVERIFY(!process.start());
    }

    void kill_stopsTheProcess()
    {
        ProcessStartInfo psi;
        psi.fileName = Q("/bin/sleep");
        psi.argumentList = {Q("60")};
        ChildProcess process(psi);
        QVERIFY(process.start());
        QVERIFY(!process.waitForExit(50));
        process.kill();
        QVERIFY(process.hasExited());
        QVERIFY(process.exitCode() != 0);
    }

    void processManager_defaultsMatchUpstream()
    {
        ProcessManager pm;
        auto process = pm.createProcess(Q("/bin/true"), Q("a b"), Q("/tmp"));
        QCOMPARE(process->startInfo().fileName, Q("/bin/true"));
        QCOMPARE(process->startInfo().arguments, Q("a b"));
        QCOMPARE(process->startInfo().workingDirectory, Q("/tmp"));
        QVERIFY(process->startInfo().redirectStandardInput);
        QVERIFY(process->startInfo().redirectStandardOutput);
        QVERIFY(!process->startInfo().redirectStandardError); // tracing may be enabled in the child
    }
};
GCM_REGISTER_TEST(ChildProcessTests)

class PlatformUtilsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void platformInformation()
    {
        const PlatformInformation info = PlatformUtils::getPlatformInformation();
        QCOMPARE(info.operatingSystemType, Q("Linux"));
        QVERIFY(!info.operatingSystemVersion.isEmpty());
        QVERIFY(!info.cpuArchitecture.isEmpty());
        QVERIFY(info.runtimeVersion.startsWith(Q("Qt/")));
    }

    void nativeEntryPath_isThisExecutable()
    {
        LinuxFileSystem fs;
        const QString path = PlatformUtils::getNativeEntryPath();
        QVERIFY2(path.startsWith(QLatin1Char('/')), qPrintable(path));
        QVERIFY(fs.isSamePath(path, QCoreApplication::applicationFilePath()));
    }
};
GCM_REGISTER_TEST(PlatformUtilsTests)

#include "platformtests.moc"
