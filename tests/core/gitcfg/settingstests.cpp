// Ports of SettingsTests, Interop/Linux/LinuxSettingsTests and Interop/Linux/LinuxConfigParserTests.
// Not ported: Settings_TlsBackend_ReturnsExpectedValue (TlsBackend / Schannel are Windows-only and not part of
// ISettings here). Settings_UseMsAuthDefaultAccount_Unset expects null (IsDevBox is Windows-only).
#include "core/constants.h"
#include "core/settings.h"
#include "core/strings.h"
#include "infra/nulltrace.h"
#include "infra/testenvironment.h"
#include "infra/testfilesystem.h"
#include "infra/testgit.h"
#include "infra/testmain.h"

#include <QRegularExpression>
#include <QUuid>

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }

namespace Envars = Constants::EnvironmentVariables;
namespace CredCfg = Constants::GitConfiguration::Credential;
namespace HttpCfg = Constants::GitConfiguration::Http;

QString key(const QString &section, const QString &property) { return section + QLatin1Char('.') + property; }
QString key(const QString &section, const QString &scope, const QString &property)
{
    return section + QLatin1Char('.') + scope + QLatin1Char('.') + property;
}

/// Settings over a TestEnvironment and a TestGit, optionally scoped to a remote URL.
struct Fixture
{
    explicit Fixture(const QString &remoteUrl = QString())
    {
        if (!remoteUrl.isEmpty()) {
            settings.setRemoteUri(QUrl(remoteUrl));
        }
    }
    void env(const QString &name, const QString &value) { environment.Variables.set(name, value); }
    void global(const QString &k, const QString &value) { git.Configuration.Global[k] = {value}; }
    void local(const QString &k, const QString &value) { git.Configuration.Local[k] = {value}; }

    TestEnvironment environment;
    TestGit git;
    Settings settings{environment, git};
};

QString uri(const char *s) { return absoluteUri(QUrl(Q(s))); }

/// System.Net.WebProxy.IsBypassed: the bypass expressions are matched (ignoring case) against
/// "scheme://host" plus ":port" when the port is not the scheme default.
bool isBypassed(const QStringList &bypassList, const QUrl &address)
{
    QString input = address.scheme() + QStringLiteral("://") + address.host();
    if (!isDefaultPort(address)) {
        input += QLatin1Char(':') + QString::number(address.port());
    }
    for (const QString &pattern : bypassList) {
        if (QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(input).hasMatch()) {
            return true;
        }
    }
    return false;
}

const QString ProxySettingValue = Q("http://john.doe:letmein123@proxy.example.com/");
const QString ExpectedNoProxy = Q("contoso.com,fabrikam.com");

void assertProxy(const std::optional<ProxyConfiguration> &actual, bool deprecated)
{
    QVERIFY(actual.has_value());
    QCOMPARE(absoluteUri(actual->address), uri("http://proxy.example.com"));
    QCOMPARE(actual->userName, OptString(Q("john.doe")));
    QCOMPARE(actual->password, OptString(Q("letmein123")));
    QCOMPARE(actual->noProxyRaw, OptString(ExpectedNoProxy));
    QCOMPARE(actual->isDeprecatedSource, deprecated);
}
} // namespace

class SettingsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // --- IsDebuggingEnabled --------------------------------------------------------------
    void settings_isDebuggingEnabled_envarUnset_returnsFalse() { QVERIFY(!Fixture().settings.isDebuggingEnabled()); }
    void settings_isDebuggingEnabled_envarTruthy_returnsTrue()
    {
        Fixture f;
        f.env(Envars::GcmDebug, Q("1"));
        QVERIFY(f.settings.isDebuggingEnabled());
    }
    void settings_isDebuggingEnabled_envarFalsey_returnsFalse()
    {
        Fixture f;
        f.env(Envars::GcmDebug, Q("0"));
        QVERIFY(!f.settings.isDebuggingEnabled());
    }

    // --- IsTerminalPromptsEnabled --------------------------------------------------------
    void settings_isTerminalPromptsEnabled_envarUnset_returnsTrue() { QVERIFY(Fixture().settings.isTerminalPromptsEnabled()); }
    void settings_isTerminalPromptsEnabled_envarTruthy_returnsTrue()
    {
        Fixture f;
        f.env(Envars::GitTerminalPrompts, Q("1"));
        QVERIFY(f.settings.isTerminalPromptsEnabled());
    }
    void settings_isTerminalPromptsEnabled_envarFalsey_returnsFalse()
    {
        Fixture f;
        f.env(Envars::GitTerminalPrompts, Q("0"));
        QVERIFY(!f.settings.isTerminalPromptsEnabled());
    }

    // --- IsInteractionAllowed ------------------------------------------------------------
    void settings_isInteractionAllowed_envarUnset_returnsTrue() { QVERIFY(Fixture().settings.isInteractionAllowed()); }
    void settings_isInteractionAllowed_envarTruthy_returnsTrue()
    {
        Fixture f;
        f.env(Envars::GcmInteractive, Q("1"));
        QVERIFY(f.settings.isInteractionAllowed());
    }
    void settings_isInteractionAllowed_envarFalsey_returnsFalse()
    {
        Fixture f;
        f.env(Envars::GcmInteractive, Q("0"));
        QVERIFY(!f.settings.isInteractionAllowed());
    }
    void settings_isInteractionAllowed_config_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("ConfigAuto_ReturnsTrue") << Q("auto") << true;
        QTest::newRow("ConfigAlways_ReturnsTrue") << Q("always") << true;
        QTest::newRow("ConfigNever_ReturnsFalse") << Q("never") << false;
        QTest::newRow("ConfigTruthy_ReturnsTrue") << Q("1") << true;
        QTest::newRow("ConfigFalsey_ReturnsFalse") << Q("0") << false;
        QTest::newRow("ConfigNonBooleanyValue_ReturnsTrue") << QUuid::createUuid().toString() << true;
    }
    void settings_isInteractionAllowed_config()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.global(key(CredCfg::SectionName, CredCfg::Interactive), value);
        QCOMPARE(f.settings.isInteractionAllowed(), expected);
    }

    // --- GetTracingEnabled ---------------------------------------------------------------
    void settings_isTracingEnabled_envarUnset_returnsFalse()
    {
        QString value;
        QVERIFY(!Fixture().settings.getTracingEnabled(value));
    }
    void settings_isTracingEnabled_envar_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("EnvarTruthy_ReturnsTrueOutValue") << Q("1") << true;
        QTest::newRow("EnvarFalsey_ReturnsFalseOutValue") << Q("0") << false;
        QTest::newRow("EnvarPathy_ReturnsTrueOutValue") << Q("/tmp/gcm.log") << true;
    }
    void settings_isTracingEnabled_envar()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.env(Envars::GcmTrace, value);
        QString actualValue;
        QCOMPARE(f.settings.getTracingEnabled(actualValue), expected);
        QCOMPARE(actualValue, value);
    }

    // --- IsSecretTracingEnabled ----------------------------------------------------------
    void settings_isSecretTracingEnabled_envarUnset_returnsFalse() { QVERIFY(!Fixture().settings.isSecretTracingEnabled()); }
    void settings_isSecretTracingEnabled_envarTruthy_returnsTrue()
    {
        Fixture f;
        f.env(Envars::GcmTraceSecrets, Q("1"));
        QVERIFY(f.settings.isSecretTracingEnabled());
    }
    void settings_isSecretTracingEnabled_envarFalsey_returnsFalse()
    {
        Fixture f;
        f.env(Envars::GcmTraceSecrets, Q("0"));
        QVERIFY(!f.settings.isSecretTracingEnabled());
    }

    // --- IsWindowsIntegratedAuthenticationEnabled ----------------------------------------
    void settings_isWindowsIntegratedAuthenticationEnabled_envarUnset_returnsTrue()
    {
        QVERIFY(Fixture().settings.isWindowsIntegratedAuthenticationEnabled());
    }
    void settings_isWindowsIntegratedAuthenticationEnabled_envar_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("EnvarTruthy_ReturnsTrue") << Q("1") << true;
        QTest::newRow("EnvarFalsey_ReturnsFalse") << Q("0") << false;
        QTest::newRow("EnvarNonBooleanyValue_ReturnsTrue") << QUuid::createUuid().toString(QUuid::Id128) << true;
    }
    void settings_isWindowsIntegratedAuthenticationEnabled_envar()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.env(Envars::GcmAllowWia, value);
        QCOMPARE(f.settings.isWindowsIntegratedAuthenticationEnabled(), expected);
    }
    void settings_isWindowsIntegratedAuthenticationEnabled_configUnset_returnsTrue()
    {
        QVERIFY(Fixture().settings.isWindowsIntegratedAuthenticationEnabled());
    }
    void settings_isWindowsIntegratedAuthenticationEnabled_config_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("ConfigTruthy_ReturnsTrue") << Q("1") << true;
        QTest::newRow("ConfigFalsey_ReturnsFalse") << Q("0") << false;
        QTest::newRow("ConfigNonBooleanyValue_ReturnsTrue") << QUuid::createUuid().toString() << true;
    }
    void settings_isWindowsIntegratedAuthenticationEnabled_config()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.global(key(CredCfg::SectionName, CredCfg::AllowWia), value);
        QCOMPARE(f.settings.isWindowsIntegratedAuthenticationEnabled(), expected);
    }

    // --- ProxyConfiguration --------------------------------------------------------------
    void settings_proxyConfiguration_convertToBypassRegexArray_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("empty") << Q("") << QStringList();
        QTest::newRow("spaces") << Q("    ") << QStringList();
        QTest::newRow("comma") << Q(",") << QStringList();
        QTest::newRow("example.com") << Q("example.com") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow("example.com:8080") << Q("example.com:8080") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com:8080$")};
        QTest::newRow("example.com,") << Q("example.com,") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow(",example.com") << Q(",example.com") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow(",example.com,") << Q(",example.com,") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow(".example.com") << Q(".example.com") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow("..example.com") << Q("..example.com") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow("*.example.com") << Q("*.example.com") << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$")};
        QTest::newRow("my.example.com") << Q("my.example.com") << QStringList{Q("(\\.|\\:\\/\\/)my\\.example\\.com$")};
        QTest::newRow("three") << Q("example.com,contoso.com,fabrikam.com")
                               << QStringList{Q("(\\.|\\:\\/\\/)example\\.com$"), Q("(\\.|\\:\\/\\/)contoso\\.com$"),
                                              Q("(\\.|\\:\\/\\/)fabrikam\\.com$")};
    }
    void settings_proxyConfiguration_convertToBypassRegexArray()
    {
        QFETCH(QString, input);
        QFETCH(QStringList, expected);
        QCOMPARE(ProxyConfiguration::convertToBypassRegexArray(input), expected);
    }

    void settings_proxyConfiguration_convertToBypassRegexArray_webProxyBypass_data()
    {
        QTest::addColumn<QString>("noProxy");
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("expected");
        QTest::newRow("1") << Q("example.com") << Q("http://example.com") << true;
        QTest::newRow("2") << Q("example.com") << Q("https://example.com") << true;
        QTest::newRow("3") << Q("example.com") << Q("https://www.example.com") << true;
        QTest::newRow("4") << Q("example.com") << Q("http://www.example.com:80") << true;
        QTest::newRow("5") << Q("example.com") << Q("https://www.example.com:443") << true;
        QTest::newRow("6") << Q("example.com") << Q("https://www.example.com:8080") << false;
        QTest::newRow("7") << Q("example.com") << Q("http://notanexample.com") << false;
        QTest::newRow("8") << Q("example.com") << Q("https://notanexample.com") << false;
        QTest::newRow("9") << Q("example.com") << Q("https://www.notanexample.com") << false;
        QTest::newRow("10") << Q("example.com") << Q("https://example.com.otherltd") << false;
        QTest::newRow("11") << Q("example.com:8080") << Q("http://example.com") << false;
        QTest::newRow("12") << Q("my.example.com") << Q("http://example.com") << false;
    }
    void settings_proxyConfiguration_convertToBypassRegexArray_webProxyBypass()
    {
        QFETCH(QString, noProxy);
        QFETCH(QString, address);
        QFETCH(bool, expected);
        QCOMPARE(isBypassed(ProxyConfiguration::convertToBypassRegexArray(noProxy), QUrl(address)), expected);
    }

    void settings_proxyConfiguration_unset_returnsNull()
    {
        QVERIFY(!Fixture(Q("http://example.com/foo.git")).settings.getProxyConfiguration().has_value());
    }

    void settings_proxyConfiguration_gcmHttpConfig_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        f.global(key(CredCfg::SectionName, CredCfg::HttpProxy), ProxySettingValue);
        assertProxy(f.settings.getProxyConfiguration(), true);
    }

    void settings_proxyConfiguration_gcmHttpsConfig_returnsValue()
    {
        Fixture f(Q("https://example.com/foo.git"));
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        f.global(key(CredCfg::SectionName, CredCfg::HttpsProxy), ProxySettingValue);
        assertProxy(f.settings.getProxyConfiguration(), true);
    }

    void settings_proxyConfiguration_gitHttpConfig_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        f.global(key(HttpCfg::SectionName, HttpCfg::Proxy), ProxySettingValue);
        assertProxy(f.settings.getProxyConfiguration(), false);
    }

    void settings_proxyConfiguration_gitHttpConfig_emptyScopedUriUnscoped_returnsNull()
    {
        const QString remoteUrl = Q("http://example.com/foo.git");
        Fixture f(remoteUrl);
        f.global(key(HttpCfg::SectionName, HttpCfg::Proxy), ProxySettingValue);
        f.global(key(HttpCfg::SectionName, remoteUrl, HttpCfg::Proxy), QString());
        QVERIFY(!f.settings.getProxyConfiguration().has_value());
    }

    void settings_proxyConfiguration_curlHttpEnvar_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::CurlHttpProxy, ProxySettingValue);
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        assertProxy(f.settings.getProxyConfiguration(), false);
    }

    void settings_proxyConfiguration_curlHttpsEnvar_returnsValue()
    {
        Fixture f(Q("https://example.com/foo.git"));
        f.env(Envars::CurlHttpsProxy, ProxySettingValue);
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        assertProxy(f.settings.getProxyConfiguration(), false);
    }

    void settings_tryGetProxy_curlAllEnvar_returnsValue()
    {
        Fixture f(Q("https://example.com/foo.git"));
        f.env(Envars::CurlAllProxy, ProxySettingValue);
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        assertProxy(f.settings.getProxyConfiguration(), false);
    }

    void settings_proxyConfiguration_legacyGcmEnvar_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::GcmHttpProxy, ProxySettingValue);
        f.env(Envars::CurlNoProxy, ExpectedNoProxy);
        assertProxy(f.settings.getProxyConfiguration(), true);
    }

    // Own test: the uppercase NO_PROXY is used when the lowercase variable is absent.
    void settings_proxyConfiguration_upperCaseNoProxy_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::CurlHttpProxy, ProxySettingValue);
        f.env(Envars::CurlNoProxyUpper, ExpectedNoProxy);
        assertProxy(f.settings.getProxyConfiguration(), false);
    }

    void settings_proxyConfiguration_precedence_returnsValue()
    {
        const QString remoteUrl = Q("http://example.com/foo.git");
        TestEnvironment envars;
        TestGit git;
        auto runTest = [&](const char *expected) {
            Settings settings(envars, git);
            settings.setRemoteUri(QUrl(remoteUrl));
            const auto actual = settings.getProxyConfiguration();
            QVERIFY(actual.has_value());
            QCOMPARE(absoluteUri(actual->address), uri(expected));
        };

        envars.Variables.set(Envars::GcmHttpProxy, Q("http://proxy1.example.com/"));
        envars.Variables.set(Envars::CurlHttpProxy, Q("http://proxy2.example.com/"));
        runTest("http://proxy2.example.com");
        if (QTest::currentTestFailed()) {
            return;
        }

        git.Configuration.Global[key(HttpCfg::SectionName, HttpCfg::Proxy)] = {Q("http://proxy3.example.com/")};
        runTest("http://proxy3.example.com");
        if (QTest::currentTestFailed()) {
            return;
        }

        git.Configuration.Global[key(CredCfg::SectionName, CredCfg::HttpProxy)] = {Q("http://proxy4.example.com/")};
        runTest("http://proxy4.example.com");
    }

    void settings_proxyConfiguration_curlEnvarPrecedence_prefersLowercase()
    {
        const QString remoteUrl = Q("https://example.com/foo.git");
        const QString value1 = Q("http://proxy1.example.com/"), value2 = Q("http://proxy2.example.com/");
        TestEnvironment envars;
        TestGit git;
        auto runTest = [&] {
            Settings settings(envars, git);
            settings.setRemoteUri(QUrl(remoteUrl));
            const auto actual = settings.getProxyConfiguration();
            QVERIFY(actual.has_value());
            QCOMPARE(absoluteUri(actual->address), value1);
        };
        const std::pair<QString, QString> cases[] = {
            {Envars::CurlHttpsProxy, Envars::CurlHttpsProxyUpper},
            {Envars::CurlHttpsProxy, Envars::CurlHttpProxy},
            {Envars::CurlHttpsProxyUpper, Envars::CurlHttpProxy},
            {Envars::CurlAllProxy, Envars::CurlAllProxyUpper},
        };
        for (const auto &[preferred, other] : cases) {
            envars.Variables.clear();
            envars.Variables.set(preferred, value1);
            envars.Variables.set(other, value2);
            runTest();
            if (QTest::currentTestFailed()) {
                return;
            }
        }
    }

    // --- ProviderOverride / LegacyAuthorityOverride --------------------------------------
    void settings_providerOverride_unset_returnsNull()
    {
        QCOMPARE(Fixture(Q("http://example.com/foo.git")).settings.providerOverride(), OptString());
    }
    void settings_providerOverride_envarSet_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::GcmProvider, Q("provider1"));
        QCOMPARE(f.settings.providerOverride(), OptString(Q("provider1")));
    }
    void settings_providerOverride_configSet_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.global(key(CredCfg::SectionName, CredCfg::Provider), Q("provider1"));
        QCOMPARE(f.settings.providerOverride(), OptString(Q("provider1")));
    }
    void settings_providerOverride_envarAndConfigSet_returnsEnvarValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::GcmProvider, Q("provider1"));
        f.global(key(CredCfg::SectionName, CredCfg::Provider), Q("provider2"));
        QCOMPARE(f.settings.providerOverride(), OptString(Q("provider1")));
    }

    void settings_legacyAuthorityOverride_unset_returnsNull()
    {
        QCOMPARE(Fixture(Q("http://example.com/foo.git")).settings.legacyAuthorityOverride(), OptString());
    }
    void settings_legacyAuthorityOverride_envarSet_returnsValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::GcmAuthority, Q("provider1"));
        QCOMPARE(f.settings.legacyAuthorityOverride(), OptString(Q("provider1")));
    }
    void settings_legacyAuthorityOverride_configSet_returnsTrueOutValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.global(key(CredCfg::SectionName, CredCfg::Authority), Q("provider1"));
        QCOMPARE(f.settings.legacyAuthorityOverride(), OptString(Q("provider1")));
    }
    void settings_legacyAuthorityOverride_envarAndConfigSet_returnsEnvarValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Envars::GcmAuthority, Q("provider1"));
        f.global(key(CredCfg::SectionName, CredCfg::Authority), Q("provider2"));
        QCOMPARE(f.settings.legacyAuthorityOverride(), OptString(Q("provider1")));
    }

    // --- TryGetSetting / GetSettingValues -------------------------------------------------
    void settings_tryGetSetting_envarSet_returnsTrueOutValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Q("GCM_TESTVAR"), Q("Hello, World!"));
        QString actual;
        QVERIFY(f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QCOMPARE(actual, Q("Hello, World!"));
    }
    void settings_tryGetSetting_envarUnset_returnsFalse()
    {
        Fixture f(Q("http://example.com/foo.git"));
        QString actual = Q("unchanged");
        QVERIFY(!f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QVERIFY(actual.isEmpty());
    }
    void settings_tryGetSetting_globalConfig_returnsTrueAndValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.global(Q("gcmtest.bar"), Q("Hello, World!"));
        QString actual;
        QVERIFY(f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QCOMPARE(actual, Q("Hello, World!"));
    }
    void settings_tryGetSetting_repoConfig_returnsTrueAndValue()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.local(Q("gcmtest.bar"), Q("Hello, World!"));
        QString actual;
        QVERIFY(f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QCOMPARE(actual, Q("Hello, World!"));
    }
    void settings_tryGetSetting_scopedConfig()
    {
        Fixture f(Q("http://example.com/foo/bar/bazz.git"));
        f.local(key(Q("gcmtest"), Q("example.com"), Q("bar")), Q("Goodbye, World!"));
        f.local(key(Q("gcmtest"), Q("example.com/foo/bar"), Q("bar")), Q("Hello, World!"));
        QString actual;
        QVERIFY(f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QCOMPARE(actual, Q("Hello, World!"));
    }
    void settings_tryGetSetting_envarAndConfig_envarTakesPrecedence()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Q("GCM_TESTVAR"), Q("Hello, World!"));
        f.local(Q("gcmtest.bar"), Q("Goodbye, World!"));
        QString actual;
        QVERIFY(f.settings.tryGetSetting(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), actual));
        QCOMPARE(actual, Q("Hello, World!"));
    }

    void settings_getSettingValues_envarAndMultipleConfig_returnsAllWithCorrectPrecedence()
    {
        Fixture f(Q("http://example.com/foo.git"));
        f.env(Q("GCM_TESTVAR"), Q("First value"));
        f.local(key(Q("gcmtest"), Q("http://example.com"), Q("bar")), Q("Second value"));
        f.local(key(Q("gcmtest"), Q("example.com"), Q("bar")), Q("Third value"));
        f.local(Q("gcmtest.bar"), Q("Last value"));
        QCOMPARE(f.settings.getSettingValues(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), false),
                 (QStringList{Q("First value"), Q("Second value"), Q("Third value"), Q("Last value")}));
    }

    void settings_getSettingValues_returnsAllMatchingValues()
    {
        Fixture f(Q("http://example.com/foo/bar/bazz.git"));
        f.env(Q("GCM_TESTVAR"), Q("envar-value"));
        f.local(Q("gcmtest.bar"), Q("value-no-scope"));
        f.local(key(Q("gcmtest"), Q("example.com"), Q("bar")), Q("value-scope2"));
        f.local(key(Q("gcmtest"), Q("example.com/foo/bar"), Q("bar")), Q("value-scope1"));
        f.local(key(Q("gcmtest"), Q("test.com"), Q("bar")), Q("other-scope1"));
        f.local(key(Q("gcmtest"), Q("sub.test.com"), Q("bar")), Q("other-scope2"));
        QCOMPARE(f.settings.getSettingValues(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), false),
                 (QStringList{Q("envar-value"), Q("value-scope1"), Q("value-scope2"), Q("value-no-scope")}));
    }

    void settings_getSettingValues_ignoresSectionAndPropertyCase_scopeIsCaseSensitive()
    {
        Fixture f(Q("http://example.com/foo/bar/bazz.git"));
        f.env(Q("GCM_TESTVAR"), Q("envar-value"));
        f.local(Q("gcmtest.BAR"), Q("the-value"));
        f.local(key(Q("GCMTEST"), Q("example.com"), Q("BAR")), Q("value-scope-lo"));
        f.local(key(Q("gcmtest"), Q("EXAMPLE.COM"), Q("bar")), Q("value-scope-hi"));
        QCOMPARE(f.settings.getSettingValues(Q("GCM_TESTVAR"), Q("GcMtEsT"), Q("bAr"), false),
                 (QStringList{Q("envar-value"), Q("value-scope-lo"), Q("the-value")}));
    }

    void settings_getSettingValues_isPath_returnsAllParsedValues_data()
    {
        QTest::addColumn<bool>("isPath");
        QTest::addColumn<QString>("expectedPrefix");
        QTest::newRow("raw") << false << Q("~");
        QTest::newRow("path") << true << TestGitConfiguration::CanonicalPathPrefix;
    }
    void settings_getSettingValues_isPath_returnsAllParsedValues()
    {
        QFETCH(bool, isPath);
        QFETCH(QString, expectedPrefix);
        Fixture f(Q("http://example.com/foo/bar/bazz.git"));
        f.env(Q("GCM_TESTVAR"), Q("envar-value"));
        f.local(Q("gcmtest.bar"), Q("~/path-no-scope"));
        f.local(key(Q("gcmtest"), Q("example.com"), Q("bar")), Q("path-broad"));
        f.local(key(Q("gcmtest"), Q("example.com/foo/bar"), Q("bar")), Q("~/path-tight"));
        QCOMPARE(f.settings.getSettingValues(Q("GCM_TESTVAR"), Q("gcmtest"), Q("bar"), isPath),
                 (QStringList{Q("envar-value"), expectedPrefix + Q("/path-tight"), Q("path-broad"),
                              expectedPrefix + Q("/path-no-scope")}));
    }

    // --- CustomCertificateBundlePath -----------------------------------------------------
    void settings_customCertificateBundlePath_returnsExpectedValue_data()
    {
        QTest::addColumn<QString>("sslCaInfoEnvar");
        QTest::addColumn<QString>("sslCaInfoConfig");
        QTest::addColumn<QString>("expectedValue");
        QTest::newRow("none") << QString() << QString() << QString();
        QTest::newRow("config") << QString() << Q("ca-config.crt") << Q("ca-config.crt");
        QTest::newRow("envar") << Q("ca-envar.crt") << Q("ca-config.crt") << Q("ca-envar.crt");
    }
    void settings_customCertificateBundlePath_returnsExpectedValue()
    {
        QFETCH(QString, sslCaInfoEnvar);
        QFETCH(QString, sslCaInfoConfig);
        QFETCH(QString, expectedValue);
        Fixture f;
        if (!sslCaInfoEnvar.isNull()) {
            f.env(Envars::GitSslCaInfo, sslCaInfoEnvar);
        }
        if (!sslCaInfoConfig.isNull()) {
            f.local(key(HttpCfg::SectionName, HttpCfg::SslCaInfo), sslCaInfoConfig);
        }
        QCOMPARE(f.settings.customCertificateBundlePath(), expectedValue.isNull() ? OptString() : OptString(expectedValue));
    }

    // --- UseMsAuthDefaultAccount ---------------------------------------------------------
    void settings_useMsAuthDefaultAccount_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("1") << Q("1") << true;
        QTest::newRow("true") << Q("true") << true;
        QTest::newRow("0") << Q("0") << false;
        QTest::newRow("false") << Q("false") << false;
    }
    void settings_useMsAuthDefaultAccount_envarSet_returnsValue_data() { settings_useMsAuthDefaultAccount_data(); }
    void settings_useMsAuthDefaultAccount_envarSet_returnsValue()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.env(Envars::MsAuthUseDefaultAccount, value);
        QCOMPARE(f.settings.useMsAuthDefaultAccount(), std::optional<bool>(expected));
    }
    void settings_useMsAuthDefaultAccount_configSet_returnsValue_data() { settings_useMsAuthDefaultAccount_data(); }
    void settings_useMsAuthDefaultAccount_configSet_returnsValue()
    {
        QFETCH(QString, value);
        QFETCH(bool, expected);
        Fixture f;
        f.global(key(CredCfg::SectionName, CredCfg::MsAuthUseDefaultAccount), value);
        QCOMPARE(f.settings.useMsAuthDefaultAccount(), std::optional<bool>(expected));
    }
    void settings_useMsAuthDefaultAccount_unset_returnsPlatformDefault()
    {
        QCOMPARE(Fixture().settings.useMsAuthDefaultAccount(), std::optional<bool>());
    }

    // --- own tests for the remaining properties -------------------------------------------
    void settings_simpleProperties_defaults()
    {
        Fixture f;
        QVERIFY(f.settings.isGuiPromptsEnabled());
        QVERIFY(f.settings.isCertificateVerificationEnabled());
        QVERIFY(!f.settings.automaticallyUseClientCertificates());
        QVERIFY(!f.settings.isMsalTracingEnabled());
        QVERIFY(!f.settings.useSoftwareRendering());
        QVERIFY(!f.settings.allowUnsafeRemotes());
        QCOMPARE(f.settings.credentialNamespace(), Constants::DefaultCredentialNamespace);
        QCOMPARE(f.settings.credentialBackingStore(), OptString());
        QCOMPARE(f.settings.customCookieFilePath(), OptString());
        QCOMPARE(f.settings.parentWindowId(), OptString());
        QCOMPARE(f.settings.autoDetectProviderTimeout(), Constants::DefaultAutoDetectProviderTimeoutMs);
    }

    void settings_simpleProperties_configured()
    {
        Fixture f;
        f.env(Envars::GitSslNoVerify, Q("1"));
        f.env(Envars::GcmCredentialStore, Q("plaintext"));
        f.env(Envars::GcmParentWindow, Q("42"));
        f.global(key(CredCfg::SectionName, CredCfg::GuiPromptsEnabled), Q("false"));
        f.global(key(CredCfg::SectionName, CredCfg::CredNamespace), Q("ns"));
        f.global(key(CredCfg::SectionName, CredCfg::AutoDetectTimeout), Q("-1"));
        f.global(key(CredCfg::SectionName, CredCfg::AllowUnsafeRemotes), Q("yes"));
        f.global(key(CredCfg::SectionName, CredCfg::TraceMsAuth), Q("on"));
        f.global(key(HttpCfg::SectionName, HttpCfg::SslAutoClientCert), Q("true"));
        f.global(key(HttpCfg::SectionName, HttpCfg::CookieFile), Q("~/cookies"));
        QVERIFY(!f.settings.isGuiPromptsEnabled());
        QVERIFY(!f.settings.isCertificateVerificationEnabled());
        QVERIFY(f.settings.automaticallyUseClientCertificates());
        QVERIFY(f.settings.isMsalTracingEnabled());
        QVERIFY(f.settings.allowUnsafeRemotes());
        QCOMPARE(f.settings.credentialNamespace(), Q("ns"));
        QCOMPARE(f.settings.credentialBackingStore(), OptString(Q("plaintext")));
        QCOMPARE(f.settings.customCookieFilePath(), OptString(TestGitConfiguration::CanonicalPathPrefix + Q("/cookies")));
        QCOMPARE(f.settings.parentWindowId(), OptString(Q("42")));
        QCOMPARE(f.settings.autoDetectProviderTimeout(), -1);
    }

    void settings_isCertificateVerificationEnabled_config()
    {
        Fixture f;
        f.global(key(HttpCfg::SectionName, HttpCfg::SslVerify), Q("false"));
        QVERIFY(!f.settings.isCertificateVerificationEnabled());
    }

    void settings_setGuiPromptsEnabled_setsEnvironmentVariable()
    {
        Fixture f;
        f.settings.setGuiPromptsEnabled(false);
        QCOMPARE(f.environment.Variables.value(Envars::GcmGuiPromptsEnabled), Q("False"));
        QVERIFY(!f.settings.isGuiPromptsEnabled());
    }
};

