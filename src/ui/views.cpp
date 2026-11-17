#include "views.h"

#include "core/constants.h"
#include "core/platform.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QBoxLayout>
#include <QFontDatabase>
#include <QListWidget>
#include <QTabBar>
#include <QTabWidget>

namespace gcm::ui {

namespace {

QVBoxLayout *newColumn(QWidget *owner, const QMargins &margins = {})
{
    auto *layout = new QVBoxLayout(owner);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    return layout;
}

/// A child StackPanel with margins, appended to `parent`.
QVBoxLayout *addPanel(QBoxLayout *parent, const QMargins &margins)
{
    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    parent->addLayout(layout);
    return layout;
}

void addFullWidth(QBoxLayout *layout, QWidget *widget, int bottomMargin)
{
    layout->addWidget(widget);
    if (bottomMargin > 0) {
        layout->addSpacing(bottomMargin);
    }
}

/// Header of the generic views: optional product header, then the description (FontSize 14, margin 0,15).
void addProductHeaderAndDescription(QBoxLayout *root, bool showProductHeader, const QString &description)
{
    QVBoxLayout *top = addPanel(root, QMargins(10, 0, 10, 10));
    if (showProductHeader) {
        top->addWidget(makeProductHeader());
    }
    top->addSpacing(15);
    QLabel *label = makeTextBlock(description, 14);
    label->setObjectName(QStringLiteral("description"));
    top->addWidget(label);
    top->addSpacing(15);
}

/// "GitHub" (20, bold) + subtitle (24, light) + divider, as the GitHub views draw it.
void addGitHubHeader(QBoxLayout *root, const QString &subtitle, const OptString &enterpriseUrl)
{
    root->addWidget(makeTextBlock(QStringLiteral("GitHub"), 20, QFont::Bold));
    root->addWidget(makeTextBlock(subtitle, 24, QFont::Light));
    root->addSpacing(10);
    root->addWidget(new ShadowDivider);
    if (enterpriseUrl && !enterpriseUrl->isEmpty()) {
        root->addSpacing(10);
        root->addWidget(makeTextBlock(QStringLiteral("GitHub Enterprise")));
        QLabel *url = makeTextBlock(*enterpriseUrl);
        url->setObjectName(QStringLiteral("enterpriseUrl"));
        root->addWidget(url);
    }
    root->addSpacing(15);
}

QTabWidget *makeTabs(QWidget *parent)
{
    auto *tabs = new QTabWidget(parent);
    tabs->setObjectName(QStringLiteral("authModes"));
    tabs->setDocumentMode(true);
    // The tab headers are centred (Style "DockPanel > ItemsPresenter > WrapPanel")
    tabs->setStyleSheet(QStringLiteral("QTabWidget::tab-bar { alignment: center; }"));
    tabs->tabBar()->setFont(fontWithPixelSize(tabs->font(), 12));
    tabs->tabBar()->setExpanding(false);
    QObject::connect(tabs, &QTabWidget::currentChanged, tabs, [tabs] {
        if (tabs->window()->isVisible()) {
            updateDefaultButton(tabs->window());
        }
    });
    return tabs;
}

/// "Don't have an account?" + "Sign up" link, centred.
QWidget *makeSignUpRow(const std::function<void()> &onSignUp)
{
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("signUpRow"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addStretch();
    layout->addWidget(new QLabel(QStringLiteral("Don't have an account?")));
    auto *link = new LinkButton(QStringLiteral("Sign up"));
    link->setObjectName(QStringLiteral("signUp"));
    QObject::connect(link, &QPushButton::clicked, link, onSignUp);
    layout->addWidget(link);
    layout->addStretch();
    return row;
}

/// Icon + link, centred (the help rows of DefaultAccountView and SelectAccountView).
QWidget *makeHelpRow(const QString &text, const std::function<void()> &onClick)
{
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("helpRow"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addStretch();
    layout->addWidget(new ImageLabel(QStringLiteral("help.svg"), 16, 16));
    auto *link = new LinkButton(text);
    link->setObjectName(QStringLiteral("learnMore"));
    QObject::connect(link, &QPushButton::clicked, link, onClick);
    layout->addWidget(link);
    layout->addStretch();
    return row;
}

QPushButton *withPadding(QPushButton *button, int horizontal, int vertical)
{
    const QSize hint = button->sizeHint();
    const QSize text = button->fontMetrics().size(Qt::TextShowMnemonic, button->text());
    button->setMinimumSize(std::max(hint.width(), text.width() + 2 * horizontal), std::max(hint.height(), text.height() + 2 * vertical));
    return button;
}

void enableWhen(QPushButton *button, const QList<QLineEdit *> &edits, const std::function<bool()> &predicate)
{
    auto update = [button, predicate] { button->setEnabled(predicate()); };
    for (QLineEdit *edit : edits) {
        QObject::connect(edit, &QLineEdit::textChanged, button, update);
    }
    update();
}

void focusEdit(QLineEdit *edit)
{
    edit->setFocus(Qt::OtherFocusReason);
}

} // namespace

void openLink(ISessionManager &sessionManager, const QString &url)
{
    try {
        sessionManager.openBrowser(url);
    } catch (const std::exception &ex) {
        Trace2::writeError(ex);
    }
}

// =============================================================================================
// CredentialsView
// =============================================================================================

CredentialsView::CredentialsView(const Options &options, QWidget *parent) : View(parent)
{
    if (options.title && !isNullOrWhiteSpace(*options.title)) {
        m_title = *options.title;
    }
    QVBoxLayout *root = newColumn(this);
    addProductHeaderAndDescription(root, options.showProductHeader, options.description);

    QVBoxLayout *fields = addPanel(root, QMargins(20, 0, 20, 0));
    m_userName = makeTextEdit(QStringLiteral("Username"));
    m_userName->setObjectName(QStringLiteral("userName"));
    if (options.userName && !isNullOrWhiteSpace(*options.userName)) {
        m_userName->setText(*options.userName);
    }
    addFullWidth(fields, m_userName, 10);
    m_password = makeSecretEdit(QStringLiteral("Password"));
    m_password->setObjectName(QStringLiteral("password"));
    addFullWidth(fields, m_password, 20);
    // Empty username and/or password are allowed, as CredUIPromptForWindowsCredentials did
    QPushButton *signIn = makeAccentButton(QStringLiteral("Continue"));
    signIn->setObjectName(QStringLiteral("signIn"));
    connect(signIn, &QPushButton::clicked, this, &View::accepted);
    addCentered(fields, signIn);
    root->addStretch();
}

QString CredentialsView::userName() const { return m_userName->text(); }
QString CredentialsView::password() const { return m_password->text(); }

void CredentialsView::setInitialFocus()
{
    focusEdit(isNullOrWhiteSpace(m_userName->text()) ? m_userName : m_password);
}

// =============================================================================================
// OAuthView
// =============================================================================================

OAuthView::OAuthView(const Options &options, QWidget *parent) : View(parent)
{
    if (options.title && !isNullOrWhiteSpace(*options.title)) {
        m_title = *options.title;
    }
    QVBoxLayout *root = newColumn(this);
    addProductHeaderAndDescription(root, options.showProductHeader, options.description);

    QVBoxLayout *buttons = addPanel(root, QMargins(20, 0, 20, 0));
    QPushButton *browser = makeAccentButton(QStringLiteral("Sign in with your browser"));
    browser->setObjectName(QStringLiteral("signInBrowser"));
    browser->setVisible(options.showBrowserLogin);
    connect(browser, &QPushButton::clicked, this, [this] {
        m_mode = OAuthMode::Browser;
        Q_EMIT accepted();
    });
    addCentered(buttons, browser);
    if (options.showBrowserLogin) {
        buttons->addSpacing(10);
    }
    auto *device = new QPushButton(QStringLiteral("Sign in with a code"));
    device->setObjectName(QStringLiteral("signInDeviceCode"));
    device->setProperty("gcmDefault", true);
    device->setVisible(options.showDeviceCodeLogin);
    connect(device, &QPushButton::clicked, this, [this] {
        m_mode = OAuthMode::DeviceCode;
        Q_EMIT accepted();
    });
    addCentered(buttons, device);
    root->addStretch();
}

// =============================================================================================
// DeviceCodeView
// =============================================================================================

DeviceCodeView::DeviceCodeView(ISessionManager &sessionManager, const Options &options, QWidget *parent) : View(parent)
{
    m_title = QStringLiteral("Device code authentication");
    QVBoxLayout *root = newColumn(this);
    if (options.gitHubHeader) {
        addGitHubHeader(root, QStringLiteral("Device authentication"), std::nullopt);
    } else {
        if (options.showProductHeader) {
            root->addWidget(makeProductHeader());
        }
        root->addSpacing(15);
    }

    root->addStretch();
    root->addWidget(makeTextBlock(QStringLiteral("Visit the URL below, sign in, and enter the following device code to continue.")));
    root->addSpacing(20);
    // A read-only, borderless text box: the code can be selected and copied
    QLabel *code = makeTextBlock(options.userCode, 24);
    code->setObjectName(QStringLiteral("userCode"));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(24);
    code->setFont(mono);
    code->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    root->addWidget(code);
    root->addSpacing(20);
    auto *link = new LinkButton(options.verificationUrl);
    link->setObjectName(QStringLiteral("verificationUrl"));
    const QString url = options.verificationUrl;
    connect(link, &QPushButton::clicked, this, [&sessionManager, url] { openLink(sessionManager, url); });
    addCentered(root, link);
    root->addStretch();
}

// =============================================================================================
// DefaultAccountView
// =============================================================================================

DefaultAccountView::DefaultAccountView(ISessionManager &sessionManager, const Options &options, QWidget *parent) : View(parent)
{
    m_title = options.title && !isNullOrWhiteSpace(*options.title) ? *options.title : Constants::DefaultWindowTitle;
    QVBoxLayout *root = newColumn(this);
    addProductHeaderAndDescription(root, options.showProductHeader, QStringLiteral("Do you want to continue with the current account?"));

    QVBoxLayout *body = addPanel(root, QMargins(20, 0, 20, 0));
    auto *account = new QWidget;
    auto *accountLayout = new QHBoxLayout(account);
    accountLayout->setContentsMargins(0, 0, 0, 0);
    accountLayout->setSpacing(10);
    accountLayout->addStretch();
    accountLayout->addWidget(new ImageLabel(QStringLiteral("person.svg"), 32, 32));
    auto *userName = new QLabel(options.userName);
    userName->setObjectName(QStringLiteral("userName"));
    userName->setTextFormat(Qt::PlainText);
    accountLayout->addWidget(userName);
    accountLayout->addStretch();
    body->addWidget(account);
    body->addSpacing(20);

    QPushButton *cont = withPadding(makeAccentButton(QStringLiteral("Continue")), 16, 8);
    cont->setObjectName(QStringLiteral("continue"));
    connect(cont, &QPushButton::clicked, this, [this] {
        m_useDefault = true;
        Q_EMIT accepted();
    });
    addCentered(body, cont);
    body->addSpacing(10);
    QPushButton *other = withPadding(new QPushButton(QStringLiteral("Use another account")), 8, 5);
    other->setObjectName(QStringLiteral("otherAccount"));
    other->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(other, &QPushButton::clicked, this, [this] {
        m_useDefault = false;
        Q_EMIT accepted();
    });
    addCentered(body, other);

    root->addStretch();
    root->addSpacing(20);
    root->addWidget(makeHelpRow(QStringLiteral("Learn more about operating system accounts"), [&sessionManager] {
        openLink(sessionManager, Constants::HelpUrls::GcmDefaultAccount);
    }));
}

// =============================================================================================
// GitHubCredentialsView
// =============================================================================================

QString GitHubCredentialsView::oauthModeTitle(bool browser, bool device)
{
    if (browser && device) {
        return QStringLiteral("Browser/Device");
    }
    if (browser) {
        return QStringLiteral("Browser");
    }
    if (device) {
        return QStringLiteral("Device");
    }
    return QStringLiteral("OAuth");
}

GitHubCredentialsView::GitHubCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent)
    : View(parent), m_options(options)
{
    m_title = QStringLiteral("Connect to GitHub");
    QVBoxLayout *root = newColumn(this);
    addGitHubHeader(root, QStringLiteral("Sign in"), options.enterpriseUrl);

    m_tabs = makeTabs(this);
    QBoxLayout *page = nullptr;

    // Browser / device
    addTabPage(m_tabs, oauthModeTitle(options.showBrowserLogin, options.showDeviceLogin), page, 10);
    m_browserButton = makeAccentButton(QStringLiteral("Sign in with your browser"));
    m_browserButton->setObjectName(QStringLiteral("signInBrowser"));
    m_browserButton->setVisible(options.showBrowserLogin);
    connect(m_browserButton, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Browser;
        Q_EMIT accepted();
    });
    addCentered(page, m_browserButton);
    page->addSpacing(options.showBrowserLogin ? 20 : 10);
    m_deviceButton = new QPushButton(QStringLiteral("Sign in with a code"));
    m_deviceButton->setObjectName(QStringLiteral("signInDevice"));
    m_deviceButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_deviceButton->setVisible(options.showDeviceLogin);
    connect(m_deviceButton, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Device;
        Q_EMIT accepted();
    });
    addCentered(page, m_deviceButton);
    page->addStretch();

