// Tests of the graphical helper (package ui): every command of git-credential-manager-ui is run through
// HelperApplication with a scripted user (DialogWindow::setShowHook) on the offscreen platform, and the
// protocol output is checked. Own tests: upstream has no tests for its Avalonia views.
//
// With GCM_UI_SCREENSHOT_DIR=<dir> every dialog is also saved as <dir>/<name>-<light|dark>.png.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "helperapp.h"
#include "views.h"
#include "widgets.h"

#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>

using namespace gcm;
using namespace gcm::test;
using namespace gcm::ui;

namespace {

const TestApplicationFactory appFactory([](int &argc, char **argv) -> std::unique_ptr<QCoreApplication> {
    auto app = std::make_unique<QApplication>(argc, argv);
    applyTheme(*app, false);
    return app;
});

struct RunResult
{
    int exitCode = 0;
    QString out;
    QString windowTitle;
};

/// Runs the helper with `args`; `script` drives the dialog (it must accept or cancel it).
RunResult runHelper(const QStringList &args, const std::function<void(DialogWindow &)> &script,
                    TestCommandContext *ctx = nullptr, const QString &stdinText = QString())
{
    TestCommandContext local;
    TestCommandContext &context = ctx ? *ctx : local;
    context.Streams.In = stdinText;
    RunResult result;
    DialogWindow::setShowHook([&](DialogWindow &window) {
        result.windowTitle = window.windowTitle();
        script(window);
        // A failed check returns from the script early: never leave the dialog open
        if (window.isVisible()) {
            if (!QTest::currentTestFailed()) {
                qWarning("dialog script did not close the window");
            }
            window.reject();
        }
    });
    HelperApplication app(context);
    result.exitCode = app.run(args);
    result.out = context.Streams.outText();
    DialogWindow::setShowHook(nullptr);
    return result;
}

template <typename T> T *child(QWidget &root, const char *name)
{
    T *w = root.findChild<T *>(QString::fromLatin1(name));
    if (!w) {
        qWarning("child '%s' not found", name);
    }
    return w;
}

QPushButton *button(QWidget &root, const char *name) { return child<QPushButton>(root, name); }
QLineEdit *edit(QWidget &root, const char *name) { return child<QLineEdit>(root, name); }

void type(QWidget &root, const char *name, const QString &text)
{
    QLineEdit *e = edit(root, name);
    QVERIFY(e);
    e->setFocus();
    QTest::keyClicks(e, text);
}

QString Q(const char *s) { return QString::fromUtf8(s); }

/// Visible (not hidden) tab titles.
QStringList visibleTabs(QWidget &root)
{
    QStringList titles;
    auto *tabs = child<QTabWidget>(root, "authModes");
    if (!tabs) {
        return titles;
    }
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->isTabVisible(i)) {
            titles.append(tabs->tabText(i));
        }
    }
    return titles;
}

QPushButton *defaultButton(QWidget &root)
{
    const QList<QPushButton *> buttons = root.findChildren<QPushButton *>();
    for (QPushButton *b : buttons) {
        if (b->isDefault()) {
            return b;
        }
    }
    return nullptr;
}

} // namespace

// =============================================================================================
// Protocol: the generic commands
// =============================================================================================

class HelperCoreCommandsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void basic_ReturnsUserNameAndPassword()
    {
        RunResult r = runHelper({Q("basic"), Q("--resource"), Q("https://example.com")}, [](DialogWindow &w) {
            QCOMPARE(child<QLabel>(w, "description")->text(), Q("Enter your credentials for 'https://example.com'"));
            QVERIFY(w.findChild<QWidget *>(Q("productHeader")));
            type(w, "userName", Q("john"));
            type(w, "password", Q("letmein123"));
            button(w, "signIn")->click();
        });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("username=john\npassword=letmein123\n\n"));
        QCOMPARE(r.windowTitle, Q("Git Credential Manager"));
    }

    void basic_EmptyCredentialsAllowed_TitleAndUserNameOptions()
    {
        RunResult r = runHelper({Q("basic"), Q("--title"), Q("My title"), Q("--username"), Q("alice"), Q("--no-logo")},
                                [](DialogWindow &w) {
                                    QCOMPARE(child<QLabel>(w, "description")->text(), Q("Enter your credentials"));
                                    QVERIFY(!w.findChild<QWidget *>(Q("productHeader")));
                                    QCOMPARE(edit(w, "userName")->text(), Q("alice"));
                                    QApplication::processEvents();
                                    QCOMPARE(QApplication::focusWidget(), edit(w, "password"));
                                    // Enter presses the default button
                                    QTest::keyClick(edit(w, "password"), Qt::Key_Return);
                                });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("username=alice\npassword=\n\n"));
        QCOMPARE(r.windowTitle, Q("My title"));
    }

    void basic_Cancelled_WritesErrorAndCancelled()
    {
        RunResult r = runHelper({Q("basic")}, [](DialogWindow &w) { w.reject(); });
        QCOMPARE(r.exitCode, -1);
        QCOMPARE(r.out, Q("error=User cancelled dialog.\ncancelled=1\n\n"));
    }

    void basic_EscapeCancels()
    {
        RunResult r = runHelper({Q("basic")}, [](DialogWindow &w) { QTest::keyClick(&w, Qt::Key_Escape); });
        QCOMPARE(r.exitCode, -1);
        QVERIFY(r.out.contains(Q("cancelled=1")));
    }

    void oauth_Browser()
    {
        RunResult r = runHelper({Q("oauth"), Q("--resource"), Q("https://example.com"), Q("--browser"), Q("--device-code")},
                                [](DialogWindow &w) {
                                    QCOMPARE(child<QLabel>(w, "description")->text(), Q("Sign in to 'https://example.com'"));
                                    QVERIFY(button(w, "signInBrowser")->isVisible());
                                    QVERIFY(button(w, "signInDeviceCode")->isVisible());
                                    QCOMPARE(defaultButton(w), button(w, "signInBrowser"));
                                    button(w, "signInBrowser")->click();
                                });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("mode=browser\n\n"));
    }

    void oauth_DeviceCodeOnly()
    {
        RunResult r = runHelper({Q("oauth"), Q("--device-code")}, [](DialogWindow &w) {
            QCOMPARE(child<QLabel>(w, "description")->text(), Q("Select a sign-in option"));
            QVERIFY(!button(w, "signInBrowser")->isVisible());
            QCOMPARE(defaultButton(w), button(w, "signInDeviceCode"));
            button(w, "signInDeviceCode")->click();
        });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("mode=devicecode\n\n"));
    }

    void device_ShowsCodeAndOpensUrl()
    {
        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        RunResult r = runHelper({Q("device"), Q("--code"), Q("ABCD-1234"), Q("--url"), Q("https://example.com/device")},
                                [](DialogWindow &w) {
                                    QCOMPARE(child<QLabel>(w, "userCode")->text(), Q("ABCD-1234"));
                                    button(w, "verificationUrl")->click();
                                    // The dialog stays open until the caller kills the helper
                                    QVERIFY(w.isVisible());
                                    w.reject();
                                },
                                &context);
        QCOMPARE(opened, QList<QUrl>{QUrl(Q("https://example.com/device"))});
        QCOMPARE(r.windowTitle, Q("Device code authentication"));
        QCOMPARE(r.exitCode, -1);
        QCOMPARE(r.out, Q("error=User cancelled dialog.\ncancelled=1\n\n"));
    }

    void defaultAccount_ContinueAndOther()
    {
        RunResult r = runHelper({Q("default-account"), Q("--username"), Q("john@contoso.com")}, [](DialogWindow &w) {
            QCOMPARE(child<QLabel>(w, "userName")->text(), Q("john@contoso.com"));
            QCOMPARE(defaultButton(w), button(w, "continue"));
            button(w, "continue")->click();
        });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("use_default_account=1\n\n"));
        QCOMPARE(r.windowTitle, Q("Git Credential Manager"));

        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        r = runHelper({Q("default-account"), Q("--username"), Q("john"), Q("--title"), Q("T")}, [](DialogWindow &w) {
            button(w, "learnMore")->click();
            button(w, "otherAccount")->click();
        }, &context);
        QCOMPARE(r.out, Q("use_default_account=0\n\n"));
        QCOMPARE(r.windowTitle, Q("T"));
        QCOMPARE(opened, QList<QUrl>{QUrl(Q("https://aka.ms/gcm/defaultaccount"))});
    }

    void defaultAccount_UserNameRequired()
    {
        TestCommandContext context;
        HelperApplication app(context);
        QCOMPARE(app.run({Q("default-account")}), 1);
        QVERIFY(context.Streams.errorText().contains(Q("Option '--username' is required.")));
    }

    void unknownCommand_IsParseError()
    {
        TestCommandContext context;
        HelperApplication app(context);
        QCOMPARE(app.run({Q("nope")}), 1);
    }
};

