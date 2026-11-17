// Ports of PlaintextCredentialStoreTests, Interop/Posix/GnuPassCredentialStoreTests and
// Interop/Linux/SecretServiceCollectionTests, plus own tests of the store selection (CredentialStore).
// The Secret Service read/write tests need a running provider and are skipped unless GCM_TEST_SECRETSERVICE=1
// (upstream skips them unconditionally).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/constants.h"
#include "core/stores.h"
#include "infra/testcommandcontext.h"
#include "infra/testgpg.h"
#include "infra/testmain.h"

#include <QDir>
#include <QUuid>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }
const QString TestNamespace = Q("git-test");
QString guid() { return QUuid::createUuid().toString(QUuid::Id128); }

/// GnuPassCredentialStoreTests.InitializePasswordStore
QString initializePasswordStore(TestFileSystem &fs, TestGpg &gpg)
{
    const QString storePath = QDir::homePath() + Q("/.password-store");
    const QString userId = Q("gcm-test@example.com");
    gpg.generateKeys(userId);
    fs.Directories.insert(storePath);
    fs.Files.set(storePath + Q("/.gpg-id"), userId.toUtf8());
    return storePath;
}
} // namespace

class PlaintextCredentialStoreTests : public QObject
{
    Q_OBJECT
    const QString StoreRoot = Q("/tmp/test-store");

private Q_SLOTS:
    void plaintextCredentialStore_readWriteDelete()
    {
        TestFileSystem fs;
        PlaintextCredentialStore collection(fs, StoreRoot, TestNamespace);
        const QString uniqueGuid = guid();
        const QString service = Q("https://example.com/") + uniqueGuid;
        const QString expectedFilePath = StoreRoot + Q("/git-test/https/example.com/") + uniqueGuid + Q("/john.doe.credential");
        const QByteArray expectedFileBytes = Q("letmein123\nservice=%1\naccount=john.doe\n").arg(service).toUtf8();

        collection.addOrUpdate(service, Q("john.doe"), Q("letmein123"));
        const std::optional<Credential> outCredential = collection.get(service, Q("john.doe"));
        QVERIFY(outCredential);
        QCOMPARE(outCredential->account, Q("john.doe"));
        QCOMPARE(outCredential->password, Q("letmein123"));
        QVERIFY(fs.Files.contains(expectedFilePath));
        QCOMPARE(fs.Files.value(expectedFilePath), expectedFileBytes);

        QVERIFY(collection.remove(service, Q("john.doe")));
        QVERIFY(!fs.Files.contains(expectedFilePath));
    }

    void plaintextCredentialStore_get_notFound_returnsNull()
    {
        TestFileSystem fs;
        PlaintextCredentialStore collection(fs, StoreRoot, TestNamespace);
        QVERIFY(!collection.get(Q("https://example.com/") + guid(), std::nullopt));
    }

    void plaintextCredentialStore_remove_notFound_returnsFalse()
    {
        TestFileSystem fs;
        PlaintextCredentialStore collection(fs, StoreRoot, TestNamespace);
        QVERIFY(!collection.remove(Q("https://example.com/") + guid(), std::nullopt));
    }

    void plaintextCredentialStore_maxCredentialSize_returnsZero()
    {
        TestFileSystem fs;
        PlaintextCredentialStore collection(fs, StoreRoot, TestNamespace);
        QCOMPARE(collection.maxCredentialSize(), 0);
    }

    // Own tests
    void plaintextCredentialStore_multipleAccounts_portAndUpdate()
    {
        TestFileSystem fs;
        PlaintextCredentialStore collection(fs, StoreRoot);
        const QString service = Q("https://example.com:8443/repo");
        collection.addOrUpdate(service, Q("alice"), Q("a1"));
        collection.addOrUpdate(service, Q("bob"), Q("b1"));
        collection.addOrUpdate(service, Q("alice"), Q("a2"));
        QVERIFY(fs.Files.contains(StoreRoot + Q("/https/example.com:8443/repo/alice.credential")));
        QCOMPARE(collection.getAccounts(service), (QStringList{Q("alice"), Q("bob")}));
        QCOMPARE(collection.get(service, Q("ALICE"))->password, Q("a2"));
        QCOMPARE(collection.get(service, std::nullopt)->account, Q("alice"));
        QVERIFY(collection.getAccounts(Q("https://other.example.com")).isEmpty());
    }