    // Token
    addTabPage(m_tabs, QStringLiteral("Token"), page, 10);
    m_token = makeSecretEdit(QStringLiteral("Personal access token"));
    m_token->setObjectName(QStringLiteral("token"));
    addFullWidth(page, m_token, 10);
    QPushButton *signInToken = makeAccentButton(QStringLiteral("Sign in"));
    signInToken->setObjectName(QStringLiteral("signInToken"));
    enableWhen(signInToken, {m_token}, [this] { return !isNullOrWhiteSpace(m_token->text()); });
    connect(signInToken, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Pat;
        Q_EMIT accepted();
    });
    addCentered(page, signInToken);
    page->addStretch();

    // Password
    addTabPage(m_tabs, QStringLiteral("Password"), page, 10);
    m_userName = makeTextEdit(QStringLiteral("Username or email"));
    m_userName->setObjectName(QStringLiteral("userName"));
    if (options.userName && !isNullOrWhiteSpace(*options.userName)) {
        m_userName->setText(*options.userName);
    }
    addFullWidth(page, m_userName, 10);
    m_password = makeSecretEdit(QStringLiteral("Password"));
    m_password->setObjectName(QStringLiteral("password"));
    addFullWidth(page, m_password, 10);
    QPushButton *signInBasic = makeAccentButton(QStringLiteral("Sign in"));
    signInBasic->setObjectName(QStringLiteral("signInBasic"));
    enableWhen(signInBasic, {m_userName, m_password}, [this] {
        return !isNullOrWhiteSpace(m_userName->text()) && !m_password->text().isEmpty();
    });
    connect(signInBasic, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Basic;
        Q_EMIT accepted();
    });
    addCentered(page, signInBasic);
    page->addStretch();

    m_tabs->setTabVisible(0, options.showBrowserLogin || options.showDeviceLogin);
    m_tabs->setTabVisible(1, options.showTokenLogin);
    m_tabs->setTabVisible(2, options.showBasicLogin);
    root->addWidget(m_tabs, 1);

    root->addSpacing(20);
    root->addWidget(makeSignUpRow([&sessionManager] { openLink(sessionManager, QStringLiteral("https://github.com/pricing")); }));
}

