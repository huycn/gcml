// Ports of HostProviderTests and HostProviderRegistryTests.
#include "appmocks.h"
#include "core/constants.h"
#include "core/strings.h"
#include "infra/testcommandcontext.h"
#include "infra/testhostprovider.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

GitRequest request(std::initializer_list<std::pair<QString, QString>> args)
{
    StringDictionary dict;
    for (const auto &[k, v] : args) {
        dict.set(k, v);
    }
    return GitRequest(dict);
}

/// HostProviderRegistryTests.CreateRequest
GitRequest createRequest(const QUrl &uri)
{
    StringDictionary dict{{Q("protocol"), uri.scheme()},
                          {Q("host"), isDefaultPort(uri) ? uri.host() : Q("%1:%2").arg(uri.host()).arg(uri.port())}};
    QString path = uri.path();
    if (!isNullOrWhiteSpace(path) && path != QLatin1String("/")) {
        while (path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }
        dict.set(Q("path"), path);
    }
    return GitRequest(dict);
}

std::shared_ptr<MockHostProvider> provider(const QString &id, bool supportsRequest, bool supportsResponse = false,
                                           QStringList authorities = {})
{
    auto p = std::make_shared<MockHostProvider>();
    p->Id = id;
    p->AuthorityIds = std::move(authorities);
    p->IsSupportedRequest = [supportsRequest](const GitRequest &) { return supportsRequest; };
    p->IsSupportedResponse = [supportsResponse](const HttpResponse *) { return supportsResponse; };
    return p;
}

const QString ProviderKey = Q("credential.https://example.com.provider");
} // namespace

class HostProviderTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void hostProvider_getCredentialAsync_credentialExists_returnsExistingCredential()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example.com"), Q("john.doe"), Q("letmein123"));
        TestHostProvider provider(context);
        provider.IsSupportedFunc = [](const GitRequest &) { return true; };
        bool generated = false;
        provider.GenerateCredentialFunc = [&](const GitRequest &) {
            generated = true;
            return Credential();
        };
        const GitResponse result = provider.getCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                                                   {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
        QVERIFY(!generated);
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("letmein123"));
    }

    void hostProvider_getCredentialAsync_credentialDoesNotExist_returnsNewGeneratedCredential()
    {
        TestCommandContext context;
        TestHostProvider provider(context);
        provider.IsSupportedFunc = [](const GitRequest &) { return true; };
        bool generated = false;
        provider.GenerateCredentialFunc = [&](const GitRequest &) {
            generated = true;
            return Credential{Q("john.doe"), Q("letmein123")};
        };
        const GitResponse result = provider.getCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                                                   {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
        QVERIFY(generated);
        QCOMPARE(result.credential()->account, Q("john.doe"));
        QCOMPARE(result.credential()->password, Q("letmein123"));
    }

    void hostProvider_storeCredentialAsync_emptyCredential_doesNotStoreCredential()
    {
        TestCommandContext context;
        TestHostProvider provider(context);
        provider.storeCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                          {Q("username"), Q("")}, {Q("password"), Q("")}}));
        QCOMPARE(context.CredentialStore.count(), 0);
    }

    void hostProvider_storeCredentialAsync_nonEmptyCredential_storesCredential()
    {
        TestCommandContext context;
        TestHostProvider provider(context);
        provider.storeCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                          {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
        QCOMPARE(context.CredentialStore.count(), 1);
        const auto stored = context.CredentialStore.tryGet(Q("https://example.com"), Q("john.doe"));
        QVERIFY(stored);
        QCOMPARE(stored->account, Q("john.doe"));
        QCOMPARE(stored->password, Q("letmein123"));
    }

    void hostProvider_storeCredentialAsync_nonEmptyCredential_existingCredential_updatesCredential()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example.com"), Q("john.doe"), Q("letmein123-old"));
        TestHostProvider provider(context);
        provider.storeCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                          {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123-new")}}));
        QCOMPARE(context.CredentialStore.count(), 1);
        QCOMPARE(context.CredentialStore.tryGet(Q("https://example.com"), Q("john.doe"))->password, Q("letmein123-new"));
    }

    void hostProvider_eraseCredentialAsync_noInputUser_credentialExists_erasesOneCredential()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example.com"), Q("john.doe"), Q("letmein123"));
        context.CredentialStore.add(Q("https://example.com"), Q("alice"), Q("do-not-erase-me"));
        context.CredentialStore.add(Q("https://example.com"), Q("bob"), Q("here-forever"));
        TestHostProvider provider(context);
        provider.eraseCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}));
        QCOMPARE(context.CredentialStore.count(), 2);
    }

    void hostProvider_eraseCredentialAsync_inputUser_credentialExists_userNotMatch_doesNothing()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example.com"), Q("alice"), Q("letmein123"));
        TestHostProvider provider(context);
        provider.eraseCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                          {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
        QCOMPARE(context.CredentialStore.count(), 1);
        QVERIFY(context.CredentialStore.contains(Q("https://example.com"), Q("alice")));
    }

    void hostProvider_eraseCredentialAsync_inputUser_credentialExists_userMatch_erasesCredential()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example.com"), Q("john.doe"), Q("letmein123"));
        TestHostProvider provider(context);
        provider.eraseCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")},
                                          {Q("username"), Q("john.doe")}, {Q("password"), Q("letmein123")}}));
        QCOMPARE(context.CredentialStore.count(), 0);
        QVERIFY(!context.CredentialStore.contains(Q("https://example.com"), Q("john.doe")));
    }

    void hostProvider_eraseCredentialAsync_differentHost_doesNothing()
    {
        TestCommandContext context;
        context.CredentialStore.add(Q("https://example2.com"), Q("john.doe"), Q("keep-me"));
        context.CredentialStore.add(Q("https://example3.com"), Q("john.doe"), Q("also-keep-me"));
        TestHostProvider provider(context);
        provider.eraseCredential(request({{Q("protocol"), Q("https")}, {Q("host"), Q("example1.com")}}));
        QCOMPARE(context.CredentialStore.count(), 2);
    }
};

