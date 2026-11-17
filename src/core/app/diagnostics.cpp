// Port of Diagnostics/{Diagnostic,EnvironmentDiagnostic,FileSystemDiagnostic,NetworkingDiagnostic,GitDiagnostic,
// CredentialStoreDiagnostic}.cs
#include "core/diagnostics.h"

#include "core/strings.h"

#include <QDir>
#include <QFile>
#include <QNetworkInterface>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

namespace gcm {

// =============================================================================================
// Diagnostic base
// =============================================================================================

QString diagnosticOutcomeName(DiagnosticOutcome outcome)
{
    switch (outcome) {
        case DiagnosticOutcome::Success: return QStringLiteral("Success");
        case DiagnosticOutcome::Warning: return QStringLiteral("Warning");
        case DiagnosticOutcome::Error: return QStringLiteral("Error");
        case DiagnosticOutcome::Skipped: return QStringLiteral("Skipped");
    }
    return QString();
}

DiagnosticResult DiagnosticResult::skipped(const QString &reason)
{
    DiagnosticResult result;
    result.skipReason = reason;
    result.outcome = isNullOrWhiteSpace(reason) ? DiagnosticOutcome::Success : DiagnosticOutcome::Skipped;
    return result;
}

DiagnosticResult DiagnosticResult::create(QList<DiagnosticReport> reports, QStringList additionalFiles)
{
    DiagnosticResult result;
    result.reports = std::move(reports);
    result.additionalFiles = std::move(additionalFiles);
    for (const DiagnosticReport &report : std::as_const(result.reports)) {
        if (report.kind == DiagnosticReportKind::Error) {
            ++result.errorCount;
        } else if (report.kind == DiagnosticReportKind::Warning) {
            ++result.warningCount;
        }
    }
    result.outcome = result.errorCount > 0     ? DiagnosticOutcome::Error
                     : result.warningCount > 0 ? DiagnosticOutcome::Warning
                                               : DiagnosticOutcome::Success;
    return result;
}

QStringList DiagnosticResult::exceptions() const
{
    QStringList result;
    for (const DiagnosticReport &report : reports) {
        if (report.exceptionText) {
            result.append(*report.exceptionText);
        }
    }
    return result;
}

namespace {

/// The .NET type name (with namespace) of an exception type of this port, as Exception.ToString() prints it.
QString dotnetTypeName(const Exception &ex)
{
    static const QHash<QString, QString> namespaces = {
        { QStringLiteral("Exception"), QStringLiteral("System") },
        { QStringLiteral("ArgumentException"), QStringLiteral("System") },
        { QStringLiteral("ArgumentNullException"), QStringLiteral("System") },
        { QStringLiteral("ArgumentOutOfRangeException"), QStringLiteral("System") },
        { QStringLiteral("InvalidOperationException"), QStringLiteral("System") },
        { QStringLiteral("NotSupportedException"), QStringLiteral("System") },
        { QStringLiteral("PlatformNotSupportedException"), QStringLiteral("System") },
        { QStringLiteral("TimeoutException"), QStringLiteral("System") },
        { QStringLiteral("OperationCanceledException"), QStringLiteral("System") },
        { QStringLiteral("IOException"), QStringLiteral("System.IO") },
        { QStringLiteral("KeyNotFoundException"), QStringLiteral("System.Collections.Generic") },
        { QStringLiteral("HttpRequestException"), QStringLiteral("System.Net.Http") },
        { QStringLiteral("CryptographicException"), QStringLiteral("System.Security.Cryptography") },
        { QStringLiteral("InterruptedException"), QStringLiteral("GitCredentialManager") },
        { QStringLiteral("GitException"), QStringLiteral("GitCredentialManager") },
        { QStringLiteral("InteropException"), QStringLiteral("GitCredentialManager.Interop") },
        { QStringLiteral("OAuth2Exception"), QStringLiteral("GitCredentialManager.Authentication.OAuth") },
        { QStringLiteral("MsalException"), QStringLiteral("Microsoft.Identity.Client") },
        { QStringLiteral("MsalClientException"), QStringLiteral("Microsoft.Identity.Client") },
        { QStringLiteral("MsalServiceException"), QStringLiteral("Microsoft.Identity.Client") },
        { QStringLiteral("MsalUiRequiredException"), QStringLiteral("Microsoft.Identity.Client") },
        { QStringLiteral("MsalThrottledServiceException"), QStringLiteral("Microsoft.Identity.Client") },
        { QStringLiteral("IntuneAppProtectionPolicyRequiredException"), QStringLiteral("Microsoft.Identity.Client") },
    };
    const QString name = QLatin1String(ex.typeName());
    const QString ns = namespaces.value(name);
    return ns.isEmpty() ? name : ns + QLatin1Char('.') + name;
}

} // namespace

QString exceptionToString(const std::exception &ex)
{
    const auto *gcmEx = dynamic_cast<const Exception *>(&ex);
    if (!gcmEx) {
        return QString::fromUtf8(ex.what());
    }
    // Exception.ToString() without the stack trace (C++ has none)
    QString text = dotnetTypeName(*gcmEx);
    if (!gcmEx->message().isEmpty()) {
        text += QStringLiteral(": ") + gcmEx->message();
    }
    if (const auto &inner = gcmEx->inner()) {
        text += QStringLiteral(" ---> ") + exceptionToString(*inner) + QStringLiteral("\n   --- End of inner exception stack trace ---");
    }
    return text;
}

namespace {

class DiagnosticReporter : public IDiagnosticReporter
{
public:
    explicit DiagnosticReporter(const std::function<void(const QString &)> &progress) : m_progress(progress) {}