QString GitHubCredentialsView::userName() const { return m_userName->text(); }
QString GitHubCredentialsView::password() const { return m_password->text(); }
QString GitHubCredentialsView::token() const { return m_token->text(); }

void GitHubCredentialsView::setInitialFocus()
{
    // Select the best available authentication mechanism that is visible and focus on the button/text box
    if (m_options.showBrowserLogin) {
        m_tabs->setCurrentIndex(0);
        m_browserButton->setFocus();
    } else if (m_options.showDeviceLogin) {
        m_tabs->setCurrentIndex(0);
        m_deviceButton->setFocus();
    } else if (m_options.showTokenLogin) {
        m_tabs->setCurrentIndex(1);
        focusEdit(m_token);
    } else if (m_options.showBasicLogin) {
        m_tabs->setCurrentIndex(2);
        focusEdit(isNullOrWhiteSpace(m_userName->text()) ? m_userName : m_password);
    }
    updateDefaultButton(window());
}

// =============================================================================================
// GitHubTwoFactorView
// =============================================================================================

QString GitHubTwoFactorView::description(bool isSms)
{
    return isSms ? QStringLiteral("We sent you a message via SMS with your authentication code.") : QStringLiteral("Open the two-factor authentication app on your device to view your authentication code.");
}

GitHubTwoFactorView::GitHubTwoFactorView(ISessionManager &sessionManager, bool isSms, QWidget *parent) : View(parent)
{
    m_title = QStringLiteral("Two-factor authentication required");
    QVBoxLayout *root = newColumn(this);
    addGitHubHeader(root, QStringLiteral("Two-factor authentication"), std::nullopt);

    root->addStretch();
    QLabel *description = makeTextBlock(GitHubTwoFactorView::description(isSms));
    description->setObjectName(QStringLiteral("description"));
    root->addWidget(description);
    root->addSpacing(20);
    m_code = new SixDigitInput;
    m_code->setObjectName(QStringLiteral("code"));
    addCentered(root, m_code);
    root->addSpacing(20);
    QPushButton *verify = makeAccentButton(QStringLiteral("Verify"));
    verify->setObjectName(QStringLiteral("verify"));
    auto update = [this, verify] {
        const QString c = m_code->text();
        verify->setEnabled(!isNullOrWhiteSpace(c) && c.size() == 6);
    };
    connect(m_code, &SixDigitInput::textChanged, verify, update);
    update();
    connect(verify, &QPushButton::clicked, this, &View::accepted);
    addCentered(root, verify);
    root->addSpacing(20);
    auto *learnMore = new LinkButton(QStringLiteral("Learn more"));
    learnMore->setObjectName(QStringLiteral("learnMore"));
    connect(learnMore, &QPushButton::clicked, this, [&sessionManager] {
        openLink(sessionManager, QStringLiteral("https://aka.ms/vs-core-github-auth-help"));
    });
    addCentered(root, learnMore);
    root->addStretch();
}