// =============================================================================================

class HostProviderRegistryTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void hostProviderRegistry_register_autoProviderId_throwException()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        QVERIFY_THROWS_EXCEPTION(ArgumentException, registry.registerProvider(provider(Constants::ProviderIdAuto, false),
                                                                              HostProviderPriority::Normal));
    }

    void hostProviderRegistry_register_autoAuthorityId_throwException()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        QVERIFY_THROWS_EXCEPTION(ArgumentException,
                                 registry.registerProvider(provider(Q("p"), false, false, {Q("foo"), Constants::AuthorityIdAuto, Q("bar")}),
                                                           HostProviderPriority::Normal));
    }

    void hostProviderRegistry_getProvider_noProviders_throwException()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        QVERIFY_THROWS_EXCEPTION(Exception, registry.getProvider(GitRequest(StringDictionary())));
    }

    void hostProviderRegistry_getProvider_auto_hasProviders_returnsSupportedProvider()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("1"), false), p2 = provider(Q("2"), true), p3 = provider(Q("3"), false);
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p2);
    }

    void hostProviderRegistry_getProvider_auto_hasProviders_staticMatch_doesNotSetProviderGlobalConfig()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p = provider(Q("myProvider"), true);
        registry.registerProvider(p, HostProviderPriority::Normal);
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p);
        QVERIFY(!context.Git.Configuration.Global.contains(ProviderKey));
    }

    void hostProviderRegistry_getProvider_auto_hasProviders_dynamicMatch_setsProviderGlobalConfig()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p = provider(Q("myProvider"), false, true);
        registry.registerProvider(p, HostProviderPriority::Normal);
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p);
        QCOMPARE(context.Git.Configuration.Global.value(ProviderKey), QStringList{Q("myProvider")});
    }

    void hostProviderRegistry_getProvider_auto_hasProviders_dynamicMatch_setsProviderGlobalConfig_hostWithPath()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p = provider(Q("myProvider"), false, true);
        registry.registerProvider(p, HostProviderPriority::Normal);
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com/alice/repo.git/")))), p);
        // Any trailing slash is removed
        QCOMPARE(context.Git.Configuration.Global.value(Q("credential.https://example.com/alice/repo.git.provider")),
                 QStringList{Q("myProvider")});
    }

    void hostProviderRegistry_getProvider_auto_multipleValidProviders_returnsFirstRegistered()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("1"), true), p2 = provider(Q("2"), true), p3 = provider(Q("3"), true);
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p1);
    }

    void hostProviderRegistry_getProvider_auto_multipleValidProvidersMultipleLevels_returnsFirstHighestRegistered()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("1"), true), p2 = provider(Q("2"), true), p3 = provider(Q("3"), true), p4 = provider(Q("4"), true);
        registry.registerProvider(p1, HostProviderPriority::Low);
        registry.registerProvider(p2, HostProviderPriority::Normal);
        registry.registerProvider(p3, HostProviderPriority::High);
        registry.registerProvider(p4, HostProviderPriority::Low);
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p3);
    }

    void hostProviderRegistry_getProvider_providerSpecified_returnsProvider()
    {
        TestCommandContext context;
        context.Settings.ProviderOverride = Q("provider3");
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("provider1"), true), p2 = provider(Q("provider2"), true), p3 = provider(Q("provider3"), false);
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(GitRequest(StringDictionary())), p3);
    }

    void hostProviderRegistry_getProvider_autoProviderSpecified_returnsFirstSupportedProvider()
    {
        TestCommandContext context;
        context.Settings.ProviderOverride = Constants::ProviderIdAuto;
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("provider1"), false), p2 = provider(Q("provider2"), true), p3 = provider(Q("provider3"), false);
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p2);
    }

    void hostProviderRegistry_getProvider_unknownProviderSpecified_returnsFirstSupportedProvider()
    {
        TestCommandContext context;
        context.Settings.ProviderOverride = Q("provider42");
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("provider1"), false), p2 = provider(Q("provider2"), true), p3 = provider(Q("provider3"), false);
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p2);
        QCOMPARE(context.Console.WrittenMessages,
                 QStringList{Q("a host provider override was set but no such provider 'provider42' was found. Falling back to auto-detection.")});
    }

    void hostProviderRegistry_getProvider_legacyAuthoritySpecified_returnsProvider()
    {
        TestCommandContext context;
        context.Settings.LegacyAuthorityOverride = Q("authorityB");
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("1"), false, false, {Q("authorityA")});
        auto p2 = provider(Q("2"), false, false, {Q("authorityB"), Q("authorityC")});
        auto p3 = provider(Q("3"), false, false, {Q("authorityD")});
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(GitRequest(StringDictionary())), p2);
    }

    void hostProviderRegistry_getProvider_autoLegacyAuthoritySpecified_returnsFirstSupportedProvider()
    {
        TestCommandContext context;
        context.Settings.LegacyAuthorityOverride = Constants::AuthorityIdAuto;
        HostProviderRegistry registry(context);
        auto p1 = provider(Q("1"), false, false, {Q("authorityA")});
        auto p2 = provider(Q("2"), true, false, {Q("authorityB"), Q("authorityC")});
        auto p3 = provider(Q("3"), false, false, {Q("authorityD")});
        for (const auto &p : {p1, p2, p3}) {
            registry.registerProvider(p, HostProviderPriority::Normal);
        }
        QCOMPARE(registry.getProvider(createRequest(QUrl(Q("https://example.com")))), p2);
    }

    void hostProviderRegistry_getProvider_auto_networkProbe_returnsSupportedProvider()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        const QUrl remoteUri(Q("https://provider2.onprem.example.com"));
        auto p1 = provider(Q("1"), false, false);
        auto p2 = std::make_shared<MockHostProvider>();
        p2->Id = Q("2");
        p2->IsSupportedResponse = [](const HttpResponse *r) { return r && r->headers.contains(Q("X-Provider2")); };
        HttpResponse response = TestHttpMessageHandler::createResponse(401);
        response.headers.add(Q("X-Provider2"), Q("true"));
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->setup(Q("HEAD"), remoteUri, response);
        context.HttpClientFactory.MessageHandler = httpHandler;
        registry.registerProvider(p1, HostProviderPriority::Normal);
        registry.registerProvider(p2, HostProviderPriority::Normal);

        QCOMPARE(registry.getProvider(createRequest(remoteUri)), p2);
        httpHandler->assertRequest(Q("HEAD"), remoteUri, 1);
    }

    void hostProviderRegistry_getProvider_auto_networkProbe_timeoutZeroOrNegative_noNetworkCall_data()
    {
        QTest::addColumn<int>("timeout");
        QTest::newRow("TimeoutZero") << 0;
        QTest::newRow("TimeoutNegative") << -1;
    }
    void hostProviderRegistry_getProvider_auto_networkProbe_timeoutZeroOrNegative_noNetworkCall()
    {
        QFETCH(int, timeout);
        TestCommandContext context;
        HostProviderRegistry registry(context);
        const QUrl remoteUri(Q("https://onprem.example.com"));
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->setup(Q("HEAD"), remoteUri, 401);
        context.HttpClientFactory.MessageHandler = httpHandler;
        registry.registerProvider(provider(Q("p"), false, true), HostProviderPriority::Normal);
        context.Settings.AutoDetectProviderTimeout = timeout;

        QVERIFY_THROWS_EXCEPTION(Exception, registry.getProvider(createRequest(remoteUri)));
        httpHandler->assertRequest(Q("HEAD"), remoteUri, 0);
    }

    void hostProviderRegistry_getProvider_auto_networkProbe_noNetwork_returnsLastProvider()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        const QUrl remoteUri(Q("https://provider2.onprem.example.com"));
        auto high = provider(Q("high"), false, false);
        auto low = provider(Q("low"), true);
        registry.registerProvider(high, HostProviderPriority::Normal);
        registry.registerProvider(low, HostProviderPriority::Low);
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->SimulateNoNetwork = true;
        httpHandler->setup(Q("HEAD"), remoteUri, 401);
        context.HttpClientFactory.MessageHandler = httpHandler;

        QCOMPARE(registry.getProvider(createRequest(remoteUri)), low);
        httpHandler->assertRequest(Q("HEAD"), remoteUri, 1);
        QVERIFY(context.Console.WrittenMessages.contains(Q("failed to probe 'https://provider2.onprem.example.com/' to detect provider")));
    }

    // Own test: a probe that times out warns and continues.
    void hostProviderRegistry_getProvider_auto_networkProbe_timeout_warns()
    {
        TestCommandContext context;
        HostProviderRegistry registry(context);
        const QUrl remoteUri(Q("https://slow.example.com"));
        auto httpHandler = std::make_shared<TestHttpMessageHandler>();
        httpHandler->setup(Q("HEAD"), remoteUri, [](const HttpRequest &) -> HttpResponse {
            throw TimeoutException(QStringLiteral("timeout"));
        });
        context.HttpClientFactory.MessageHandler = httpHandler;
        auto low = provider(Q("low"), true);
        registry.registerProvider(provider(Q("normal"), false), HostProviderPriority::Normal);
        registry.registerProvider(low, HostProviderPriority::Low);
        QCOMPARE(registry.getProvider(createRequest(remoteUri)), low);
        QCOMPARE(context.Console.WrittenMessages.value(0), Q("auto-detection of host provider took too long (>2000ms)"));
        QVERIFY(!context.Git.Configuration.Global.contains(Q("credential.https://slow.example.com.provider")));
    }
};

GCM_REGISTER_TEST(HostProviderTests)
GCM_REGISTER_TEST(HostProviderRegistryTests)
#include "hostprovidertests.moc"
