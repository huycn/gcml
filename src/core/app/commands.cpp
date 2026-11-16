// Port of Commands/*.cs
#include "core/commands.h"

#include "core/constants.h"
#include "core/diagnostics.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <unistd.h>

namespace gcm {

// =============================================================================================
// GitCommandBase
// =============================================================================================

GitCommandBase::GitCommandBase(ICommandContext &context, const QString &name, const QString &description, IHostProviderRegistry &hostProviderRegistry)
    : Command(name, description), m_context(context), m_hostProviderRegistry(hostProviderRegistry)
{
    setHandler([this](const ParseResult &) {
        execute();
        return 0;
    });
}

void GitCommandBase::execute()
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_cmd"), QStringLiteral("run"));
    Trace2::writeData(QStringLiteral("git_cmd"), QStringLiteral("name"), name());
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Start '%1' command...").arg(name()));

    // Parse standard input arguments; git-credential treats the keys as case-sensitive, so do we.
    MultiDictionary inputDict;
    {
        Trace2::Scope parse = Trace2::startRegion(QStringLiteral("git_cmd"), QStringLiteral("parse_input"));
        inputDict = readMultiDictionary(m_context.streams().in(), Qt::CaseSensitive);
    }
    const GitRequest request(inputDict);

    // Validate minimum arguments are present
    ensureMinimumRequest(request);

    // Scope settings to the remote URI from now on
    m_context.settings().setRemoteUri(request.getRemoteUri());

    // Determine the host provider
    trace.writeLine(QStringLiteral("Detecting host provider for request:"));
    trace.writeDictionarySecrets(inputDict, {QStringLiteral("password"), QStringLiteral("credential")}, Qt::CaseInsensitive);

    std::shared_ptr<IHostProvider> provider;
    {
        Trace2::Scope resolve = Trace2::startRegion(QStringLiteral("git_cmd"), QStringLiteral("resolve_provider"));
        provider = m_hostProviderRegistry.getProvider(request);
        if (!provider) {
            throw Exception(QStringLiteral("No host provider available to service this request."));
        }
        Trace2::writeData(QStringLiteral("git_cmd"), QStringLiteral("provider/id"), provider->id());
        Trace2::writeData(QStringLiteral("git_cmd"), QStringLiteral("provider/name"), provider->name());
    }
    trace.writeLine(QStringLiteral("Host provider '%1' was selected.").arg(provider->name()));

    executeInternal(request, *provider);

    trace.writeLine(QStringLiteral("End '%1' command...").arg(name()));
}

void GitCommandBase::ensureMinimumRequest(const GitRequest &request)
{
    if (!request.protocol()) {
        throw InvalidOperationException(QStringLiteral("Missing 'protocol' request argument"));
    }
    if (isNullOrWhiteSpace(request.protocol())) {
        throw InvalidOperationException(QStringLiteral("Invalid 'protocol' request argument (cannot be empty)"));
    }
    if (!request.host()) {
        throw InvalidOperationException(QStringLiteral("Missing 'host' request argument"));
    }
    if (isNullOrWhiteSpace(request.host())) {
        throw InvalidOperationException(QStringLiteral("Invalid 'host' request argument (cannot be empty)"));
    }
}

// =============================================================================================
// get / store / erase
// =============================================================================================

GetCommand::GetCommand(ICommandContext &context, IHostProviderRegistry &registry)
    : GitCommandBase(context, QStringLiteral("get"), QStringLiteral("[Git] Return a stored credential"), registry)
{
    setHidden(true);
}