QString GitHubTwoFactorView::code() const { return m_code->text(); }

void GitHubTwoFactorView::setInitialFocus()
{
    m_code->setFocusToFirst();
}

// =============================================================================================
// GitHubSelectAccountView
// =============================================================================================

GitHubSelectAccountView::GitHubSelectAccountView(ISessionManager &sessionManager, const Options &options, QWidget *parent)
    : View(parent)
{
    m_title = QStringLiteral("Select an account");
    QVBoxLayout *root = newColumn(this);
    addGitHubHeader(root, QStringLiteral("Select an account"), options.enterpriseUrl);

    if (options.showHelpLink) {
        root->addSpacing(5);
        root->addWidget(makeHelpRow(QStringLiteral("Why am I being asked to select an account?"), [&sessionManager] {
            openLink(sessionManager, Constants::HelpUrls::GcmMultipleUsers);
        }));
        root->addSpacing(10);
    }

    root->addStretch();
    QVBoxLayout *listPanel = addPanel(root, QMargins(20, 0, 20, 10));
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("accounts"));
    m_list->setIconSize(QSize(24, 24));
    m_list->setSpacing(2);
    const QIcon person(themedPixmap(QStringLiteral("person.svg"), 24, 24));
    for (const QString &account : options.accounts) {
        m_list->addItem(new QListWidgetItem(person, account));
    }
    // Sized to the accounts, at most 200 px high
    const int rowHeight = std::max(m_list->sizeHintForRow(0), 28) + 2 * m_list->spacing();
    m_list->setFixedHeight(std::min(200, std::max(1, int(options.accounts.size())) * rowHeight + 2 * m_list->frameWidth()));
    m_list->setFrameShape(QFrame::NoFrame);
    QPalette p = m_list->palette();
    p.setColor(QPalette::Base, Qt::transparent);
    m_list->setPalette(p);
    listPanel->addWidget(m_list);

    QPushButton *cont = withPadding(makeAccentButton(QStringLiteral("Continue")), 20, 10);
    cont->setObjectName(QStringLiteral("continue"));
    auto update = [this, cont] { cont->setEnabled(m_list->currentItem() != nullptr && m_list->currentItem()->isSelected()); };
    connect(m_list, &QListWidget::itemSelectionChanged, cont, update);
    update();
    auto accept = [this] {
        QListWidgetItem *item = m_list->currentItem();
        if (item && item->isSelected()) {
            m_selected = item->text();
            Q_EMIT accepted();
        }
    };
    connect(cont, &QPushButton::clicked, this, accept);
    connect(m_list, &QListWidget::itemDoubleClicked, this, accept);
    addCentered(root, cont);
    root->addSpacing(15);
    auto *newAccount = new LinkButton(QStringLiteral("Add a new account"));
    newAccount->setObjectName(QStringLiteral("newAccount"));
    connect(newAccount, &QPushButton::clicked, this, [this] {
        m_selected.reset();
        Q_EMIT accepted();
    });
    addCentered(root, newAccount);
    root->addStretch();
}

