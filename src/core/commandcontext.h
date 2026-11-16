// Port of CommandContext.cs: the execution environment handed to every component.
#pragma once

#include "core/common.h"
#include "core/console.h"
#include "core/credential.h"
#include "core/git.h"
#include "core/http.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/streams.h"
#include "core/trace.h"

namespace gcm {

class ICommandContext
{
public:
    virtual ~ICommandContext() = default;

    /// Absolute path of the entry executable (what `configure` writes into credential.helper).
    virtual QString applicationPath() const = 0;
    virtual void setApplicationPath(const QString &path) = 0;
    /// Directory containing the executable (where the UI helper is looked up first).
    virtual QString installationDirectory() const = 0;
    virtual ISettings &settings() = 0;
    virtual IStandardStreams &streams() = 0;
    virtual IConsoleService &console() = 0;
    virtual ISessionManager &sessionManager() = 0;
    virtual ITrace &trace() = 0;
    virtual IFileSystem &fileSystem() = 0;
    virtual ICredentialStore &credentialStore() = 0;
    virtual IHttpClientFactory &httpClientFactory() = 0;
    virtual IGit &git() = 0;
    virtual IEnvironment &environment() = 0;
    virtual IProcessManager &processManager() = 0;
};

/// The real context: actual standard streams, file system, environment and Git.
class CommandContext : public ICommandContext
{
public:
    CommandContext();
    ~CommandContext() override;

    QString applicationPath() const override;
    void setApplicationPath(const QString &path) override;
    QString installationDirectory() const override;
    ISettings &settings() override;
    IStandardStreams &streams() override;
    IConsoleService &console() override;
    ISessionManager &sessionManager() override;
    ITrace &trace() override;
    IFileSystem &fileSystem() override;
    ICredentialStore &credentialStore() override;
    IHttpClientFactory &httpClientFactory() override;
    IGit &git() override;
    IEnvironment &environment() override;
    IProcessManager &processManager() override;

    /// GIT_EXEC_PATH/git if it exists, otherwise `git` from PATH.
    static QString getGitPath(IEnvironment &environment, IFileSystem &fileSystem, ITrace &trace);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace gcm