// =============================================================================================
// Protocol: the provider commands
// =============================================================================================

class HelperProviderCommandsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void github_Prompt_AllModes_BrowserFirst()
    {
        RunResult r = runHelper({Q("github"), Q("prompt"), Q("--all")}, [](DialogWindow &w) {
            QCOMPARE(visibleTabs(w), (QStringList{Q("Browser/Device"), Q("Token"), Q("Password")}));
            QApplication::processEvents();
            QCOMPARE(child<QTabWidget>(w, "authModes")->currentIndex(), 0);
            QCOMPARE(defaultButton(w), button(w, "signInBrowser"));
            QVERIFY(!w.findChild<QLabel *>(Q("enterpriseUrl")));
            button(w, "signInDevice")->click();
        });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("mode=device\n\n"));
        QCOMPARE(r.windowTitle, Q("Connect to GitHub"));
    }

    void github_Prompt_Token()
    {
        RunResult r = runHelper({Q("github"), Q("prompt"), Q("--pat"), Q("--basic"), Q("--enterprise-url"), Q("https://ghe.example.com")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), (QStringList{Q("Token"), Q("Password")}));
                                    QCOMPARE(child<QLabel>(w, "enterpriseUrl")->text(), Q("https://ghe.example.com"));
                                    QApplication::processEvents();
                                    QCOMPARE(child<QTabWidget>(w, "authModes")->currentIndex(), 1);
                                    QCOMPARE(QApplication::focusWidget(), edit(w, "token"));
                                    QVERIFY(!button(w, "signInToken")->isEnabled());
                                    type(w, "token", Q("   "));
                                    QVERIFY(!button(w, "signInToken")->isEnabled());
                                    edit(w, "token")->clear();
                                    type(w, "token", Q("ghp_abc"));
                                    QVERIFY(button(w, "signInToken")->isEnabled());
                                    QCOMPARE(defaultButton(w), button(w, "signInToken"));
                                    QTest::keyClick(edit(w, "token"), Qt::Key_Return);
                                });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("mode=pat\npat=ghp_abc\n\n"));
    }

    void github_Prompt_Basic()
    {
        RunResult r = runHelper({Q("github"), Q("prompt"), Q("--basic"), Q("--username"), Q("octocat"),
                                 Q("--enterprise-url"), Q("https://GitHub.com")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), QStringList{Q("Password")});
                                    QVERIFY(!w.findChild<QLabel *>(Q("enterpriseUrl")));
                                    QApplication::processEvents();
                                    QCOMPARE(QApplication::focusWidget(), edit(w, "password"));
                                    QVERIFY(!button(w, "signInBasic")->isEnabled());
                                    type(w, "password", Q("pw"));
                                    QVERIFY(button(w, "signInBasic")->isEnabled());
                                    button(w, "signInBasic")->click();
                                });
        QCOMPARE(r.out, Q("mode=basic\nusername=octocat\npassword=pw\n\n"));
    }

    void github_Prompt_SignUpLink()
    {
        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        runHelper({Q("github"), Q("prompt"), Q("--browser")}, [](DialogWindow &w) {
            QCOMPARE(visibleTabs(w), QStringList{Q("Browser")});
            button(w, "signUp")->click();
            button(w, "signInBrowser")->click();
        }, &context);
        QCOMPARE(opened, QList<QUrl>{QUrl(Q("https://github.com/pricing"))});
    }

    void github_TwoFactor()
    {
        RunResult r = runHelper({Q("github"), Q("2fa"), Q("--sms")}, [](DialogWindow &w) {
            QCOMPARE(child<QLabel>(w, "description")->text(), Q("We sent you a message via SMS with your authentication code."));
            QApplication::processEvents();
            QCOMPARE(QApplication::focusWidget(), edit(w, "digit1"));
            QVERIFY(!button(w, "verify")->isEnabled());
            // Letters are ignored, digits advance to the next box (keys go to whichever box has the focus)
            const QString keys = Q("1a2345");
            for (const QChar c : keys) {
                QTest::keyClick(QApplication::focusWidget(), c.toLatin1());
            }
            QCOMPARE(QApplication::focusWidget(), edit(w, "digit6"));
            QVERIFY(!button(w, "verify")->isEnabled());
            // Backspace moves back and clears the box it was pressed in
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Backspace);
            QCOMPARE(QApplication::focusWidget(), edit(w, "digit5"));
            QCOMPARE(edit(w, "digit5")->text(), Q("5"));
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right);
            QTest::keyClicks(QApplication::focusWidget(), Q("6"));
            QVERIFY(button(w, "verify")->isEnabled());
            button(w, "verify")->click();
        });
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("code=123456\n\n"));
        QCOMPARE(r.windowTitle, Q("Two-factor authentication required"));
    }

    void github_TwoFactor_Paste()
    {
        QGuiApplication::clipboard()->setText(Q("Code: 987 654 321"));
        RunResult r = runHelper({Q("github"), Q("2fa")}, [](DialogWindow &w) {
            QCOMPARE(child<QLabel>(w, "description")->text(),
                     Q("Open the two-factor authentication app on your device to view your authentication code."));
            QApplication::processEvents();
            QTest::keySequence(QApplication::focusWidget(), QKeySequence::Paste);
            QCOMPARE(edit(w, "digit6")->text(), Q("4"));
            button(w, "verify")->click();
        });
        QCOMPARE(r.out, Q("code=987654\n\n"));
    }

    void github_Device()
    {
        RunResult r = runHelper({Q("github"), Q("device"), Q("--code"), Q("1234-5678"), Q("--url"), Q("https://github.com/login/device")},
                                [](DialogWindow &w) {
                                    QCOMPARE(child<QLabel>(w, "userCode")->text(), Q("1234-5678"));
                                    QCOMPARE(button(w, "verificationUrl")->text(), Q("https://github.com/login/device"));
                                    w.reject();
                                });
        QCOMPARE(r.exitCode, -1);
        QCOMPARE(r.windowTitle, Q("Device code authentication"));
    }

    void github_SelectAccount()
    {
        TestCommandContext context;
        RunResult r = runHelper({Q("github"), Q("select-account"), Q("--enterprise-url"), Q("https://ghe.example.com")},
                                [](DialogWindow &w) {
                                    auto *list = child<QListWidget>(w, "accounts");
                                    QCOMPARE(list->count(), 2);
                                    QCOMPARE(list->item(0)->text(), Q("alice"));
                                    QVERIFY(!button(w, "continue")->isEnabled());
                                    QVERIFY(w.findChild<QWidget *>(Q("helpRow")));
                                    list->setCurrentRow(1);
                                    QVERIFY(button(w, "continue")->isEnabled());
                                    button(w, "continue")->click();
                                },
                                &context, Q("  alice  \nbob\n\nignored\n"));
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, Q("account=bob\n\n"));
        QCOMPARE(r.windowTitle, Q("Select an account"));
    }

    void github_SelectAccount_NewAccount()
    {
        TestCommandContext context;
        RunResult r = runHelper({Q("github"), Q("select-account"), Q("--no-help")},
                                [](DialogWindow &w) {
                                    QVERIFY(!w.findChild<QWidget *>(Q("helpRow")));
                                    button(w, "newAccount")->click();
                                },
                                &context, Q("alice\n"));
        QCOMPARE(r.out, Q("account=\n\n"));
    }

    void gitlab_Prompt_DotCom()
    {
        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        RunResult r = runHelper({Q("gitlab"), Q("prompt"), Q("--url"), Q("https://gitlab.com"), Q("--all"), Q("--username"), Q("jo")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), (QStringList{Q("Browser"), Q("Token"), Q("Password")}));
                                    QVERIFY(w.findChild<QWidget *>(Q("gitLabLogo")));
                                    QVERIFY(!w.findChild<QLabel *>(Q("url")));
                                    QCOMPARE(edit(w, "tokenUserName")->text(), Q("jo"));
                                    button(w, "signUp")->click();
                                    child<QTabWidget>(w, "authModes")->setCurrentIndex(1);
                                    QCOMPARE(defaultButton(w), button(w, "signInToken"));
                                    type(w, "token", Q("glpat-1"));
                                    button(w, "signInToken")->click();
                                },
                                &context);
        QCOMPARE(r.out, Q("mode=pat\npat=glpat-1\nusername=jo\n\n"));
        QCOMPARE(r.windowTitle, Q("Connect to GitLab"));
        QCOMPARE(opened, QList<QUrl>{QUrl(Q("https://about.gitlab.com/"))});
    }

    void gitlab_Prompt_SelfHosted()
    {
        RunResult r = runHelper({Q("gitlab"), Q("prompt"), Q("--url"), Q("https://gitlab.example.com"), Q("--browser"), Q("--basic")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), (QStringList{Q("Browser"), Q("Password")}));
                                    QVERIFY(!w.findChild<QWidget *>(Q("gitLabLogo")));
                                    QVERIFY(!w.findChild<QWidget *>(Q("signUpRow")));
                                    QCOMPARE(child<QLabel>(w, "url")->text(), Q("https://gitlab.example.com"));
                                    button(w, "signInBrowser")->click();
                                });
        QCOMPARE(r.out, Q("mode=browser\n\n"));
    }

    void bitbucket_Prompt_Cloud()
    {
        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        RunResult r = runHelper({Q("bitbucket"), Q("prompt"), Q("--show-oauth"), Q("--show-basic")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), (QStringList{Q("Browser"), Q("Password/Token")}));
                                    button(w, "forgotPassword")->click();
                                    button(w, "signUp")->click();
                                    child<QTabWidget>(w, "authModes")->setCurrentIndex(1);
                                    type(w, "userName", Q("u"));
                                    QVERIFY(!button(w, "signInBasic")->isEnabled());
                                    type(w, "password", Q("p"));
                                    button(w, "signInBasic")->click();
                                },
                                &context);
        QCOMPARE(r.out, Q("mode=basic\nusername=u\npassword=p\n\n"));
        QCOMPARE(r.windowTitle, Q("Connect to Bitbucket"));
        QCOMPARE(opened, (QList<QUrl>{QUrl(Q("https://bitbucket.org/account/password/reset/")),
                                      QUrl(Q("https://bitbucket.org/account/signup/"))}));
    }

    void bitbucket_Prompt_DataCenter()
    {
        TestCommandContext context;
        QList<QUrl> opened;
        context.SessionManager.OpenBrowserFunc = [&](const QUrl &u) { opened.append(u); };
        RunResult r = runHelper({Q("bitbucket"), Q("prompt"), Q("--url"), Q("https://bb.example.com/scm"), Q("--show-oauth")},
                                [](DialogWindow &w) {
                                    QCOMPARE(visibleTabs(w), QStringList{Q("Browser")});
                                    QCOMPARE(child<QLabel>(w, "url")->text(), Q("https://bb.example.com/scm"));
                                    button(w, "forgotPassword")->click();
                                    button(w, "signUp")->click();
                                    button(w, "signInBrowser")->click();
                                },
                                &context);
        QCOMPARE(r.out, Q("mode=oauth\n\n"));
        QCOMPARE(opened, (QList<QUrl>{QUrl(Q("https://bb.example.com/passwordreset")), QUrl(Q("https://bb.example.com/login"))}));
    }

    void isDotCom()
    {
        QVERIFY(HelperApplication::isGitHubDotCom(Q("https://github.com")));
        QVERIFY(HelperApplication::isGitHubDotCom(Q("https://GIST.github.com/x")));
        QVERIFY(!HelperApplication::isGitHubDotCom(Q("https://ghe.example.com")));
        QVERIFY(!HelperApplication::isGitHubDotCom(std::nullopt));
        QVERIFY(!HelperApplication::isGitHubDotCom(Q("github.com")));
        QVERIFY(HelperApplication::isGitLabDotCom(QUrl(Q("https://GitLab.com/"))));
        QVERIFY(!HelperApplication::isGitLabDotCom(QUrl(Q("https://gitlab.example.com/"))));
    }
};

