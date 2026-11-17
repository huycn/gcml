// Port of GitHubAuthentication.cs. Upstream shows its Avalonia views in-process when no GitHub helper is found;
// this port uses the `github` commands of git-credential-manager-ui instead, and the terminal when that is
// missing too.
#include "github.h"
#include "githubresources.h"

#include "core/commandcontext.h"
#include "core/strings.h"
#include "core/trace2.h"

namespace gcm::github {

namespace {

AuthenticationPromptResult promptResult(AuthenticationModes mode, std::optional<Credential> credential = std::nullopt)
{
    return AuthenticationPromptResult{mode, std::move(credential)};
}

/// Uri.ToString() of the target URI as shown in prompts ("https://github.com/").
QString display(const QUrl &uri)
{
    return uri.isEmpty() ? QString() : absoluteUri(uri);
}

} // namespace

bool GitHubAuthentication::tryFindGitHubHelper(QString &command, QString &args)
{
    // A helper speaking the GitHub.UI protocol (GCM_GITHUB_HELPER / credential.gitHubHelper, default "GitHub.UI")
    if (tryFindHelperCommand(GitHubConstants::EnvironmentVariables::AuthenticationHelper,
                             GitHubConstants::GitConfiguration::Credential::AuthenticationHelper,
                             GitHubConstants::DefaultAuthenticationHelper,
                             command,
                             args)) {
        return true;
    }
    // The GitHub commands of git-credential-manager-ui (upstream: the in-process views)
    if (tryFindUiHelper(command, args)) {
        args += QStringLiteral("github ");
        return true;
    }
    return false;
}

HttpClient &GitHubAuthentication::httpClient()
{
    if (!m_httpClient) {
        m_httpClient = m_context.httpClientFactory().createClient();
    }
    return *m_httpClient;
}

OptString GitHubAuthentication::selectAccount(const QUrl &targetUri, const QStringList &accounts)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("github"), QStringLiteral("select_account"));
    throwIfUserInteractionDisabled();

    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() &&
        tryFindGitHubHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("select-account");
        if (!GitHubHostProvider::isGitHubDotCom(targetUri)) {
            promptArgs += QStringLiteral(" --enterprise-url %1").arg(quoteCmdArg(display(targetUri)));
        }
        // Write the accounts to the standard input of the helper process to avoid any issues with escaping special
        // characters, and to avoid max argument length problems.
        const StringDictionary resultDict = invokeHelper(command, promptArgs, accounts.join(QLatin1Char('\n')));
        const OptString selectedAccount = resultDict.tryGet(QStringLiteral("account"));
        if (!selectedAccount) {
            throw Exception(QStringLiteral("Missing 'account' in response"));
        }
        return isNullOrWhiteSpace(*selectedAccount) ? OptString() : selectedAccount;
    }

    throwIfTerminalPromptsDisabled();
    QStringList choices{QStringLiteral("Add a new account")};
    choices.append(accounts);
    const int choice = m_context.console().promptSelection(QStringLiteral("Select an account for '%1'").arg(display(targetUri)), choices);
    return choice == 0 ? OptString() : OptString(accounts.at(choice - 1));
}

AuthenticationPromptResult GitHubAuthentication::getAuthentication(const QUrl &targetUri, const OptString &userName, AuthenticationModes modes)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("github"), QStringLiteral("get_auth"));
    Trace2::writeData(QStringLiteral("github"), QStringLiteral("modes/initial"), modesToString(modes));

    // If we cannot start a browser then don't offer the option
    if (!m_context.sessionManager().isWebBrowserAvailable()) {
        modes = modes & ~AuthenticationModes::Browser;
    }
    Trace2::writeData(QStringLiteral("github"), QStringLiteral("modes/available"), modesToString(modes));

    // We need at least one mode!
    if (modes == AuthenticationModes::None) {
        throw ArgumentException(QStringLiteral("Must specify at least one AuthenticationModes (Parameter 'modes')"));
    }

    // If there is no mode choice to be made and no interaction required, just return that result.
    if (modes == AuthenticationModes::Browser || modes == AuthenticationModes::Device) {
        return promptResult(modes);
    }

    throwIfUserInteractionDisabled();

    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() &&
        tryFindGitHubHelper(command, args)) {
        return getAuthenticationViaHelper(targetUri, userName, modes, command, args);
    }
    return getAuthenticationViaTty(targetUri, userName, modes);
}

