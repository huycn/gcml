// Port of TestInfrastructure/Objects/TestCommandContext.cs
#pragma once

#include "core/commandcontext.h"
#include "infra/nulltrace.h"
#include "infra/testconsoleservice.h"
#include "infra/testcredentialstore.h"
#include "infra/testenvironment.h"
#include "infra/testfilesystem.h"
#include "infra/testgit.h"
#include "infra/testhttpmessagehandler.h"
#include "infra/testprocessmanager.h"
#include "infra/testsessionmanager.h"
#include "infra/testsettings.h"
#include "infra/teststandardstreams.h"

namespace gcm::test {

class TestCommandContext : public ICommandContext
{
public:
    TestCommandContext()
    {
        Settings.Environment = &Environment;
        Settings.GitConfiguration = &Git.Configuration;
    }
    TestCommandContext(const TestCommandContext &) = delete;
    TestCommandContext &operator=(const TestCommandContext &) = delete;

    QString AppPath = QStringLiteral("/usr/local/bin/git-credential-manager");
    QString InstallDir = QStringLiteral("/usr/local/bin");
    TestStandardStreams Streams;
    TestConsoleService Console;
    TestSessionManager SessionManager;
    /// Replaceable, e.g. by a real Trace with a listener.
    std::shared_ptr<ITrace> Trace = std::make_shared<NullTrace>();
    TestFileSystem FileSystem;
    TestCredentialStore CredentialStore;
    TestHttpClientFactory HttpClientFactory;
    TestGit Git;
    TestEnvironment Environment{&FileSystem};
    TestSettings Settings;
    TestProcessManager ProcessManager;

    // ICommandContext
    QString applicationPath() const override { return AppPath; }
    void setApplicationPath(const QString &path) override { AppPath = path; }
    QString installationDirectory() const override { return InstallDir; }
    ISettings &settings() override { return Settings; }
    IStandardStreams &streams() override { return Streams; }
    IConsoleService &console() override { return Console; }
    ISessionManager &sessionManager() override { return SessionManager; }
    ITrace &trace() override { return *Trace; }
    IFileSystem &fileSystem() override { return FileSystem; }
    ICredentialStore &credentialStore() override { return CredentialStore; }
    IHttpClientFactory &httpClientFactory() override { return HttpClientFactory; }
    IGit &git() override { return Git; }
    IEnvironment &environment() override { return Environment; }
    IProcessManager &processManager() override { return ProcessManager; }
};

} // namespace gcm::test
