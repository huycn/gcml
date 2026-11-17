// Port of Core/UI/HelperApplication + HelperCommand and the UI commands of Core, GitHub, GitLab and Bitbucket
// (src/*/UI/Commands/*.cs): the command-line front end of git-credential-manager-ui.
//
// Protocol (see PLAN.md 3.3): options on the command line, optional input on stdin, a `key=value` dictionary ended
// by a blank line on stdout. Errors exit with -1 (255) and `error=<message>`; a dismissed dialog also writes
// `cancelled=1`.
#pragma once

#include "core/application.h"

namespace gcm::ui {

class HelperApplication : public ApplicationBase
{
public:
    explicit HelperApplication(ICommandContext &context) : ApplicationBase(context) {}

    /// The command tree: basic, oauth, device, default-account, github {prompt, 2fa, device, select-account},
    /// gitlab prompt, bitbucket prompt.
    std::shared_ptr<Command> createRootCommand();

    /// True when the GitHub URL is github.com or gist.github.com (GitHubHostProvider.IsGitHubDotCom).
    static bool isGitHubDotCom(const OptString &url);
    /// True when the URL is absolute and its host is gitlab.com (GitLabConstants.IsGitLabDotCom).
    static bool isGitLabDotCom(const QUrl &url);

protected:
    int runInternal(const QStringList &args) override;

private:
    int onException(const std::exception &ex);
    void writeResult(const StringDictionary &result);
};

} // namespace gcm::ui
