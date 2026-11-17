// Ports of Microsoft.AzureRepos.Tests: UriHelpersTests (13 methods), AzureReposAuthorityCacheTests (7),
// AzureReposBindingManagerTests (43) and AzureDevOpsApiTests (15). A null string/Uri argument upstream is a null
// QString / empty QUrl here (ArgumentNullException is an ArgumentException). The SignIn/SignOut tests are data-driven
// with one row per upstream method. Own tests: authority cache clear(), getBindings with an organization filter and
// a binding outside a repository, and the PAT request body.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "azrepos/azrepos.h"
#include "infra/nulltrace.h"
#include "infra/testcommandcontext.h"
#include "infra/testgit.h"
#include "infra/testmain.h"
#include "infra/testutils.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

using namespace gcm;
using namespace gcm::azrepos;
using namespace gcm::test;

namespace {

QString Q(const char *s) { return QString::fromUtf8(s); }

QString uri(const QUrl &u) { return absoluteUri(u); }

QString authorityKey(const QString &orgName)
{
    return QStringLiteral("%1.%2:%3/%4.%5").arg(Constants::GitConfiguration::Credential::SectionName, AzureDevOpsConstants::UrnScheme, AzureDevOpsConstants::UrnOrgPrefix, orgName, AzureDevOpsConstants::GitConfiguration::Credential::AzureAuthority);
}

QString userKey(const QString &orgName)
{
    return QStringLiteral("%1.%2:%3/%4.%5").arg(Constants::GitConfiguration::Credential::SectionName, AzureDevOpsConstants::UrnScheme, AzureDevOpsConstants::UrnOrgPrefix, orgName, Constants::GitConfiguration::Credential::UserName);
}

QString newGuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

const QString ExpectedLocationServicePath = Q("_apis/ServiceDefinitions/LocationService2/951917AC-A960-4999-8464-E3F0AA25B381?api-version=1.0");
const QString ExpectedIdentityServicePath = Q("_apis/token/sessiontokens?api-version=1.0&tokentype=compact");
const QString CommonAuthority = Q("https://login.microsoftonline.com/common");
const QString OrganizationsAuthority = Q("https://login.microsoftonline.com/organizations");

HttpResponse jsonResponse(int status, const QString &field, const QString &value)
{
    return TestHttpMessageHandler::createResponse(status, QJsonDocument(QJsonObject{{field, value}}).toJson(QJsonDocument::Compact));
}

void assertAcceptJson(const HttpRequest &request) { QVERIFY(request.headers.values(Q("Accept")).contains(Constants::Http::MimeTypeJson)); }

struct ApiFixture
{
    TestCommandContext context;
    std::shared_ptr<TestHttpMessageHandler> handler = std::make_shared<TestHttpMessageHandler>();
    ApiFixture()
    {
        handler->ThrowOnUnexpectedRequest = true;
        context.HttpClientFactory.MessageHandler = handler;
    }
    /// HEAD https://example.com answering 401 with the given headers
    void setupHead(const QList<std::pair<QString, QString>> &headers)
    {
        HttpResponse response = TestHttpMessageHandler::createResponse(401);
        for (const auto &[name, value] : headers) {
            response.headers.add(name, value);
        }
        handler->setup(Q("HEAD"), QUrl(Q("https://example.com")), response);
    }
};

const QUrl PatOrgUri(Q("https://dev.azure.com/org/"));
const QString PatAccessToken = Q("ACCESS-TOKEN");
const QUrl IdentityServiceUri(Q("https://identity.example.com/"));

/// Location service answering the identity service URI (with the Accept and Bearer checks of upstream)
void setupLocationService(TestHttpMessageHandler &handler)
{
    handler.setup(Q("GET"), PatOrgUri.resolved(QUrl(ExpectedLocationServicePath)), [](const HttpRequest &request) {
        assertAcceptJson(request);
        RestTestUtilities::assertBearerAuth(request, PatAccessToken);
        return jsonResponse(200, Q("location"), uri(IdentityServiceUri));
    });
}

} // namespace

// =============================================================================================
// UriHelpersTests
// =============================================================================================

class UriHelpersTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void combinePath_data()
    {
        QTest::addColumn<QString>("basePath");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        QTest::newRow("1") << Q("a/b") << Q("c/d") << Q("a/b/c/d");
        QTest::newRow("2") << Q("a/b/") << Q("c/d") << Q("a/b/c/d");
        QTest::newRow("3") << Q("a/b") << Q("/c/d") << Q("a/b/c/d");
        QTest::newRow("4") << Q("a/b/") << Q("/c/d") << Q("a/b/c/d");
    }

    void combinePath()
    {
        QFETCH(QString, basePath);
        QFETCH(QString, path);
        QFETCH(QString, expected);
        QCOMPARE(UriHelpers::combinePath(basePath, path), expected);
    }

    void hostChecks_data()
    {
        // host, IsAzureDevOpsHost, IsDevAzureComHost, IsVisualStudioComHost (the three upstream theories share inputs)
        QTest::addColumn<OptString>("host");
        QTest::addColumn<bool>("azureDevOps");
        QTest::addColumn<bool>("devAzureCom");
        QTest::addColumn<bool>("visualStudioCom");
        QTest::newRow("dev.azure.com") << OptString(Q("dev.azure.com")) << true << true << false;
        QTest::newRow("myorg.visualstudio.com") << OptString(Q("myorg.visualstudio.com")) << true << false << true;
        QTest::newRow("vs-ssh.myorg.visualstudio.com") << OptString(Q("vs-ssh.myorg.visualstudio.com")) << true << false << true;
        QTest::newRow("DEV.AZURE.COM") << OptString(Q("DEV.AZURE.COM")) << true << true << false;
        QTest::newRow("MYORG.VISUALSTUDIO.COM") << OptString(Q("MYORG.VISUALSTUDIO.COM")) << true << false << true;
        QTest::newRow("null") << OptString() << false << false << false;
        QTest::newRow("empty") << OptString(Q("")) << false << false << false;
        QTest::newRow("whitespace") << OptString(Q("    ")) << false << false << false;
        QTest::newRow("testdev.azure.com") << OptString(Q("testdev.azure.com")) << false << false << false;
        QTest::newRow("test.dev.azure.com") << OptString(Q("test.dev.azure.com")) << false << false << false;
        QTest::newRow("visualstudio.com") << OptString(Q("visualstudio.com")) << false << false << false;
        QTest::newRow("testvisualstudio.com") << OptString(Q("testvisualstudio.com")) << false << false << false;
    }

    // UriHelpers_IsAzureDevOpsHost, UriHelpers_IsDevAzureComHost, UriHelpers_IsVisualStudioComHost
    void hostChecks()
    {
        QFETCH(OptString, host);
        QFETCH(bool, azureDevOps);
        QFETCH(bool, devAzureCom);
        QFETCH(bool, visualStudioCom);
        QCOMPARE(UriHelpers::isAzureDevOpsHost(host), azureDevOps);
        QCOMPARE(UriHelpers::isDevAzureComHost(host), devAzureCom);
        QCOMPARE(UriHelpers::isVisualStudioComHost(host), visualStudioCom);
    }

    void createOrganizationUri_Null_ThrowsException()
    {
        QString org;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, UriHelpers::createOrganizationUri(QUrl(), org));
    }

    void createOrganizationUri_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<QString>("expectedOrg");
        QTest::newRow("AzureHost_ReturnsCorrectUri") << Q("https://dev.azure.com/myorg/myproject/_git/myrepo") << Q("https://dev.azure.com/myorg") << Q("myorg");
        QTest::newRow("AzureHost_WithPort_ReturnsCorrectUri")
            << Q("https://dev.azure.com:456/myorg/myproject/_git/myrepo") << Q("https://dev.azure.com:456/myorg") << Q("myorg");
        QTest::newRow("AzureHost_OrgAlsoInUser_PrefersPathOrg") << Q("https://myorg-user@dev.azure.com/myorg-path") << Q("https://dev.azure.com/myorg-path")
                                                                << Q("myorg-path");
        QTest::newRow("AzureHost_InputArgsMissingPath_HasUser_UsesUserOrg")
            << Q("https://myorg-user@dev.azure.com") << Q("https://dev.azure.com/myorg-user") << Q("myorg-user");
        QTest::newRow("VisualStudioHost_ReturnsCorrectUri") << Q("https://myorg.visualstudio.com") << Q("https://myorg.visualstudio.com") << Q("myorg");
    }

    void createOrganizationUri()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QFETCH(QString, expectedOrg);
        QString actualOrg;
        const QUrl actual = UriHelpers::createOrganizationUri(QUrl(input), actualOrg);
        QCOMPARE(uri(actual), uri(QUrl(expected)));
        QCOMPARE(actualOrg, expectedOrg);
    }

    void createOrganizationUri_Throws_data()
    {
        QTest::addColumn<QString>("input");
        QTest::newRow("AzureHost_InputArgsMissingPathAndUser_ThrowsException") << Q("https://dev.azure.com");
        QTest::newRow("VisualStudioHost_MissingOrgInHost_ThrowsException") << Q("https://visualstudio.com");
        QTest::newRow("NonAzureDevOpsHost_ThrowsException") << Q("https://example.com");
    }

    void createOrganizationUri_Throws()
    {
        QFETCH(QString, input);
        QString org;
        QVERIFY_THROWS_EXCEPTION(InvalidOperationException, UriHelpers::createOrganizationUri(QUrl(input), org));
    }
};
GCM_REGISTER_TEST(UriHelpersTests)