// =============================================================================================
// GitLabCredentialsView
// =============================================================================================

GitLabCredentialsView::GitLabCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent)
    : View(parent), m_options(options)
{
    m_title = QStringLiteral("Connect to GitLab");
    const bool isDotCom = !options.url || options.url->isEmpty();
    QVBoxLayout *root = newColumn(this);

    // Only show the GitLab logo when this is GitLab.com, otherwise just the logo type "GitLab"
    auto *logo = new QWidget;
    auto *logoLayout = new QHBoxLayout(logo);
    logoLayout->setContentsMargins(0, 0, 0, 0);
    logoLayout->setSpacing(8);
    logoLayout->addStretch();
    if (isDotCom) {
        auto *mark = new ImageLabel(QStringLiteral("gitlab-logo.svg"), 46);
        mark->setObjectName(QStringLiteral("gitLabLogo"));
        logoLayout->addWidget(mark);
    }
    logoLayout->addWidget(new ImageLabel(QStringLiteral("gitlab-logotype.svg"), 24));
    logoLayout->addStretch();
    root->addWidget(logo);
    root->addSpacing(10);
    root->addWidget(makeTextBlock(QStringLiteral("Sign in"), 16, QFont::Light));
    root->addSpacing(5);
    if (!isDotCom) {
        root->addSpacing(10);
        QLabel *url = makeTextBlock(*options.url);
        url->setObjectName(QStringLiteral("url"));
        root->addWidget(url);
    }
    root->addSpacing(15);

    m_tabs = makeTabs(this);
    QBoxLayout *page = nullptr;

    // Browser
    addTabPage(m_tabs, options.showBrowserLogin ? QStringLiteral("Browser") : QStringLiteral("OAuth"), page, 10);
    m_browserButton = makeAccentButton(QStringLiteral("Sign in with your browser"));
    m_browserButton->setObjectName(QStringLiteral("signInBrowser"));
    m_browserButton->setVisible(options.showBrowserLogin);
    connect(m_browserButton, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Browser;
        Q_EMIT accepted();
    });
    addCentered(page, m_browserButton);
    page->addStretch();

    // Token
    addTabPage(m_tabs, QStringLiteral("Token"), page, 10);
    m_tokenUserName = makeTextEdit(QStringLiteral("Username or email (optional)"));
    m_tokenUserName->setObjectName(QStringLiteral("tokenUserName"));
    addFullWidth(page, m_tokenUserName, 10);
    m_token = makeSecretEdit(QStringLiteral("Personal access token"));
    m_token->setObjectName(QStringLiteral("token"));
    addFullWidth(page, m_token, 10);
    QPushButton *signInToken = makeAccentButton(QStringLiteral("Sign in"));
    signInToken->setObjectName(QStringLiteral("signInToken"));
    enableWhen(signInToken, {m_token}, [this] { return !isNullOrWhiteSpace(m_token->text()); });
    connect(signInToken, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Pat;
        Q_EMIT accepted();
    });
    addCentered(page, signInToken);
    page->addStretch();

    // Password
    addTabPage(m_tabs, QStringLiteral("Password"), page, 10);
    m_userName = makeTextEdit(QStringLiteral("Username or email"));
    m_userName->setObjectName(QStringLiteral("userName"));
    addFullWidth(page, m_userName, 10);
    m_password = makeSecretEdit(QStringLiteral("Password"));
    m_password->setObjectName(QStringLiteral("password"));
    addFullWidth(page, m_password, 10);
    QPushButton *signInBasic = makeAccentButton(QStringLiteral("Sign in"));
    signInBasic->setObjectName(QStringLiteral("signInBasic"));
    enableWhen(signInBasic, {m_userName, m_password}, [this] {
        return !isNullOrWhiteSpace(m_userName->text()) && !m_password->text().isEmpty();
    });
    connect(signInBasic, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Basic;
        Q_EMIT accepted();
    });
    addCentered(page, signInBasic);
    page->addStretch();

    if (options.userName && !isNullOrWhiteSpace(*options.userName)) {
        m_userName->setText(*options.userName);
        m_tokenUserName->setText(*options.userName);
    }

    m_tabs->setTabVisible(0, options.showBrowserLogin);
    m_tabs->setTabVisible(1, options.showTokenLogin);
    m_tabs->setTabVisible(2, options.showBasicLogin);
    root->addWidget(m_tabs, 1);

    if (isDotCom) {
        root->addSpacing(20);
        root->addWidget(makeSignUpRow([&sessionManager] { openLink(sessionManager, QStringLiteral("https://about.gitlab.com/")); }));
    }
}

