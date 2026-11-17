// git-credential-manager-ui-scripted: the real UI helper with a scripted user, for end-to-end tests
// (tests/e2e/ui-helper.sh). Test-only: it is built from the same library as git-credential-manager-ui and only adds
// a DialogWindow show hook that performs the actions of GCM_UI_SCRIPT on the offscreen platform.
//
// GCM_UI_SCRIPT: actions separated by ';', applied in order once the dialog is shown:
//   type:<objectName>=<text>   type text into the line edit named <objectName>
//   click:<objectName>         click the push button named <objectName>
//   tab:<index>                switch the authentication tab
//   select:<row>               select a row of the account list
//   cancel                     close the dialog (as the window's close button)
//   wait                       do nothing: the dialog stays open (device code dialogs, killed by the caller)
// GCM_UI_SCRIPT_LOG: file that receives one line per dialog: "<window title>|<visible tab titles>".
#include "helperapp.h"
#include "widgets.h"

#include "core/commandcontext.h"
#include "core/trace2.h"

#include <QApplication>
#include <QFile>
#include <QListWidget>
#include <QTabWidget>

using namespace gcm;
using namespace gcm::ui;

namespace {

void log(const QString &line)
{
    const QString path = qEnvironmentVariable("GCM_UI_SCRIPT_LOG");
    if (path.isEmpty()) {
        return;
    }
    QFile f(path);
    if (f.open(QIODevice::Append)) {
        f.write((line + QLatin1Char('\n')).toUtf8());
    }
}

void runScript(DialogWindow &window)
{
    QStringList tabs;
    if (auto *tw = window.findChild<QTabWidget *>(QStringLiteral("authModes"))) {
        for (int i = 0; i < tw->count(); ++i) {
            if (tw->isTabVisible(i)) {
                tabs.append(tw->tabText(i));
            }
        }
    }
    log(window.windowTitle() + QLatin1Char('|') + tabs.join(QLatin1Char(',')));

    const QStringList actions = qEnvironmentVariable("GCM_UI_SCRIPT").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &action : actions) {
        const QString verb = action.section(QLatin1Char(':'), 0, 0);
        const QString arg = action.section(QLatin1Char(':'), 1);
        if (verb == QLatin1String("type")) {
            if (auto *e = window.findChild<QLineEdit *>(arg.section(QLatin1Char('='), 0, 0))) {
                e->setText(arg.section(QLatin1Char('='), 1));
            }
        } else if (verb == QLatin1String("click")) {
            if (auto *b = window.findChild<QPushButton *>(arg)) {
                b->click();
            } else {
                qWarning("scripted helper: no button '%s'", qPrintable(arg));
            }
        } else if (verb == QLatin1String("tab")) {
            if (auto *tw = window.findChild<QTabWidget *>(QStringLiteral("authModes"))) {
                tw->setCurrentIndex(arg.toInt());
            }
        } else if (verb == QLatin1String("select")) {
            if (auto *list = window.findChild<QListWidget *>(QStringLiteral("accounts"))) {
                list->setCurrentRow(arg.toInt());
            }
        } else if (verb == QLatin1String("cancel")) {
            window.close();
        } else if (verb == QLatin1String("wait")) {
            return;
        }
        if (!window.isVisible()) {
            return;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args.append(QString::fromLocal8Bit(argv[i]));
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    Trace2::initialize(args);
    int exitCode = 0;
    {
        int qtArgc = 1;
        QApplication app(qtArgc, argv);
        DialogWindow::setShowHook(runScript);
        CommandContext context;
        HelperApplication application(context);
        exitCode = application.run(args);
    }
    Trace2::stop(exitCode);
    return exitCode & 0xFF;
}
