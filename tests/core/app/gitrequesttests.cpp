// Ports of GitRequestTests and GitResponseTests.
// Not ported: null-argument cases (a GitRequest is built from a dictionary value, a Credential cannot be null) and
// the null key/value cases of SetState (QString cannot be null); "same instance" checks compare values.
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "core/gitrequest.h"
#include "core/strings.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::test;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }
QString uri(const std::optional<QUrl> &u) { return u ? absoluteUri(*u) : Q("<null>"); }
QString expected(const char *s) { return absoluteUri(QUrl(Q(s))); }
} // namespace

class GitRequestTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void gitRequest_commonArguments_valuePresent_returnsValues()
    {
        const QStringList wwwauth{Q("basic realm=\"example.com\""), Q("bearer authorize_uri=https://id.example.com p=1 q=0")};
        const GitRequest request(MultiDictionary{{Q("protocol"), {Q("https")}},
                                                 {Q("host"), {Q("example.com")}},
                                                 {Q("path"), {Q("an/example/path")}},
                                                 {Q("username"), {Q("john.doe")}},
                                                 {Q("password"), {Q("password123")}},
                                                 {Q("wwwauth"), wwwauth}});
        QCOMPARE(request.protocol(), OptString(Q("https")));
        QCOMPARE(request.host(), OptString(Q("example.com")));
        QCOMPARE(request.path(), OptString(Q("an/example/path")));
        QCOMPARE(request.userName(), OptString(Q("john.doe")));
        QCOMPARE(request.password(), OptString(Q("password123")));
        QCOMPARE(request.wwwAuth(), wwwauth);
    }

    void gitRequest_commonArguments_valueMissing_returnsNullOrEmptyCollection()
    {
        const GitRequest request{StringDictionary()};
        QCOMPARE(request.protocol(), OptString());
        QCOMPARE(request.host(), OptString());
        QCOMPARE(request.path(), OptString());
        QCOMPARE(request.userName(), OptString());
        QCOMPARE(request.password(), OptString());
        QVERIFY(request.wwwAuth().isEmpty());
    }

    void gitRequest_otherArguments()
    {
        const GitRequest request(MultiDictionary{{Q("foo"), {Q("bar")}}, {Q("multi"), {Q("val1"), Q("val2"), Q("val3")}}});
        QCOMPARE(request[Q("foo")], OptString(Q("bar")));
        QCOMPARE(request.getArgumentOrDefault(Q("foo")), OptString(Q("bar")));
        QCOMPARE(request.getMultiArgumentOrDefault(Q("multi")), (QStringList{Q("val1"), Q("val2"), Q("val3")}));
    }

    void gitRequest_getRemoteUri_noAuthority_returnsNull()
    {
        QVERIFY(!GitRequest(StringDictionary()).getRemoteUri().has_value());
    }

    void gitRequest_getRemoteUri_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("host");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("userName");
        QTest::addColumn<bool>("includeUser");
        QTest::addColumn<QString>("expected");
        QTest::newRow("Authority_ReturnsUriWithAuthority") << Q("https") << Q("example.com") << QString() << QString() << false << expected("https://example.com/");
        QTest::newRow("IncludeUser_Authority_ReturnsUriWithAuthorityAndUser") << Q("https") << Q("example.com") << QString() << Q("john.doe") << true << expected("https://john.doe@example.com/");
        QTest::newRow("IncludeUserSpecialCharacters") << Q("https") << Q("example.com") << QString() << Q("john.doe@domain.com") << true << expected("https://john.doe%40domain.com@example.com/");
        QTest::newRow("IncludeUserNoUser_Authority") << Q("https") << Q("example.com") << QString() << QString() << true << expected("https://example.com/");
        QTest::newRow("AuthorityAndPort") << Q("https") << Q("example.com:456") << QString() << QString() << false << expected("https://example.com:456/");
        QTest::newRow("AuthorityPath") << Q("https") << Q("example.com") << Q("an/example/path") << QString() << false << expected("https://example.com/an/example/path");
        QTest::newRow("AuthorityPathUserInfo") << Q("https") << Q("example.com") << Q("an/example/path") << Q("john.doe") << false << expected("https://example.com/an/example/path");
        QTest::newRow("PathQuery") << Q("https") << Q("example.com") << Q("foo?query=true") << QString() << false << expected("https://example.com/foo?query=true");
        QTest::newRow("PathFragment") << Q("https") << Q("example.com") << Q("foo#fragment") << QString() << false << expected("https://example.com/foo#fragment");
        QTest::newRow("PathQueryFragment") << Q("https") << Q("example.com") << Q("foo?query=true#fragment") << QString() << false << expected("https://example.com/foo?query=true#fragment");
        QTest::newRow("IncludeUser_AuthorityPathUserInfo") << Q("https") << Q("example.com") << Q("an/example/path") << Q("john.doe") << true << expected("https://john.doe@example.com/an/example/path");
    }
    void gitRequest_getRemoteUri()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, host);
        QFETCH(QString, path);
        QFETCH(QString, userName);
        QFETCH(bool, includeUser);
        QFETCH(QString, expected);
        StringDictionary dict{{Q("protocol"), protocol}, {Q("host"), host}};
        if (!path.isNull()) {
            dict.set(Q("path"), path);
        }
        if (!userName.isNull()) {
            dict.set(Q("username"), userName);
            dict.set(Q("password"), Q("password123")); // never part of the URI
        }
        QCOMPARE(uri(GitRequest(dict).getRemoteUri(includeUser)), expected);
    }

    void gitRequest_tryGetHostAndPort_noPort_returnsHostName()
    {
        const GitRequest request(StringDictionary{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}});
        QString host;
        std::optional<int> port;
        QVERIFY(request.tryGetHostAndPort(host, port));
        QCOMPARE(host, Q("example.com"));
        QVERIFY(!port);
    }

    void gitRequest_tryGetHostAndPort_port_returnsHostNameAndPort()
    {
        const GitRequest request(StringDictionary{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com:456")}});
        QString host;
        std::optional<int> port;
        QVERIFY(request.tryGetHostAndPort(host, port));
        QCOMPARE(host, Q("example.com"));
        QCOMPARE(port, std::optional<int>(456));
    }

    void gitRequest_tryGetHostAndPort_badPort_returnsFalse()
    {
        const GitRequest request(StringDictionary{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com:not-a-port")}});
        QString host;
        std::optional<int> port;
        QVERIFY(!request.tryGetHostAndPort(host, port));
        QVERIFY(!port);
    }

    void gitRequest_tryGetHostAndPort_noHostNoPort_returnsFalse()
    {
        QString host;
        std::optional<int> port;
        QVERIFY(!GitRequest(StringDictionary{{Q("protocol"), Q("https")}}).tryGetHostAndPort(host, port));
    }

    void gitRequest_capabilities_noInput_returnsNone()
    {
        const GitRequest request(StringDictionary{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}});
        QCOMPARE(request.capabilities(), GitCapabilities::None);
    }

    void gitRequest_capabilities_unknownNames_areSilentlyDiscarded()
    {
        const GitRequest request(MultiDictionary{{Q("protocol"), {Q("https")}},
                                                 {Q("host"), {Q("example.com")}},
                                                 {Q("capability"), {Q("this-cap-does-not-exist"), Q("another-unknown")}}});
        QCOMPARE(request.capabilities(), GitCapabilities::None);
    }

    void gitRequest_state_noStateInput_isEmpty()
    {
        QVERIFY(GitRequest(StringDictionary{{Q("protocol"), Q("https")}, {Q("host"), Q("example.com")}}).state().isEmpty());
    }

    void gitRequest_state_keepsOnlyGcmPrefixedEntries_andStripsPrefix()
    {
        const GitRequest request(MultiDictionary{
            {Q("protocol"), {Q("https")}},
            {Q("host"), {Q("example.com")}},
            {Q("state"), {Q("gcm.github.account=alice"), Q("other-helper.foo=bar"), Q("gcm.azure.tenant=contoso")}}});
        QCOMPARE(request.state().size(), 2);
        QCOMPARE(request.state().value(Q("github.account")), Q("alice"));
        QCOMPARE(request.state().value(Q("azure.tenant")), Q("contoso"));
        QVERIFY(!request.state().contains(Q("other-helper.foo")));
    }

    void gitRequest_state_malformedEntries_areSilentlyDiscarded()
    {
        const GitRequest request(MultiDictionary{
            {Q("protocol"), {Q("https")}},
            {Q("host"), {Q("example.com")}},
            {Q("state"), {Q("gcm.valid=ok"), Q("gcm.no-equals"), Q("=value-without-key"), Q("gcm.=empty-key-after-prefix")}}});
        QCOMPARE(request.state().size(), 1);
        QCOMPARE(request.state().value(Q("valid")), Q("ok"));
    }

    void gitRequest_state_valueMayContainEquals()
    {
        const GitRequest request(MultiDictionary{
            {Q("protocol"), {Q("https")}}, {Q("host"), {Q("example.com")}}, {Q("state"), {Q("gcm.token=abc=def=ghi")}}});
        QCOMPARE(request.state().value(Q("token")), Q("abc=def=ghi"));
    }

    // Own test: like UriBuilder, text after a '/' in the host argument goes in front of the path
    void gitRequest_getRemoteUri_hostWithSlash()
    {
        const GitRequest request(MultiDictionary{{Q("protocol"), {Q("https")}}, {Q("host"), {Q("example.com:8443/")}}, {Q("path"), {Q("repo.git")}}});
        const std::optional<QUrl> uri = request.getRemoteUri();
        QVERIFY(uri.has_value());
        QCOMPARE(uri->host(), Q("example.com"));
        QCOMPARE(uri->port(), 8443);
        QCOMPARE(uri->path(), Q("//repo.git"));
    }
};

// =============================================================================================

class GitResponseTests : public QObject
{
    Q_OBJECT
    const Credential Alice{Q("alice"), Q("hunter2")};

private Q_SLOTS:
    void gitResponse_constructor_setsCredential()
    {
        const GitResponse response(Alice);
        QCOMPARE(*response.credential(), Alice);
    }

    void gitResponse_additionalProperties_defaultsToEmptyDictionary()
    {
        QVERIFY(GitResponse(Alice).additionalProperties.isEmpty());
    }

    void gitResponse_additionalProperties_caseInsensitive()
    {
        GitResponse response(Alice);
        response.additionalProperties[Q("NTLM")] = Q("allow");
        QString value;
        QVERIFY(response.additionalProperties.tryGetValue(Q("ntlm"), value));
        QCOMPARE(value, Q("allow"));
    }

    void gitResponse_additionalProperties_canBeReplaced()
    {
        GitResponse response(Alice);
        response.additionalProperties = StringDictionary{{Q("ntlm"), Q("allow")}};
        QCOMPARE(response.additionalProperties.size(), 1);
        QCOMPARE(response.additionalProperties.value(Q("ntlm")), Q("allow"));
    }

    void gitResponse_ok_returnsSuccessfulResponseWithCredential()
    {
        const GitResponse response = GitResponse::ok(Alice);
        QCOMPARE(*response.credential(), Alice);
        QVERIFY(!response.isCancelled());
    }

    void gitResponse_cancel_returnsCancellationResponseWithNoCredential()
    {
        const GitResponse response = GitResponse::cancel();
        QVERIFY(response.isCancelled());
        QVERIFY(!response.isYielded());
        QVERIFY(!response.credential());
    }

    void gitResponse_yield_returnsYieldedResponseWithNoCredential()
    {
        const GitResponse response = GitResponse::yield();
        QVERIFY(response.isYielded());
        QVERIFY(!response.isCancelled());
        QVERIFY(!response.credential());
    }

    void gitResponse_ok_isNeitherCancelledNorYielded()
    {
        const GitResponse response = GitResponse::ok(Alice);
        QVERIFY(!response.isCancelled());
        QVERIFY(!response.isYielded());
        QVERIFY(!response.isContinue());
    }

    void gitResponse_continue_returnsResponseWithCredentialAndIsContinue()
    {
        const GitResponse response = GitResponse::continue_(Alice);
        QCOMPARE(*response.credential(), Alice);
        QVERIFY(response.isContinue());
        QVERIFY(!response.isCancelled());
        QVERIFY(!response.isYielded());
    }

    void gitResponse_ok_state_startsEmpty() { QVERIFY(GitResponse::ok(Alice).state().isEmpty()); }

    void gitResponse_setState_onOk_storesEntry()
    {
        GitResponse response = GitResponse::ok(Alice);
        response.setState(Q("github.account"), Q("alice"));
        QCOMPARE(response.state().size(), 1);
        QCOMPARE(response.state().value(Q("github.account")), Q("alice"));
    }

    void gitResponse_setState_onContinue_storesEntry()
    {
        GitResponse response = GitResponse::continue_(Alice);
        response.setState(Q("attempt"), Q("1"));
        QCOMPARE(response.state().value(Q("attempt")), Q("1"));
    }

    void gitResponse_setState_onCancel_isSilentNoOp()
    {
        GitResponse response = GitResponse::cancel();
        response.setState(Q("k"), Q("v"));
        QVERIFY(response.state().isEmpty());
    }

    void gitResponse_setState_onYield_isSilentNoOp()
    {
        GitResponse response = GitResponse::yield();
        response.setState(Q("k"), Q("v"));
        QVERIFY(response.state().isEmpty());
    }

    void gitResponse_setState_invalidKey_alwaysThrows()
    {
        GitResponse ok = GitResponse::ok(Alice);
        GitResponse cancel = GitResponse::cancel();
        QVERIFY_THROWS_EXCEPTION(ArgumentException, ok.setState(Q("gcm.foo"), Q("v")));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, ok.setState(Q("k=k"), Q("v")));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, cancel.setState(Q("gcm.foo"), Q("v")));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, cancel.setState(Q("k=k"), Q("v")));
    }

    void gitResponse_setState_invalidValue_alwaysThrows()
    {
        GitResponse ok = GitResponse::ok(Alice);
        GitResponse yield = GitResponse::yield();
        QVERIFY_THROWS_EXCEPTION(ArgumentException, ok.setState(Q("k"), Q("has\nnewline")));
        QVERIFY_THROWS_EXCEPTION(ArgumentException, yield.setState(Q("k"), Q("has\nnewline")));
    }

    void gitResponse_withState_returnsSameInstanceForChaining()
    {
        GitResponse response = GitResponse::ok(Alice);
        GitResponse &result = response.withState(Q("k"), Q("v"));
        QCOMPARE(&result, &response);
        QCOMPARE(response.state().value(Q("k")), Q("v"));
    }

    void gitResponse_withState_chainsMultipleEntries()
    {
        GitResponse response = GitResponse::continue_(Alice);
        response.withState(Q("github.account"), Q("alice")).withState(Q("attempt"), Q("1"));
        QCOMPARE(response.state().size(), 2);
        QCOMPARE(response.state().value(Q("github.account")), Q("alice"));
        QCOMPARE(response.state().value(Q("attempt")), Q("1"));
    }

    void gitResponse_withState_onCancel_stillReturnsResponseStateRemainsEmpty()
    {
        GitResponse response = GitResponse::cancel();
        response.withState(Q("k"), Q("v"));
        QVERIFY(response.isCancelled());
        QVERIFY(response.state().isEmpty());
    }
};

GCM_REGISTER_TEST(GitRequestTests)
GCM_REGISTER_TEST(GitResponseTests)
#include "gitrequesttests.moc"