void GetCommand::executeInternal(const GitRequest &request, IHostProvider &provider)
{
    std::optional<GitResponse> responseOpt;
    try {
        Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_cmd_get"), QStringLiteral("provider_get"));
        responseOpt = provider.getCredential(request);
    } catch (const OperationCanceledException &) {
        responseOpt = GitResponse::cancel();
    } catch (const InterruptedException &) {
        responseOpt = GitResponse::cancel();
    }
    const GitResponse &response = *responseOpt;

    ITrace &trace = m_context.trace();
    TextWriter &stdoutWriter = m_context.streams().out();

    if (response.isCancelled()) {
        // Tell Git to stop the credential pipeline (no fallback prompt) so a user who cancelled is not re-prompted.
        trace.writeLine(QStringLiteral("Provider cancelled the credential request; emitting quit=1."));
        m_context.console().writeInfo(QStringLiteral("user cancelled the credential request."));
        stdoutWriter.writeLine(QStringLiteral("quit=1"));
        stdoutWriter.writeLine();
        return;
    }
    if (response.isYielded()) {
        // An empty response lets Git proceed to the next helper or its own prompt.
        trace.writeLine(QStringLiteral("Provider yielded; emitting empty response."));
        stdoutWriter.writeLine();
        return;
    }

    const Credential &credential = *response.credential();

    // Capability-gated fields may only be emitted for capabilities both Git and GCM support.
    const GitCapabilities negotiated = request.capabilities() & Constants::SupportedCapabilities;
    const bool authTypeCapNegotiated = hasFlag(negotiated, GitCapabilities::AuthType);
    const bool stateCapNegotiated = hasFlag(negotiated, GitCapabilities::State);

    trace.writeLine(QStringLiteral("Git capability: %1").arg(GitCapabilitiesUtils::toString(request.capabilities())));
    trace.writeLine(QStringLiteral("GCM capabilities: %1").arg(GitCapabilitiesUtils::toString(Constants::SupportedCapabilities)));
    trace.writeLine(QStringLiteral("Negotiated capabilities: %1").arg(GitCapabilitiesUtils::toString(negotiated)));
    trace.writeLine(QStringLiteral("Writing credentials to output:"));

    auto emit = [&](const QString &line) {
        stdoutWriter.writeLine(line);
        trace.writeLine(QLatin1Char('\t') + line);
    };
    namespace CP = Constants::CredentialProtocol;

    // Capabilities
    const QStringList protocolNames = GitCapabilitiesUtils::toProtocolNames(negotiated);
    for (const QString &name : protocolNames) {
        emit(QStringLiteral("capability[]=%1").arg(name));
    }

    // Common arguments
    if (const OptString v = request.protocol()) {
        emit(QStringLiteral("protocol=%1").arg(*v));
    }
    if (const OptString v = request.host()) {
        emit(QStringLiteral("host=%1").arg(*v));
    }
    if (const OptString v = request.path()) {
        emit(QStringLiteral("path=%1").arg(*v));
    }

    // Credential
    if (authTypeCapNegotiated && response.authType()) {
        emit(QStringLiteral("%1=%2").arg(CP::AuthTypeKey, *response.authType()));
        stdoutWriter.writeLine(QStringLiteral("%1=%2").arg(CP::CredentialKey, credential.password));
        trace.writeLineSecrets(QLatin1Char('\t') + CP::CredentialKey + QStringLiteral("={0}"), {credential.password});
    } else {
        emit(QStringLiteral("%1=%2").arg(CP::UserNameKey, credential.account));
        stdoutWriter.writeLine(QStringLiteral("%1=%2").arg(CP::PasswordKey, credential.password));
        trace.writeLineSecrets(QLatin1Char('\t') + CP::PasswordKey + QStringLiteral("={0}"), {credential.password});
    }
    if (authTypeCapNegotiated && response.isCredentialEphemeral()) {
        emit(QStringLiteral("%1=1").arg(CP::EphemeralKey));
    }

    // Custom additional properties
    for (const auto &[key, value] : response.additionalProperties) {
        emit(QStringLiteral("%1=%2").arg(key, value));
    }

    if (response.isContinue()) {
        if (stateCapNegotiated) {
            emit(QStringLiteral("%1=1").arg(CP::ContinueKey));
        } else {
            // Dropping continue=1 changes the auth semantics: Git treats this credential as final.
            trace.writeLine(QStringLiteral(
                "WARNING: Provider set continue=1 but the 'state' capability was not negotiated with Git. Dropping "
                "continue=1; multistage authentication will not work and the credential will likely fail on the next 401."));
        }
    }

    if (!response.state().isEmpty()) {
        if (stateCapNegotiated) {
            for (const auto &[key, value] : response.state()) {
                emit(QStringLiteral("%1[]=%2%3=%4").arg(CP::StateKey, CP::GcmStatePrefix, key, value));
            }
        } else {
            trace.writeLine(QStringLiteral("Provider set %1 state entries but the 'state' capability was not negotiated with Git; dropping.").arg(response.state().size()));
        }
    }

    // Terminating blank line per the credential protocol.
    stdoutWriter.writeLine();
}