AuthenticationPromptResult GitHubAuthentication::getAuthenticationViaTty(const QUrl &targetUri, OptString userName, AuthenticationModes modes)
{
    throwIfTerminalPromptsDisabled();
    IConsoleService &console = m_context.console();

    auto basic = [&]() {
        console.writeLine(QStringLiteral("Enter GitHub credentials for '%1'...").arg(display(targetUri)));
        if (isNullOrWhiteSpace(userName)) {
            userName = console.promptText(QStringLiteral("Username"));
        } else {
            console.writeLine(QStringLiteral("Username: %1").arg(*userName));
        }
        const QString password = console.promptSecret(QStringLiteral("Password"));
        return promptResult(AuthenticationModes::Basic, Credential{valueOr(userName), password});
    };
    auto pat = [&]() {
        console.writeLine(QStringLiteral("Enter GitHub personal access token for '%1'...").arg(display(targetUri)));
        const QString token = console.promptSecret(QStringLiteral("Token"));
        return promptResult(AuthenticationModes::Pat, Credential{valueOr(userName), token});
    };

    switch (modes) {
    case AuthenticationModes::Basic: return basic();
    case AuthenticationModes::Browser: return promptResult(AuthenticationModes::Browser);
    case AuthenticationModes::Device: return promptResult(AuthenticationModes::Device);
    case AuthenticationModes::Pat: return pat();
    case AuthenticationModes::None:
        throw ArgumentOutOfRangeException(QStringLiteral("At least one AuthenticationModes must be supplied (Parameter 'modes')"));
    default: break;
    }

    QStringList labels;
    QList<AuthenticationModes> values;
    auto add = [&](AuthenticationModes mode, const QString &label) {
        if (hasFlag(modes, mode)) {
            labels.append(label);
            values.append(mode);
        }
    };
    add(AuthenticationModes::Browser, QStringLiteral("Web browser"));
    add(AuthenticationModes::Device, QStringLiteral("Device code"));
    add(AuthenticationModes::Pat, QStringLiteral("Personal access token"));
    add(AuthenticationModes::Basic, QStringLiteral("Username/password"));
    const int choice = console.promptSelection(QStringLiteral("Select an authentication method for '%1'").arg(display(targetUri)), labels);
    switch (values.value(choice, AuthenticationModes::None)) {
    case AuthenticationModes::Browser: return promptResult(AuthenticationModes::Browser);
    case AuthenticationModes::Device: return promptResult(AuthenticationModes::Device);
    case AuthenticationModes::Basic: return basic();
    case AuthenticationModes::Pat: return pat();
    default: throw Exception(QStringLiteral("Exception of type 'System.Exception' was thrown."));
    }
}

AuthenticationPromptResult GitHubAuthentication::getAuthenticationViaHelper(const QUrl &targetUri, OptString userName,
                                                                            AuthenticationModes modes, const QString &command,
                                                                            const QString &args)
{
    QString promptArgs = args + QStringLiteral("prompt");
    if (modes == AuthenticationModes::All) {
        promptArgs += QStringLiteral(" --all");
    } else {
        if (hasFlag(modes, AuthenticationModes::Basic)) {
            promptArgs += QStringLiteral(" --basic");
        }
        if (hasFlag(modes, AuthenticationModes::Browser)) {
            promptArgs += QStringLiteral(" --browser");
        }
        if (hasFlag(modes, AuthenticationModes::Device)) {
            promptArgs += QStringLiteral(" --device");
        }
        if (hasFlag(modes, AuthenticationModes::Pat)) {
            promptArgs += QStringLiteral(" --pat");
        }
    }
    if (!GitHubHostProvider::isGitHubDotCom(targetUri)) {
        promptArgs += QStringLiteral(" --enterprise-url %1").arg(quoteCmdArg(display(targetUri)));
    }
    if (!isNullOrWhiteSpace(userName)) {
        promptArgs += QStringLiteral(" --username %1").arg(quoteCmdArg(*userName));
    }

    const StringDictionary resultDict = invokeHelper(command, promptArgs);
    const OptString responseMode = resultDict.tryGet(QStringLiteral("mode"));
    if (!responseMode) {
        throw Exception(QStringLiteral("Missing 'mode' in response"));
    }

    const QString mode = responseMode->toLower();
    if (mode == QLatin1String("pat")) {
        const OptString pat = resultDict.tryGet(QStringLiteral("pat"));
        if (!pat) {
            throw Exception(QStringLiteral("Missing 'pat' in response"));
        }
        return promptResult(AuthenticationModes::Pat, Credential{valueOr(userName), *pat});
    }
    if (mode == QLatin1String("browser")) {
        return promptResult(AuthenticationModes::Browser);
    }
    if (mode == QLatin1String("device")) {
        return promptResult(AuthenticationModes::Device);
    }
    if (mode == QLatin1String("basic")) {
        userName = resultDict.tryGet(QStringLiteral("username"));
        if (!userName) {
            throw Exception(QStringLiteral("Missing 'username' in response"));
        }
        const OptString password = resultDict.tryGet(QStringLiteral("password"));
        if (!password) {
            throw Exception(QStringLiteral("Missing 'password' in response"));
        }
        return promptResult(AuthenticationModes::Basic, Credential{*userName, *password});
    }
    throw Exception(QStringLiteral("Unknown mode value in response '%1'").arg(*responseMode));
}