// =============================================================================================
// AzureReposAuthorityCacheTests
// =============================================================================================

class AzureReposAuthorityCacheTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getAuthority_Null_ThrowException()
    {
        NullTrace trace;
        TestGit git;
        AzureDevOpsAuthorityCache cache(trace, git);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, cache.getAuthority(QString()));
    }

    void getAuthority_NoCachedAuthority_ReturnsNull()
    {
        NullTrace trace;
        TestGit git;
        AzureDevOpsAuthorityCache cache(trace, git);
        QCOMPARE(cache.getAuthority(authorityKey(Q("contoso"))), OptString());
    }

    void getAuthority_CachedAuthority_ReturnsAuthority()
    {
        const QString expectedAuthority = Q("https://login.contoso.com");
        NullTrace trace;
        TestGit git;
        git.Configuration.Global[authorityKey(Q("contoso"))] = QStringList{expectedAuthority};
        AzureDevOpsAuthorityCache cache(trace, git);
        QCOMPARE(cache.getAuthority(Q("contoso")), OptString(expectedAuthority));
    }

    void updateAuthority_NoCachedAuthority_SetsAuthority()
    {
        const QString expectedAuthority = Q("https://login.contoso.com");
        NullTrace trace;
        TestGit git;
        AzureDevOpsAuthorityCache cache(trace, git);
        cache.updateAuthority(Q("contoso"), expectedAuthority);
        QStringList values;
        QVERIFY(git.Configuration.Global.tryGetValue(authorityKey(Q("contoso")), values));
        QCOMPARE(values, QStringList{expectedAuthority});
    }

    void updateAuthority_CachedAuthority_UpdatesAuthority()
    {
        const QString expectedAuthority = Q("https://login.contoso.com");
        NullTrace trace;
        TestGit git;
        git.Configuration.Global[authorityKey(Q("contoso"))] = QStringList{Q("https://old-login.contoso.com")};
        AzureDevOpsAuthorityCache cache(trace, git);
        cache.updateAuthority(Q("contoso"), expectedAuthority);
        QStringList values;
        QVERIFY(git.Configuration.Global.tryGetValue(authorityKey(Q("contoso")), values));
        QCOMPARE(values, QStringList{expectedAuthority});
    }

    void eraseAuthority_NoCachedAuthority_DoesNothing()
    {
        const QString otherKey = authorityKey(Q("org.fabrikam.authority"));
        const QString otherAuthority = Q("https://fabrikam.com/login");
        NullTrace trace;
        TestGit git;
        git.Configuration.Global[otherKey] = QStringList{otherAuthority};
        AzureDevOpsAuthorityCache cache(trace, git);
        cache.eraseAuthority(Q("contoso"));
        // Other entries should remain
        QVERIFY(!git.Configuration.Global.contains(authorityKey(Q("contoso"))));
        QCOMPARE(git.Configuration.Global.count(), 1);
        QCOMPARE(git.Configuration.Global.value(otherKey), QStringList{otherAuthority});
    }

    void eraseAuthority_CachedAuthority_RemovesAuthority()
    {
        const QString otherKey = authorityKey(Q("fabrikam"));
        const QString otherAuthority = Q("https://fabrikam.com/login");
        NullTrace trace;
        TestGit git;
        git.Configuration.Global[authorityKey(Q("contoso"))] = QStringList{Q("https://login.contoso.com")};
        git.Configuration.Global[otherKey] = QStringList{otherAuthority};
        AzureDevOpsAuthorityCache cache(trace, git);
        cache.eraseAuthority(Q("contoso"));
        // Only the other entries should remain
        QVERIFY(!git.Configuration.Global.contains(authorityKey(Q("contoso"))));
        QCOMPARE(git.Configuration.Global.count(), 1);
        QCOMPARE(git.Configuration.Global.value(otherKey), QStringList{otherAuthority});
    }

    // Own test: clear() removes every cached authority (and nothing else)
    void clear_RemovesAllAuthorities()
    {
        NullTrace trace;
        TestGit git;
        git.Configuration.Global[authorityKey(Q("contoso"))] = QStringList{Q("https://login.contoso.com")};
        git.Configuration.Global[authorityKey(Q("fabrikam"))] = QStringList{Q("https://fabrikam.com/login")};
        git.Configuration.Global[userKey(Q("contoso"))] = QStringList{Q("john.doe")};
        git.Configuration.Global[Q("credential.helper")] = QStringList{Q("manager")};
        AzureDevOpsAuthorityCache cache(trace, git);
        cache.clear();
        QCOMPARE(git.Configuration.Global.keys(), (QStringList{userKey(Q("contoso")), Q("credential.helper")}));
    }
};
GCM_REGISTER_TEST(AzureReposAuthorityCacheTests)

