// git-credential-manager-ui: the graphical prompts of Git Credential Manager, run as a helper process by
// git-credential-manager (see PLAN.md 3.3 and core/auth/authbase.h). Port of the Avalonia UI of upstream, which
// shows the same dialogs in-process.
#include "helperapp.h"
#include "widgets.h"

#include "core/commandcontext.h"
#include "core/trace2.h"

#include <QApplication>

#include <memory>

using namespace gcm;

namespace {

/// Help, version and an empty command line only print text: they must work without a display.
bool needsGui(const QStringList &args)
{
    if (args.isEmpty()) {
        return false;
    }
    for (const QString &arg : args) {
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h") || arg == QLatin1String("-?") ||
            arg == QLatin1String("--version")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    // The raw arguments: Qt must not interpret any of them.
    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args.append(QString::fromLocal8Bit(argv[i]));
    }

    Trace2::initialize(args);

    int exitCode = 0;
    {
        int qtArgc = 1;
        std::unique_ptr<QCoreApplication> app;
        if (needsGui(args)) {
            auto *gui = new QApplication(qtArgc, argv);
            QGuiApplication::setDesktopFileName(QStringLiteral("git-credential-manager"));
            ui::applySystemTheme(*gui);
            app.reset(gui);
        } else {
            app = std::make_unique<QCoreApplication>(qtArgc, argv);
        }

        CommandContext context;
        ui::HelperApplication application(context);
        exitCode = application.run(args);
    }

    Trace2::stop(exitCode);
    // Exit code -1 gives the process exit status 255, as upstream
    return exitCode & 0xFF;
}
