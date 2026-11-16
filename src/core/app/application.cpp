// Port of ApplicationBase.cs and Application.cs
#include "core/application.h"

#include "core/commands.h"
#include "core/constants.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <unistd.h>

namespace gcm {

namespace {

/// A trace listener writing to a file opened for appending.
class FileTextWriter : public TextWriter
{
public:
    explicit FileTextWriter(const QString &path) : m_file(path) {
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            throw IOException(m_file.errorString());
        }
    }

    void write(const QString &text) override {
        m_file.write(text.toUtf8());
    }

    void flush() override {
        m_file.flush();
    }

private:
    QFile m_file;
};

bool isDebuggerAttached()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QList<QByteArray> lines = status.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (line.startsWith("TracerPid:")) {
            return line.mid(10).trimmed().toInt() != 0;
        }
    }
    return false;
}

/// Regex.Escape: \, *, +, ?, |, {, [, (, ), ^, $, ., #, and white space.
QString dotnetRegexEscape(const QString &s)
{
    QString out;
    for (const QChar c : s) {
        switch (c.unicode()) {
            case '\\': case '*': case '+': case '?': case '|': case '{': case '[': case '(': case ')': case '^': case '$':
            case '.': case '#': case ' ':
                out += QLatin1Char('\\') + c;
                break;
            case '\t': out += QLatin1String("\\t"); break;
            case '\n': out += QLatin1String("\\n"); break;
            case '\r': out += QLatin1String("\\r"); break;
            case '\f': out += QLatin1String("\\f"); break;
            case '\v': out += QLatin1String("\\v"); break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

// =============================================================================================
// ApplicationBase
// =============================================================================================

ApplicationBase::~ApplicationBase()
{
    if (m_traceFileWriter) {
        m_traceFileWriter->flush();
    }
}

int ApplicationBase::run(const QStringList &args)
{
    ISettings &settings = m_context.settings();

    // Wait for a debugger
    if (settings.isDebuggingEnabled()) {
        m_context.streams().error().writeLine(QStringLiteral("Waiting for debugger to be attached (pid=%1)...").arg(QCoreApplication::applicationPid()));
        while (!isDebuggerAttached()) {
            QThread::sleep(1);
        }
    }

    // Enable tracing
    QString traceValue;
    if (settings.getTracingEnabled(traceValue)) {
        if (isTruthy(traceValue)) {
            // Trace to stderr (the streams outlive this call; the listener does not own them)
            m_context.trace().addListener(std::shared_ptr<TextWriter>(std::shared_ptr<TextWriter>(), &m_context.streams().error()));
        } else if (traceValue.startsWith(QLatin1Char('/'))) {
            // Trace to a file
            try {
                m_traceFileWriter = std::make_shared<FileTextWriter>(traceValue);
                m_context.trace().addListener(m_traceFileWriter);
            } catch (const Exception &ex) {
                m_context.console().writeWarning(QStringLiteral("unable to trace to file '%1': %2").arg(traceValue, ex.message()));
            }
        } else {
            m_context.console().writeWarning(
                QStringLiteral("unknown value for %1 '%2'").arg(Constants::EnvironmentVariables::GcmTrace, traceValue));
        }
    }

    // Enable sensitive tracing and show warning
    if (settings.isSecretTracingEnabled()) {
        m_context.trace().setSecretTracingEnabled(true);
        m_context.trace().writeLine(QStringLiteral("Tracing of secrets is enabled. Trace output may contain sensitive information."));
    }

    return runInternal(args);
}

// =============================================================================================
// Application
// =============================================================================================

struct Application::Private
{
    std::shared_ptr<IHostProviderRegistry> providerRegistry;
    std::shared_ptr<IConfigurationService> configurationService;
    QList<std::shared_ptr<IHostProvider>> providers; // keeps configurable facets alive
    QList<std::shared_ptr<Command>> providerCommands;
    QList<std::shared_ptr<IDiagnostic>> diagnostics;
};

Application::Application(ICommandContext &context)
    : Application(context, std::make_shared<HostProviderRegistry>(context), std::make_shared<ConfigurationService>(context))
{
}

Application::Application(ICommandContext &context, std::shared_ptr<IHostProviderRegistry> providerRegistry, std::shared_ptr<IConfigurationService> configurationService)
    : ApplicationBase(context), d(std::make_unique<Private>())
{
    EnsureArgument::notNull(providerRegistry, "providerRegistry");
    EnsureArgument::notNull(configurationService, "configurationService");
    d->providerRegistry = std::move(providerRegistry);
    d->configurationService = std::move(configurationService);
    d->configurationService->addComponent(this);
}

Application::~Application() = default;

void Application::registerProvider(std::shared_ptr<IHostProvider> provider, HostProviderPriority priority)
{
    d->providerRegistry->registerProvider(provider, priority);
    d->providers.append(provider);

    // A configurable provider takes part in configure / unconfigure
    if (auto *configurable = dynamic_cast<IConfigurableComponent *>(provider.get())) {
        d->configurationService->addComponent(configurable);
    }

    // Custom commands of the provider
    if (auto *cmdProvider = dynamic_cast<ICommandProvider *>(provider.get())) {
        d->providerCommands.append(cmdProvider->createCommand());
    }

    // Custom diagnostics of the provider
    if (auto *diagnosticProvider = dynamic_cast<IDiagnosticProvider *>(provider.get())) {
        d->diagnostics.append(diagnosticProvider->getDiagnostics());
    }
}

bool Application::containsInterrupt(const std::exception &ex)
{
    if (dynamic_cast<const InterruptedException *>(&ex)) {
        return true;
    }
    if (const auto *gcmEx = dynamic_cast<const Exception *>(&ex)) {
        if (const auto &inner = gcmEx->inner()) {
            return containsInterrupt(*inner);
        }
    }
    return false;
}

int Application::runInternal(const QStringList &args)
{
    std::shared_ptr<Command> rootCommand;
    std::unique_ptr<CommandLineParser> parser;
    {
        Trace2::Scope region = Trace2::startRegion(QStringLiteral("main"), QStringLiteral("cmd_setup"));

        rootCommand = std::make_shared<Command>(QStringLiteral("git-credential-manager"), QString());
        auto diagnoseCommand = std::make_shared<DiagnoseCommand>(m_context);
        diagnoseCommand->addStandardDiagnostics();

        // Common options
        rootCommand->addGlobalOption(Option::flag(QStringLiteral("--no-ui"), QStringLiteral("Do not use graphical user interface prompts")));

        // Standard commands
        IHostProviderRegistry &registry = *d->providerRegistry;
        rootCommand->addCommand(std::make_shared<GetCommand>(m_context, registry));
        rootCommand->addCommand(std::make_shared<StoreCommand>(m_context, registry));
        rootCommand->addCommand(std::make_shared<EraseCommand>(m_context, registry));
        rootCommand->addCommand(std::make_shared<CapabilityCommand>(m_context));
        rootCommand->addCommand(std::make_shared<ConfigureCommand>(m_context, *d->configurationService));
        rootCommand->addCommand(std::make_shared<UnconfigureCommand>(m_context, *d->configurationService));
        rootCommand->addCommand(diagnoseCommand);

        // Custom provider commands and diagnostics
        for (const auto &providerCommand : std::as_const(d->providerCommands)) {
            rootCommand->addCommand(providerCommand);
        }
        diagnoseCommand->addDiagnostics(d->diagnostics);

        // Trace the current version, OS, runtime, and program arguments
        const PlatformInformation info = PlatformUtils::getPlatformInformation();
        ITrace &trace = m_context.trace();
        trace.writeLine(QStringLiteral("Version: %1").arg(Constants::gcmVersion()));
        trace.writeLine(QStringLiteral("Runtime: %1").arg(info.runtimeVersion));
        trace.writeLine(QStringLiteral("Platform: %1 (%2)").arg(info.operatingSystemType, info.cpuArchitecture));
        trace.writeLine(QStringLiteral("OSVersion: %1").arg(info.operatingSystemVersion));
        trace.writeLine(QStringLiteral("AppPath: %1").arg(m_context.applicationPath()));
        trace.writeLine(QStringLiteral("InstallDir: %1").arg(m_context.installationDirectory()));
        trace.writeLine(QStringLiteral("Arguments: %1").arg(args.join(QLatin1Char(' '))));

        const QString exeName = QFileInfo(m_context.applicationPath()).fileName();
        parser = std::make_unique<CommandLineParser>(
            rootCommand, exeName.isEmpty() ? rootCommand->name() : exeName,
            Constants::gcmInformationalVersion(),
            m_context.streams().out(),
            m_context.streams().error()
        );

        parser->setExceptionHandler([this](const std::exception &ex) {
            return onException(ex);
        });

        parser->addMiddleware([this](const ParseResult &result) {
            if (result.has(QStringLiteral("--no-ui"))) {
                m_context.settings().setGuiPromptsEnabled(false);
            }
        });

        const Command *root = rootCommand.get();
        parser->addMiddleware([root](const ParseResult &result) {
            if (result.command && result.command != root) {
                Trace2::writeCommandName(result.command->name());
            }
        });
    }
    return parser->invoke(args);
}

int Application::onException(const std::exception &ex)
{
    // A user interrupt (Ctrl+C at a prompt) is not an error: exit quietly with the conventional code.
    if (containsInterrupt(ex)) {
        m_context.trace().writeLine(QStringLiteral("Operation interrupted by the user (Ctrl+C)."));
        return 130;
    }
    writeException(ex);
    return -1;
}

void Application::writeException(const std::exception &ex)
{
    Trace2::writeError(ex);
    IConsoleService &console = m_context.console();

    // A nicer format for some well-known exception types
    if (const auto *gitEx = dynamic_cast<const GitException *>(&ex)) {
        console.writeFatal(QStringLiteral("%1 [%2]").arg(gitEx->message()).arg(gitEx->exitCode()));
        console.writeLine(gitEx->gitErrorMessage());
    } else if (const auto *interopEx = dynamic_cast<const InteropException *>(&ex)) {
        console.writeFatal(QStringLiteral("%1 [0x%2]").arg(interopEx->message()).arg(uint(interopEx->errorCode()), 0, 16));
    } else if (const auto *gcmEx = dynamic_cast<const Exception *>(&ex)) {
        console.writeFatal(gcmEx->message());
    } else {
        console.writeFatal(QString::fromUtf8(ex.what()));
    }

    // Inner exceptions too (there is no stack trace to print)
    if (const auto *gcmEx = dynamic_cast<const Exception *>(&ex)) {
        if (const auto &inner = gcmEx->inner()) {
            writeException(*inner);
        }
    }
}

QString Application::getGitConfigAppPath() const
{
    // Characters like ' ', '(' and ')' must be escaped
    QString path = m_context.applicationPath();
    path.replace(QLatin1String(" "), QLatin1String("\\ "))
        .replace(QLatin1String("("), QLatin1String("\\("))
        .replace(QLatin1String(")"), QLatin1String("\\)"));
    return path;
}

void Application::configure(ConfigurationTarget target)
{
    const QString helperKey = QStringLiteral("%1.%2").arg(
        Constants::GitConfiguration::Credential::SectionName,
        Constants::GitConfiguration::Credential::Helper);

    const QString appPath = getGitConfigAppPath();
    const GitConfigurationLevel configLevel = target == ConfigurationTarget::System ? GitConfigurationLevel::System : GitConfigurationLevel::Global;

    m_context.trace().writeLine(QStringLiteral("Configuring for config level '%1'.").arg(target == ConfigurationTarget::System ? QStringLiteral("System") : QStringLiteral("Global")));

    IGitConfiguration &config = m_context.git().getConfiguration();

    // Wanted:
    //   [credential]
    //       ...                # any number of helper entries (possibly none)
    //       helper =           # an empty value to reset/clear any previous entries (if applicable)
    //       helper = {appPath} # the executable, directly following the empty value
    //       ...                # any number of helper entries, but not the empty value
    QStringList currentValues = config.getAll(configLevel, GitConfigurationType::Raw, helperKey);

    // An existing app entry with a blank entry right before it and no blank entry after it (which would disable us)
    qsizetype appIndex = -1, lastEmptyIndex = -1;
    for (qsizetype i = 0; i < currentValues.size(); ++i) {
        if (m_context.fileSystem().isSamePath(currentValues[i], appPath)) {
            appIndex = i;
        }
        if (isNullOrWhiteSpace(currentValues[i])) {
            lastEmptyIndex = i;
        }
    }

    if (appIndex > 0 && isNullOrWhiteSpace(currentValues[appIndex - 1]) && lastEmptyIndex < appIndex) {
        m_context.trace().writeLine(QStringLiteral("Credential helper configuration is already set correctly."));
        return;
    }

    m_context.trace().writeLine(QStringLiteral("Updating Git credential helper configuration..."));

    // Clear any existing app entries
    config.unsetAll(configLevel, helperKey, dotnetRegexEscape(appPath));

    // Reload (unset only clears entries in the primary file, ignoring includes and alternatives)
    currentValues = config.getAll(configLevel, GitConfigurationType::Raw, helperKey);

    // An empty value clears helpers from lower-level configuration; then the executable path
    if (currentValues.isEmpty() || !isNullOrWhiteSpace(currentValues.last())) {
        config.add(configLevel, helperKey, QString());
    }
    config.add(configLevel, helperKey, appPath);
}

void Application::unconfigure(ConfigurationTarget target)
{
    const QString helperKey = QStringLiteral("%1.%2").arg(Constants::GitConfiguration::Credential::SectionName, Constants::GitConfiguration::Credential::Helper);
    const QString appPath = getGitConfigAppPath();
    const GitConfigurationLevel configLevel = target == ConfigurationTarget::System ? GitConfigurationLevel::System : GitConfigurationLevel::Global;

    m_context.trace().writeLine(QStringLiteral("Unconfiguring for config level '%1'.").arg(target == ConfigurationTarget::System ? QStringLiteral("System") : QStringLiteral("Global")));

    IGitConfiguration &config = m_context.git().getConfiguration();

    // Remove the {appPath} entry, and the blank entry right before it if nothing follows.
    m_context.trace().writeLine(QStringLiteral("Removing Git credential helper configuration..."));
    const QStringList currentValues = config.getAll(configLevel, GitConfigurationType::Raw, helperKey);
    qsizetype appIndex = -1;
    for (qsizetype i = 0; i < currentValues.size() && appIndex < 0; ++i) {
        if (m_context.fileSystem().isSamePath(currentValues[i], appPath)) {
            appIndex = i;
        }
    }

    if (appIndex > -1) {
        // A blank entry immediately before an app entry in the last position
        if (appIndex > 0 && appIndex == currentValues.size() - 1 && isNullOrWhiteSpace(currentValues[appIndex - 1])) {
            config.unsetAll(configLevel, helperKey, Constants::RegexPatterns::Empty);
        }

        // The app entry
        config.unsetAll(configLevel, helperKey, dotnetRegexEscape(currentValues[appIndex]));
    }
}

} // namespace gcm