    void reportProgress(const QString &message) override
    {
        m_reports.append({DiagnosticReportKind::Progress, message, std::nullopt});
        if (m_progress) {
            m_progress(message);
        }
    }

    void reportInfo(const QString &message) override {
        m_reports.append({DiagnosticReportKind::Information, message, std::nullopt});
    }

    void reportWarning(const QString &message) override {
        m_reports.append({DiagnosticReportKind::Warning, message, std::nullopt});
    }

    void reportError(const QString &message, const std::exception *exception = nullptr) override
    {
        OptString text;
        if (exception) {
            text = exceptionToString(*exception);
        }
        m_reports.append({DiagnosticReportKind::Error, message, text});
    }

    void addFile(const QString &path) override {
        m_files.append(path);
    }

    DiagnosticResult createResult() const {
        return DiagnosticResult::create(m_reports, m_files);
    }

private:
    std::function<void(const QString &)> m_progress;
    QList<DiagnosticReport> m_reports;
    QStringList m_files;
};

} // namespace

DiagnosticResult Diagnostic::run(const std::function<void(const QString &)> &progress)
{
    DiagnosticReporter reporter(progress);
    try {
        QString skipReason;
        if (!canRun(skipReason)) {
            return DiagnosticResult::skipped(skipReason);
        }
        runInternal(reporter);
    } catch (const Exception &ex) {
        reporter.reportError(QStringLiteral("Unhandled exception: %1").arg(ex.message()), &ex);
    } catch (const std::exception &ex) {
        reporter.reportError(QStringLiteral("Unhandled exception: %1").arg(QString::fromUtf8(ex.what())), &ex);
    }
    return reporter.createResult();
}

// =============================================================================================
// Environment
// =============================================================================================

void EnvironmentDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    const PlatformInformation platformInfo = PlatformUtils::getPlatformInformation();
    reporter.reportInfo(QStringLiteral("OSType: %1").arg(platformInfo.operatingSystemType));
    reporter.reportInfo(QStringLiteral("OSVersion: %1").arg(platformInfo.operatingSystemVersion));

    reporter.reportProgress(QStringLiteral("Reading environment variables"));
    // The process environment itself, not the context's view of it
    const QProcessEnvironment envars = QProcessEnvironment::systemEnvironment();
    reporter.reportInfo(QStringLiteral("Variables:"));
    const auto keys = envars.keys();
    for (const QString &key : keys) {
        reporter.reportInfo(QStringLiteral("%1=%2").arg(key, envars.value(key)));
    }
}

// =============================================================================================
// File system
// =============================================================================================

void FileSystemDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    // Path.GetTempPath() ends with a separator
    const QString tempDir = QDir::tempPath() + QLatin1Char('/');
    reporter.reportInfo(QStringLiteral("Temporary directory is '%1'").arg(tempDir));

    reporter.reportProgress(QStringLiteral("Checking basic file I/O"));
    const QString testContent = QStringLiteral("Hello, GCM!");
    const QString path = tempDir + QUuid::createUuid().toString(QUuid::Id128).mid(8);

    reporter.reportProgress(QStringLiteral("Writing to temporary file '%1'").arg(path));
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(testContent.toUtf8()) < 0) {
            throw IOException(QStringLiteral("Could not write '%1': %2").arg(path, file.errorString()));
        }
    }

    reporter.reportProgress(QStringLiteral("Reading from temporary file '%1'").arg(path));
    QString actualContent;
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            throw IOException(QStringLiteral("Could not read '%1': %2").arg(path, file.errorString()));
        }
        actualContent = QString::fromUtf8(file.readAll());
    }

    if (testContent != actualContent) {
        reporter.reportError(QStringLiteral("File data did not match!"));
        reporter.reportError(QStringLiteral("Expected: %1").arg(testContent));
        reporter.reportError(QStringLiteral("Actual: %1").arg(actualContent));
        return;
    }

    reporter.reportProgress(QStringLiteral("Deleting temporary file '%1'").arg(path));
    QFile::remove(path);

    reporter.reportProgress(QStringLiteral("Testing IFileSystem instance"));
    reporter.reportInfo(QStringLiteral("UserHomePath: %1").arg(m_context.fileSystem().userHomePath()));
    reporter.reportInfo(QStringLiteral("UserDataDirectoryPath: %1").arg(m_context.fileSystem().userDataDirectoryPath()));
    reporter.reportInfo(QStringLiteral("GetCurrentDirectory(): %1").arg(m_context.fileSystem().getCurrentDirectory()));
}

// =============================================================================================
// Networking
// =============================================================================================

namespace {

// NOLINTBEGIN(non-pod-global-static)
const QString TestHttpUri = QStringLiteral("http://example.com");
const QString TestHttpUriFallback = QStringLiteral("http://httpforever.com");
const QString TestHttpsUri = QStringLiteral("https://example.com");
// NOLINTEND(non-pod-global-static)

bool isNetworkAvailable()
{
    // NetworkInterface.GetIsNetworkAvailable: any interface up, other than loopback
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    return std::any_of(interfaces.begin(), interfaces.end(), [](const QNetworkInterface &i) {
        return i.flags().testFlag(QNetworkInterface::IsUp) && i.flags().testFlag(QNetworkInterface::IsRunning) && !i.flags().testFlag(QNetworkInterface::IsLoopBack);
    });
}
} // namespace

void NetworkingDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    reporter.reportProgress(QStringLiteral("Checking networking and HTTP stack"));
    reporter.reportProgress(QStringLiteral("Creating HTTP client"));
    const HttpClient httpClient = m_context.httpClientFactory().createClient();

    reporter.reportInfo(QStringLiteral("IsNetworkAvailable: %1").arg(isNetworkAvailable() ? QStringLiteral("True") : QStringLiteral("False")));

    sendHttpRequest(reporter, httpClient);

    reporter.reportProgress(QStringLiteral("Sending HEAD request to %1").arg(TestHttpsUri));
    httpClient.head(QUrl(TestHttpsUri));

    reporter.reportProgress(QStringLiteral("Acquiring free TCP port"));
    // Listen on the loopback addresses "localhost" can resolve to
    QTcpServer listener4, listener6;
    if (!listener4.listen(QHostAddress::LocalHost, 0) || listener4.serverPort() == 0) {
        reporter.reportError(QStringLiteral("Failed to acquire local TCP port - cannot test local HTTP loopback connections!"));
        return;
    }
    const quint16 tcpPort = listener4.serverPort();
    (void)listener6.listen(QHostAddress::LocalHostIPv6, tcpPort);
    reporter.reportInfo(QStringLiteral("Got port %1").arg(tcpPort));

    reporter.reportProgress(QStringLiteral("Testing local HTTP loopback connections..."));
    const QString responseContent = QStringLiteral("Hello, GCM!");
    const QString localAddress = QStringLiteral("http://localhost:%1/").arg(tcpPort);
    reporter.reportProgress(QStringLiteral("Creating new HTTP listener for %1").arg(localAddress));

    bool responded = false;
    auto serve = [&](QTcpServer &server) {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &responded, &reporter, responseContent] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                // clazy:exclude-next-line=lambda-in-connect
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &responded, &reporter, responseContent] {
                    if (responded || !socket->readAll().contains("\r\n\r\n")) {
                        return;
                    }
                    responded = true;
                    reporter.reportProgress(QStringLiteral("Writing response"));
                    const QByteArray body = responseContent.toUtf8();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    };
    serve(listener4);
    serve(listener6);

    reporter.reportProgress(QStringLiteral("Waiting for loopback connection"));
    const HttpResponse localResponse = httpClient.get(QUrl(localAddress));

    reporter.reportProgress(QStringLiteral("Waiting for response data"));
    const QString actualResponseContent = localResponse.contentString();
    if (responseContent != actualResponseContent) {
        reporter.reportError(QStringLiteral("Loopback connection data did not match!"));
        reporter.reportError(QStringLiteral("Expected: %1").arg(responseContent));
        reporter.reportError(QStringLiteral("Actual: %1").arg(actualResponseContent));
        return;
    }
    reporter.reportInfo(QStringLiteral("Loopback connection data OK"));
}

