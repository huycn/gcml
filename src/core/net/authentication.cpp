// Port of Authentication/{AuthenticationBase,BasicAuthentication,OAuthAuthentication,WindowsIntegratedAuthentication}.cs.
// Graphical prompts go through the git-credential-manager-ui helper (see authbase.h); without it the terminal is used.
#include "core/auth/authentication.h"

#include "core/constants.h"
#include "core/strings.h"

#include <QProcess>

namespace gcm {

// =============================================================================================
// AuthenticationBase
// =============================================================================================

namespace {

StringDictionary readHelperResult(ChildProcess &process)
{
    // Same rules as readDictionary: stop at a blank line or EOF; keys case-insensitive.
    StringDictionary result(Qt::CaseInsensitive);
    while (const OptString line = process.readStandardOutputLine()) {
        if (isNullOrWhiteSpace(*line)) {
            break;
        }
        const qsizetype eq = line->indexOf(QLatin1Char('='));
        if (eq > 0) {
            result.set(line->left(eq), line->mid(eq + 1));
        }
    }
    return result;
}

void throwIfHelperFailed(int exitCode, const StringDictionary &result)
{
    if (isTruthy(result.tryGet(QStringLiteral("cancelled")))) {
        throw OperationCanceledException(QStringLiteral("User cancelled dialog."));
    }
    if (exitCode != 0) {
        throw Exception(QStringLiteral("helper error (%1): %2").arg(QString::number(exitCode), result.value(QStringLiteral("error"), QStringLiteral("Unknown"))));
    }
}

ProcessStartInfo helperStartInfo(const QString &path, const QString &args)
{
    ProcessStartInfo psi;
    psi.fileName = path;
    psi.arguments = args;
    psi.redirectStandardInput = true;
    psi.redirectStandardOutput = true;
    psi.redirectStandardError = false; // Do not redirect stderr as tracing might be enabled
    return psi;
}

} // namespace

StringDictionary AuthenticationBase::invokeHelper(const QString &path, const QString &args, const OptString &standardInput, const CancellationToken &ct)
{
    m_context.trace().writeLine(QStringLiteral("Starting helper process: %1 %2").arg(path, args));
    // Flush so we don't stomp over the helper's messages
    m_context.trace().flush();

    std::shared_ptr<ChildProcess> process = m_context.processManager().createProcess(helperStartInfo(path, args), Trace2ProcessClass::UiHelper);
    if (!process->start()) {
        throw Exception(QStringLiteral("Failed to start helper process: %1 %2").arg(path, args));
    }

    // Kill the process upon a cancellation request
    std::weak_ptr<ChildProcess> weak = process;
    ct.onCancel([weak] {
        if (auto p = weak.lock()) {
            p->kill();
        }
    });

    if (standardInput) {
        process->writeStandardInput(standardInput->toUtf8());
    }
    process->closeStandardInput();

    const StringDictionary result = readHelperResult(*process);
    process->waitForExit();
    ct.throwIfCancellationRequested();
    throwIfHelperFailed(process->exitCode(), result);
    return result;
}

void AuthenticationBase::throwIfUserInteractionDisabled()
{
    if (!m_context.settings().isInteractionAllowed()) {
        m_context.trace().writeLine(QStringLiteral("%1 / %2.%3 is false/never; user interactivity has been disabled.")
                                        .arg(Constants::EnvironmentVariables::GcmInteractive,
                                             Constants::GitConfiguration::Credential::SectionName,
                                             Constants::GitConfiguration::Credential::Interactive));
        throw InvalidOperationException(QStringLiteral("Cannot prompt because user interactivity has been disabled."));
    }
}

void AuthenticationBase::throwIfGuiPromptsDisabled()
{
    if (!m_context.settings().isGuiPromptsEnabled()) {
        m_context.trace().writeLine(QStringLiteral("%1 is 0; GUI prompts have been disabled.")
                                        .arg(Constants::EnvironmentVariables::GitTerminalPrompts));
        throw InvalidOperationException(QStringLiteral("Cannot show prompt because GUI prompts have been disabled."));
    }
}

void AuthenticationBase::throwIfTerminalPromptsDisabled()
{
    if (!m_context.settings().isTerminalPromptsEnabled()) {
        m_context.trace().writeLine(QStringLiteral("%1 is 0; terminal prompts have been disabled.")
                                        .arg(Constants::EnvironmentVariables::GitTerminalPrompts));
        throw InvalidOperationException(QStringLiteral("Cannot prompt because terminal prompts have been disabled."));
    }
}

bool AuthenticationBase::tryFindUiHelper(QString &command, QString &args)
{
    command.clear();
    args.clear();
    if (!m_context.settings().isGuiPromptsEnabled() || !m_context.sessionManager().isDesktopSession()) {
        return false;
    }
    return tryFindHelperCommand(Constants::EnvironmentVariables::GcmUiHelper, Constants::GitConfiguration::Credential::UiHelper,
                                Constants::DefaultUiHelper, command, args);
}

bool AuthenticationBase::tryFindHelperCommand(const QString &envar, const QString &configName,
                                              const QString &defaultValue, QString &command, QString &args)
{
    command.clear();
    args.clear();
    ITrace &trace = m_context.trace();

    // 1. (unset): use the default helper name and continue with 3.
    // 2. <absolute>: use the absolute path exactly as entered.
    // 3. <relative>: <appdir>/<relative>, then <relative> on PATH.
    // Warnings are printed for missing helpers the user configured; a missing default helper only means that
    // prompts fall back to the terminal.
    QString helperName;
    bool userSpecified = false;
    if (m_context.settings().tryGetPathSetting(envar, Constants::GitConfiguration::Credential::SectionName, configName,
                                               helperName)) {
        // The empty string signals not to use a helper
        if (helperName.isEmpty()) {
            trace.writeLine(QStringLiteral("UI helper override specified as the empty string."));
            return false;
        }
        trace.writeLine(QStringLiteral("UI helper override specified: '%1'.").arg(helperName));
        userSpecified = true;
    } else if (isNullOrWhiteSpace(defaultValue)) {
        trace.writeLine(QStringLiteral("No default UI supplied."));
        return false;
    } else {
        helperName = defaultValue;
    }

    // An absolute path is run directly, without intermediaries or modification
    if (helperName.startsWith(QLatin1Char('/'))) {
        if (m_context.fileSystem().fileExists(helperName)) {
            trace.writeLine(QStringLiteral("UI helper found at '%1'.").arg(helperName));
            command = helperName;
            return true;
        }
        trace.writeLine(QStringLiteral("UI helper was not found at '%1'.").arg(helperName));
        m_context.console().writeWarning(QStringLiteral("could not find configured UI helper '%1'").arg(helperName));
        return false;
    }

    // The installation directory, for an in-box helper
    QString appDir = m_context.installationDirectory();
    const QString inBoxPath = (appDir.endsWith(QLatin1Char('/')) ? appDir : appDir + QLatin1Char('/')) + helperName;
    if (m_context.fileSystem().fileExists(inBoxPath)) {
        trace.writeLine(QStringLiteral("Found in-box native UI helper: '%1'").arg(inBoxPath));
        command = inBoxPath;
        return true;
    }

    // The PATH
    QString pathCommand;
    if (m_context.environment().tryLocateExecutable(helperName, pathCommand)) {
        trace.writeLine(QStringLiteral("Found UI helper on PATH: '%1'").arg(helperName));
        command = pathCommand;
        return true;
    }

    trace.writeLine(QStringLiteral("UI helper '%1' was not found.").arg(helperName));
    if (userSpecified) {
        m_context.console().writeWarning(QStringLiteral("could not find UI helper '%1'").arg(helperName));
    }
    return false;
}

// =============================================================================================
// BasicAuthentication
// =============================================================================================

Credential BasicAuthentication::getCredentials(const QString &resource, const OptString &userName)
{
    EnsureArgument::notNullOrWhiteSpace(resource, "resource");
    throwIfUserInteractionDisabled();

    QString command, args;
    if (tryFindUiHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("basic");
        if (!isNullOrWhiteSpace(resource)) {
            promptArgs += QStringLiteral(" --resource %1").arg(quoteCmdArg(resource));
        }
        if (!isNullOrWhiteSpace(userName)) {
            promptArgs += QStringLiteral(" --username %1").arg(quoteCmdArg(*userName));
        }

        const StringDictionary resultDict = invokeHelper(command, promptArgs);
        QString resultUserName, password;
        if (!resultDict.tryGetValue(QStringLiteral("username"), resultUserName)) {
            throw Exception(QStringLiteral("Missing 'username' in response"));
        }
        if (!resultDict.tryGetValue(QStringLiteral("password"), password)) {
            throw Exception(QStringLiteral("Missing 'password' in response"));
        }
        return Credential{resultUserName, password};
    }

    throwIfTerminalPromptsDisabled();

    IConsoleService &console = m_context.console();
    console.writeLine(QStringLiteral("Enter basic credentials for '%1':").arg(resource));
    QString user;
    if (!isNullOrWhiteSpace(userName)) {
        // No need to prompt for the user name if it has been specified already
        user = *userName;
        console.writeLine(QStringLiteral("Username: %1").arg(user));
    } else {
        user = console.promptText(QStringLiteral("Username"));
    }
    const QString password = console.promptSecret(QStringLiteral("Password"));
    return Credential{user, password};
}

// =============================================================================================
// OAuthAuthentication
// =============================================================================================

OAuthAuthenticationModes OAuthAuthentication::getAuthenticationMode(const QString &resource, OAuthAuthenticationModes modes)
{
    EnsureArgument::notNullOrWhiteSpace(resource, "resource");
    throwIfUserInteractionDisabled();

    // Browser requires a desktop session!
    if (!m_context.sessionManager().isDesktopSession()) {
        modes = modes & ~OAuthAuthenticationModes::Browser;
    }

    // We need at least one mode!
    if (modes == OAuthAuthenticationModes::None) {
        throw ArgumentException(QStringLiteral("Must specify at least one OAuthAuthenticationModes (Parameter 'modes')"));
    }

    // If there is no mode choice to be made then just return that result
    if (modes == OAuthAuthenticationModes::Browser || modes == OAuthAuthenticationModes::DeviceCode) {
        return modes;
    }

    QString command, args;
    if (tryFindUiHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("oauth");
        if (!isNullOrWhiteSpace(resource)) {
            promptArgs += QStringLiteral(" --resource %1").arg(quoteCmdArg(resource));
        }
        if (hasFlag(modes, OAuthAuthenticationModes::Browser)) {
            promptArgs += QStringLiteral(" --browser");
        }
        if (hasFlag(modes, OAuthAuthenticationModes::DeviceCode)) {
            promptArgs += QStringLiteral(" --device-code");
        }

        const StringDictionary resultDict = invokeHelper(command, promptArgs);
        QString responseMode;
        if (!resultDict.tryGetValue(QStringLiteral("mode"), responseMode)) {
            throw Exception(QStringLiteral("Missing 'mode' in response"));
        }
        const QString lower = responseMode.toLower();
        if (lower == QLatin1String("browser")) {
            return OAuthAuthenticationModes::Browser;
        }
        if (lower == QLatin1String("devicecode")) {
            return OAuthAuthenticationModes::DeviceCode;
        }
        throw Exception(QStringLiteral("Unknown mode value in response '%1'").arg(responseMode));
    }

    throwIfTerminalPromptsDisabled();
    QStringList choices;
    QList<OAuthAuthenticationModes> values;
    if (hasFlag(modes, OAuthAuthenticationModes::Browser)) {
        choices.append(QStringLiteral("Web browser"));
        values.append(OAuthAuthenticationModes::Browser);
    }
    if (hasFlag(modes, OAuthAuthenticationModes::DeviceCode)) {
        choices.append(QStringLiteral("Device code"));
        values.append(OAuthAuthenticationModes::DeviceCode);
    }
    const int choice =
        m_context.console().promptSelection(QStringLiteral("Select an authentication method for '%1'").arg(resource), choices);
    return values.value(choice, OAuthAuthenticationModes::None);
}

OAuth2TokenResult OAuthAuthentication::getTokenByBrowser(IOAuth2Client &client, const QStringList &scopes)
{
    throwIfUserInteractionDisabled();

    // We require a desktop session to launch the user's default web browser
    if (!m_context.sessionManager().isDesktopSession()) {
        throw InvalidOperationException(QStringLiteral("Browser authentication requires a desktop session"));
    }

    OAuth2SystemWebBrowser browser(m_context.sessionManager(), OAuth2WebBrowserOptions());
    const OAuth2AuthorizationCodeResult authCode = client.getAuthorizationCode(scopes, browser, CancellationToken());
    return client.getTokenByAuthorizationCode(authCode, CancellationToken());
}

OAuth2TokenResult OAuthAuthentication::getTokenByDeviceCode(IOAuth2Client &client, const QStringList &scopes)
{
    throwIfUserInteractionDisabled();

    const OAuth2DeviceCodeResult dcr = client.getDeviceCode(scopes, CancellationToken());

    // With a desktop session and the UI helper, show the device code in a dialog while polling for the token
    QString command, args;
    if (tryFindUiHelper(command, args)) {
        QString promptArgs = args + QStringLiteral("device");
        promptArgs += QStringLiteral(" --code %1 ").arg(quoteCmdArg(dcr.userCode));
        promptArgs += QStringLiteral(" --url %1").arg(quoteCmdArg(absoluteUri(dcr.verificationUri)));

        m_context.trace().writeLine(QStringLiteral("Starting helper process: %1 %2").arg(command, promptArgs));
        m_context.trace().flush();
        std::unique_ptr<ChildProcess> helper =
            m_context.processManager().createProcess(helperStartInfo(command, promptArgs), Trace2ProcessClass::UiHelper);
        if (!helper->start()) {
            throw Exception(QStringLiteral("Failed to start helper process: %1 %2").arg(command, promptArgs));
        }
        helper->closeStandardInput();

        // If the dialog is closed the user wishes to cancel the request
        CancellationToken tokenCt;
        if (QProcess *p = helper->process()) {
            QObject::connect(p, &QProcess::finished, p, [tokenCt] { tokenCt.cancel(); });
        }

        OAuth2TokenResult tokenResult;
        try {
            tokenResult = client.getTokenByDeviceCode(dcr, tokenCt);
        } catch (const OperationCanceledException &) {
            throw Exception(QStringLiteral("User canceled device code authentication"));
        }

        // Close the dialog
        helper->kill();
        return tokenResult;
    }

    throwIfTerminalPromptsDisabled();
    m_context.console().writeLine(
        QStringLiteral("To complete authentication please visit %1 and enter the following code:\n%2")
            .arg(absoluteUri(dcr.verificationUri), dcr.userCode));
    return client.getTokenByDeviceCode(dcr, CancellationToken());
}

// =============================================================================================
// WindowsIntegratedAuthentication
// =============================================================================================

NtlmSupport WindowsIntegratedAuthentication::askEnableNtlm(const QUrl &uri)
{
    throwIfUserInteractionDisabled();
    // Upstream shows an in-process dialog on a desktop; the UI helper has no NTLM prompt, so the terminal is used.
    throwIfTerminalPromptsDisabled();

    const int choice = m_context.console().promptSelection(
        QStringLiteral("Re-enable NTLM support in Git?"),
        {QStringLiteral("Yes - just this time"), QStringLiteral("Yes - always for %1").arg(absoluteUri(uri)),
         QStringLiteral("No - do not enable NTLM")});
    switch (choice) {
    case 0: return NtlmSupport::Once;
    case 1: return NtlmSupport::Always;
    default: return NtlmSupport::Disabled;
    }
}

WindowsAuthenticationTypes WindowsIntegratedAuthentication::getAuthenticationTypes(const QUrl &uri)
{
    EnsureArgument::absoluteUri(uri, "uri");

    WindowsAuthenticationTypes types = WindowsAuthenticationTypes::None;
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("HTTP: HEAD %1").arg(absoluteUri(uri)));
    const HttpResponse response = m_context.httpClientFactory().createClient().head(uri);
    trace.writeLine(QStringLiteral("HTTP: Response code ignored."));
    trace.writeLine(QStringLiteral("Inspecting WWW-Authenticate headers..."));
    for (const QStringList headers = response.wwwAuthenticate(); const QString &header : headers) {
        const QString scheme = header.trimmed().section(QLatin1Char(' '), 0, 0);
        if (equalsIgnoreCase(scheme, Constants::Http::WwwAuthenticateNegotiateScheme)) {
            trace.writeLine(QStringLiteral("Found WWW-Authenticate header for Negotiate"));
            types = types | WindowsAuthenticationTypes::Negotiate;
        } else if (equalsIgnoreCase(scheme, Constants::Http::WwwAuthenticateNtlmScheme)) {
            trace.writeLine(QStringLiteral("Found WWW-Authenticate header for NTLM"));
            types = types | WindowsAuthenticationTypes::Ntlm;
        }
    }
    return types;
}

} // namespace gcm