// =============================================================================================

class LinuxSettingsTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void linuxSettings_tryGetExternalDefault_combinesFiles()
    {
        TestEnvironment env;
        TestGit git;
        NullTrace trace;
        TestFileSystem fs;
        fs.Directories = {Q("/"), Q("/etc"), Q("/etc/git-credential-manager"), Q("/etc/git-credential-manager/config.d")};
        fs.Files.set(Q("/etc/git-credential-manager/config.d/01-first"), "core.overrideMe=value1");
        fs.Files.set(Q("/etc/git-credential-manager/config.d/02-second"), "core.overrideMe=value2");
        fs.Files.set(Q("/etc/git-credential-manager/config.d/03-third"), "core.overrideMe=value3");

        LinuxSettings settings(env, git, trace, fs);
        QString value;
        QVERIFY(settings.tryGetExternalDefault(Q("core"), std::nullopt, Q("overrideMe"), value));
        QCOMPARE(value, Q("value3"));
    }

    // Own test: external defaults come last in the precedence order. As upstream, the parser only accepts keys
    // made of [a-zA-Z0-9.-], so a URL-scoped line ("credential.https://...") is rejected as invalid.
    void linuxSettings_getSettingValues_includesExternalDefaults()
    {
        TestEnvironment env;
        TestGit git;
        NullTrace trace;
        TestFileSystem fs;
        fs.Directories = {Q("/etc/git-credential-manager/config.d")};
        fs.Files.set(Q("/etc/git-credential-manager/config.d/defaults"),
                     "credential.https://example.com.provider=scoped-default\ncredential.provider=default\n");
        git.Configuration.Global[Q("credential.provider")] = {Q("from-config")};

        LinuxSettings settings(env, git, trace, fs);
        settings.setRemoteUri(QUrl(Q("https://example.com/repo.git")));
        QCOMPARE(settings.getSettingValues(Q("GCM_PROVIDER"), Q("credential"), Q("provider"), false),
                 (QStringList{Q("from-config"), Q("default")}));
    }

    void linuxSettings_noConfigDirectory_returnsFalse()
    {
        TestEnvironment env;
        TestGit git;
        NullTrace trace;
        TestFileSystem fs;
        LinuxSettings settings(env, git, trace, fs);
        QString value;
        QVERIFY(!settings.tryGetExternalDefault(Q("core"), std::nullopt, Q("overrideMe"), value));
    }
};