    void plaintextCredentialStore_realFileSystem_roundTrips()
    {
        LinuxFileSystem fs;
        const QString root = QDir::tempPath() + Q("/gcm-plaintext-") + guid();
        PlaintextCredentialStore collection(fs, root, TestNamespace);
        collection.addOrUpdate(Q("https://example.com"), Q("john"), Q("secret"));
        QCOMPARE(collection.get(Q("https://example.com"), Q("john"))->password, Q("secret"));
        QCOMPARE(QFileInfo(root).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        QVERIFY(collection.remove(Q("https://example.com"), Q("john")));
        QDir(root).removeRecursively();
    }
};

// =============================================================================================

class GnuPassCredentialStoreTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void gnuPassCredentialStore_readWriteDelete()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        const QString storeRoot = initializePasswordStore(fs, gpg);
        GpgPassCredentialStore collection(fs, gpg, storeRoot, TestNamespace);
        const QString uniqueGuid = guid();
        const QString service = Q("https://example.com/") + uniqueGuid;
        const QString expectedFilePath = storeRoot + Q("/git-test/https/example.com/") + uniqueGuid + Q("/john.doe.gpg");

        collection.addOrUpdate(service, Q("john.doe"), Q("letmein123"));
        const std::optional<Credential> outCredential = collection.get(service, Q("john.doe"));
        QVERIFY(outCredential);
        QCOMPARE(outCredential->password, Q("letmein123"));
        QCOMPARE(fs.Files.value(expectedFilePath), Q("letmein123\nservice=%1\naccount=john.doe\n").arg(service).toUtf8());
        QVERIFY(collection.remove(service, Q("john.doe")));
    }

    void gnuPassCredentialStore_get_notFound_returnsNull()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        GpgPassCredentialStore collection(fs, gpg, initializePasswordStore(fs, gpg), TestNamespace);
        QVERIFY(!collection.get(Q("https://example.com/") + guid(), std::nullopt));
    }

    void gnuPassCredentialStore_remove_notFound_returnsFalse()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        GpgPassCredentialStore collection(fs, gpg, initializePasswordStore(fs, gpg), TestNamespace);
        QVERIFY(!collection.remove(Q("https://example.com/") + guid(), std::nullopt));
    }

    void gnuPassCredentialStore_readWriteDelete_gpgIdInSubdirectory()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        const QString storePath = QDir::homePath() + Q("/.password-store");
        const QString subDirPath = storePath + Q("/") + TestNamespace;
        gpg.generateKeys(Q("gcm-test@example.com"));
        fs.Directories.insert(storePath);
        fs.Directories.insert(subDirPath);
        fs.Files.set(subDirPath + Q("/.gpg-id"), "gcm-test@example.com");

        GpgPassCredentialStore collection(fs, gpg, storePath, TestNamespace);
        const QString service = Q("https://example.com/") + guid();
        const QString password = guid();
        collection.addOrUpdate(service, Q("john.doe"), password);
        const std::optional<Credential> outCredential = collection.get(service, Q("john.doe"));
        QVERIFY(outCredential);
        QCOMPARE(outCredential->account, Q("john.doe"));
        QCOMPARE(outCredential->password, password);
        collection.remove(service, Q("john.doe"));
    }

    void gnuPassCredentialStore_writeCredential_multipleGpgIds_usesNearestGpgId()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        const QString storePath = QDir::homePath() + Q("/.password-store");
        // Only the personal key exists: picking the work key would make encryptFile throw
        gpg.generateKeys(Q("personal@example.com"));
        fs.Directories = {storePath, storePath + Q("/personal"), storePath + Q("/work")};
        fs.Files.set(storePath + Q("/personal/.gpg-id"), "personal@example.com");
        fs.Files.set(storePath + Q("/work/.gpg-id"), "work@example.com");

        GpgPassCredentialStore collection(fs, gpg, storePath, Q("personal"));
        const QString service = Q("https://example.com/") + guid();
        const QString password = guid();
        collection.addOrUpdate(service, Q("john.doe"), password);
        const std::optional<Credential> outCredential = collection.get(service, Q("john.doe"));
        QVERIFY(outCredential);
        QCOMPARE(outCredential->password, password);
        collection.remove(service, Q("john.doe"));
    }

    void gpgPassCredentialStore_maxCredentialSize_returnsZero()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        GpgPassCredentialStore collection(fs, gpg, initializePasswordStore(fs, gpg), TestNamespace);
        QCOMPARE(collection.maxCredentialSize(), 0);
    }

    // Own test: without any .gpg-id the store cannot encrypt.
    void gpgPassCredentialStore_noGpgId_throws()
    {
        TestFileSystem fs;
        TestGpg gpg(fs);
        const QString storePath = Q("/home/u/.password-store");
        fs.Directories.insert(storePath);
        GpgPassCredentialStore collection(fs, gpg, storePath);
        try {
            collection.addOrUpdate(Q("https://example.com"), Q("a"), Q("b"));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("Cannot find GPG ID in password store at '/home/u/.password-store'; run `pass init <gpg-id>` to initialize the store."));
        }
    }

    // Own test: the real Gpg wrapper builds the expected command lines.
    void gpg_commandLines()
    {
        TestSessionManager session;
        TestProcessManager processes;
        processes.setResult("secret\nservice=x\n");
        Gpg gpg(Q("/usr/bin/gpg"), session, processes);
        QCOMPARE(gpg.decryptFile(Q("/s/a b.gpg")), Q("secret\nservice=x\n"));
        gpg.encryptFile(Q("/s/c.gpg"), Q("me@example.com"), Q("pw"));
        QCOMPARE(processes.CreatedProcesses[0].argumentList, (QStringList{Q("--batch"), Q("--decrypt"), Q("/s/a b.gpg")}));
        QCOMPARE(processes.CreatedProcesses[1].argumentList,
                 (QStringList{Q("--encrypt"), Q("--batch"), Q("--recipient"), Q("me@example.com"), Q("--output"), Q("/s/c.gpg")}));

        processes.setResult("", 2, "gpg: decryption failed");
        try {
            gpg.decryptFile(Q("/s/x.gpg"));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("Failed to decrypt file '/s/x.gpg' with gpg. exit=2, out=, err=gpg: decryption failed"));
        }
    }
};

