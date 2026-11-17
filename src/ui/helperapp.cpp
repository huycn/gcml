#include "helperapp.h"

#include "views.h"

#include "core/cmdline.h"
#include "core/constants.h"
#include "core/settings.h"
#include "core/streams.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QFileInfo>

namespace gcm::ui {

namespace {

// clazy:exclude-next-line=non-pod-global-static
const QString NoLogoDescription = QStringLiteral("Hide the Git Credential Manager logo and logotype.");

Option noLogoOption()
{
    return Option::flag(QStringLiteral("--no-logo"), NoLogoDescription);
}

Option stringOption(const QString &name, const QString &description, bool required = false)
{
    return Option::value(name, description, required);
}

/// Shows the view in a dialog window (which takes ownership of it); throws OperationCanceledException when the
/// user dismisses it. The returned window keeps the view alive while the caller reads the result.
[[nodiscard]] std::unique_ptr<DialogWindow> showOrThrow(ICommandContext &context, View *view)
{
    auto window = std::make_unique<DialogWindow>(view);
    if (!window->showAndWait(context.settings().parentWindowId())) {
        throw OperationCanceledException(QStringLiteral("User cancelled dialog."));
    }
    return window;
}

} // namespace

bool HelperApplication::isGitHubDotCom(const OptString &url)
{
    const std::optional<QUrl> uri = tryCreateUri(url);
    if (!uri) {
        return false;
    }
    const QString host = uri->host();
    return host.compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0 ||
           host.compare(QStringLiteral("gist.github.com"), Qt::CaseInsensitive) == 0;
}

bool HelperApplication::isGitLabDotCom(const QUrl &url)
{
    return url.host().compare(QStringLiteral("gitlab.com"), Qt::CaseInsensitive) == 0;
}

void HelperApplication::writeResult(const StringDictionary &result)
{
    writeDictionary(m_context.streams().out(), result);
}

int HelperApplication::onException(const std::exception &ex)
{
    Trace2::writeError(ex);
    const auto *gcmEx = dynamic_cast<const Exception *>(&ex);
    StringDictionary result;
    result.set(QStringLiteral("error"), gcmEx ? gcmEx->message() : QString::fromUtf8(ex.what()));
    // Addition to upstream: tell the caller that the user dismissed the dialog (answered with quit=1)
    if (dynamic_cast<const OperationCanceledException *>(&ex)) {
        result.set(QStringLiteral("cancelled"), QStringLiteral("1"));
    }
    writeResult(result);
    return -1;
}

std::shared_ptr<Command> HelperApplication::createRootCommand()
{
    ICommandContext &context = m_context;
    auto root = std::make_shared<Command>(QStringLiteral("git-credential-manager-ui"), QString());

    // --- Core/UI/Commands -------------------------------------------------------------------
    {
        auto basic = std::make_shared<Command>(QStringLiteral("basic"), QStringLiteral("Show basic authentication prompt."));
        basic->addOption(stringOption(QStringLiteral("--title"), QStringLiteral("Window title (optional).")));
        basic->addOption(stringOption(QStringLiteral("--resource"), QStringLiteral("Resource name or URL (optional).")));
        basic->addOption(stringOption(QStringLiteral("--username"), QStringLiteral("User name (optional).")));
        basic->addOption(noLogoOption());
        basic->setHandler([this, &context](const ParseResult &r) {
            CredentialsView::Options o;
            o.title = r.value(QStringLiteral("--title"));
            const OptString resource = r.value(QStringLiteral("--resource"));
            o.description = !isNullOrWhiteSpace(resource) ? QStringLiteral("Enter your credentials for '%1'").arg(*resource) : QStringLiteral("Enter your credentials");
            o.userName = r.value(QStringLiteral("--username"));
            o.showProductHeader = !r.has(QStringLiteral("--no-logo"));
            auto *view = new CredentialsView(o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            result.set(QStringLiteral("username"), view->userName());
            result.set(QStringLiteral("password"), view->password());
            writeResult(result);
            return 0;
        });
        root->addCommand(basic);
    }
    {
        auto oauth = std::make_shared<Command>(QStringLiteral("oauth"), QStringLiteral("Show OAuth authentication prompt."));
        oauth->addOption(stringOption(QStringLiteral("--title"), QStringLiteral("Window title (optional).")));
        oauth->addOption(stringOption(QStringLiteral("--resource"), QStringLiteral("Resource name or URL (optional).")));
        oauth->addOption(Option::flag(QStringLiteral("--browser"), QStringLiteral("Show browser authentication option.")));
        oauth->addOption(Option::flag(QStringLiteral("--device-code"), QStringLiteral("Show device code authentication option.")));
        oauth->addOption(noLogoOption());
        oauth->setHandler([this, &context](const ParseResult &r) {
            OAuthView::Options o;
            o.title = r.value(QStringLiteral("--title"));
            const OptString resource = r.value(QStringLiteral("--resource"));
            o.description = !isNullOrWhiteSpace(resource) ? QStringLiteral("Sign in to '%1'").arg(*resource) : QStringLiteral("Select a sign-in option");
            o.showBrowserLogin = r.has(QStringLiteral("--browser"));
            o.showDeviceCodeLogin = r.has(QStringLiteral("--device-code"));
            o.showProductHeader = !r.has(QStringLiteral("--no-logo"));
            auto *view = new OAuthView(o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            switch (view->selectedMode()) {
            case OAuthMode::Browser: result.set(QStringLiteral("mode"), QStringLiteral("browser")); break;
            case OAuthMode::DeviceCode: result.set(QStringLiteral("mode"), QStringLiteral("devicecode")); break;
            default: throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values."));
            }
            writeResult(result);
            return 0;
        });
        root->addCommand(oauth);
    }
    {
        auto device = std::make_shared<Command>(QStringLiteral("device"), QStringLiteral("Show device code prompt."));
        device->addOption(stringOption(QStringLiteral("--code"), QStringLiteral("User code.")));
        device->addOption(stringOption(QStringLiteral("--url"), QStringLiteral("Verification URL.")));
        device->addOption(noLogoOption());
        device->setHandler([&context](const ParseResult &r) {
            DeviceCodeView::Options o;
            o.userCode = valueOr(r.value(QStringLiteral("--code")));
            o.verificationUrl = valueOr(r.value(QStringLiteral("--url")));
            o.showProductHeader = !r.has(QStringLiteral("--no-logo"));
            const auto window = showOrThrow(context, new DeviceCodeView(context.sessionManager(), o));
            return 0;
        });
        root->addCommand(device);
    }
    {
        auto defaultAccount = std::make_shared<Command>(QStringLiteral("default-account"), QStringLiteral("Show prompt to confirm use of the default OS account."));
        defaultAccount->addOption(stringOption(QStringLiteral("--title"), QStringLiteral("Window title (optional).")));
        defaultAccount->addOption(stringOption(QStringLiteral("--username"), QStringLiteral("User name to display."), true));
        defaultAccount->addOption(noLogoOption());
        defaultAccount->setHandler([this, &context](const ParseResult &r) {
            DefaultAccountView::Options o;
            o.title = r.value(QStringLiteral("--title"));
            o.userName = valueOr(r.value(QStringLiteral("--username")));
            o.showProductHeader = !r.has(QStringLiteral("--no-logo"));
            auto *view = new DefaultAccountView(context.sessionManager(), o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            result.set(QStringLiteral("use_default_account"), view->useDefaultAccount() ? QStringLiteral("1") : QStringLiteral("0"));
            writeResult(result);
            return 0;
        });
        root->addCommand(defaultAccount);
    }

    // --- GitHub/UI/Commands -----------------------------------------------------------------
    {
        auto github = std::make_shared<Command>(QStringLiteral("github"), QStringLiteral("GitHub authentication prompts."));

        auto prompt = std::make_shared<Command>(QStringLiteral("prompt"), QStringLiteral("Show authentication prompt."));
        prompt->addOption(stringOption(QStringLiteral("--enterprise-url"), QStringLiteral("GitHub Enterprise URL.")));
        prompt->addOption(stringOption(QStringLiteral("--username"), QStringLiteral("Username or email.")));
        prompt->addOption(Option::flag(QStringLiteral("--basic"), QStringLiteral("Enable username/password (basic) authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--browser"), QStringLiteral("Enable browser-based OAuth authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--device"), QStringLiteral("Enable device code OAuth authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--pat"), QStringLiteral("Enable personal access token authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--all"), QStringLiteral("Enable all available authentication options.")));
        prompt->setHandler([this, &context](const ParseResult &r) {
            const bool all = r.has(QStringLiteral("--all"));
            GitHubCredentialsView::Options o;
            o.showBrowserLogin = all || r.has(QStringLiteral("--browser"));
            o.showDeviceLogin = all || r.has(QStringLiteral("--device"));
            o.showTokenLogin = all || r.has(QStringLiteral("--pat"));
            o.showBasicLogin = all || r.has(QStringLiteral("--basic"));
            const OptString enterpriseUrl = r.value(QStringLiteral("--enterprise-url"));
            if (!isGitHubDotCom(enterpriseUrl)) {
                o.enterpriseUrl = enterpriseUrl;
            }
            o.userName = r.value(QStringLiteral("--username"));
            auto *view = new GitHubCredentialsView(context.sessionManager(), o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            switch (view->selectedMode()) {
            case ProviderAuthMode::Basic:
                result.set(QStringLiteral("mode"), QStringLiteral("basic"));
                result.set(QStringLiteral("username"), view->userName());
                result.set(QStringLiteral("password"), view->password());
                break;
            case ProviderAuthMode::Browser: result.set(QStringLiteral("mode"), QStringLiteral("browser")); break;
            case ProviderAuthMode::Device: result.set(QStringLiteral("mode"), QStringLiteral("device")); break;
            case ProviderAuthMode::Pat:
                result.set(QStringLiteral("mode"), QStringLiteral("pat"));
                result.set(QStringLiteral("pat"), view->token());
                break;
            default: throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values."));
            }
            writeResult(result);
            return 0;
        });
        github->addCommand(prompt);

        auto twoFactor = std::make_shared<Command>(QStringLiteral("2fa"), QStringLiteral("Show two-factor prompt."));
        twoFactor->addOption(Option::flag(QStringLiteral("--sms"), QStringLiteral("Two-factor code was sent via SMS.")));
        twoFactor->setHandler([this, &context](const ParseResult &r) {
            auto *view = new GitHubTwoFactorView(context.sessionManager(), r.has(QStringLiteral("--sms")));
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            result.set(QStringLiteral("code"), view->code());
            writeResult(result);
            return 0;
        });
        github->addCommand(twoFactor);

        auto device = std::make_shared<Command>(QStringLiteral("device"), QStringLiteral("Show device code prompt."));
        device->addOption(stringOption(QStringLiteral("--code"), QStringLiteral("User code.")));
        device->addOption(stringOption(QStringLiteral("--url"), QStringLiteral("Verification URL.")));
        device->setHandler([&context](const ParseResult &r) {
            DeviceCodeView::Options o;
            o.userCode = valueOr(r.value(QStringLiteral("--code")));
            o.verificationUrl = valueOr(r.value(QStringLiteral("--url")));
            o.gitHubHeader = true;
            const auto window = showOrThrow(context, new DeviceCodeView(context.sessionManager(), o));
            return 0;
        });
        github->addCommand(device);

        auto selectAccount = std::make_shared<Command>(QStringLiteral("select-account"), QStringLiteral("Show account selection prompt. Accounts are read line-by-line from standard input."));
        selectAccount->addOption(stringOption(QStringLiteral("--enterprise-url"), QStringLiteral("Enterprise URL.")));
        selectAccount->addOption(Option::flag(QStringLiteral("--no-help"), QStringLiteral("Hide the help link.")));
        selectAccount->setHandler([this, &context](const ParseResult &r) {
            // Read accounts from standard input
            GitHubSelectAccountView::Options o;
            TextReader &in = context.streams().in();
            while (true) {
                const OptString line = in.readLine();
                if (isNullOrWhiteSpace(line)) {
                    break;
                }
                o.accounts.append(line->trimmed());
            }
            o.enterpriseUrl = r.value(QStringLiteral("--enterprise-url"));
            o.showHelpLink = !r.has(QStringLiteral("--no-help"));
            auto *view = new GitHubSelectAccountView(context.sessionManager(), o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            // Upstream writes a null value (an empty one on the wire) for "Add a new account"
            result.set(QStringLiteral("account"), valueOr(view->selectedAccount()));
            writeResult(result);
            return 0;
        });
        github->addCommand(selectAccount);
        root->addCommand(github);
    }

    // --- GitLab/UI/Commands -----------------------------------------------------------------
    {
        auto gitlab = std::make_shared<Command>(QStringLiteral("gitlab"), QStringLiteral("GitLab authentication prompts."));
        auto prompt = std::make_shared<Command>(QStringLiteral("prompt"), QStringLiteral("Show authentication prompt."));
        prompt->addOption(stringOption(QStringLiteral("--url"), QStringLiteral("GitLab instance URL.")));
        prompt->addOption(stringOption(QStringLiteral("--username"), QStringLiteral("Username or email.")));
        prompt->addOption(Option::flag(QStringLiteral("--basic"), QStringLiteral("Enable username/password (basic) authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--browser"), QStringLiteral("Enable browser-based OAuth authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--pat"), QStringLiteral("Enable personal access token authentication.")));
        prompt->addOption(Option::flag(QStringLiteral("--all"), QStringLiteral("Enable all available authentication options.")));
        prompt->setHandler([this, &context](const ParseResult &r) {
            const bool all = r.has(QStringLiteral("--all"));
            GitLabCredentialsView::Options o;
            o.showBrowserLogin = all || r.has(QStringLiteral("--browser"));
            o.showTokenLogin = all || r.has(QStringLiteral("--pat"));
            o.showBasicLogin = all || r.has(QStringLiteral("--basic"));
            const OptString url = r.value(QStringLiteral("--url"));
            if (const std::optional<QUrl> uri = tryCreateUri(url); uri && !isGitLabDotCom(*uri)) {
                o.url = url;
            }
            o.userName = r.value(QStringLiteral("--username"));
            auto *view = new GitLabCredentialsView(context.sessionManager(), o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            switch (view->selectedMode()) {
            case ProviderAuthMode::Basic:
                result.set(QStringLiteral("mode"), QStringLiteral("basic"));
                result.set(QStringLiteral("username"), view->userName());
                result.set(QStringLiteral("password"), view->password());
                break;
            case ProviderAuthMode::Browser: result.set(QStringLiteral("mode"), QStringLiteral("browser")); break;
            case ProviderAuthMode::Pat:
                result.set(QStringLiteral("mode"), QStringLiteral("pat"));
                result.set(QStringLiteral("pat"), view->token());
                result.set(QStringLiteral("username"), view->tokenUserName());
                break;
            default: throw ArgumentOutOfRangeException(QStringLiteral("Specified argument was out of the range of valid values."));
            }
            writeResult(result);
            return 0;
        });
        gitlab->addCommand(prompt);
        root->addCommand(gitlab);
    }

    // --- Atlassian.Bitbucket/UI/Commands ----------------------------------------------------
    {
        auto bitbucket = std::make_shared<Command>(QStringLiteral("bitbucket"), QStringLiteral("Bitbucket authentication prompts."));
        auto prompt = std::make_shared<Command>(QStringLiteral("prompt"), QStringLiteral("Show authentication prompt."));
        prompt->addOption(stringOption(QStringLiteral("--url"), QStringLiteral("Bitbucket Server or Data Center URL")));
        prompt->addOption(stringOption(QStringLiteral("--username"), QStringLiteral("Username or email.")));
        prompt->addOption(Option::flag(QStringLiteral("--show-oauth"), QStringLiteral("Show OAuth option.")));
        prompt->addOption(Option::flag(QStringLiteral("--show-basic"), QStringLiteral("Show username/password option.")));
        prompt->setHandler([this, &context](const ParseResult &r) {
            BitbucketCredentialsView::Options o;
            if (const OptString url = r.value(QStringLiteral("--url")); url && !url->isEmpty()) {
                o.url = QUrl(*url);
            }
            o.userName = r.value(QStringLiteral("--username"));
            o.showOAuth = r.has(QStringLiteral("--show-oauth"));
            o.showBasic = r.has(QStringLiteral("--show-basic"));
            auto *view = new BitbucketCredentialsView(context.sessionManager(), o);
            const auto window = showOrThrow(context, view);
            StringDictionary result;
            switch (view->selectedMode()) {
            case ProviderAuthMode::OAuth: result.set(QStringLiteral("mode"), QStringLiteral("oauth")); break;
            case ProviderAuthMode::Basic:
                result.set(QStringLiteral("mode"), QStringLiteral("basic"));
                result.set(QStringLiteral("username"), view->userName());
                result.set(QStringLiteral("password"), view->password());
                break;
            default: throw OperationCanceledException(QStringLiteral("User cancelled dialog."));
            }
            writeResult(result);
            return 0;
        });
        bitbucket->addCommand(prompt);
        root->addCommand(bitbucket);
    }

    return root;
}

int HelperApplication::runInternal(const QStringList &args)
{
    std::shared_ptr<Command> root = createRootCommand();
    const QString exeName = QFileInfo(m_context.applicationPath()).fileName();
    CommandLineParser parser(root, exeName.isEmpty() ? root->name() : exeName, Constants::gcmInformationalVersion(), m_context.streams().out(), m_context.streams().error());
    parser.setExceptionHandler([this](const std::exception &ex) { return onException(ex); });
    return parser.invoke(args);
}

} // namespace gcm::ui