// =============================================================================================
// Window layout and screenshots
// =============================================================================================

class DialogWindowTests : public QObject
{
    Q_OBJECT

private:
    struct Case
    {
        const char *name;
        QStringList args;
        QString stdinText;
    };

    static QList<Case> cases()
    {
        return {
            {"basic", {Q("basic"), Q("--resource"), Q("https://example.com"), Q("--username"), Q("john")}, {}},
            {"oauth", {Q("oauth"), Q("--resource"), Q("https://example.com"), Q("--browser"), Q("--device-code")}, {}},
            {"device", {Q("device"), Q("--code"), Q("ABCD-1234"), Q("--url"), Q("https://example.com/device")}, {}},
            {"default-account", {Q("default-account"), Q("--username"), Q("john@contoso.com")}, {}},
            {"github-prompt", {Q("github"), Q("prompt"), Q("--all")}, {}},
            {"github-prompt-enterprise-basic", {Q("github"), Q("prompt"), Q("--basic"), Q("--enterprise-url"), Q("https://ghe.example.com")}, {}},
            {"github-2fa", {Q("github"), Q("2fa")}, {}},
            {"github-device", {Q("github"), Q("device"), Q("--code"), Q("1234-5678"), Q("--url"), Q("https://github.com/login/device")}, {}},
            {"github-select-account", {Q("github"), Q("select-account")}, Q("alice\nbob\ncarol\n")},
            {"gitlab-prompt", {Q("gitlab"), Q("prompt"), Q("--all")}, {}},
            {"gitlab-prompt-selfhosted", {Q("gitlab"), Q("prompt"), Q("--url"), Q("https://gitlab.example.com"), Q("--pat")}, {}},
            {"bitbucket-prompt", {Q("bitbucket"), Q("prompt"), Q("--show-oauth"), Q("--show-basic")}, {}},
            {"bitbucket-prompt-datacenter", {Q("bitbucket"), Q("prompt"), Q("--url"), Q("https://bb.example.com"), Q("--show-basic")}, {}},
        };
    }

private Q_SLOTS:
    void cleanup()
    {
        applyTheme(*qApp, false);
    }

