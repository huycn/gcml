// Port of PlaintextCredentialStore.cs, FileCredential.cs, Interop/Posix/GpgPassCredentialStore.cs, Gpg.cs,
// CredentialCacheStore.cs
#include "core/stores.h"

#include "core/git.h"
#include "core/streams.h"
#include "core/strings.h"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <sys/stat.h>

namespace gcm {

namespace {

QString combine(const QString &a, const QString &b)
{
    if (a.isEmpty()) {
        return b;
    }
    if (b.startsWith(QLatin1Char('/'))) {
        return b;
    }
    return a.endsWith(QLatin1Char('/')) ? a + b : a + QLatin1Char('/') + b;
}

/// Path.GetDirectoryName: nullopt for the root
OptString directoryName(const QString &path)
{
    QString p = path;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/'))) {
        p.chop(1);
    }
    const qsizetype slash = p.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
        return QString();
    }
    if (slash == 0) {
        return p == QLatin1String("/") ? OptString() : OptString(QStringLiteral("/"));
    }
    return p.left(slash);
}

QString fileNameWithoutExtension(const QString &path)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? name : name.left(dot);
}

} // namespace

// =============================================================================================
// PlaintextCredentialStore
// =============================================================================================

PlaintextCredentialStore::PlaintextCredentialStore(IFileSystem &fileSystem, QString storeRoot, OptString ns)
    : m_fileSystem(fileSystem), m_storeRoot(std::move(storeRoot)), m_namespace(std::move(ns))
{
    EnsureArgument::notNullOrWhiteSpace(m_storeRoot, "storeRoot");
}

QStringList PlaintextCredentialStore::getAccounts(const OptString &service)
{
    QStringList accounts;
    for (const QList<FileCredential> credentials = enumerate(service, std::nullopt); const FileCredential &c : credentials) {
        const QString account = c.account.value_or(QString());
        if (!accounts.contains(account)) {
            accounts.append(account);
        }
    }
    return accounts;
}

std::optional<Credential> PlaintextCredentialStore::get(const OptString &service, const OptString &account)
{
    const QList<FileCredential> matches = enumerate(service, account);
    if (matches.isEmpty()) {
        return std::nullopt;
    }
    return Credential{matches.first().account.value_or(QString()), matches.first().password};
}

void PlaintextCredentialStore::addOrUpdate(const QString &service, const QString &account, const QString &secret)
{
    // Ensure the store root exists and permissions are set
    ensureStoreRoot();

    // No need to update an existing credential if nothing has changed
    const QList<FileCredential> existing = enumerate(service, account);
    if (!existing.isEmpty() && existing.first().account == account && existing.first().password == secret) {
        return;
    }

    const QString servicePath = combine(m_storeRoot, createServiceSlug(service));
    if (!m_fileSystem.directoryExists(servicePath)) {
        m_fileSystem.createDirectory(servicePath);
    }

    const QString fullPath = combine(servicePath, account + credentialFileExtension());
    serializeCredential(FileCredential{fullPath, service, account, secret});
}

bool PlaintextCredentialStore::remove(const OptString &service, const OptString &account)
{
    // Only delete the first match
    for (const QList<FileCredential> credentials = enumerate(service, account); const FileCredential &credential : credentials) {
        m_fileSystem.deleteFile(credential.fullPath);
        return true;
    }
    return false;
}

bool PlaintextCredentialStore::tryParseCredentialText(const QString &path, const QString &text, FileCredential &credential)
{
    const qsizetype line1Idx = text.indexOf(QLatin1Char('\n'));
    if (line1Idx > 0) {
        // The password is the first line, then metadata/attributes
        const QString password = text.left(line1Idx);
        StringReader attrReader(text.mid(line1Idx + 1));
        const StringDictionary attrs = readDictionary(attrReader, Qt::CaseInsensitive);
        // The service is required, the account optional
        if (const OptString service = attrs.tryGet(QStringLiteral("service"))) {
            credential = FileCredential{path, *service, attrs.tryGet(QStringLiteral("account")), password};
            return true;
        }
    }
    return false;
}

bool PlaintextCredentialStore::tryDeserializeCredential(const QString &path, FileCredential &credential)
{
    return tryParseCredentialText(path, m_fileSystem.readAllText(path), credential);
}

void PlaintextCredentialStore::serializeCredential(const FileCredential &credential)
{
    // Ensure the parent directory exists
    const OptString parentDir = directoryName(credential.fullPath);
    if (parentDir && !m_fileSystem.directoryExists(*parentDir)) {
        m_fileSystem.createDirectory(*parentDir);
    }

    const QString contents = QStringLiteral("%1\nservice=%2\naccount=%3\n")
                                 .arg(credential.password, credential.service, credential.account.value_or(QString()));
    m_fileSystem.writeAllBytes(credential.fullPath, contents.toUtf8());
}