// =============================================================================================

class SecretServiceCollectionTests : public QObject
{
    Q_OBJECT
    using Operation = SecretServiceCollection::Operation;

    static bool providerAvailable() { return qEnvironmentVariable("GCM_TEST_SECRETSERVICE") == QLatin1String("1"); }

private Q_SLOTS:
    void credentialStore_secretService_headlessSession_selectsStore()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Constants::CredentialStoreNames::SecretService;
        context.SessionManager.IsDesktopSession = false;
        CredentialStore store(context);
        QCOMPARE(store.name(), Q("freedesktop.org Secret Service"));
    }

    void secretServiceCollection_nativeFailure_preservesErrorAndExplainsCause_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<QString>("domain");
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("expectedHint");
        QTest::newRow("1") << int(Operation::ConnectBus) << Q("g-io-error-quark") << 0 << Q("D-Bus session bus is running and accessible");
        QTest::newRow("2") << int(Operation::OpenService) << Q("g-dbus-error-quark") << 2 << Q("No Secret Service provider is available");
        QTest::newRow("3") << int(Operation::OpenService) << Q("g-dbus-error-quark") << 3 << Q("No Secret Service provider is available");
        QTest::newRow("4") << int(Operation::Unlock) << Q("secret-error") << 2 << Q("secret item or collection is locked");
        QTest::newRow("5") << int(Operation::Load) << Q("secret-error") << 2 << Q("secret item or collection is locked");
        QTest::newRow("6") << int(Operation::Unlock) << Q("g-io-error-quark") << 19 << Q("operation was cancelled");
        QTest::newRow("7") << int(Operation::Search) << Q("g-dbus-error-quark") << 2 << Q("required D-Bus service is unavailable");
    }
    void secretServiceCollection_nativeFailure_preservesErrorAndExplainsCause()
    {
        QFETCH(int, operation);
        QFETCH(QString, domain);
        QFETCH(int, code);
        QFETCH(QString, expectedHint);
        const InteropException ex = SecretServiceCollection::createException(Operation(operation), domain, code, Q("Native failure details"));
        QCOMPARE(ex.errorCode(), code);
        QVERIFY(ex.inner());
        QCOMPARE(ex.inner()->message(), domain + Q(": Native failure details"));
        QVERIFY2(ex.message().contains(expectedHint), qPrintable(ex.message()));
        QVERIFY(ex.message().contains(Constants::HelpUrls::GcmCredentialStores));
    }

    void secretServiceCollection_nativeFailure_doesNotMisclassifyOtherDomains_data()
    {
        QTest::addColumn<QString>("domain");
        QTest::addColumn<int>("code");
        QTest::newRow("io") << Q("g-io-error-quark") << 2;
        QTest::newRow("unrelated") << Q("unrelated-error-domain") << 19;
    }
    void secretServiceCollection_nativeFailure_doesNotMisclassifyOtherDomains()
    {
        QFETCH(QString, domain);
        QFETCH(int, code);
        const InteropException ex = SecretServiceCollection::createException(Operation::Search, domain, code, Q("Native failure"));
        QVERIFY(!ex.message().contains(Q("locked")));
        QVERIFY(!ex.message().contains(Q("cancelled")));
        QVERIFY(!ex.message().contains(Q("service is unavailable")));
        QCOMPARE(ex.errorCode(), code);
        QCOMPARE(ex.inner()->message(), domain + Q(": Native failure"));
    }

    void secretServiceCollection_failureWithoutNativeError_explainsIncompleteOperation_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<QString>("expectedHint");
        QTest::newRow("unlock") << int(Operation::Unlock) << Q("dismissed or cancelled");
        QTest::newRow("store") << int(Operation::Store) << Q("dismissed or cancelled");
        QTest::newRow("load") << int(Operation::Load) << Q("No secret value was returned");
    }
    void secretServiceCollection_failureWithoutNativeError_explainsIncompleteOperation()
    {
        QFETCH(int, operation);
        QFETCH(QString, expectedHint);
        const InteropException ex = SecretServiceCollection::createException(Operation(operation), std::nullopt, -1, std::nullopt);
        QCOMPARE(ex.errorCode(), -1);
        QVERIFY(!ex.inner());
        QVERIFY(ex.message().contains(expectedHint));
    }

    void secretServiceCollection_readWriteDelete()
    {
        if (!providerAvailable()) {
            QSKIP("Requires a configured Secret Service provider (set GCM_TEST_SECRETSERVICE=1)");
        }
        SecretServiceCollection collection(TestNamespace);
        const QString service = Q("https://example.com/") + guid();
        collection.addOrUpdate(service, Q("john.doe"), Q("letmein123"));
        const std::optional<Credential> outCredential = collection.get(service, Q("john.doe"));
        const bool removed = collection.remove(service, Q("john.doe"));
        QVERIFY(outCredential);
        QCOMPARE(outCredential->account, Q("john.doe"));
        QCOMPARE(outCredential->password, Q("letmein123"));
        QVERIFY(removed);
    }

    void secretServiceCollection_get_notFound_returnsNull()
    {
        if (!providerAvailable()) {
            QSKIP("Requires a configured Secret Service provider (set GCM_TEST_SECRETSERVICE=1)");
        }
        SecretServiceCollection collection(TestNamespace);
        QVERIFY(!collection.get(Q("https://example.com/") + guid(), std::nullopt));
    }

    void secretServiceCollection_remove_notFound_returnsFalse()
    {
        if (!providerAvailable()) {
            QSKIP("Requires a configured Secret Service provider (set GCM_TEST_SECRETSERVICE=1)");
        }
        SecretServiceCollection collection(TestNamespace);
        QVERIFY(!collection.remove(Q("https://example.com/") + guid(), std::nullopt));
    }

    void secretServiceCollection_maxCredentialSize_returnsZero()
    {
        SecretServiceCollection collection(TestNamespace);
        QCOMPARE(collection.maxCredentialSize(), 0);
    }
};

