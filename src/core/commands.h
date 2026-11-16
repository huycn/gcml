// The git-credential-manager commands. Ports Commands/*.cs.
#pragma once

#include "core/cmdline.h"
#include "core/application.h"

namespace gcm {

/// Base of get / store / erase: reads the request from stdin, selects the provider, runs.
class GitCommandBase : public Command
{
public:
    GitCommandBase(ICommandContext &context, const QString &name, const QString &description, IHostProviderRegistry &hostProviderRegistry);

    /// The handler: returns after the provider ran (exceptions propagate).
    void execute();

protected:
    virtual void ensureMinimumRequest(const GitRequest &request);
    virtual void executeInternal(const GitRequest &request, IHostProvider &provider) = 0;
    ICommandContext &m_context;

private:
    IHostProviderRegistry &m_hostProviderRegistry;
};

class GetCommand : public GitCommandBase
{
public:
    GetCommand(ICommandContext &context, IHostProviderRegistry &registry);

protected:
    void executeInternal(const GitRequest &request, IHostProvider &provider) override;
};

class StoreCommand : public GitCommandBase
{
public:
    StoreCommand(ICommandContext &context, IHostProviderRegistry &registry);

protected:
    void ensureMinimumRequest(const GitRequest &request) override;
    void executeInternal(const GitRequest &request, IHostProvider &provider) override;
};

class EraseCommand : public GitCommandBase
{
public:
    EraseCommand(ICommandContext &context, IHostProviderRegistry &registry);

protected:
    void executeInternal(const GitRequest &request, IHostProvider &provider) override;
};

class CapabilityCommand : public Command
{
public:
    explicit CapabilityCommand(ICommandContext &context);

    void execute();

private:
    ICommandContext &m_context;
};

/// configure / unconfigure with `--system`.
class ConfigurationCommandBase : public Command
{
public:
    ConfigurationCommandBase(ICommandContext &context, const QString &name, const QString &description, IConfigurationService &configurationService);

    void execute(bool system);

protected:
    virtual void executeInternal(ConfigurationTarget target) = 0;
    ICommandContext &m_context;
    IConfigurationService &m_configurationService;
};

class ConfigureCommand : public ConfigurationCommandBase
{
public:
    ConfigureCommand(ICommandContext &context, IConfigurationService &configurationService);

protected:
    void executeInternal(ConfigurationTarget target) override;
};

class UnconfigureCommand : public ConfigurationCommandBase
{
public:
    UnconfigureCommand(ICommandContext &context, IConfigurationService &configurationService);

protected:
    void executeInternal(ConfigurationTarget target) override;
};

/// `diagnose [--output|-o <dir>] [--strict]`. Writes its report to `out` (stdout by default) rather than through
/// the context's streams or console, which may be what is broken.
class DiagnoseCommand : public Command
{
public:
    explicit DiagnoseCommand(ICommandContext &context);
    DiagnoseCommand(ICommandContext &context, TextWriter &out, bool useColor = false);
    ~DiagnoseCommand() override;

    /// Environment, file system, networking, Git and credential storage (Entra is added by the Azure Repos provider).
    void addStandardDiagnostics();
    void addDiagnostic(std::shared_ptr<IDiagnostic> diagnostic);
    void addDiagnostics(const QList<std::shared_ptr<IDiagnostic>> &diagnostics);

    /// Parses `args` with this command as the root and runs it (System.CommandLine's InvokeAsync).
    int invoke(const QStringList &args);
    int execute(const OptString &output, bool strict);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/// A command named after a provider, carrying its sub-commands.
class ProviderCommand : public Command
{
public:
    explicit ProviderCommand(const IHostProvider &provider);
};

} // namespace gcm
