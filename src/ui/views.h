// The dialogs of the graphical helper: ports of the Avalonia views and their view models
// (Core/UI/Views, GitHub/UI/Views, GitLab/UI/Views, Atlassian.Bitbucket/UI/Views).
// Each view takes its inputs in the constructor and exposes the user's choice once accepted.
#pragma once

#include "widgets.h"

#include <QUrl>

class QListWidget;
class QTabWidget;

namespace gcm {
class ISessionManager;
}

namespace gcm::ui {

/// Opens `url` in the browser; failures are traced and otherwise ignored (a broken link must not end the prompt).
void openLink(ISessionManager &sessionManager, const QString &url);

// --- generic ------------------------------------------------------------------------------------

/// Core/UI/Views/CredentialsView: username + password.
class CredentialsView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        OptString title;
        QString description;
        OptString userName;
        bool showProductHeader = true;
    };
    explicit CredentialsView(const Options &options, QWidget *parent = nullptr);
    QString userName() const;
    QString password() const;
    void setInitialFocus() override;

private:
    QLineEdit *m_userName;
    QLineEdit *m_password;
};

enum class OAuthMode { None, Browser, DeviceCode };

/// Core/UI/Views/OAuthView: choose browser or device code sign-in.
class OAuthView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        OptString title;
        QString description;
        bool showBrowserLogin = false;
        bool showDeviceCodeLogin = false;
        bool showProductHeader = true;
    };
    explicit OAuthView(const Options &options, QWidget *parent = nullptr);
    OAuthMode selectedMode() const { return m_mode; }

private:
    OAuthMode m_mode = OAuthMode::None;
};

/// Core/UI/Views/DeviceCodeView and GitHub/UI/Views/DeviceCodeView (with `gitHubHeader`): shows the user code
/// and a link to the verification URL. Stays open until the caller kills the helper or the user closes it.
class DeviceCodeView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        QString userCode;
        QString verificationUrl;
        bool showProductHeader = true;
        bool gitHubHeader = false;
    };
    DeviceCodeView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
};

/// Core/UI/Views/DefaultAccountView: continue with the operating system account, or use another one.
class DefaultAccountView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        OptString title;
        QString userName;
        bool showProductHeader = true;
    };
    DefaultAccountView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
    bool useDefaultAccount() const { return m_useDefault; }

private:
    bool m_useDefault = false;
};

// --- GitHub -------------------------------------------------------------------------------------

enum class ProviderAuthMode { None, Basic, Browser, Device, Pat, OAuth };

/// GitHub/UI/Views/CredentialsView: browser/device, token and password tabs.
class GitHubCredentialsView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        OptString enterpriseUrl;
        OptString userName;
        bool showBrowserLogin = false;
        bool showDeviceLogin = false;
        bool showTokenLogin = false;
        bool showBasicLogin = false;
    };
    GitHubCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
    ProviderAuthMode selectedMode() const { return m_mode; }
    QString userName() const;
    QString password() const;
    QString token() const;
    void setInitialFocus() override;

    /// "Browser/Device", "Browser", "Device" or "OAuth" (CredentialsViewModel.OAuthModeTitle).
    static QString oauthModeTitle(bool browser, bool device);

private:
    Options m_options;
    ProviderAuthMode m_mode = ProviderAuthMode::None;
    QTabWidget *m_tabs;
    QPushButton *m_browserButton;
    QPushButton *m_deviceButton;
    QLineEdit *m_token;
    QLineEdit *m_userName;
    QLineEdit *m_password;
};

/// GitHub/UI/Views/TwoFactorView: six-digit authentication code.
class GitHubTwoFactorView : public View
{
    Q_OBJECT
public:
    GitHubTwoFactorView(ISessionManager &sessionManager, bool isSms, QWidget *parent = nullptr);
    QString code() const;
    void setInitialFocus() override;

    static QString description(bool isSms);

private:
    SixDigitInput *m_code;
};

/// GitHub/UI/Views/SelectAccountView: pick one of the known accounts or add a new one.
class GitHubSelectAccountView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        OptString enterpriseUrl;
        QStringList accounts;
        bool showHelpLink = true;
    };
    GitHubSelectAccountView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
    /// The selected account; nullopt for "Add a new account".
    OptString selectedAccount() const { return m_selected; }

private:
    QListWidget *m_list;
    OptString m_selected;
};

// --- GitLab -------------------------------------------------------------------------------------

/// GitLab/UI/Views/CredentialsView: browser, token (with optional username) and password tabs.
class GitLabCredentialsView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        /// Set only for instances other than GitLab.com.
        OptString url;
        OptString userName;
        bool showBrowserLogin = false;
        bool showTokenLogin = false;
        bool showBasicLogin = false;
    };
    GitLabCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
    ProviderAuthMode selectedMode() const { return m_mode; }
    QString userName() const;
    QString password() const;
    QString token() const;
    QString tokenUserName() const;
    void setInitialFocus() override;

private:
    Options m_options;
    ProviderAuthMode m_mode = ProviderAuthMode::None;
    QTabWidget *m_tabs;
    QPushButton *m_browserButton;
    QLineEdit *m_tokenUserName;
    QLineEdit *m_token;
    QLineEdit *m_userName;
    QLineEdit *m_password;
};

// --- Bitbucket ----------------------------------------------------------------------------------

/// Atlassian.Bitbucket/UI/Views/CredentialsView: browser (OAuth) and password/token tabs.
class BitbucketCredentialsView : public View
{
    Q_OBJECT
public:
    struct Options
    {
        /// Bitbucket Data Center URL (empty for Bitbucket Cloud).
        QUrl url;
        OptString userName;
        bool showOAuth = false;
        bool showBasic = false;
    };
    BitbucketCredentialsView(ISessionManager &sessionManager, const Options &options, QWidget *parent = nullptr);
    ProviderAuthMode selectedMode() const { return m_mode; }
    QString userName() const;
    QString password() const;
    void setInitialFocus() override;

    /// Targets of the "Can't log in?" and "Sign up for an account" links.
    static QString passwordResetUrl(const QUrl &url);
    static QString signUpUrl(const QUrl &url);

private:
    Options m_options;
    ProviderAuthMode m_mode = ProviderAuthMode::None;
    QTabWidget *m_tabs;
    QPushButton *m_oauthButton;
    QLineEdit *m_userName;
    QLineEdit *m_password;
};

} // namespace gcm::ui