void NetworkingDiagnostic::sendHttpRequest(IDiagnosticReporter &reporter, const HttpClient &httpClient)
{
    for (const QString &uri : {TestHttpUri, TestHttpUriFallback}) {
        try {
            reporter.reportProgress(QStringLiteral("Sending HEAD request to %1").arg(uri));
            httpClient.head(QUrl(uri));
            break;
        } catch (const HttpRequestException &) {
            reporter.reportWarning(QStringLiteral("HEAD request failed"));
        }
    }
}

// =============================================================================================
// Git
// =============================================================================================

void GitDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    IGit &git = m_context.git();
    reporter.reportProgress(QStringLiteral("Getting Git version"));
    reporter.reportInfo(QStringLiteral("Git version is '%1'").arg(git.version().originalString()));

    reporter.reportProgress(QStringLiteral("Locating current repository"));
    if (!git.isInsideRepository()) {
        reporter.reportInfo(QStringLiteral("Not inside a Git repository."));
    } else {
        reporter.reportInfo(QStringLiteral("Git repository at '%1'").arg(git.getCurrentRepository().value_or(QString())));
    }

    reporter.reportProgress(QStringLiteral("Listing all Git configuration"));
    std::unique_ptr<ChildProcess> configProc = git.createProcess(QStringLiteral("config --list --show-origin"));
    configProc->start();
    QString gitConfig = QString::fromUtf8(configProc->readAllStandardOutput());
    configProc->waitForExit();
    while (!gitConfig.isEmpty() && gitConfig.back().isSpace()) {
        gitConfig.chop(1);
    }
    reporter.reportInfo(QStringLiteral("Git configuration:"));
    reporter.reportInfo(gitConfig);
}

// =============================================================================================
// Credential storage
// =============================================================================================

void CredentialStoreDiagnostic::runInternal(IDiagnosticReporter &reporter)
{
    ICredentialStore &store = m_context.credentialStore();
    reporter.reportInfo(QStringLiteral("Credential store is: %1").arg(store.name()));

    // A service that is guaranteed to be unique
    const QString service = QStringLiteral("https://example.com/%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    const QString account = QStringLiteral("john.doe");
    const QString password = QStringLiteral("letmein123");

    const auto cleanup = qScopeGuard([&reporter, &store, &service, &account] {
        reporter.reportProgress(QStringLiteral("Deleting test credential"));
        try {
            store.remove(service, account);
        } catch (...) {
        }
    });

    reporter.reportProgress(QStringLiteral("Writing test credential..."));
    store.addOrUpdate(service, account, password);

    reporter.reportProgress(QStringLiteral("Reading test credential..."));
    const std::optional<Credential> outCredential = store.get(service, account);
    if (!outCredential) {
        reporter.reportError(QStringLiteral("Test credential object is null!"));
        return;
    }
    if (account != outCredential->account) {
        reporter.reportError(QStringLiteral("Test credential account did not match!"));
        reporter.reportError(QStringLiteral("Expected: %1").arg(account));
        reporter.reportError(QStringLiteral("Actual: %1").arg(outCredential->account));
        return;
    }
    if (password != outCredential->password) {
        reporter.reportError(QStringLiteral("Test credential password did not match!"));
        reporter.reportError(QStringLiteral("Expected: %1").arg(password));
        reporter.reportError(QStringLiteral("Actual: %1").arg(outCredential->password));
        return;
    }
}

} // namespace gcm
