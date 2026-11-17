// git-credential-manager: port of src/git-credential-manager/Program.cs.
// The .NET program runs the application on a second thread so the main thread can pump the UI dispatcher; this port
// has no in-process UI and runs everything on the main thread, under the same TRACE2 thread name ("AppMain").
#include "core/application.h"
#include "core/commandcontext.h"
#include "core/generic.h"
#include "core/trace2.h"
#ifdef GCM_HAVE_AZREPOS
#include "azrepos/azrepos.h"
#endif
#ifdef GCM_HAVE_BITBUCKET
#include "bitbucket/bitbucket.h"
#endif
#ifdef GCM_HAVE_GITHUB
#include "github/github.h"
#endif
#ifdef GCM_HAVE_GITLAB
#include "gitlab/gitlab.h"
#endif

#include <QCoreApplication>

using namespace gcm;

int main(int argc, char **argv)
{
    // The raw arguments: QCoreApplication must not interpret any of them.
    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args.append(QString::fromLocal8Bit(argv[i]));
    }

    Trace2::initialize(args);

    int exitCode = 0;
    {
        int qtArgc = 1;
        QCoreApplication app(qtArgc, argv);

        Trace2::Scope thread = Trace2::startThread(QStringLiteral("AppMain"));
        CommandContext context;
        Application application(context);
        {
            Trace2::Scope region = Trace2::startRegion(QStringLiteral("main"), QStringLiteral("provider_reg"));
            // All supported host providers at normal priority; the generic provider should never win against a more
            // specific one, so it is registered with low priority.
#ifdef GCM_HAVE_AZREPOS
            application.registerProvider(std::make_shared<azrepos::AzureReposHostProvider>(context), HostProviderPriority::Normal);
#endif
#ifdef GCM_HAVE_BITBUCKET
            application.registerProvider(std::make_shared<bitbucket::BitbucketHostProvider>(context), HostProviderPriority::Normal);
#endif
#ifdef GCM_HAVE_GITHUB
            application.registerProvider(std::make_shared<github::GitHubHostProvider>(context), HostProviderPriority::Normal);
#endif
#ifdef GCM_HAVE_GITLAB
            application.registerProvider(std::make_shared<gitlab::GitLabHostProvider>(context), HostProviderPriority::Normal);
#endif
            application.registerProvider(std::make_shared<GenericHostProvider>(context), HostProviderPriority::Low);
        }
        exitCode = application.run(args);
    }

    Trace2::stop(exitCode);
    // Environment.Exit(-1) gives the process exit status 255
    return exitCode & 0xFF;
}
