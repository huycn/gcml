// Host providers and their registry. Ports HostProvider.cs, HostProviderRegistry.cs,
// Commands/ProviderCommand.cs (ICommandProvider), Diagnostics/IDiagnosticProvider.cs.
#pragma once

#include "core/common.h"
#include "core/gitrequest.h"
#include "core/http.h"

namespace gcm {

class ICommandContext;
class Command;
class IDiagnostic;

class IHostProvider
{
public:
    virtual ~IHostProvider() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QStringList supportedAuthorityIds() const { return {}; }
    virtual bool isSupported(const GitRequest &request) = 0;
    /// `response` is the auto-detect probe result (may be null when the probe failed).
    virtual bool isSupported(const HttpResponse *response) { Q_UNUSED(response); return false; }
    virtual GitResponse getCredential(const GitRequest &request) = 0;
    virtual void storeCredential(const GitRequest &request) = 0;
    virtual void eraseCredential(const GitRequest &request) = 0;
};

/// Base for providers that keep credentials in the OS credential store.
class HostProvider : public IHostProvider
{
public:
    explicit HostProvider(ICommandContext &context) : m_context(context) {}

    /// Stable key the credential is stored under: the remote URI without userinfo, trailing '/' trimmed.
    virtual QString getServiceName(const GitRequest &request);
    virtual Credential generateCredential(const GitRequest &request) = 0;

    GitResponse getCredential(const GitRequest &request) override;
    void storeCredential(const GitRequest &request) override;
    void eraseCredential(const GitRequest &request) override;

protected:
    ICommandContext &context() const { return m_context; }
    ICommandContext &m_context;
};

/// Optional provider facets, discovered with dynamic_cast by Application::registerProvider.
class ICommandProvider
{
public:
    virtual ~ICommandProvider() = default;
    /// A command named after the provider id carrying provider specific sub-commands.
    virtual std::shared_ptr<Command> createCommand() = 0;
};

class IDiagnosticProvider
{
public:
    virtual ~IDiagnosticProvider() = default;
    virtual QList<std::shared_ptr<IDiagnostic>> getDiagnostics() = 0;
};

enum class HostProviderPriority {
    Low = 0,
    Normal = 1,
    High = 2,
};

class IHostProviderRegistry
{
public:
    virtual ~IHostProviderRegistry() = default;

    /// Throws ArgumentException for providers using the reserved id / authority "auto".
    virtual void registerProvider(std::shared_ptr<IHostProvider> provider, HostProviderPriority priority) = 0;
    /// Override -> legacy authority -> auto-detect (static match, then one HEAD probe shared by all providers;
    /// a probe-based match is remembered in the global Git config). Throws when nothing matches.
    virtual std::shared_ptr<IHostProvider> getProvider(const GitRequest &request) = 0;
};

class HostProviderRegistry : public IHostProviderRegistry
{
public:
    explicit HostProviderRegistry(ICommandContext &context);
    ~HostProviderRegistry() override;

    void registerProvider(std::shared_ptr<IHostProvider> provider, HostProviderPriority priority) override;
    std::shared_ptr<IHostProvider> getProvider(const GitRequest &request) override;

private:
    ICommandContext &m_context;
    QList<std::shared_ptr<IHostProvider>> m_providers[3];
    /// Priorities in the order they were first registered (enumeration order of upstream's dictionary).
    QList<int> m_priorityOrder;
};

} // namespace gcm