QString GitLabCredentialsView::userName() const { return m_userName->text(); }
QString GitLabCredentialsView::password() const { return m_password->text(); }
QString GitLabCredentialsView::token() const { return m_token->text(); }
QString GitLabCredentialsView::tokenUserName() const { return m_tokenUserName->text(); }

void GitLabCredentialsView::setInitialFocus()
{
    if (m_options.showBrowserLogin) {
        m_tabs->setCurrentIndex(0);
        m_browserButton->setFocus();
    } else if (m_options.showTokenLogin) {
        m_tabs->setCurrentIndex(1);
        focusEdit(m_token);
    } else if (m_options.showBasicLogin) {
        m_tabs->setCurrentIndex(2);
        focusEdit(isNullOrWhiteSpace(m_userName->text()) ? m_userName : m_password);
    }
    updateDefaultButton(window());
}

// =============================================================================================
// BitbucketCredentialsView
// =============================================================================================

QString BitbucketCredentialsView::passwordResetUrl(const QUrl &url)
{
    return url.isEmpty() || url.isRelative()
               ? QStringLiteral("https://bitbucket.org/account/password/reset/")
               : absoluteUri(url.resolved(QUrl(QStringLiteral("/passwordreset"))));
}

QString BitbucketCredentialsView::signUpUrl(const QUrl &url)
{
    return url.isEmpty() || url.isRelative()
               ? QStringLiteral("https://bitbucket.org/account/signup/")
               : absoluteUri(url.resolved(QUrl(QStringLiteral("/login"))));
}