// =============================================================================================
// AzureReposBindingManagerTests
// =============================================================================================

class AzureReposBindingManagerTests : public QObject
{
    Q_OBJECT

    struct Fixture
    {
        NullTrace trace;
        TestGit git;
        AzureReposBindingManager manager{trace, git};
    };

private Q_SLOTS:
    // ---- Bind ----

    void bind_NullOrganization_ThrowException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.bind(QString(), Q("user"), false));
    }

    void bind_SetsOrgKey_data()
    {
        QTest::addColumn<bool>("local");
        QTest::addColumn<OptString>("existing");
        QTest::newRow("Bind_NoUser_SetsOrgKey") << false << OptString();
        QTest::newRow("BindLocal_NoUser_SetsOrgKey") << true << OptString();
        QTest::newRow("Bind_ExistingUser_SetsOrgKey") << false << OptString(Q("org-user"));
        QTest::newRow("BindLocal_ExistingUser_SetsOrgKey") << true << OptString(Q("org-user"));
    }

    void bind_SetsOrgKey()
    {
        QFETCH(bool, local);
        QFETCH(OptString, existing);
        Fixture f;
        TestConfigLevel &level = local ? f.git.Configuration.Local : f.git.Configuration.Global;
        if (existing) {
            level[userKey(Q("org"))] = QStringList{*existing};
        }
        f.manager.bind(Q("org"), Q("user1"), local);
        QStringList users;
        QVERIFY(level.tryGetValue(userKey(Q("org")), users));
        QCOMPARE(users, QStringList{Q("user1")});
    }

    // ---- Unbind ----

    void unbind_NullOrganization_ThrowException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.unbind(QString(), false));
    }

    void unbind_data()
    {
        QTest::addColumn<bool>("local");
        QTest::addColumn<OptString>("existing");
        QTest::newRow("Unbind_NoUser_DoesNothing") << false << OptString();
        QTest::newRow("UnbindLocal_NoUser_DoesNothing") << true << OptString();
        QTest::newRow("Unbind_ExistingUser_RemovesKey") << false << OptString(Q("org-user"));
        QTest::newRow("UnbindLocal_ExistingUser_RemovesKey") << true << OptString(Q("org-user"));
    }

    void unbind()
    {
        QFETCH(bool, local);
        QFETCH(OptString, existing);
        Fixture f;
        TestConfigLevel &level = local ? f.git.Configuration.Local : f.git.Configuration.Global;
        if (existing) {
            level[userKey(Q("org"))] = QStringList{*existing};
        }
        f.manager.unbind(Q("org"), local);
        QVERIFY(level.isEmpty());
    }

    // Own test: a local binding outside a repository is not written
    void bindLocal_NotInsideRepository_DoesNothing()
    {
        NullTrace trace;
        TestGit git(false);
        AzureReposBindingManager manager(trace, git);
        manager.bind(Q("org"), Q("user1"), true);
        manager.unbind(Q("org"), true);
        QCOMPARE(git.Configuration.SetCallCount, 0);
        QCOMPARE(git.Configuration.UnsetCallCount, 0);
    }

    // ---- GetBinding ----

    void getBinding_Null_ThrowException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.getBinding(QString()));
    }

    void getBinding_NoUser_ReturnsNull()
    {
        Fixture f;
        QVERIFY(!f.manager.getBinding(Q("org")));
    }

    void getBinding_data()
    {
        QTest::addColumn<OptString>("global");
        QTest::addColumn<OptString>("local");
        QTest::newRow("GlobalUser_ReturnsBinding") << OptString(Q("john.doe")) << OptString();
        QTest::newRow("LocalUser_ReturnsBinding") << OptString() << OptString(Q("john.doe"));
        QTest::newRow("LocalAndGlobalUsers_ReturnsBinding") << OptString(Q("jane.doe")) << OptString(Q("john.doe"));
    }

    void getBinding()
    {
        QFETCH(OptString, global);
        QFETCH(OptString, local);
        Fixture f;
        if (global) {
            f.git.Configuration.Global[userKey(Q("org"))] = QStringList{*global};
        }
        if (local) {
            f.git.Configuration.Local[userKey(Q("org"))] = QStringList{*local};
        }
        const std::optional<AzureReposBinding> binding = f.manager.getBinding(Q("org"));
        QVERIFY(binding);
        QCOMPARE(binding->organization, Q("org"));
        QCOMPARE(binding->globalUserName, global);
        QCOMPARE(binding->localUserName, local);
    }

    // ---- GetBindings ----

    void getBindings_NoUsers_ReturnsEmpty()
    {
        Fixture f;
        QVERIFY(f.manager.getBindings().isEmpty());
    }

    void getBindings_Users_ReturnsUsers()
    {
        Fixture f;
        f.git.Configuration.Global[userKey(Q("org1"))] = QStringList{Q("user1")};
        f.git.Configuration.Global[userKey(Q("org2"))] = QStringList{Q("user2")};
        const QList<AzureReposBinding> bindings = f.manager.getBindings();
        QCOMPARE(bindings.size(), 2);
        QCOMPARE(bindings[0].organization, Q("org1"));
        QCOMPARE(bindings[0].globalUserName, OptString(Q("user1")));
        QCOMPARE(bindings[0].localUserName, OptString());
        QCOMPARE(bindings[1].organization, Q("org2"));
        QCOMPARE(bindings[1].globalUserName, OptString(Q("user2")));
        QCOMPARE(bindings[1].localUserName, OptString());
    }

    // Own test: local and global bindings are merged; the filter is case-insensitive; other keys are ignored
    void getBindings_LocalAndFilter()
    {
        Fixture f;
        f.git.Configuration.Global[userKey(Q("org1"))] = QStringList{Q("user1")};
        f.git.Configuration.Global[userKey(Q("org2"))] = QStringList{Q("user2")};
        f.git.Configuration.Global[authorityKey(Q("org3"))] = QStringList{Q("https://login.example.com")};
        f.git.Configuration.Global[Q("credential.username")] = QStringList{Q("someone")};
        f.git.Configuration.Local[userKey(Q("org2"))] = QStringList{AzureReposBinding::NoInherit};
        f.git.Configuration.Local[userKey(Q("org4"))] = QStringList{Q("user4")};
        const QList<AzureReposBinding> all = f.manager.getBindings();
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[1].organization, Q("org2"));
        QCOMPARE(all[1].localUserName, OptString(AzureReposBinding::NoInherit));
        QCOMPARE(all[2].organization, Q("org4"));
        QCOMPARE(all[2].globalUserName, OptString());
        const QList<AzureReposBinding> filtered = f.manager.getBindings(Q("ORG2"));
        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered[0].globalUserName, OptString(Q("user2")));
    }

    // ---- GetUser ----

    void getUser_NullOrg_ThrowsException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.getUser(QString()));
    }

    void getUser_data()
    {
        QTest::addColumn<OptString>("global");
        QTest::addColumn<OptString>("local");
        QTest::addColumn<OptString>("expected");
        QTest::newRow("NoUser_ReturnsNull") << OptString() << OptString() << OptString();
        QTest::newRow("GlobalUser_ReturnsGlobalUser") << OptString(Q("user1")) << OptString() << OptString(Q("user1"));
        QTest::newRow("LocalUser_ReturnsLocalUser") << OptString() << OptString(Q("user1")) << OptString(Q("user1"));
        QTest::newRow("GlobalAndLocalUsers_ReturnsLocalUser") << OptString(Q("user1")) << OptString(Q("user2")) << OptString(Q("user2"));
        // Own row: a "no inherit" local binding hides the global user
        QTest::newRow("GlobalAndNoInheritLocal_ReturnsNull") << OptString(Q("user1")) << OptString(AzureReposBinding::NoInherit) << OptString();
    }

    void getUser()
    {
        QFETCH(OptString, global);
        QFETCH(OptString, local);
        QFETCH(OptString, expected);
        Fixture f;
        if (global) {
            f.git.Configuration.Global[userKey(Q("org"))] = QStringList{*global};
        }
        if (local) {
            f.git.Configuration.Local[userKey(Q("org"))] = QStringList{*local};
        }
        QCOMPARE(f.manager.getUser(Q("org")), expected);
    }

    // ---- SignIn ----

    void signIn_NullOrg_ThrowsException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.signIn(QString(), Q("user1")));
    }

    void signIn_NullUser_ThrowsException()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(ArgumentException, f.manager.signIn(Q("org"), QString()));
    }

    void signIn_data()
    {
        // Initial global/local binding -> expected global/local binding (nullopt: no entry); and for the
        // *_NoConfigWrites / *_OnlyUnbindsLocal rows the expected number of set/unset calls (-1: not checked)
        QTest::addColumn<OptString>("global");
        QTest::addColumn<OptString>("local");
        QTest::addColumn<OptString>("expectedGlobal");
        QTest::addColumn<OptString>("expectedLocal");
        QTest::addColumn<int>("sets");
        QTest::addColumn<int>("unsets");
        const OptString none, u1(Q("user1")), u2(Q("user2"));
        QTest::newRow("NoGlobalNoLocal_BindsGlobal") << none << none << u1 << none << -1 << -1;
        QTest::newRow("NoGlobalSameLocal_BindsGlobalUnbindLocal") << none << u1 << u1 << none << -1 << -1;
        QTest::newRow("NoGlobalOtherLocal_BindsGlobalUnbindLocal") << none << u2 << u1 << none << -1 << -1;
        QTest::newRow("SameGlobalNoLocal_DoesNothing") << u1 << none << u1 << none << -1 << -1;
        QTest::newRow("SameGlobalSameLocal_UnbindLocal") << u1 << u1 << u1 << none << -1 << -1;
        QTest::newRow("SameGlobalOtherLocal_UnbindLocal") << u1 << u2 << u1 << none << -1 << -1;
        QTest::newRow("OtherGlobalNoLocal_BindsLocal") << u2 << none << u2 << u1 << -1 << -1;
        QTest::newRow("OtherGlobalSameLocal_DoesNothing") << u2 << u1 << u2 << u1 << -1 << -1;
        QTest::newRow("OtherGlobalOtherLocal_BindsLocal") << u2 << u2 << u2 << u1 << -1 << -1;
        QTest::newRow("SameGlobalNoLocal_NoConfigWrites") << u1 << none << u1 << none << 0 << 0;
        QTest::newRow("OtherGlobalSameLocal_NoConfigWrites") << u2 << u1 << u2 << u1 << 0 << 0;
        QTest::newRow("SameGlobalSameLocal_OnlyUnbindsLocal") << u1 << u1 << u1 << none << 0 << 1;
        QTest::newRow("SameGlobalOtherLocal_OnlyUnbindsLocal") << u1 << u2 << u1 << none << 0 << 1;
    }

    void signIn()
    {
        QFETCH(OptString, global);
        QFETCH(OptString, local);
        QFETCH(OptString, expectedGlobal);
        QFETCH(OptString, expectedLocal);
        QFETCH(int, sets);
        QFETCH(int, unsets);
        Fixture f;
        setBindings(f.git, global, local);
        f.manager.signIn(Q("org"), Q("user1"));
        assertBindings(f.git, expectedGlobal, expectedLocal);
        if (sets >= 0) {
            QCOMPARE(f.git.Configuration.SetCallCount, sets);
            QCOMPARE(f.git.Configuration.UnsetCallCount, unsets);
        }
    }

    // ---- SignOut ----

    void signOut_data()
    {
        QTest::addColumn<OptString>("global");
        QTest::addColumn<OptString>("local");
        QTest::addColumn<OptString>("expectedGlobal");
        QTest::addColumn<OptString>("expectedLocal");
        const OptString none, u1(Q("user1")), u2(Q("user2")), noInherit(AzureReposBinding::NoInherit);
        QTest::newRow("NoGlobalNoLocal_DoesNothing") << none << none << none << none;
        QTest::newRow("NoGlobalUserLocal_UnbindsLocal") << none << u1 << none << none;
        QTest::newRow("NoGlobalNoInheritLocal_UnbindsLocal") << none << noInherit << none << none;
        QTest::newRow("UserGlobalNoLocal_BindLocalNoInherit") << u1 << none << u1 << noInherit;
        QTest::newRow("UserGlobalNoInheritLocal_DoesNothing") << u1 << noInherit << u1 << noInherit;
        QTest::newRow("UserGlobalUserLocal_BindLocalNoInherit") << u1 << u2 << u1 << noInherit;
    }

    void signOut()
    {
        QFETCH(OptString, global);
        QFETCH(OptString, local);
        QFETCH(OptString, expectedGlobal);
        QFETCH(OptString, expectedLocal);
        Fixture f;
        setBindings(f.git, global, local);
        f.manager.signOut(Q("org"));
        assertBindings(f.git, expectedGlobal, expectedLocal);
    }

