// The standard diagnostics of `diagnose`. Ports Diagnostics/*.cs (Entra lives with the Azure Repos provider).
#pragma once

#include "core/application.h"

namespace gcm {

class EnvironmentDiagnostic : public Diagnostic
{
public:
    explicit EnvironmentDiagnostic(ICommandContext &context) : Diagnostic(QStringLiteral("Environment"), context) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;
};

class FileSystemDiagnostic : public Diagnostic
{
public:
    explicit FileSystemDiagnostic(ICommandContext &context) : Diagnostic(QStringLiteral("File system"), context) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;
};

class NetworkingDiagnostic : public Diagnostic
{
public:
    explicit NetworkingDiagnostic(ICommandContext &context) : Diagnostic(QStringLiteral("Networking"), context) {}
    /// HEAD http://example.com, falling back to http://httpforever.com (public for tests).
    void sendHttpRequest(IDiagnosticReporter &reporter, const HttpClient &httpClient);

protected:
    void runInternal(IDiagnosticReporter &reporter) override;
};

class GitDiagnostic : public Diagnostic
{
public:
    explicit GitDiagnostic(ICommandContext &context) : Diagnostic(QStringLiteral("Git"), context) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;
};

class CredentialStoreDiagnostic : public Diagnostic
{
public:
    explicit CredentialStoreDiagnostic(ICommandContext &context) : Diagnostic(QStringLiteral("Credential storage"), context) {}

protected:
    void runInternal(IDiagnosticReporter &reporter) override;
};

} // namespace gcm
