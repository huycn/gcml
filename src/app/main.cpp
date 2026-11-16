// git-credential-manager: port of src/git-credential-manager/Program.cs.
// The .NET program runs the application on a second thread so the main thread can pump the UI dispatcher; this port
// has no in-process UI and runs everything on the main thread, under the same TRACE2 thread name ("AppMain").
#include "core/application.h"
#include "core/commandcontext.h"
#include "core/generic.h"
#include "core/trace2.h"

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
            // specific one, so it is registered with low priority. (Azure Repos, Bitbucket, GitHub and GitLab are
            // added here as their packages are ported.)
            application.registerProvider(std::make_shared<GenericHostProvider>(context), HostProviderPriority::Low);
        }
        exitCode = application.run(args);
    }

    Trace2::stop(exitCode);
    // Environment.Exit(-1) gives the process exit status 255
    return exitCode & 0xFF;
}