private:
    static void setBindings(TestGit &git, const OptString &global, const OptString &local)
    {
        if (global) {
            git.Configuration.Global[userKey(Q("org"))] = QStringList{*global};
        }
        if (local) {
            git.Configuration.Local[userKey(Q("org"))] = QStringList{*local};
        }
    }

    static void assertBindings(TestGit &git, const OptString &expectedGlobal, const OptString &expectedLocal)
    {
        auto check = [](const TestConfigLevel &level, const OptString &expected) {
            if (!expected) {
                QVERIFY(level.isEmpty());
                return;
            }
            QStringList users;
            QVERIFY(level.tryGetValue(userKey(Q("org")), users));
            QCOMPARE(users, QStringList{*expected});
        };
        check(git.Configuration.Global, expectedGlobal);
        if (QTest::currentTestFailed()) {
            return;
        }
        check(git.Configuration.Local, expectedLocal);
    }
};
GCM_REGISTER_TEST(AzureReposBindingManagerTests)

// =============================================================================================
// AzureDevOpsApiTests
// =============================================================================================

class AzureDevOpsApiTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void getAuthority_NullUri_ThrowsException()
    {
        TestCommandContext context;
        AzureDevOpsRestApi api(context);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, api.getAuthority(QUrl()));
    }

    void getAuthority_NoNetwork_ThrowsException()
    {
        TestCommandContext context;
        auto handler = std::make_shared<TestHttpMessageHandler>();
        handler->SimulateNoNetwork = true;
        context.HttpClientFactory.MessageHandler = handler;
        AzureDevOpsRestApi api(context);
        QVERIFY_THROWS_EXCEPTION(HttpRequestException, api.getAuthority(QUrl(Q("https://example.com"))));
    }

    void getAuthority_data()
    {
        const QString wwwAuth = Q("WWW-Authenticate");
        const QString vss = AzureDevOpsConstants::VssResourceTenantHeader;
        const QString testAuthority = Q("https://login.microsoftonline.com/test-authority");
        const QString aad1 = newGuid(), aad2 = newGuid(), msa = QUuid().toString(QUuid::WithoutBraces);
        QTest::addColumn<QList<std::pair<QString, QString>>>("headers");
        QTest::addColumn<QString>("expected");
        QTest::newRow("NoHeaders_ReturnsCommonAuthority") << QList<std::pair<QString, QString>>{} << CommonAuthority;
        QTest::newRow("WwwAuthenticateBearer_ReturnsAuthority")
            << QList<std::pair<QString, QString>>{{wwwAuth, Q("Bearer authorization_uri=") + testAuthority}} << testAuthority;
        QTest::newRow("WwwAuthenticateMultiple_ReturnsBearerAuthority")
            << QList<std::pair<QString, QString>>{{wwwAuth, Q("Bearer")}, {wwwAuth, Q("Bearer authorization_uri=") + testAuthority},
                                                  {wwwAuth, Q("NTLM [test-challenge-string]")}}
            << testAuthority;
        QTest::newRow("VssResourceTenantAad_ReturnsAadAuthority")
            << QList<std::pair<QString, QString>>{{vss, aad1}} << Q("https://login.microsoftonline.com/") + aad1;
        QTest::newRow("VssResourceTenantMultiple_ReturnsFirstAadAuthority")
            << QList<std::pair<QString, QString>>{{vss, aad1}, {vss, msa}, {vss, aad2}} << Q("https://login.microsoftonline.com/") + aad1;
        QTest::newRow("VssResourceTenantMsa_ReturnsOrganizationsAuthority") << QList<std::pair<QString, QString>>{{vss, msa}} << OrganizationsAuthority;
        QTest::newRow("BothWwwAuthAndVssResourceHeaders_ReturnsWwwAuthAuthority")
            << QList<std::pair<QString, QString>>{{vss, aad2}, {wwwAuth, Q("Bearer authorization_uri=https://login.microsoftonline.com/") + aad1}}
            << Q("https://login.microsoftonline.com/") + aad1;
    }

    void getAuthority()
    {
        using Headers = QList<std::pair<QString, QString>>;
        QFETCH(Headers, headers);
        QFETCH(QString, expected);
        ApiFixture f;
        f.setupHead(headers);
        AzureDevOpsRestApi api(f.context);
        QCOMPARE(api.getAuthority(QUrl(Q("https://example.com"))), expected);
    }

    void createPersonalAccessToken_ReturnsPAT()
    {
        const QString expectedPat = Q("PERSONAL-ACCESS-TOKEN");
        ApiFixture f;
        setupLocationService(*f.handler);
        QByteArray body;
        f.handler->setup(Q("POST"), IdentityServiceUri.resolved(QUrl(ExpectedIdentityServicePath)), [&](const HttpRequest &request) {
            assertAcceptJson(request);
            RestTestUtilities::assertBearerAuth(request, PatAccessToken);
            body = request.content;
            return jsonResponse(200, Q("token"), expectedPat);
        });
        AzureDevOpsRestApi api(f.context);
        QCOMPARE(api.createPersonalAccessToken(PatOrgUri, PatAccessToken, {AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite}), expectedPat);
        // Own check: the request body
        const QJsonObject json = QJsonDocument::fromJson(body).object();
        QCOMPARE(json.value(Q("scope")).toString(), AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite);
        QCOMPARE(json.value(Q("displayName")).toString(), Q("Git: https://dev.azure.com/org/ on %1").arg(QSysInfo::machineHostName()));
    }

    void createPersonalAccessToken_LocSvcReturnsHttp500_ThrowsException()
    {
        ApiFixture f;
        f.handler->setup(Q("GET"), PatOrgUri.resolved(QUrl(ExpectedLocationServicePath)), 500);
        AzureDevOpsRestApi api(f.context);
        QVERIFY_THROWS_EXCEPTION(Exception, api.createPersonalAccessToken(PatOrgUri, PatAccessToken, {AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite}));
    }

    void createPersonalAccessToken_IdentSvcReturnsHttp500_ThrowsException()
    {
        ApiFixture f;
        setupLocationService(*f.handler);
        f.handler->setup(Q("POST"), IdentityServiceUri.resolved(QUrl(ExpectedIdentityServicePath)), 500);
        AzureDevOpsRestApi api(f.context);
        QVERIFY_THROWS_EXCEPTION(Exception, api.createPersonalAccessToken(PatOrgUri, PatAccessToken, {AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite}));
    }

    void createPersonalAccessToken_IdentSvcReturnsHttp500WithError_ThrowsExceptionWithErrorMessage()
    {
        const QString serverErrorMessage = Q("ERROR123: This is a test error.");
        ApiFixture f;
        setupLocationService(*f.handler);
        f.handler->setup(Q("POST"), IdentityServiceUri.resolved(QUrl(ExpectedIdentityServicePath)), jsonResponse(500, Q("message"), serverErrorMessage));
        AzureDevOpsRestApi api(f.context);
        try {
            api.createPersonalAccessToken(PatOrgUri, PatAccessToken, {AzureDevOpsConstants::PersonalAccessTokenScopes::ReposWrite});
            QFAIL("no exception");
        } catch (const Exception &ex) {
            QVERIFY(ex.message().contains(serverErrorMessage));
        }
    }

    void tryGetAuthorityFromHeader_data()
    {
        QTest::addColumn<OptString>("headerValue");
        QTest::addColumn<bool>("expectedResult");
        QTest::addColumn<QString>("expectedAuthority");
        QTest::newRow("null") << OptString() << false << QString();
        QTest::newRow("NotBearer") << OptString(Q("NotBearer")) << false << QString();
        QTest::newRow("Bearer") << OptString(Q("Bearer")) << false << QString();
        QTest::newRow("Bearer foobar") << OptString(Q("Bearer foobar")) << false << QString();
        QTest::newRow("Bearer authorization_uri") << OptString(Q("Bearer authorization_uri=https://example.com")) << true << Q("https://example.com");
    }

    void tryGetAuthorityFromHeader()
    {
        QFETCH(OptString, headerValue);
        QFETCH(bool, expectedResult);
        QFETCH(QString, expectedAuthority);
        QString actualAuthority;
        QCOMPARE(AzureDevOpsRestApi::tryGetAuthorityFromHeader(headerValue, actualAuthority), expectedResult);
        QCOMPARE(actualAuthority, expectedAuthority);
    }

    void tryGetFirstJsonStringField_data()
    {
        QTest::addColumn<OptString>("json");
        QTest::addColumn<bool>("expectedResult");
        QTest::addColumn<QString>("expectedValue");
        QTest::newRow("null") << OptString() << false << QString();
        QTest::newRow("empty object") << OptString(Q("{}")) << false << QString();
        QTest::newRow("other field") << OptString(Q("{\"foo\": \"123\"}")) << false << QString();
        QTest::newRow("field") << OptString(Q("{\"target\": \"42\"}")) << true << Q("42");
        QTest::newRow("field case") << OptString(Q("{\"TARGET\": \"42\"}")) << true << Q("42");
        QTest::newRow("first") << OptString(Q("{\"target\": \"42\", \"other\": { \"target\": \"123\" } }")) << true << Q("42");
        QTest::newRow("nested") << OptString(Q("{\"foo\": { \"bar\": {\"target\":\"42\"} } }")) << true << Q("42");
    }

    void tryGetFirstJsonStringField()
    {
        QFETCH(OptString, json);
        QFETCH(bool, expectedResult);
        QFETCH(QString, expectedValue);
        QString actualValue;
        QCOMPARE(AzureDevOpsRestApi::tryGetFirstJsonStringField(json, Q("target"), actualValue), expectedResult);
        QCOMPARE(actualValue, expectedValue);
    }
};
GCM_REGISTER_TEST(AzureDevOpsApiTests)

#include "azreposcoretests.moc"