StoreCommand::StoreCommand(ICommandContext &context, IHostProviderRegistry &registry)
    : GitCommandBase(context, QStringLiteral("store"), QStringLiteral("[Git] Store a credential"), registry)
{
    setHidden(true);
}

void StoreCommand::ensureMinimumRequest(const GitRequest &request)
{
    GitCommandBase::ensureMinimumRequest(request);

    // With "authtype" Git uses the "credential" field for the secret
    if (request.authType()) {
        if (!request.credential()) {
            throw InvalidOperationException(QStringLiteral("Missing 'credential' request argument"));
        }
        return;
    }
    // An empty user name/password is valid input, only a missing one is not
    if (!request.userName()) {
        throw InvalidOperationException(QStringLiteral("Missing 'username' request argument"));
    }
    if (!request.password()) {
        throw InvalidOperationException(QStringLiteral("Missing 'password' request argument"));
    }
}

void StoreCommand::executeInternal(const GitRequest &request, IHostProvider &provider)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_cmd_store"), QStringLiteral("provider_store"));
    provider.storeCredential(request);
}

EraseCommand::EraseCommand(ICommandContext &context, IHostProviderRegistry &registry)
    : GitCommandBase(context, QStringLiteral("erase"), QStringLiteral("[Git] Erase a stored credential"), registry)
{
    setHidden(true);
}

void EraseCommand::executeInternal(const GitRequest &request, IHostProvider &provider)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_cmd_erase"), QStringLiteral("provider_erase"));
    provider.eraseCredential(request);
}

// =============================================================================================
// capability
// =============================================================================================

CapabilityCommand::CapabilityCommand(ICommandContext &context)
    : Command(QStringLiteral("capability"), QStringLiteral("[Git] Advertise supported credential helper protocol capabilities")),
      m_context(context)
{
    setHidden(true);
    setHandler([this](const ParseResult &) {
        execute();
        return 0;
    });
}

void CapabilityCommand::execute()
{
    constexpr int ProtocolVersion = 0;
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("git_cmd"), QStringLiteral("run"));
    Trace2::writeData(QStringLiteral("git_cmd"), QStringLiteral("name"), QStringLiteral("capability"));
    m_context.trace().writeLine(QStringLiteral("Start 'capability' command..."));
    m_context.streams().out().writeLine(QStringLiteral("version %1").arg(ProtocolVersion));

    const QStringList protocolNames = GitCapabilitiesUtils::toProtocolNames(Constants::SupportedCapabilities);
    for (const QString &name : protocolNames) {
        m_context.streams().out().writeLine(QStringLiteral("capability %1").arg(name));
    }
    m_context.trace().writeLine(QStringLiteral("End 'capability' command..."));
}

// =============================================================================================
// configure / unconfigure
// =============================================================================================

ConfigurationCommandBase::ConfigurationCommandBase(ICommandContext &context, const QString &name, const QString &description,
                                                   IConfigurationService &configurationService)
    : Command(name, description), m_context(context), m_configurationService(configurationService)
{
    addOption(Option::flag(QStringLiteral("--system"), QStringLiteral("Modify the system-wide Git configuration instead of the current user")));
    setHandler([this](const ParseResult &result) {
        execute(result.has(QStringLiteral("--system")));
        return 0;
    });
}

void ConfigurationCommandBase::execute(bool system)
{
    const ConfigurationTarget target = system ? ConfigurationTarget::System : ConfigurationTarget::User;
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cfg_cmd"), QStringLiteral("run"));
    Trace2::writeData(QStringLiteral("cfg_cmd"), QStringLiteral("name"), name());
    Trace2::writeData(QStringLiteral("cfg_cmd"), QStringLiteral("target"), system ? QStringLiteral("System") : QStringLiteral("User"));
    executeInternal(target);
}