QString GitHubAuthentication::getTwoFactorCode(const QUrl &targetUri, bool isSms)
{
    Q_UNUSED(targetUri);
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("github"), QStringLiteral("get_tfa"));
    throwIfUserInteractionDisabled();

    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() &&
        tryFindGitHubHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("2fa");
        if (isSms) {
            promptArgs += QStringLiteral(" --sms");
        }
        const StringDictionary resultDict = invokeHelper(command, promptArgs);
        const OptString authCode = resultDict.tryGet(QStringLiteral("code"));
        if (!authCode) {
            throw Exception(QStringLiteral("Missing 'code' in response"));
        }
        return *authCode;
    }

    throwIfTerminalPromptsDisabled();
    IConsoleService &console = m_context.console();
    console.writeLine(QStringLiteral("Two-factor authentication is enabled and an authentication code is required."));
    console.writeLine(isSms ? QStringLiteral("An SMS containing the authentication code has been sent to your registered device.") : QStringLiteral("Use your registered authentication app to generate an authentication code."));
    return console.promptText(QStringLiteral("Authentication code"));
}

OAuth2TokenResult GitHubAuthentication::getOAuthTokenViaBrowser(const QUrl &targetUri, const QStringList &scopes, const OptString &loginHint)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("github"), QStringLiteral("oauth_browser"));
    throwIfUserInteractionDisabled();

    GitHubOAuth2Client oauthClient(httpClient(), m_context.settings(), targetUri);

    // Can we launch the user's default web browser?
    if (!m_context.sessionManager().isWebBrowserAvailable()) {
        throw InvalidOperationException(QStringLiteral("Browser authentication requires a desktop session"));
    }

    OAuth2WebBrowserOptions browserOptions;
    browserOptions.successResponseHtml = GitHubResources::authenticationResponseSuccessHtml;
    browserOptions.failureResponseHtmlFormat = GitHubResources::authenticationResponseFailureHtmlFormat;
    OAuth2SystemWebBrowser browser(m_context.sessionManager(), browserOptions);

    // If we have a login hint we should pass this to GitHub as an extra query parameter
    StringDictionary queryParams;
    if (loginHint) {
        queryParams.set(QStringLiteral("login"), *loginHint);
    }

    // Write message to the terminal (if any is attached) for some feedback that we're waiting for a web response
    m_context.console().writeInfo(QStringLiteral("please complete authentication in your browser..."));

    const OAuth2AuthorizationCodeResult authCodeResult = oauthClient.getAuthorizationCode(scopes, browser, queryParams, CancellationToken());
    return oauthClient.getTokenByAuthorizationCode(authCodeResult, CancellationToken());
}

OAuth2TokenResult GitHubAuthentication::getOAuthTokenViaDeviceCode(const QUrl &targetUri, const QStringList &scopes)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("github"), QStringLiteral("oauth_device_code"));
    throwIfUserInteractionDisabled();

    GitHubOAuth2Client oauthClient(httpClient(), m_context.settings(), targetUri);
    const OAuth2DeviceCodeResult dcr = oauthClient.getDeviceCode(scopes, CancellationToken());

    // If we have a desktop session show the device code in a dialog
    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() &&
        tryFindGitHubHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("device");
        promptArgs += QStringLiteral(" --code %1 ").arg(quoteCmdArg(dcr.userCode));
        promptArgs += QStringLiteral(" --url %1").arg(quoteCmdArg(absoluteUri(dcr.verificationUri)));
        return getTokenByDeviceCodeWithDialog(command, promptArgs, oauthClient, dcr);
    }

    throwIfTerminalPromptsDisabled();
    m_context.console().writeLine(QStringLiteral("To complete authentication please visit %1 and enter the following code:\n%2").arg(absoluteUri(dcr.verificationUri), dcr.userCode));
    return oauthClient.getTokenByDeviceCode(dcr, CancellationToken());
}

} // namespace gcm::github