// =============================================================================================
// Own tests of the store selection

class CredentialStoreSelectionTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void unset_throwsHelpText()
    {
        TestCommandContext context;
        CredentialStore store(context);
        try {
            store.name();
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(),
                     Q("No credential store has been selected.\n"
                       "\n"
                       "Set the GCM_CREDENTIAL_STORE environment variable or the credential.credentialStore Git configuration setting to one of the following options:\n"
                       "\n"
                       "  secretservice : freedesktop.org Secret Service (requires D-Bus and a Secret Service provider)\n"
                       "  gpg           : GNU `pass` compatible credential storage (requires GPG and `pass`)\n"
                       "  cache         : Git's in-memory credential cache\n"
                       "  plaintext     : store credentials in plain-text files (UNSECURE)\n"
                       "  none          : disable internal credential storage\n"
                       "\n"
                       "See https://aka.ms/gcm/credstores for more information.\n"));
        }
    }

    void unknown_throws()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Q("Bogus");
        CredentialStore store(context);
        try {
            store.name();
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().startsWith(Q("Unknown credential store 'bogus'.\n")));
        }
    }

    void otherPlatformStores_throw_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("message");
        QTest::newRow("wincredman") << Q("wincredman") << Q("Can only use the 'wincredman' credential store on Windows.\nSee https://aka.ms/gcm/credstores for more information.");
        QTest::newRow("dpapi") << Q("dpapi") << Q("Can only use the 'dpapi' credential store on Windows.\nSee https://aka.ms/gcm/credstores for more information.");
        QTest::newRow("keychain") << Q("keychain") << Q("Can only use the 'keychain' credential store on macOS.\nSee https://aka.ms/gcm/credstores for more information.");
    }
    void otherPlatformStores_throw()
    {
        QFETCH(QString, name);
        QFETCH(QString, message);
        TestCommandContext context;
        context.Settings.CredentialBackingStore = name;
        CredentialStore store(context);
        try {
            store.name();
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), message);
        }
    }

    void plaintext_defaultAndRedirectedRoot()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Q("PLAINTEXT");
        CredentialStore store(context);
        QCOMPARE(store.name(), Q("Plaintext"));
        store.addOrUpdate(Q("https://example.com"), Q("john"), Q("pw"));
        QVERIFY(context.FileSystem.Files.contains(context.FileSystem.UserDataDirectoryPath +
                                                  Q("/store/git-test/https/example.com/john.credential")));

        TestCommandContext redirected;
        redirected.Settings.CredentialBackingStore = Q("plaintext");
        redirected.Environment.Variables.set(Q("GCM_PLAINTEXT_STORE_PATH"), Q("/data/creds"));
        CredentialStore store2(redirected);
        store2.addOrUpdate(Q("https://example.com"), Q("john"), Q("pw"));
        QVERIFY(redirected.FileSystem.Files.contains(Q("/data/creds/git-test/https/example.com/john.credential")));
    }

    void none_isNoOp()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Q("none");
        CredentialStore store(context);
        QCOMPARE(store.name(), Q("No-op"));
        store.addOrUpdate(Q("s"), Q("a"), Q("p"));
        QVERIFY(!store.get(Q("s"), Q("a")));
        QVERIFY(!store.remove(Q("s"), Q("a")));
    }

    void gpg_headlessWithoutTty_throws()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Q("gpg");
        context.FileSystem.Files.set(Q("/usr/bin/gpg"), {});
        context.Environment.Variables.set(Q("GCM_GPG_PATH"), Q("/usr/bin/gpg"));
        CredentialStore store(context);
        try {
            store.name();
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QCOMPARE(ex.message(), Q("GPG_TTY is not set; add `export GPG_TTY=$(tty)` to your profile.\nSee https://aka.ms/gcm/credstores for more information."));
        }
        context.Environment.Variables.set(Q("GPG_TTY"), Q("/dev/pts/1"));
        CredentialStore store2(context);
        QCOMPARE(store2.name(), Q("GPG/Pass"));

        context.Environment.Variables.set(Q("GCM_GPG_PATH"), Q("/missing/gpg"));
        CredentialStore store3(context);
        QVERIFY_THROWS_EXCEPTION(Exception, store3.name());
    }

    void cache_name()
    {
        TestCommandContext context;
        context.Settings.CredentialBackingStore = Q("cache");
        CredentialStore store(context);
        QCOMPARE(store.name(), Q("Git Credential Cache"));
    }
};

GCM_REGISTER_TEST(PlaintextCredentialStoreTests)
GCM_REGISTER_TEST(GnuPassCredentialStoreTests)
GCM_REGISTER_TEST(SecretServiceCollectionTests)
GCM_REGISTER_TEST(CredentialStoreSelectionTests)
#include "storestests.moc"