QList<FileCredential> PlaintextCredentialStore::enumerate(const OptString &service, const OptString &account)
{
    QList<FileCredential> result;
    const QString searchPath = combine(m_storeRoot, createServiceSlug(service));
    const bool anyAccount = isNullOrWhiteSpace(account);

    if (!m_fileSystem.directoryExists(searchPath)) {
        return result;
    }

    const QStringList allFiles = m_fileSystem.enumerateFiles(searchPath, QLatin1Char('*') + credentialFileExtension());
    for (const QString &fullPath : allFiles) {
        const QString accountFile = fileNameWithoutExtension(fullPath);
        if (!anyAccount && !equalsIgnoreCase(*account, accountFile)) {
            continue;
        }
        // The credential metadata must match the search too
        FileCredential credential;
        if (tryDeserializeCredential(fullPath, credential) && service && equalsIgnoreCase(*service, credential.service) &&
            (anyAccount || equalsIgnoreCase(*account, credential.account.value_or(QString())))) {
            result.append(credential);
            }
    }
    return result;
}

void PlaintextCredentialStore::ensureStoreRoot()
{
    // Don't touch the permissions of an existing directory
    if (m_fileSystem.directoryExists(m_storeRoot)) {
        return;
    }
    m_fileSystem.createDirectory(m_storeRoot);
    // Only the owner can read/write/execute; best effort
    ::chmod(QFile::encodeName(m_storeRoot).constData(), S_IRUSR | S_IWUSR | S_IXUSR);
}

QString PlaintextCredentialStore::createServiceSlug(const OptString &service) const
{
    QString slug;
    if (!isNullOrWhiteSpace(m_namespace)) {
        slug += *m_namespace + QLatin1Char('/');
    }

    if (const std::optional<QUrl> serviceUri = tryCreateUri(service)) {
        slug += serviceUri->scheme() + QLatin1Char('/');
        slug += serviceUri->host();
        if (!isDefaultPort(*serviceUri)) {
            slug += QLatin1Char(':') + QString::number(serviceUri->port());
        }
        const QString path = serviceUri->path(QUrl::FullyEncoded);
        slug += path.isEmpty() ? QStringLiteral("/") : path;
    } else {
        slug += service.value_or(QString());
    }
    return slug;
}

// =============================================================================================
// GpgPassCredentialStore
// =============================================================================================

GpgPassCredentialStore::GpgPassCredentialStore(IFileSystem &fileSystem, IGpg &gpg, QString storeRoot, OptString ns)
    : PlaintextCredentialStore(fileSystem, std::move(storeRoot), std::move(ns)), m_gpg(gpg)
{
}

QString GpgPassCredentialStore::getGpgId(const QString &credentialFullPath)
{
    // Like GNU pass: the nearest .gpg-id walking up from the credential's directory to the store root.
    OptString dir = directoryName(credentialFullPath);
    while (dir) {
        const QString gpgIdPath = combine(*dir, QStringLiteral(".gpg-id"));
        if (m_fileSystem.fileExists(gpgIdPath)) {
            return m_fileSystem.readAllLines(gpgIdPath).value(0);
        }
        // Stop after checking the store root
        if (m_fileSystem.isSamePath(*dir, m_storeRoot)) {
            break;
        }
        dir = directoryName(*dir);
    }
    throw Exception(QStringLiteral("Cannot find GPG ID in password store at '%1'; run `pass init <gpg-id>` to initialize the store.")
                        .arg(m_storeRoot));
}

bool GpgPassCredentialStore::tryDeserializeCredential(const QString &path, FileCredential &credential)
{
    return tryParseCredentialText(path, m_gpg.decryptFile(path), credential);
}

void GpgPassCredentialStore::serializeCredential(const FileCredential &credential)
{
    const QString gpgId = getGpgId(credential.fullPath);
    const QString fileContents = QStringLiteral("%1\nservice=%2\naccount=%3\n")
                                     .arg(credential.password, credential.service, credential.account.value_or(QString()));

    // Ensure the parent directory exists
    const OptString parentDir = directoryName(credential.fullPath);
    if (parentDir && !m_fileSystem.directoryExists(*parentDir)) {
        m_fileSystem.createDirectory(*parentDir);
    }

    // Delete any existing file
    if (m_fileSystem.fileExists(credential.fullPath)) {
        m_fileSystem.deleteFile(credential.fullPath);
    }

    // Encrypt!
    m_gpg.encryptFile(credential.fullPath, gpgId, fileContents);
}

// =============================================================================================
// Gpg
// =============================================================================================

Gpg::Gpg(QString gpgPath, ISessionManager &sessionManager, IProcessManager &processManager)
    : m_gpgPath(std::move(gpgPath)), m_sessionManager(sessionManager), m_processManager(processManager)
{
    EnsureArgument::notNullOrWhiteSpace(m_gpgPath, "gpgPath");
}

