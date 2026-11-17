// Port of Microsoft.AzureRepos.Tests/AzureReposHostProviderTests.cs: the 43 methods that run on Linux (the 5
// [WindowsFact] UnconfigureAsync tests check the Git for Windows "manager" helper exception, which does not exist
// here). The MSAL failure theories throw the matching Entra exceptions. Own tests: store/erase in every mode, the
// authority from the request's WWW-Authenticate headers, the public client configuration, identity configuration
// errors, and the `azure-repos` commands.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "azreposmocks.h"

#include "core/cmdline.h"
#include "core/streams.h"
#include "infra/testcommandcontext.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::azrepos;
using namespace gcm::azrepos::test;
using namespace gcm::test;
using entra::EntraAccount;
using entra::EntraAuthResult;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

GitRequest request(std::initializer_list<std::pair<const char *, const char *>> items)
{
    StringDictionary d;
    for (const auto &[k, v] : items) {
        d.set(Q(k), Q(v));
    }
    return GitRequest(d);
}

namespace Env = AzureDevOpsConstants::EnvironmentVariables;

const QString HelperKey = Q("credential.helper");
const QString AzDevUseHttpPathKey = Q("credential.https://dev.azure.com.useHttpPath");
const QString AzDevUseSharedCacheKey = Q("credential.") + AzureDevOpsConstants::GitConfiguration::Credential::UseSharedCache;
const QString OrgName = Q("org");
const QString CommonAuthority = Q("https://login.microsoftonline.com/common");
const QStringList ExpectedScopes = AzureDevOpsConstants::AzureDevOpsDefaultScopes;

QString userKey(const QString &org) { return Q("credential.azrepos:org/%1.username").arg(org); }
QString authorityKey(const QString &org) { return Q("credential.azrepos:org/%1.azureAuthority").arg(org); }

struct Fixture
{
    TestCommandContext context;
    std::shared_ptr<MockAzureDevOpsRestApi> azDevOps = std::make_shared<MockAzureDevOpsRestApi>();
    std::shared_ptr<MockEntraAuthentication> entraAuth = std::make_shared<MockEntraAuthentication>();
    std::shared_ptr<MockAzureDevOpsAuthorityCache> authorityCache = std::make_shared<MockAzureDevOpsAuthorityCache>();
    std::shared_ptr<MockAzureReposBindingManager> userMgr = std::make_shared<MockAzureReposBindingManager>();

    explicit Fixture(const OptString &credentialType = std::nullopt)
    {
        if (credentialType) {
            context.Environment.Variables.set(Env::CredentialType, *credentialType);
        }
    }

    AzureReposHostProvider provider() { return AzureReposHostProvider(context, azDevOps, entraAuth, authorityCache, userMgr); }
};

/// The kinds of failure of the MSAL theories
enum class Failure { Service, Client, Base, UiRequired, Policy, Throttled, Canceled, NonMsal };

[[noreturn]] void throwFailure(Failure kind, const QString &errorCode, int statusCode, const OptString &claims)
{
    const QString message = Q("Test failure");
    switch (kind) {
    case Failure::Service:
        throw entra::EntraServiceException(errorCode, message, statusCode, claims);
    case Failure::Client:
        throw entra::EntraClientException(errorCode, message);
    case Failure::Base:
        throw entra::EntraException(errorCode, message);
    case Failure::UiRequired:
        throw entra::EntraUiRequiredException(errorCode, message);
    case Failure::Policy:
        throw entra::IntuneAppProtectionPolicyRequiredException(errorCode, message);
    case Failure::Throttled:
        throw entra::EntraThrottledServiceException(errorCode, message);
    case Failure::Canceled:
        throw OperationCanceledException(message);
    case Failure::NonMsal:
        throw InvalidOperationException(message);
    }
    throw Exception(message);
}

const char *expectedTypeName(Failure kind)
{
    switch (kind) {
    case Failure::Service:
        return "MsalServiceException";
    case Failure::Client:
        return "MsalClientException";
    case Failure::Base:
        return "MsalException";
    case Failure::UiRequired:
        return "MsalUiRequiredException";
    case Failure::Policy:
        return "IntuneAppProtectionPolicyRequiredException";
    case Failure::Throttled:
        return "MsalThrottledServiceException";
    case Failure::Canceled:
        return "OperationCanceledException";
    case Failure::NonMsal:
        return "InvalidOperationException";
    }
    return "";
}

} // namespace

class AzureReposHostProviderTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- Settings ----

    void getUseSharedCache_NoConfiguration_ReturnsTrue()
    {
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        QVERIFY(provider.getUseSharedCache());
    }

    void getUseSharedCache_GitConfigFalse_ReturnsFalse()
    {
        TestCommandContext context;
        context.Git.Configuration.Global[AzDevUseSharedCacheKey] = QStringList{Q("false")};
        AzureReposHostProvider provider(context);
        QVERIFY(!provider.getUseSharedCache());
    }

    void getUseSharedCache_EnvironmentOverridesGitConfig()
    {
        TestCommandContext context;
        context.Git.Configuration.Global[AzDevUseSharedCacheKey] = QStringList{Q("false")};
        context.Environment.Variables.set(Env::UseSharedCache, Q("true"));
        AzureReposHostProvider provider(context);
        QVERIFY(provider.getUseSharedCache());
    }

    void usePersonalAccessTokens_data()
    {
        QTest::addColumn<OptString>("environmentValue");
        QTest::addColumn<OptString>("gitConfigValue");
        QTest::addColumn<bool>("expected");
        const OptString none;
        auto row = [](const OptString &env, const OptString &cfg, bool expected) {
            QTest::addRow("env=%s cfg=%s", env ? qPrintable(Q("'%1'").arg(*env)) : "null", cfg ? qPrintable(Q("'%1'").arg(*cfg)) : "null")
                << env << cfg << expected;
        };
        row(none, none, false);
        row(none, Q("oauth"), false);
        row(none, Q("OAUTH"), false);
        row(none, Q("pat"), true);
        row(none, Q("PAT"), true);
        row(none, Q("invalid"), false);
        row(none, Q(""), false);
        row(none, Q(" "), false);
        row(Q("oauth"), none, false);
        row(Q("OAUTH"), none, false);
        row(Q("pat"), none, true);
        row(Q("PAT"), none, true);
        row(Q("invalid"), none, false);
        row(Q(""), none, false);
        row(Q(" "), none, false);
        row(Q("pat"), Q("oauth"), true);
        row(Q("oauth"), Q("pat"), false);
        row(Q("invalid"), Q("pat"), false);
        row(Q(""), Q("pat"), false);
    }

    void usePersonalAccessTokens()
    {
        QFETCH(OptString, environmentValue);
        QFETCH(OptString, gitConfigValue);
        QFETCH(bool, expected);
        TestCommandContext context;
        if (gitConfigValue) {
            context.Git.Configuration.Global[Q("credential.azreposCredentialType")] = QStringList{*gitConfigValue};
        }
        if (environmentValue) {
            context.Environment.Variables.set(Env::CredentialType, *environmentValue);
        }
        AzureReposHostProvider provider(context);
        QCOMPARE(provider.usePersonalAccessTokens(), expected);
    }

    // ---- IsSupported ----

    void isSupported_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("host");
        QTest::addColumn<OptString>("path");
        QTest::addColumn<bool>("expected");
        // We report that we support unencrypted HTTP so that we can fail and show a helpful error message in getCredential
        QTest::newRow("AzureHost_UnencryptedHttp_ReturnsTrue") << Q("http") << Q("dev.azure.com") << OptString(Q("org/proj/_git/repo")) << true;
        QTest::newRow("VisualStudioHost_UnencryptedHttp_ReturnsTrue") << Q("http") << Q("org.visualstudio.com") << OptString() << true;
        QTest::newRow("AzureHost_WithPath_ReturnsTrue") << Q("https") << Q("dev.azure.com") << OptString(Q("org/proj/_git/repo")) << true;
        QTest::newRow("AzureHost_MissingPath_ReturnsTrue") << Q("https") << Q("dev.azure.com") << OptString() << true;
        QTest::newRow("VisualStudioHost_ReturnsTrue") << Q("https") << Q("org.visualstudio.com") << OptString() << true;
        QTest::newRow("VisualStudioHost_MissingOrgInHost_ReturnsFalse") << Q("https") << Q("visualstudio.com") << OptString() << false;
        QTest::newRow("NonAzureRepos_ReturnsFalse") << Q("https") << Q("example.com") << OptString(Q("org/proj/_git/repo")) << false;
    }

    void isSupported()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, host);
        QFETCH(OptString, path);
        QFETCH(bool, expected);
        StringDictionary d;
        d.set(Q("protocol"), protocol);
        d.set(Q("host"), host);
        if (path) {
            d.set(Q("path"), *path);
        }
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        QCOMPARE(provider.isSupported(GitRequest(d)), expected);
    }

    // ---- GetCredential: OAuth access tokens ----

    void getCredential_UnencryptedHttp_ThrowsException()
    {
        Fixture f;
        AzureReposHostProvider provider = f.provider();
        QVERIFY_THROWS_EXCEPTION(Exception, provider.getCredential(request({{"protocol", "http"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}})));
    }

    void getCredential_JwtMode_CachedAuthority_VsComUrlUser_ReturnsCredential()
    {
        const QString urlAccount = Q("jane.doe");
        const EntraAccount expectedAccount{Q("account-id"), urlAccount};
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.azDevOps->Authorities = {{Q("https://org.visualstudio.com/"), CommonAuthority}};
        f.entraAuth->Accounts = QList<EntraAccount>{expectedAccount};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, expectedAccount, [&] { return createAuthResult(urlAccount, Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "org.visualstudio.com"}, {"username", "jane.doe"}}));
        QVERIFY(result.credential());
        QCOMPARE(result.credential()->account, urlAccount);
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
    }

    void getCredential_JwtMode_CachedAuthority_DevAzureUrlUser_ReturnsCredential()
    {
        const QString urlAccount = Q("jane.doe");
        const EntraAccount expectedAccount{Q("account-id"), urlAccount};
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.azDevOps->Authorities = {{Q("https://dev.azure.com/org"), CommonAuthority}};
        f.entraAuth->Accounts = QList<EntraAccount>{expectedAccount};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, expectedAccount, [&] { return createAuthResult(urlAccount, Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result =
            provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/project/_git/repo"}, {"username", "jane.doe"}}));
        QCOMPARE(result.credential()->account, urlAccount);
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
    }

    void getCredential_JwtMode_CachedAuthority_DevAzureUrlOrgName_ReturnsCredential()
    {
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.azDevOps->Authorities = {{Q("https://dev.azure.com/org"), CommonAuthority}};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt, [] { return createAuthResult(Q("jane.doe"), Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"username", "org"}}));
        QCOMPARE(result.credential()->account, Q("jane.doe"));
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
    }

    void getCredential_JwtMode_CachedAuthority_NoUser_ReturnsCredential()
    {
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
    }

    void getCredential_JwtMode_CachedAuthority_BoundUser_ReturnsCredential()
    {
        const EntraAccount expectedAccount{Q("account-id"), Q("john.doe")};
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.entraAuth->Accounts = QList<EntraAccount>{expectedAccount};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, expectedAccount, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, AzureReposBinding{OrgName, Q("john.doe"), std::nullopt}}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
    }

    void getCredential_JwtMode_NoCachedAuthority_NoUser_ReturnsCredential()
    {
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.azDevOps->Authorities = {{Q("https://dev.azure.com/org"), CommonAuthority}};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{std::nullopt, std::nullopt}};
        f.authorityCache->ExpectedUpdates = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("ACCESS-TOKEN"));
        QCOMPARE(f.authorityCache->Updates, (QList<std::pair<QString, QString>>{{OrgName, CommonAuthority}}));
    }

    // Own test: the authority in the request's WWW-Authenticate headers wins over the cache (which is not consulted)
    void getCredential_JwtMode_WwwAuthHeader_UsesHeaderAuthority()
    {
        const QString authority = Q("https://login.microsoftonline.com/tenant1");
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.entraAuth->setupTokenForUser(ExpectedScopes, authority, std::nullopt, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        MultiDictionary d;
        d[Q("protocol")] = QStringList{Q("https")};
        d[Q("host")] = QStringList{Q("dev.azure.com")};
        d[Q("path")] = QStringList{Q("org/proj/_git/repo")};
        d[Q("wwwauth")] = QStringList{Q("Basic realm=\"x\""), Q("Bearer authorization_uri=") + authority};
        AzureReposHostProvider provider = f.provider();
        QCOMPARE(provider.getCredential(GitRequest(d)).credential()->password, Q("ACCESS-TOKEN"));
    }

    // ---- GetCredential: Entra failures and the legacy client hint ----

    void getCredential_Failure_data()
    {
        QTest::addColumn<bool>("usePat");
        QTest::addColumn<bool>("useLegacyClient");
        QTest::addColumn<Failure>("kind");
        QTest::addColumn<QString>("errorCode");
        QTest::addColumn<int>("statusCode");
        QTest::addColumn<OptString>("claims");
        QTest::addColumn<bool>("expectWarning");
        const QString invalidClient = entra::EntraError::InvalidClient, unauthorizedClient = entra::EntraError::UnauthorizedClient;
        auto row = [](const char *test, bool usePat, bool legacy, Failure kind, const QString &code, int status, const OptString &claims, bool warn) {
            QTest::addRow("%s pat=%d legacy=%d %s %d %s", test, usePat, legacy, qPrintable(code), status, claims ? qPrintable(Q("'%1'").arg(QString(*claims).replace(Q("\t"), Q("\\t")).replace(Q("\r"), Q("\\r")).replace(Q("\n"), Q("\\n")))) : "null")
                << usePat << legacy << kind << code << status << claims << warn;
        };
        // MsalServiceFailure_WarnsOnlyForClientConfiguration
        const QList<std::tuple<bool, QString, bool>> serviceRows = {
            {false, invalidClient, true}, {false, unauthorizedClient, true}, {true, invalidClient, false}, {true, unauthorizedClient, false},
            {false, Q("authentication_canceled"), false}, {false, Q("access_denied"), false}, {false, Q("authorization_declined"), false},
            {false, Q("invalid_request"), false}, {false, Q("invalid_grant"), false}, {false, Q("interaction_required"), false},
            {false, Q("invalid_scope"), false}, {false, Q("temporarily_unavailable"), false}, {false, Q("test_error"), false}};
        for (const auto &[legacy, code, warn] : serviceRows) {
            for (const bool usePat : {false, true}) {
                row("ServiceFailure", usePat, legacy, Failure::Service, code, 0, std::nullopt, warn);
            }
        }
        for (const bool usePat : {false, true}) {
            // MsalClientFailure_DoesNotWarn
            for (const QString &code : {invalidClient, unauthorizedClient, Q("authentication_canceled"), Q("redirect_uri_validation_failed"), Q("code_expired")}) {
                row("ClientFailure", usePat, false, Failure::Client, code, 0, std::nullopt, false);
            }
            // MsalBaseFailure_DoesNotWarn, MsalUiRequiredFailure_DoesNotWarn
            for (const QString &code : {invalidClient, unauthorizedClient}) {
                row("BaseFailure", usePat, false, Failure::Base, code, 0, std::nullopt, false);
                row("UiRequiredFailure", usePat, false, Failure::UiRequired, code, 0, std::nullopt, false);
            }
            // MsalPolicyFailure_DoesNotWarn, MsalThrottledFailure_DoesNotWarn
            row("PolicyFailure", usePat, false, Failure::Policy, unauthorizedClient, 0, std::nullopt, false);
            row("ThrottledFailure", usePat, false, Failure::Throttled, invalidClient, 0, std::nullopt, false);
            // MsalRetryableFailure_DoesNotWarn
            for (const int status : {408, 429, 500}) {
                row("RetryableFailure", usePat, false, Failure::Service, invalidClient, status, std::nullopt, false);
            }
            // MsalClaimsFailure_WarnsOnlyWithoutClaims
            row("ClaimsFailure", usePat, false, Failure::Service, invalidClient, 400, std::nullopt, true);
            row("ClaimsFailure", usePat, false, Failure::Service, invalidClient, 400, Q(""), true);
            row("ClaimsFailure", usePat, false, Failure::Service, invalidClient, 400, Q(" \t\r\n"), true);
            row("ClaimsFailure", usePat, false, Failure::Service, invalidClient, 400, Q("CLAIMS"), false);
            // UserCancellation_DoesNotWarn, NonMsalFailure_DoesNotWarn
            row("UserCancellation", usePat, false, Failure::Canceled, QString(), 0, std::nullopt, false);
            row("NonMsalFailure", usePat, false, Failure::NonMsal, QString(), 0, std::nullopt, false);
        }
    }

    // AssertUserCredentialFailureAsync
    void getCredential_Failure()
    {
        QFETCH(bool, usePat);
        QFETCH(bool, useLegacyClient);
        QFETCH(Failure, kind);
        QFETCH(QString, errorCode);
        QFETCH(int, statusCode);
        QFETCH(OptString, claims);
        QFETCH(bool, expectWarning);

        Fixture f(usePat ? AzureDevOpsConstants::PatCredentialType : AzureDevOpsConstants::OAuthCredentialType);
        if (useLegacyClient) {
            f.context.Environment.Variables.set(Env::UseLegacyClientId, Q("true"));
        }
        if (usePat) {
            f.azDevOps->Authorities = {{Q("https://dev.azure.com/org"), CommonAuthority}};
        }
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt,
                                       [=]() -> EntraAuthResult { throwFailure(kind, errorCode, statusCode, claims); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        QStringList clientIds;
        auto factory = [&](const entra::PublicClientConfig &config) {
            clientIds.append(config.clientId);
            f.entraAuth->Config = config;
            return std::static_pointer_cast<entra::IEntraAuthentication>(f.entraAuth);
        };
        AzureReposHostProvider provider(f.context, f.azDevOps, AzureReposHostProvider::EntraAuthFactory(factory), f.authorityCache, f.userMgr);

        try {
            provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            // Assert.Same(failure, exception): the same exception, passed through unchanged
            QCOMPARE(QLatin1String(ex.typeName()), QLatin1String(expectedTypeName(kind)));
            QCOMPARE(ex.message(), Q("Test failure"));
            if (const auto *entraEx = dynamic_cast<const entra::EntraException *>(&ex)) {
                QCOMPARE(entraEx->errorCode(), errorCode);
            }
        }
        QCOMPARE(clientIds, QStringList{useLegacyClient ? AzureDevOpsConstants::LegacyClientId : AzureDevOpsConstants::ClientId});
        QCOMPARE(f.entraAuth->tokenForUserCalls(ExpectedScopes, CommonAuthority, std::nullopt), 1);
        if (expectWarning) {
            QCOMPARE(f.context.Console.WrittenMessages,
                     (QStringList{Q("Authentication using the new GCM Entra application failed. To retry this command with the legacy Entra application, "
                                    "set 'credential.azreposUseLegacyClientId' to 'true' in Git configuration."),
                                  Q("If that succeeds, please report the original failure at %1").arg(Constants::HelpUrls::GcmNewIssue)}));
        } else {
            QVERIFY(f.context.Console.WrittenMessages.isEmpty());
        }
    }

    // Own test: the public client configuration passed to the Entra authentication factory
    void getCredential_PublicClientConfig()
    {
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        f.context.Git.Configuration.Global[AzDevUseSharedCacheKey] = QStringList{Q("false")};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        f.authorityCache->Authorities = {{OrgName, CommonAuthority}};
        f.userMgr->Bindings = {{OrgName, std::nullopt}};
        QList<entra::PublicClientConfig> configs;
        auto factory = [&](const entra::PublicClientConfig &config) {
            configs.append(config);
            return std::static_pointer_cast<entra::IEntraAuthentication>(f.entraAuth);
        };
        AzureReposHostProvider provider(f.context, f.azDevOps, AzureReposHostProvider::EntraAuthFactory(factory), f.authorityCache, f.userMgr);
        provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(configs.size(), 1); // created once (Lazy)
        QCOMPARE(configs[0].clientId, AzureDevOpsConstants::ClientId);
        QVERIFY(configs[0].isMsaPassthroughEnabled);
        QVERIFY(!configs[0].useSharedCache);
        QVERIFY(configs[0].supportsWindowsBroker && configs[0].supportsMacBroker && configs[0].supportsLinuxBroker);

        // The legacy client: no broker on Mac and Linux
        f.context.Git.Configuration.Global[Q("credential.azreposUseLegacyClientId")] = QStringList{Q("1")};
        AzureReposHostProvider legacy(f.context, f.azDevOps, AzureReposHostProvider::EntraAuthFactory(factory), f.authorityCache, f.userMgr);
        legacy.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(configs.size(), 2);
        QCOMPARE(configs[1].clientId, AzureDevOpsConstants::LegacyClientId);
        QVERIFY(configs[1].supportsWindowsBroker && !configs[1].supportsMacBroker && !configs[1].supportsLinuxBroker);
    }

    // ---- GetCredential: personal access tokens ----

    void getCredential_PatMode_NoExistingPat_GeneratesCredential_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("userName");
        QTest::newRow("PatMode_OrgInUserName_NoExistingPat_GeneratesCredential") << QString() << Q("org");
        QTest::newRow("PatMode_NoExistingPat_GeneratesCredential") << Q("org/proj/_git/repo") << QString();
    }

    void getCredential_PatMode_NoExistingPat_GeneratesCredential()
    {
        QFETCH(QString, path);
        QFETCH(QString, userName);
        Fixture f(AzureDevOpsConstants::PatCredentialType);
        f.azDevOps->Authorities = {{Q("https://dev.azure.com/org"), CommonAuthority}};
        f.azDevOps->Pats = {{Q("https://dev.azure.com/org"), Q("ACCESS-TOKEN"), Q("PERSONAL-ACCESS-TOKEN")}};
        f.entraAuth->setupTokenForUser(ExpectedScopes, CommonAuthority, std::nullopt, [] { return createAuthResult(Q("john.doe"), Q("ACCESS-TOKEN")); });
        StringDictionary d;
        d.set(Q("protocol"), Q("https"));
        d.set(Q("host"), Q("dev.azure.com"));
        if (!path.isEmpty()) {
            d.set(Q("path"), path);
        }
        if (!userName.isEmpty()) {
            d.set(Q("username"), userName);
        }
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(GitRequest(d));
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("PERSONAL-ACCESS-TOKEN"));
        // Own check: the PAT scopes
        QCOMPARE(f.azDevOps->PatScopes, (QList<QStringList>{{Q("vso.code_write"), Q("vso.packaging")}}));
    }

    void getCredential_PatMode_ExistingPat_ReturnsExistingCredential()
    {
        Fixture f(AzureDevOpsConstants::PatCredentialType);
        f.context.CredentialStore.add(Q("https://dev.azure.com/org"), Q("john.doe"), Q("PERSONAL-ACCESS-TOKEN"));
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("PERSONAL-ACCESS-TOKEN"));
    }

    // ---- GetCredential: identities ----

    void getCredential_ManagedIdentity_ReturnsManagedIdCredential()
    {
        const QString managedIdentity = Q("22222222-2222-2222-2222-222222222222");
        const entra::ManagedIdentity expectedIdentity = entra::ManagedIdentity::create(managedIdentity);
        Fixture f;
        f.context.Environment.Variables.set(Env::ManagedIdentity, managedIdentity);
        f.entraAuth->IdentityResult = EntraAuthResult{Q("MANAGED-IDENTITY-TOKEN"), std::nullopt};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, expectedIdentity.id());
        QCOMPARE(result.credential()->password, Q("MANAGED-IDENTITY-TOKEN"));
        QCOMPARE(f.entraAuth->ManagedIdentityCalls.size(), 1);
        QCOMPARE(f.entraAuth->ManagedIdentityCalls[0].first, AzureDevOpsConstants::AzureDevOpsResourceId);
        QCOMPARE(f.entraAuth->ManagedIdentityCalls[0].second.id(), expectedIdentity.id());
    }

    void getCredential_InvalidManagedIdentity_ThrowsException()
    {
        Fixture f;
        f.context.Environment.Variables.set(Env::ManagedIdentity, Q("not-a-managed-identity"));
        AzureReposHostProvider provider = f.provider();
        QVERIFY_THROWS_EXCEPTION(ArgumentException,
                                 provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}})));
    }

    void getCredential_WorkloadFederation_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<StringDictionary>("variables");
        QTest::addColumn<QByteArray>("assertionFile");
        QTest::addColumn<entra::WorkloadFederationScenario>("expectedScenario");
        const QString tenantId = Q("00000000-0000-0000-0000-000000000000");
        const QString clientId = Q("11111111-1111-1111-1111-111111111111");
        auto vars = [&](std::initializer_list<std::pair<QString, QString>> extra) {
            StringDictionary d;
            d.set(Env::WorkloadFederationTenantId, tenantId);
            d.set(Env::WorkloadFederationClientId, clientId);
            for (const auto &[k, v] : extra) {
                d.set(k, v);
            }
            return d;
        };
        QTest::newRow("WorkloadFederation_Generic_ReturnsFederationOptions")
            << Q("generic") << vars({{Env::WorkloadFederationAssertion, Q("CLIENT-ASSERTION")}}) << QByteArray() << entra::WorkloadFederationScenario::Generic;
        QTest::newRow("WorkloadFederation_GenericFileAssertion_ReadsFromFile")
            << Q("generic") << vars({{Env::WorkloadFederationAssertion, Q("file:///tmp/assertion-token.txt")}}) << QByteArray("CLIENT-ASSERTION-FROM-FILE")
            << entra::WorkloadFederationScenario::Generic;
        QTest::newRow("WorkloadFederation_MI_ReturnsFederationOptions")
            << Q("managedidentity") << vars({{Env::WorkloadFederationManagedIdentity, Q("22222222-2222-2222-2222-222222222222")}}) << QByteArray()
            << entra::WorkloadFederationScenario::ManagedIdentity;
        QTest::newRow("WorkloadFederation_GitHubActions_ReturnsFederationOptions")
            << Q("githubactions")
            << vars({{Constants::EnvironmentVariables::GitHubActionsTokenRequestUrl, Q("https://token.actions.example.com/oidc/example?param=value")},
                     {Constants::EnvironmentVariables::GitHubActionsTokenRequestToken, Q("OIDC-TOKEN")}})
            << QByteArray() << entra::WorkloadFederationScenario::GitHubActions;
    }

    void getCredential_WorkloadFederation()
    {
        QFETCH(QString, scenario);
        QFETCH(StringDictionary, variables);
        QFETCH(QByteArray, assertionFile);
        QFETCH(entra::WorkloadFederationScenario, expectedScenario);
        Fixture f;
        f.context.Environment.Variables.set(Env::WorkloadFederation, scenario);
        for (const auto &[k, v] : variables) {
            f.context.Environment.Variables.set(k, v);
        }
        if (!assertionFile.isNull()) {
            f.context.FileSystem.Files[Q("/tmp/assertion-token.txt")] = assertionFile;
        }
        f.entraAuth->IdentityResult = EntraAuthResult{Q("FEDERATED-IDENTITY-TOKEN"), std::nullopt};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, Q("11111111-1111-1111-1111-111111111111"));
        QCOMPARE(result.credential()->password, Q("FEDERATED-IDENTITY-TOKEN"));

        QCOMPARE(f.entraAuth->WorkloadFederationCalls.size(), 1);
        const auto &[scopes, fed] = f.entraAuth->WorkloadFederationCalls[0];
        QCOMPARE(scopes, ExpectedScopes);
        QCOMPARE(fed.scenario, expectedScenario);
        QCOMPARE(fed.tenantId, Q("00000000-0000-0000-0000-000000000000"));
        QCOMPARE(fed.clientId, Q("11111111-1111-1111-1111-111111111111"));
        QCOMPARE(fed.audience(), entra::WorkloadFederationOptions::DefaultAudience);
        switch (expectedScenario) {
        case entra::WorkloadFederationScenario::Generic:
            QCOMPARE(fed.genericClientAssertion, assertionFile.isNull() ? Q("CLIENT-ASSERTION") : Q("CLIENT-ASSERTION-FROM-FILE"));
            break;
        case entra::WorkloadFederationScenario::ManagedIdentity:
            QCOMPARE(fed.managedIdentityId, Q("22222222-2222-2222-2222-222222222222"));
            break;
        case entra::WorkloadFederationScenario::GitHubActions:
            QCOMPARE(absoluteUri(fed.gitHubTokenRequestUrl), Q("https://token.actions.example.com/oidc/example?param=value"));
            QCOMPARE(fed.gitHubTokenRequestToken, Q("OIDC-TOKEN"));
            break;
        }
    }

    void getCredential_ServicePrincipal_ReturnsSPCredential()
    {
        const QString tenantId = Q("78B1822F-107D-40A3-A29C-AB68D8066074");
        const QString clientId = Q("49B4DC1A-58A8-4EEE-A81B-616A40D0BA64");
        Fixture f;
        f.context.Environment.Variables.set(Env::ServicePrincipalId, tenantId + Q("/") + clientId);
        f.context.Environment.Variables.set(Env::ServicePrincipalSecret, Q("CLIENT-SECRET"));
        f.entraAuth->IdentityResult = EntraAuthResult{Q("SP-TOKEN"), std::nullopt};
        AzureReposHostProvider provider = f.provider();
        const GitResponse result = provider.getCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
        QCOMPARE(result.credential()->account, clientId);
        QCOMPARE(result.credential()->password, Q("SP-TOKEN"));
        QCOMPARE(f.entraAuth->ServicePrincipalCalls.size(), 1);
        const auto &[scopes, sp] = f.entraAuth->ServicePrincipalCalls[0];
        QCOMPARE(scopes, ExpectedScopes);
        QCOMPARE(sp.tenantId, tenantId);
        QCOMPARE(sp.id, clientId);
        QCOMPARE(sp.clientSecret, OptString(Q("CLIENT-SECRET")));
        QVERIFY(!sp.certificate);
    }

    // Own test: configuration errors of the identities are reported, then the user credential flow runs (here it
    // fails on the unencrypted remote before any network or Entra call)
    void getCredential_IdentityConfigurationErrors_data()
    {
        QTest::addColumn<StringDictionary>("variables");
        QTest::addColumn<QString>("expectedError");
        auto vars = [](std::initializer_list<std::pair<QString, QString>> items) {
            StringDictionary d;
            for (const auto &[k, v] : items) {
                d.set(k, v);
            }
            return d;
        };
        QTest::newRow("sp missing client") << vars({{Env::ServicePrincipalId, Q("tenant")}})
                                           << Q("unable to use configured service principal - missing client ID in configuration");
        QTest::newRow("sp missing tenant") << vars({{Env::ServicePrincipalId, Q("/client")}})
                                           << Q("unable to use configured service principal - missing tenant ID in configuration");
        QTest::newRow("wif no ids") << vars({{Env::WorkloadFederation, Q("generic")}}) << Q("both client ID and tenant ID are required for workload federation");
        QTest::newRow("wif no assertion") << vars({{Env::WorkloadFederation, Q("generic")}, {Env::WorkloadFederationClientId, Q("c")}, {Env::WorkloadFederationTenantId, Q("t")}})
                                          << Q("assertion is required for the generic workload federation scenario");
        QTest::newRow("wif assertion file missing")
            << vars({{Env::WorkloadFederation, Q("generic")}, {Env::WorkloadFederationClientId, Q("c")}, {Env::WorkloadFederationTenantId, Q("t")},
                     {Env::WorkloadFederationAssertion, Q("file:///missing")}})
            << Q("assertion file not found: /missing");
        QTest::newRow("wif no mi") << vars({{Env::WorkloadFederation, Q("mi")}, {Env::WorkloadFederationClientId, Q("c")}, {Env::WorkloadFederationTenantId, Q("t")}})
                                   << Q("managed identity is required for the managed identity workload federation scenario");
        QTest::newRow("wif no github url")
            << vars({{Env::WorkloadFederation, Q("github")}, {Env::WorkloadFederationClientId, Q("c")}, {Env::WorkloadFederationTenantId, Q("t")}})
            << Q("unable to get valid token request URL from environment variable for the GitHub Actions workload federation scenario");
        QTest::newRow("wif no github token")
            << vars({{Env::WorkloadFederation, Q("github")}, {Env::WorkloadFederationClientId, Q("c")}, {Env::WorkloadFederationTenantId, Q("t")},
                     {Constants::EnvironmentVariables::GitHubActionsTokenRequestUrl, Q("https://example.com/token")}})
            << Q("unable to get valid token request token from environment variable for the GitHub Actions workload federation scenario");
    }

    void getCredential_IdentityConfigurationErrors()
    {
        QFETCH(StringDictionary, variables);
        QFETCH(QString, expectedError);
        Fixture f(AzureDevOpsConstants::OAuthCredentialType);
        for (const auto &[k, v] : variables) {
            f.context.Environment.Variables.set(k, v);
        }
        AzureReposHostProvider provider = f.provider();
        try {
            provider.getCredential(request({{"protocol", "http"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}}));
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().startsWith(Q("Unencrypted HTTP is not recommended for Azure Repos.")));
        }
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{expectedError});
    }

    // ---- Store / erase (own tests) ----

    void storeErase_PatMode()
    {
        TestCommandContext context;
        context.Environment.Variables.set(Env::CredentialType, Q("pat"));
        AzureReposHostProvider provider(context);
        provider.storeCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}, {"username", "org"}, {"password", "pat1"}}));
        provider.storeCredential(request({{"protocol", "https"}, {"host", "org.visualstudio.com"}, {"path", "proj/_git/repo"}, {"username", "alice"}, {"password", "pat2"}}));
        QCOMPARE(context.CredentialStore.count(), 2);
        QCOMPARE(context.CredentialStore.tryGet(Q("https://dev.azure.com/org"), Q("org"))->password, Q("pat1"));
        QCOMPARE(context.CredentialStore.tryGet(Q("https://org.visualstudio.com/proj/_git/repo"), Q("alice"))->password, Q("pat2"));
        // dev.azure.com: erased whatever the account; visualstudio.com: only the given account
        provider.eraseCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/other/_git/repo"}, {"username", "someone"}}));
        provider.eraseCredential(request({{"protocol", "https"}, {"host", "org.visualstudio.com"}, {"path", "proj/_git/repo"}, {"username", "bob"}}));
        QCOMPARE(context.CredentialStore.count(), 1);
        QVERIFY(context.CredentialStore.tryGet(Q("https://org.visualstudio.com/proj/_git/repo"), Q("alice")));
    }

    void storeErase_OAuthMode_SignsInAndOut()
    {
        TestCommandContext context;
        context.Git.Configuration.Global[authorityKey(OrgName)] = QStringList{CommonAuthority};
        AzureReposHostProvider provider(context);
        provider.storeCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}, {"username", "alice"}, {"password", "token"}}));
        QCOMPARE(context.Git.Configuration.Global.value(userKey(OrgName)), QStringList{Q("alice")});
        QCOMPARE(context.CredentialStore.count(), 0);
        provider.eraseCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}, {"username", "alice"}}));
        // Signed out: "do not inherit" locally, and the cached authority is gone
        QCOMPARE(context.Git.Configuration.Local.value(userKey(OrgName)), QStringList{AzureReposBinding::NoInherit});
        QVERIFY(!context.Git.Configuration.Global.contains(authorityKey(OrgName)));
    }

    void storeErase_Identities_DoNothing_data()
    {
        QTest::addColumn<QString>("variable");
        QTest::addColumn<QString>("value");
        QTest::newRow("managed identity") << QString(Env::ManagedIdentity) << Q("system");
        QTest::newRow("service principal") << QString(Env::ServicePrincipalId) << Q("tenant/client");
        QTest::newRow("workload federation") << QString(Env::WorkloadFederation) << Q("generic");
    }

    void storeErase_Identities_DoNothing()
    {
        QFETCH(QString, variable);
        QFETCH(QString, value);
        Fixture f(AzureDevOpsConstants::PatCredentialType);
        f.context.Environment.Variables.set(variable, value);
        f.context.Environment.Variables.set(Env::WorkloadFederationClientId, Q("c"));
        f.context.Environment.Variables.set(Env::WorkloadFederationTenantId, Q("t"));
        f.context.Environment.Variables.set(Env::WorkloadFederationAssertion, Q("a"));
        AzureReposHostProvider provider = f.provider();
        provider.storeCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}, {"username", "x"}, {"password", "y"}}));
        provider.eraseCredential(request({{"protocol", "https"}, {"host", "dev.azure.com"}, {"path", "org/proj/_git/repo"}, {"username", "x"}}));
        QCOMPARE(f.context.CredentialStore.count(), 0);
        QCOMPARE(f.context.Git.Configuration.SetCallCount + f.context.Git.Configuration.UnsetCallCount, 0);
    }

    // ---- Configure / unconfigure ----

    void configure_data()
    {
        QTest::addColumn<OptString>("existing");
        QTest::newRow("ConfigureAsync_UseHttpPathSetTrue_DoesNothing") << OptString(Q("true"));
        QTest::newRow("ConfigureAsync_UseHttpPathSetFalse_SetsUseHttpPathTrue") << OptString(Q("false"));
        QTest::newRow("ConfigureAsync_UseHttpPathUnset_SetsUseHttpPathTrue") << OptString();
    }

    void configure()
    {
        QFETCH(OptString, existing);
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        if (existing) {
            context.Git.Configuration.Global[AzDevUseHttpPathKey] = QStringList{*existing};
        }
        provider.configure(ConfigurationTarget::User);
        QCOMPARE(context.Git.Configuration.Global.count(), 1);
        QCOMPARE(context.Git.Configuration.Global.value(AzDevUseHttpPathKey), QStringList{Q("true")});
        QCOMPARE(context.Git.Configuration.SetCallCount, existing == OptString(Q("true")) ? 0 : 1);
    }

    void unconfigure_UseHttpPathSet_RemovesEntry()
    {
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        context.Git.Configuration.Global[AzDevUseHttpPathKey] = QStringList{Q("true")};
        provider.unconfigure(ConfigurationTarget::User);
        QVERIFY(context.Git.Configuration.Global.isEmpty());
    }

    // Own test: on Linux the "manager" helper does not keep the system entry (upstream's exception is Windows only)
    void unconfigure_System_ManagerHelper_RemovesEntry()
    {
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        context.Git.Configuration.System[HelperKey] = QStringList{Q("manager")};
        context.Git.Configuration.System[AzDevUseHttpPathKey] = QStringList{Q("true")};
        provider.unconfigure(ConfigurationTarget::System);
        QVERIFY(!context.Git.Configuration.System.contains(AzDevUseHttpPathKey));
    }

    // ---- TryGetAuthorityFromHeaders ----

    void tryGetAuthorityFromHeaders_data()
    {
        QTest::addColumn<bool>("expectedResult");
        QTest::addColumn<QString>("expectedAuthority");
        QTest::addColumn<QStringList>("headers");
        const QString tenant = Q("https://login.microsoftonline.com/79c4d065-d599-442e-b0ea-c4ab36ad63c3");
        const QString basicVs = Q("Basic realm=\"https://tfsprodwcus0.app.visualstudio.com/\"");
        QTest::newRow("empty") << false << QString() << QStringList{Q("")};
        QTest::newRow("blank") << false << QString() << QStringList{Q("   ")};
        QTest::newRow("none") << false << QString() << QStringList{};
        QTest::newRow("basic") << false << QString() << QStringList{Q("Basic realm=\"test\"")};
        QTest::newRow("basic vs") << false << QString() << QStringList{basicVs};
        QTest::newRow("tfs") << false << QString() << QStringList{Q("TFS-Federated")};
        QTest::newRow("bearer") << true << tenant << QStringList{Q("Bearer authorization_uri=") + tenant};
        QTest::newRow("bearer case") << true << tenant << QStringList{Q("bEArEr auThORizAtIoN_uRi=") + tenant};
        QTest::newRow("double quoted") << true << tenant << QStringList{Q("\"Bearer authorization_uri=%1\"").arg(tenant)};
        QTest::newRow("single quoted") << true << tenant << QStringList{Q("'Bearer authorization_uri=%1'").arg(tenant)};
        QTest::newRow("first of several") << true << Q("https://login.microsoftonline.com/tenant1")
                                          << QStringList{Q("Bearer authorization_uri=https://login.microsoftonline.com/tenant1"),
                                                         Q("Bearer authorization_uri=https://login.microsoftonline.com/tenant2"),
                                                         Q("Bearer authorization_uri=https://login.microsoftonline.com/tenant3")};
        QTest::newRow("bearer first") << true << tenant << QStringList{Q("Bearer authorization_uri=") + tenant, basicVs, Q("TFS-Federated")};
        QTest::newRow("bearer last") << true << tenant << QStringList{Q("TFS-Federated"), basicVs, Q("Bearer authorization_uri=") + tenant};
    }

    void tryGetAuthorityFromHeaders()
    {
        QFETCH(bool, expectedResult);
        QFETCH(QString, expectedAuthority);
        QFETCH(QStringList, headers);
        QString actualAuthority;
        QCOMPARE(AzureReposHostProvider::tryGetAuthorityFromHeaders(headers, actualAuthority), expectedResult);
        QCOMPARE(actualAuthority, expectedAuthority);
    }

    // ---- azure-repos commands (own tests) ----

    void commands_BindListUnbind()
    {
        TestCommandContext context;
        AzureReposHostProvider provider(context);
        QCOMPARE(runCommand(provider, {Q("bind"), Q("org1"), Q("alice@example.com")}), 0);
        QCOMPARE(runCommand(provider, {Q("bind"), Q("org2"), Q("bob")}), 0);
        QCOMPARE(runCommand(provider, {Q("bind"), Q("--local"), Q("org1"), Q("carol")}), 0);
        QCOMPARE(runCommand(provider, {Q("bind"), Q("--local"), Q("org2"), Q("")}), 0);
        context.Console.WrittenMessages.clear();
        QCOMPARE(runCommand(provider, {Q("list")}), 0);
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("org1:"), Q("  (global) -> alice@example.com"), Q("  (local)  -> carol"), Q("org2:"),
                                                               Q("  (global) -> bob"), Q("  (local)  -> (no inherit)")}));
        context.Console.WrittenMessages.clear();
        QCOMPARE(runCommand(provider, {Q("list"), Q("ORG2")}), 0);
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("(global) -> bob"), Q("(local)  -> (no inherit)")}));
        QCOMPARE(runCommand(provider, {Q("unbind"), Q("org1")}), 0);
        QCOMPARE(runCommand(provider, {Q("unbind"), Q("--local"), Q("org1")}), 0);
        QVERIFY(!context.Git.Configuration.Global.contains(userKey(Q("org1"))));
        QVERIFY(!context.Git.Configuration.Local.contains(userKey(Q("org1"))));
    }

    void commands_ListShowRemotes()
    {
        TestCommandContext context;
        context.Git.Configuration.Global[userKey(Q("org1"))] = QStringList{Q("alice")};
        context.Git.Remotes = {
            GitRemote{Q("origin"), Q("https://org1@dev.azure.com/org1/proj/_git/repo"), Q("https://org1@dev.azure.com/org1/proj/_git/repo")},
            GitRemote{Q("other"), Q("https://bob@org2.visualstudio.com/proj/_git/repo"), Q("https://dev.azure.com/org2/proj%20x/_git/repo")},
            GitRemote{Q("gh"), Q("https://github.com/foo/bar"), Q("https://github.com/foo/bar")},
        };
        AzureReposHostProvider provider(context);
        QCOMPARE(runCommand(provider, {Q("list"), Q("--show-remotes")}), 0);
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("org1:"), Q("  (global) -> alice"), Q("  origin:"), Q("    (fetch) -> (inherit)"),
                                                               Q("    (push)  -> (inherit)"), Q("org2:"), Q("  other:"), Q("    (fetch) -> bob"),
                                                               Q("    (push)  -> (inherit)")}));
        context.Console.WrittenMessages.clear();
        // (as upstream, the organization filter applies to the bindings only: the remotes of every organization are
        // listed, without organization headers)
        QCOMPARE(runCommand(provider, {Q("list"), Q("--show-remotes"), Q("-v"), Q("org2")}), 0);
        QCOMPARE(context.Console.WrittenMessages,
                 (QStringList{Q("origin:"), Q("  https://dev.azure.com/org1/proj/_git/repo (fetch) -> (inherit)"),
                              Q("  https://dev.azure.com/org1/proj/_git/repo (push)  -> (inherit)"), Q("other:"),
                              Q("  https://org2.visualstudio.com/proj/_git/repo (fetch) -> bob"), Q("  https://dev.azure.com/org2/proj x/_git/repo (push)  -> (inherit)")}));
    }

    void commands_NotInsideRepository()
    {
        TestCommandContext context;
        context.Git.CurrentRepository.reset();
        AzureReposHostProvider provider(context);
        QCOMPARE(runCommand(provider, {Q("bind"), Q("--local"), Q("org1"), Q("carol")}), -1);
        QCOMPARE(runCommand(provider, {Q("unbind"), Q("--local"), Q("org1")}), -1);
        QCOMPARE(runCommand(provider, {Q("list"), Q("--show-remotes")}), 0);
        QCOMPARE(context.Console.WrittenMessages, (QStringList{Q("not inside a git repository (cannot use --local)"), Q("not inside a git repository (cannot use --local)"),
                                                               Q("not inside a git repository (--show-remotes has no effect)")}));
    }

    void commands_ClearCache()
    {
        Fixture f;
        f.authorityCache->AllowClear = true;
        AzureReposHostProvider provider = f.provider();
        QCOMPARE(runCommand(provider, {Q("clear-cache")}), 0);
        QCOMPARE(f.authorityCache->ClearCalls, 1);
        QCOMPARE(f.context.Console.WrittenMessages, QStringList{Q("Authority cache cleared")});
    }

private:
    static int runCommand(AzureReposHostProvider &provider, const QStringList &args)
    {
        auto root = std::make_shared<Command>(Q("git-credential-manager"), QString());
        root->addCommand(provider.createCommand());
        StringWriter out, err;
        CommandLineParser parser(root, Q("git-credential-manager"), Q("0"), out, err);
        return parser.invoke(QStringList{Q("azure-repos")} + args);
    }
};
GCM_REGISTER_TEST(AzureReposHostProviderTests)

#include "azreposhostprovidertests.moc"