    void everyDialog_FixedWidthAndHeightWithinLimits_Screenshots()
    {
        const QString dir = qEnvironmentVariable("GCM_UI_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
        }
        for (const bool dark : {false, true}) {
            applyTheme(*qApp, dark);
            QCOMPARE(isDarkTheme(), dark);
            const QList<Case> allCases = cases();
            for (const Case &c : allCases) {
                TestCommandContext context;
                QSize size;
                bool contentFits = false;
                RunResult r = runHelper(c.args, [&](DialogWindow &w) {
                    QApplication::processEvents();
                    size = w.size();
                    contentFits = w.view()->minimumSizeHint().height() <= w.view()->height() + 1;
                    if (!dir.isEmpty()) {
                        w.grab().save(QStringLiteral("%1/%2-%3.png").arg(dir, QString::fromLatin1(c.name), dark ? Q("dark") : Q("light")));
                    }
                    w.reject();
                }, &context, c.stdinText);
                QVERIFY2(r.exitCode == -1, c.name);
                QVERIFY2(size.width() == 420, c.name);
                QVERIFY2(size.height() >= 280 && size.height() <= 520, qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(c.name)).arg(size.height())));
                QVERIFY2(contentFits, c.name);
            }
        }
    }

    void themedPixmap_IconFollowsTheme()
    {
        applyTheme(*qApp, false);
        const QImage light = themedPixmap(Q("person.svg"), 32, 32).toImage();
        applyTheme(*qApp, true);
        const QImage dark = themedPixmap(Q("person.svg"), 32, 32).toImage();
        QVERIFY(!light.isNull());
        QCOMPARE(light.size(), dark.size());
        // The darkest opaque pixel is dark in light mode and white in dark mode
        auto maxLightness = [](const QImage &img) {
            int best = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() == 255) {
                        best = std::max(best, c.lightness());
                    }
                }
            }
            return best;
        };
        QVERIFY(maxLightness(light) < 80);
        QVERIFY(maxLightness(dark) > 200);
        // Raster images keep their aspect ratio
        const QPixmap logo = themedPixmap(Q("atlassian-logo.png"), 90);
        QCOMPARE(logo.deviceIndependentSize().toSize(), QSize(323, 90));
    }

    void sixDigitInput_SetText()
    {
        SixDigitInput input;
        QSignalSpy spy(&input, &SixDigitInput::textChanged);
        input.setText(Q("12a3"));
        QCOMPARE(input.text(), Q("123"));
        QCOMPARE(input.box(3)->text(), QString());
        input.setText(Q("12345678"));
        QCOMPARE(input.text(), Q("123456"));
        input.setText(Q("  "));
        QCOMPARE(input.text(), QString());
        QCOMPARE(spy.count(), 3);
    }

    void bitbucketLinks()
    {
        QCOMPARE(BitbucketCredentialsView::passwordResetUrl(QUrl()), Q("https://bitbucket.org/account/password/reset/"));
        QCOMPARE(BitbucketCredentialsView::signUpUrl(QUrl()), Q("https://bitbucket.org/account/signup/"));
        QCOMPARE(BitbucketCredentialsView::passwordResetUrl(QUrl(Q("https://bb.example.com/a/b"))), Q("https://bb.example.com/passwordreset"));
        QCOMPARE(BitbucketCredentialsView::signUpUrl(QUrl(Q("https://bb.example.com:8443"))), Q("https://bb.example.com:8443/login"));
    }

    void gitHubOAuthModeTitle()
    {
        QCOMPARE(GitHubCredentialsView::oauthModeTitle(true, true), Q("Browser/Device"));
        QCOMPARE(GitHubCredentialsView::oauthModeTitle(true, false), Q("Browser"));
        QCOMPARE(GitHubCredentialsView::oauthModeTitle(false, true), Q("Device"));
        QCOMPARE(GitHubCredentialsView::oauthModeTitle(false, false), Q("OAuth"));
    }
};

GCM_REGISTER_TEST(HelperCoreCommandsTests)
GCM_REGISTER_TEST(HelperProviderCommandsTests)
GCM_REGISTER_TEST(DialogWindowTests)
#include "uitests.moc"