// =============================================================================================

class LinuxConfigParserTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void linuxConfigParser_parse()
    {
        const QString contents = Q(
            "#\n"
            "# This is a config file complete with comments\n"
            "# and empty..\n"
            "\n"
            "# lines, as well as lines with..\n"
            "#                              \n"
            "# only whitespace (like above ^), and..\n"
            "invalid lines like this one, not a comment\n"
            "# Here's the first real properties:\n"
            "core.overrideMe=This is the first config value\n"
            "baz.specialChars=I contain special chars like = in my value # this is a comment\n"
            "# and let's have with a comment that also contains a = in side\n"
            "#\n"
            "core.overrideMe=This is the second config value\n"
            "bar.scope.foo=123456\n"
            "core.overrideMe=This is the correct value\n"
            "      ###### comments that start ## with whitespace and extra ## inside\n"
            "strings.one=\"here we have a dq string\"\n"
            "strings.two='here we have a sq string'\n"
            "strings.three=    'here we have another sq string'   # have another sq string\n"
            "strings.four=\"this has 'nested quotes' inside\"\n"
            "strings.five='mixed \"quotes\" the other way around'\n"
            "strings.six='this has an \\'escaped\\' set of quotes'");

        const StringDictionary expected{
            {Q("core.overrideMe"), Q("This is the correct value")},
            {Q("baz.specialChars"), Q("I contain special chars like = in my value")},
            {Q("bar.scope.foo"), Q("123456")},
            {Q("strings.one"), Q("here we have a dq string")},
            {Q("strings.two"), Q("here we have a sq string")},
            {Q("strings.three"), Q("here we have another sq string")},
            {Q("strings.four"), Q("this has 'nested quotes' inside")},
            {Q("strings.five"), Q("mixed \"quotes\" the other way around")},
            {Q("strings.six"), Q("this has an \\'escaped\\' set of quotes")},
        };

        NullTrace trace;
        const StringDictionary actual = LinuxConfigParser(trace).parse(contents);
        // Dictionary equality upstream ignores order
        QCOMPARE(actual.size(), expected.size());
        for (const auto &[k, v] : expected) {
            QCOMPARE(actual.value(k), v);
        }
    }
};

GCM_REGISTER_TEST(SettingsTests)
GCM_REGISTER_TEST(LinuxSettingsTests)
GCM_REGISTER_TEST(LinuxConfigParserTests)
#include "settingstests.moc"