ConfigureCommand::ConfigureCommand(ICommandContext &context, IConfigurationService &configurationService)
    : ConfigurationCommandBase(context, QStringLiteral("configure"), QStringLiteral("Configure Git Credential Manager as the Git credential helper"), configurationService)
{
}

void ConfigureCommand::executeInternal(ConfigurationTarget target) { m_configurationService.configure(target); }

UnconfigureCommand::UnconfigureCommand(ICommandContext &context, IConfigurationService &configurationService)
    : ConfigurationCommandBase(context, QStringLiteral("unconfigure"), QStringLiteral("Unconfigure Git Credential Manager as the Git credential helper"), configurationService)
{
}

void UnconfigureCommand::executeInternal(ConfigurationTarget target)
{
    m_configurationService.unconfigure(target);
}

// =============================================================================================
// diagnose
// =============================================================================================

struct DiagnoseCommand::Private
{
    explicit Private(ICommandContext &c) : context(c) {}
    ICommandContext &context;
    std::unique_ptr<TextWriter> ownedOut;
    TextWriter *out = nullptr;
    bool useColor = false;
    QList<std::shared_ptr<IDiagnostic>> diagnostics;

    // Minimal rendering of the Spectre.Console markup upstream uses.
    QString style(const QString &text, const char *ansi) const {
        return useColor ? QStringLiteral("\x1b[%1m%2\x1b[0m").arg(QLatin1String(ansi), text) : text;
    }
    void line(const QString &text = QString()) {
        out->writeLine(text);
    }
};

DiagnoseCommand::DiagnoseCommand(ICommandContext &context)
    : Command(QStringLiteral("diagnose"), QStringLiteral("Run diagnostics and gather logs to diagnose problems with Git Credential Manager")),
      d(std::make_unique<Private>(context))
{
    auto file = std::make_unique<QFile>();
    if (!file->open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        qWarning("diagnose: failed to open stdout");
    }

    struct OwningWriter : DeviceTextWriter
    {
        explicit OwningWriter(std::unique_ptr<QFile> f) : DeviceTextWriter(f.get()), file(std::move(f)) {}
        std::unique_ptr<QFile> file;
    };
    d->ownedOut = std::make_unique<OwningWriter>(std::move(file));
    d->out = d->ownedOut.get();
    d->useColor = ::isatty(STDOUT_FILENO);

    addOption(Option{{QStringLiteral("--output"), QStringLiteral("-o")}, QStringLiteral("Output directory for diagnostic logs."), true});
    addOption(Option::flag(QStringLiteral("--strict"), QStringLiteral("Exit with a non-zero code if diagnostic warnings are present.")));
    setHandler([this](const ParseResult &result) {
        return execute(result.value(QStringLiteral("--output")), result.has(QStringLiteral("--strict")));
    });
}

DiagnoseCommand::DiagnoseCommand(ICommandContext &context, TextWriter &out, bool useColor) : DiagnoseCommand(context)
{
    d->ownedOut.reset();
    d->out = &out;
    d->useColor = useColor;
}

DiagnoseCommand::~DiagnoseCommand() = default;

void DiagnoseCommand::addStandardDiagnostics()
{
    d->diagnostics.append(std::make_shared<EnvironmentDiagnostic>(d->context));
    d->diagnostics.append(std::make_shared<FileSystemDiagnostic>(d->context));
    d->diagnostics.append(std::make_shared<NetworkingDiagnostic>(d->context));
    d->diagnostics.append(std::make_shared<GitDiagnostic>(d->context));
    d->diagnostics.append(std::make_shared<CredentialStoreDiagnostic>(d->context));
}

void DiagnoseCommand::addDiagnostic(std::shared_ptr<IDiagnostic> diagnostic)
{
    d->diagnostics.append(std::move(diagnostic));
}

