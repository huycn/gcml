// Port of ApplicationBase.cs + Application.cs and Diagnostics/Diagnostic.cs interfaces.
#pragma once

#include "core/commandcontext.h"
#include "core/hostprovider.h"

namespace gcm {

// --- diagnostics ----------------------------------------------------------------------------
enum class DiagnosticReportKind {
    Progress,
    Information,
    Warning,
    Error,
};

enum class DiagnosticOutcome {
    Success,
    Warning,
    Error,
    Skipped,
};

/// The enum names as .NET prints them ("Success", "Warning", ...).
QString diagnosticOutcomeName(DiagnosticOutcome outcome);

struct DiagnosticReport
{
    DiagnosticReportKind kind;
    QString message;
    /// "TypeName: message" of the exception that caused an Error report, if any.
    OptString exceptionText;
};

struct DiagnosticResult
{
    DiagnosticOutcome outcome = DiagnosticOutcome::Success;
    OptString skipReason;
    int errorCount = 0;
    int warningCount = 0;
    QList<DiagnosticReport> reports;
    QStringList additionalFiles;

    static DiagnosticResult skipped(const QString &reason);
    /// The exceptions of all reports, like upstream's DiagnosticResult.Exception (one, or an aggregate).
    QStringList exceptions() const;

    /// Derives outcome/counts from the reports.
    static DiagnosticResult create(QList<DiagnosticReport> reports, QStringList additionalFiles);
};

class IDiagnosticReporter
{
public:
    virtual ~IDiagnosticReporter() = default;
    virtual void reportProgress(const QString &message) = 0;
    virtual void reportInfo(const QString &message) = 0;
    virtual void reportWarning(const QString &message) = 0;
    virtual void reportError(const QString &message, const std::exception *exception = nullptr) = 0;
    /// Attach a file to be mentioned in the diagnostic summary.
    virtual void addFile(const QString &path) = 0;
};

class IDiagnostic
{
public:
    virtual ~IDiagnostic() = default;
    virtual QString name() const = 0;
    virtual bool canRun(QString &reason) = 0;
    virtual DiagnosticResult run(const std::function<void(const QString &)> &progress = nullptr) = 0;
};

/// Base class: collects reports from runInternal; an exception escaping it becomes an Error report.
class Diagnostic : public IDiagnostic
{
public:
    Diagnostic(QString name, ICommandContext &context) : m_name(std::move(name)), m_context(context) {}
    QString name() const override { return m_name; }
    bool canRun(QString &reason) override { Q_UNUSED(reason); return true; }
    DiagnosticResult run(const std::function<void(const QString &)> &progress = nullptr) override;

protected:
    virtual void runInternal(IDiagnosticReporter &reporter) = 0;
    QString m_name;
    ICommandContext &m_context;
};

// --- application ----------------------------------------------------------------------------
class ApplicationBase
{
public:
    explicit ApplicationBase(ICommandContext &context) : m_context(context) {}
    virtual ~ApplicationBase();

    /// Enables GCM_TRACE listeners / secret tracing as configured, then calls runInternal.
    int run(const QStringList &args);

protected:
    /// Trace file opened for GCM_TRACE=<path> (kept open until the application is destroyed).
    std::shared_ptr<TextWriter> m_traceFileWriter;
    virtual int runInternal(const QStringList &args) = 0;
    ICommandContext &m_context;
};

class Application : public ApplicationBase, public IConfigurableComponent
{
public:
    explicit Application(ICommandContext &context);
    /// Test seam.
    Application(ICommandContext &context, std::shared_ptr<IHostProviderRegistry> providerRegistry, std::shared_ptr<IConfigurationService> configurationService);
    ~Application() override;

    /// Also wires up the provider's optional IConfigurableComponent / ICommandProvider / IDiagnosticProvider facets.
    void registerProvider(std::shared_ptr<IHostProvider> provider, HostProviderPriority priority);

    // IConfigurableComponent: add/remove ourselves as `credential.helper` (with the leading empty reset entry).
    QString componentName() const override { return QStringLiteral("Git Credential Manager"); }
    void configure(ConfigurationTarget target) override;
    void unconfigure(ConfigurationTarget target) override;

    /// True when `ex` is an InterruptedException or wraps one as an inner exception.
    static bool containsInterrupt(const std::exception &ex);

protected:
    /// Builds the command tree (get/store/erase/capability/configure/unconfigure/diagnose + provider commands),
    /// traces version/platform information and dispatches. Exit codes: handler result, -1 on error,
    /// 130 when interrupted with Ctrl+C.
    int runInternal(const QStringList &args) override;

private:
    int onException(const std::exception &ex);
    void writeException(const std::exception &ex);
    QString getGitConfigAppPath() const;
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace gcm