BitbucketCredentialsView::BitbucketCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent)
    : View(parent), m_options(options)
{
    m_title = QStringLiteral("Connect to Bitbucket");
    QVBoxLayout *root = newColumn(this);

    root->addWidget(new ImageLabel(QStringLiteral("atlassian-logo.png"), 90), 0, Qt::AlignHCenter);
    root->addSpacing(-10);
    root->addWidget(makeTextBlock(QStringLiteral("Log in to your account")));
    if (!options.url.isEmpty()) {
        root->addSpacing(10);
        QLabel *url = makeTextBlock(options.url.isRelative() ? options.url.toString() : absoluteUri(options.url));
        url->setObjectName(QStringLiteral("url"));
        root->addWidget(url);
    }
    root->addSpacing(20);

    m_tabs = makeTabs(this);
    m_tabs->setFixedWidth(288);
    QBoxLayout *page = nullptr;

    addTabPage(m_tabs, QStringLiteral("Browser"), page, 15);
    m_oauthButton = makeAccentButton(QStringLiteral("Sign in with your browser"));
    m_oauthButton->setObjectName(QStringLiteral("signInBrowser"));
    m_oauthButton->setMinimumHeight(40);
    withPadding(m_oauthButton, 15, 5);
    m_oauthButton->setEnabled(options.showOAuth);
    connect(m_oauthButton, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::OAuth;
        Q_EMIT accepted();
    });
    addCentered(page, m_oauthButton);
    page->addStretch();

    addTabPage(m_tabs, QStringLiteral("Password/Token"), page, 15);
    m_userName = makeTextEdit(QStringLiteral("Email or username"));
    m_userName->setObjectName(QStringLiteral("userName"));
    if (options.userName) {
        m_userName->setText(*options.userName);
    }
    addFullWidth(page, m_userName, 10);
    m_password = makeSecretEdit(QStringLiteral("Password or token"));
    m_password->setObjectName(QStringLiteral("password"));
    addFullWidth(page, m_password, 10);
    QPushButton *signIn = makeAccentButton(QStringLiteral("Sign in"));
    signIn->setObjectName(QStringLiteral("signInBasic"));
    signIn->setFixedSize(140, 40);
    enableWhen(signIn, {m_userName, m_password}, [this] {
        return !isNullOrWhiteSpace(m_userName->text()) && !isNullOrWhiteSpace(m_password->text());
    });
    connect(signIn, &QPushButton::clicked, this, [this] {
        m_mode = ProviderAuthMode::Basic;
        Q_EMIT accepted();
    });
    addCentered(page, signIn);
    page->addStretch();

    m_tabs->setTabVisible(0, options.showOAuth);
    m_tabs->setTabVisible(1, options.showBasic);
    root->addWidget(m_tabs, 1, Qt::AlignHCenter);
    root->addSpacing(20);

    auto *forgot = new LinkButton(QStringLiteral("Can't log in?"));
    forgot->setObjectName(QStringLiteral("forgotPassword"));
    const QUrl url = options.url;
    connect(forgot, &QPushButton::clicked, this, [&sessionManager, url] { openLink(sessionManager, passwordResetUrl(url)); });
    addCentered(root, forgot);
    root->addSpacing(10);
    auto *signUp = new LinkButton(QStringLiteral("Sign up for an account"));
    signUp->setObjectName(QStringLiteral("signUp"));
    connect(signUp, &QPushButton::clicked, this, [&sessionManager, url] { openLink(sessionManager, signUpUrl(url)); });
    addCentered(root, signUp);
    root->addSpacing(8);
}

QString BitbucketCredentialsView::userName() const { return m_userName->text(); }
QString BitbucketCredentialsView::password() const { return m_password->text(); }

void BitbucketCredentialsView::setInitialFocus()
{
    if (m_options.showOAuth) {
        m_tabs->setCurrentIndex(0);
        m_oauthButton->setFocus();
    } else if (m_options.showBasic) {
        m_tabs->setCurrentIndex(1);
        focusEdit(isNullOrWhiteSpace(m_userName->text()) ? m_userName : m_password);
    }
    updateDefaultButton(window());
}

} // namespace gcm::ui