void DiagnoseCommand::addDiagnostics(const QList<std::shared_ptr<IDiagnostic>> &diagnostics)
{
    d->diagnostics.append(diagnostics);
}

int DiagnoseCommand::invoke(const QStringList &args)
{
    // A non-owning handle to this command as the parser root
    std::shared_ptr<Command> root(std::shared_ptr<Command>(), this);
    StringWriter err;
    CommandLineParser parser(root, QStringLiteral("diagnose"), Constants::gcmVersion(), *d->out, err);
    const int result = parser.invoke(args);
    if (!err.toString().isEmpty()) {
        d->out->write(err.toString());
    }
    return result;
}

int DiagnoseCommand::execute(const OptString &output, bool strict)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("diag_cmd"), QStringLiteral("run"));

    // Neither IStandardStreams nor IConsoleService are used: any component of the context may be what is broken.
    d->line(d->style(QStringLiteral("Running diagnostics..."), "1"));
    d->line();

    if (d->diagnostics.isEmpty()) {
        d->line(QStringLiteral("No diagnostics to run."));
        return 0;
    }

    const QString currentDir = QDir::currentPath();
    QString outputDir;
    if (isNullOrWhiteSpace(output)) {
        outputDir = currentDir;
    } else {
        QDir().mkpath(*output);
        outputDir = QDir::cleanPath(QDir(currentDir).absoluteFilePath(*output));
    }

    const QString logFilePath = QDir(outputDir).filePath(QStringLiteral("gcm-diagnose.log"));
    QFile logFile(logFilePath);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw IOException(QStringLiteral("Could not open '%1' for writing: %2").arg(logFilePath, logFile.errorString()));
    }
    DeviceTextWriter fullLog(&logFile);

    // Header
    fullLog.writeLine(QStringLiteral("Diagnose log at %1Z").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).chopped(1)));
    fullLog.writeLine();
    fullLog.writeLine(QStringLiteral("AppPath: %1").arg(d->context.applicationPath()));
    fullLog.writeLine(QStringLiteral("InstallDir: %1").arg(d->context.installationDirectory()));
    fullLog.writeLine(QStringLiteral("Version: %1").arg(Constants::gcmInformationalVersion()));
    fullLog.writeLine();

    QList<DiagnosticResult> results;
    for (const auto &diagnostic : std::as_const(d->diagnostics)) {
        fullLog.writeLine(QStringLiteral("------------"));
        fullLog.writeLine(QStringLiteral("Diagnostic: %1").arg(diagnostic->name()));

        QString skipReason;
        if (!diagnostic->canRun(skipReason)) {
            fullLog.write(QStringLiteral("Outcome: Skipped"));
            if (!isNullOrWhiteSpace(skipReason)) {
                fullLog.write(QStringLiteral(" (%1)").arg(skipReason));
            }
            fullLog.writeLine();
            d->line(QStringLiteral("%1 %2 %3").arg(
                d->style(QStringLiteral("[SKIP]"), "1;90"),
                diagnostic->name(),
                d->style(QStringLiteral("(%1)").arg(skipReason), "3;90")));
            results.append(DiagnosticResult::skipped(skipReason));
            continue;
        }

        const DiagnosticResult result = diagnostic->run();
        fullLog.writeLine(QStringLiteral("Outcome: %1").arg(diagnosticOutcomeName(result.outcome)));
        const QStringList exceptions = result.exceptions();
        if (exceptions.size() == 1) {
            fullLog.writeLine(QStringLiteral("Exception: %1").arg(exceptions.first()));
        } else if (exceptions.size() > 1) {
            fullLog.writeLine(QStringLiteral("Exception: AggregateException"));
            fullLog.writeLine(QStringLiteral("InnerExceptions (flattened):"));
            for (const QString &e : exceptions) {
                fullLog.writeLine(e);
            }
        }
        fullLog.writeLine(QStringLiteral("Log:"));
        for (const DiagnosticReport &report : result.reports) {
            fullLog.writeLine(report.message);
        }

        switch (result.outcome) {
            case DiagnosticOutcome::Success:
                d->line(QStringLiteral("%1 %2").arg(d->style(QStringLiteral("[ OK ]"), "1;32"), diagnostic->name()));
                break;
            case DiagnosticOutcome::Warning:
                d->line(QStringLiteral("%1 %2").arg(d->style(QStringLiteral("[WARN]"), "1;33"), diagnostic->name()));
                break;
            case DiagnosticOutcome::Error:
                d->line(QStringLiteral("%1 %2").arg(d->style(QStringLiteral("[FAIL]"), "1;31"), diagnostic->name()));
                break;
            case DiagnosticOutcome::Skipped:
                throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values."));
        }

        if (result.outcome != DiagnosticOutcome::Success) {
            if (!exceptions.isEmpty()) {
                d->line();
                d->line(d->style(QStringLiteral("Exception Details"), "4;31"));
                d->line(exceptions.join(QLatin1Char('\n')));
            }
            d->line();
            d->line(d->style(QStringLiteral("Diagnostic Log"), "4"));
            for (const DiagnosticReport &report : result.reports) {
                d->line(report.message);
            }
            d->line();
        }
        fullLog.flush();
        results.append(result);
    }

    // Copy additional files next to the log
    QStringList additionalFiles;
    for (const DiagnosticResult &result : std::as_const(results)) {
        for (const QString &filePath : result.additionalFiles) {
            const QString destPath = QDir(outputDir).filePath(QFileInfo(filePath).fileName());
            QFile::remove(destPath);
            if (!QFile::copy(filePath, destPath)) {
                d->line(d->style(QStringLiteral("Failed to copy additional file '%1'").arg(filePath), "31"));
            }
            additionalFiles.append(destPath);
        }
    }

    d->line();

    // Summary
    auto count = [&](DiagnosticOutcome o) {
        return int(std::count_if(results.begin(), results.end(), [o](const DiagnosticResult &r) { return r.outcome == o; }));
    };
    const int numPassed = count(DiagnosticOutcome::Success), numFailed = count(DiagnosticOutcome::Error);
    const int numWarned = count(DiagnosticOutcome::Warning), numSkipped = count(DiagnosticOutcome::Skipped);
    auto countText = [&](int n, const QString &label, const char *color) {
        const QString text = d->style(QString::number(n), "1") + QLatin1Char(' ') + label;
        return color && n > 0 ? d->style(text, color) : text;
    };
    d->line(d->style(QStringLiteral("Summary"), "1;4"));
    d->line(countText(numPassed, QStringLiteral("passed"), "32") + QStringLiteral("    ") +
            countText(numSkipped, QStringLiteral("skipped"), nullptr) + QStringLiteral("    ") +
            countText(numWarned, QStringLiteral("warned"), "33") + QStringLiteral("    ") +
            countText(numFailed, QStringLiteral("failed"), "31"));
    d->line();
    d->line(d->style(QStringLiteral("Log files"), "1;4"));
    d->line(logFilePath);
    for (const QString &filePath : std::as_const(additionalFiles)) {
        d->line(filePath);
    }
    d->line();
    d->line(d->style(QStringLiteral("Caution: Log files may include sensitive information - redact before sharing!"), "33"));
    d->line();
    if (numFailed + numWarned > 0) {
        d->line(d->style(QStringLiteral("Diagnostics indicate a possible problem with your installation."), "33"));
        d->line(d->style(QStringLiteral("Please open an issue at %1 and include log files.").arg(Constants::HelpUrls::GcmNewIssue), "33"));
        d->line();
    }

    // In strict mode warnings count as failures too
    const bool failed = std::any_of(results.begin(), results.end(), [strict](const DiagnosticResult &r) {
        return r.outcome == DiagnosticOutcome::Error || (strict && r.outcome == DiagnosticOutcome::Warning);
    });
    return failed ? 1 : 0;
}

// =============================================================================================
// ProviderCommand
// =============================================================================================

ProviderCommand::ProviderCommand(const IHostProvider &provider)
    : Command(provider.id(), QStringLiteral("Commands for interacting with the %1 host provider").arg(provider.name()))
{
}

} // namespace gcm