void Gpg::prepareEnvironment(ProcessStartInfo &psi) const
{
    // Headless over SSH without an explicit GPG_TTY: use SSH_TTY as GPG_TTY.
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_sessionManager.isDesktopSession() && !env.contains(QStringLiteral("GPG_TTY")) &&
        env.contains(QStringLiteral("SSH_TTY"))) {
        psi.environment.set(QStringLiteral("GPG_TTY"), env.value(QStringLiteral("SSH_TTY")));
        }
}

QString Gpg::decryptFile(const QString &path)
{
    ProcessStartInfo psi;
    psi.fileName = m_gpgPath;
    psi.argumentList = {QStringLiteral("--batch"), QStringLiteral("--decrypt"), path};
    psi.redirectStandardInput = false;
    psi.redirectStandardOutput = true;
    // Suppress verbose decryption messages
    psi.redirectStandardError = true;
    prepareEnvironment(psi);

    auto gpg = m_processManager.createProcess(psi, Trace2ProcessClass::Other);
    if (!gpg->start()) {
        throw Exception(QStringLiteral("Failed to start gpg."));
    }
    gpg->waitForExit();
    const QString stdoutText = QString::fromUtf8(gpg->readAllStandardOutput());
    if (gpg->exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(gpg->readAllStandardError());
        throw Exception(QStringLiteral("Failed to decrypt file '%1' with gpg. exit=%2, out=%3, err=%4")
                            .arg(path, QString::number(gpg->exitCode()), stdoutText, stderrText));
    }
    return stdoutText;
}

void Gpg::encryptFile(const QString &path, const QString &gpgId, const QString &contents)
{
    ProcessStartInfo psi;
    psi.fileName = m_gpgPath;
    psi.argumentList = {QStringLiteral("--encrypt"), QStringLiteral("--batch"), QStringLiteral("--recipient"), gpgId,
                        QStringLiteral("--output"), path};
    psi.redirectStandardInput = true;
    psi.redirectStandardOutput = true;
    psi.redirectStandardError = true;
    prepareEnvironment(psi);

    auto gpg = m_processManager.createProcess(psi, Trace2ProcessClass::Other);
    if (!gpg->start()) {
        throw Exception(QStringLiteral("Failed to start gpg."));
    }
    gpg->writeStandardInput(contents.toUtf8());
    gpg->closeStandardInput();
    gpg->waitForExit();
    if (gpg->exitCode() != 0) {
        const QString stdoutText = QString::fromUtf8(gpg->readAllStandardOutput());
        const QString stderrText = QString::fromUtf8(gpg->readAllStandardError());
        throw Exception(QStringLiteral("Failed to encrypt file '%1' with gpg. exit=%2, out=%3, err=%4")
                            .arg(path, QString::number(gpg->exitCode()), stdoutText, stderrText));
    }
}

// =============================================================================================
// CredentialCacheStore
// =============================================================================================

namespace {
StringDictionary makeGitCredentialsEntry(const OptString &service, const OptString &account)
{
    StringDictionary result;
    result.set(QStringLiteral("url"), service.value_or(QString()));
    if (!isNullOrEmpty(account)) {
        result.set(QStringLiteral("username"), *account);
    }
    return result;
}
} // namespace

CredentialCacheStore::CredentialCacheStore(IGit &git, const OptString &options)
    : m_git(git), m_options(options.value_or(QString()))
{
}

QStringList CredentialCacheStore::getAccounts(const OptString &service)
{
    // Listing accounts is not supported: return the user name of the first credential for the service, if any.
    const StringDictionary input = makeGitCredentialsEntry(service, std::nullopt);
    const StringDictionary result = m_git.invokeHelper(QStringLiteral("credential-cache get %1").arg(m_options), &input);
    if (const OptString user = result.tryGet(QStringLiteral("username"))) {
        return {*user};
    }
    return {};
}

std::optional<Credential> CredentialCacheStore::get(const OptString &service, const OptString &account)
{
    const StringDictionary input = makeGitCredentialsEntry(service, account);
    const StringDictionary result = m_git.invokeHelper(QStringLiteral("credential-cache get %1").arg(m_options), &input);
    const OptString user = result.tryGet(QStringLiteral("username"));
    const OptString password = result.tryGet(QStringLiteral("password"));
    if (user && password) {
        return Credential{*user, *password};
    }
    return std::nullopt;
}

void CredentialCacheStore::addOrUpdate(const QString &service, const QString &account, const QString &secret)
{
    StringDictionary input = makeGitCredentialsEntry(service, account);
    input.set(QStringLiteral("password"), secret);
    // For a store or erase operation, the helper's output is ignored (gitcredentials(7)).
    m_git.invokeHelper(QStringLiteral("credential-cache store %1").arg(m_options), &input);
}

bool CredentialCacheStore::remove(const OptString &service, const OptString &account)
{
    const StringDictionary input = makeGitCredentialsEntry(service, account);
    m_git.invokeHelper(QStringLiteral("credential-cache erase %1").arg(m_options), &input);
    // The cache doesn't tell whether anything was erased; be optimistic
    return true;
}

} // namespace gcm
