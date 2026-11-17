// Port of GitHub.Tests/GitHubAuthChallengeTests.cs (all 10 methods).
// clazy:excludeall=non-pod-global-static (test registrations and test data are global objects by design)
#include "github/github.h"
#include "infra/testmain.h"

using namespace gcm;
using namespace gcm::github;

namespace {
QString Q(const char *s) { return QString::fromUtf8(s); }
OptString opt(const char *s) { return s ? OptString(QString::fromUtf8(s)) : OptString(); }
} // namespace

class GitHubAuthChallengeTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fromHeaders_CaseInsensitive()
    {
        const QList<GitHubAuthChallenge> challenges = GitHubAuthChallenge::fromHeaders(
            {Q("BASIC REALM=\"GITHUB\""), Q("basic realm=\"github\""), Q("bAsIc ReAlM=\"gItHuB\"")});
        QCOMPARE(challenges.size(), 3);
        for (const GitHubAuthChallenge &challenge : challenges) {
            QVERIFY(!challenge.domain());
            QVERIFY(!challenge.enterprise());
        }
    }

    void fromHeaders_MultipleRealms_ReturnsGitHubOnly()
    {
        const QList<GitHubAuthChallenge> challenges = GitHubAuthChallenge::fromHeaders(
            {Q("Basic realm=\"contoso\""), Q("Basic realm=\"GitHub\""), Q("Basic realm=\"fabrikam\"")});
        QCOMPARE(challenges.size(), 1);
        QVERIFY(!challenges[0].domain());
        QVERIFY(!challenges[0].enterprise());
    }

    void fromHeaders_NoMatchingRealms_ReturnsEmpty()
    {
        QVERIFY(GitHubAuthChallenge::fromHeaders({Q("Basic realm=\"contoso\""), Q("Basic realm=\"fabrikam\""), Q("Basic realm=\"example\"")})
                    .isEmpty());
    }

    void fromHeaders_Hints_ReturnsWithHints_data()
    {
        QTest::addColumn<QString>("header");
        QTest::addColumn<OptString>("domain");
        QTest::addColumn<OptString>("enterprise");
        QTest::newRow("1") << Q("Basic realm=\"GitHub\" enterprise_hint=\"contoso-corp\" domain_hint=\"contoso\"") << opt("contoso") << opt("contoso-corp");
        QTest::newRow("2") << Q("Basic realm=\"GitHub\" domain_hint=\"contoso\"") << opt("contoso") << opt(nullptr);
        QTest::newRow("3") << Q("Basic realm=\"GitHub\" enterprise_hint=\"contoso-corp\"") << opt(nullptr) << opt("contoso-corp");
        QTest::newRow("4") << Q("Basic realm=\"GitHub\" domain_hint=\"fab\" enterprise_hint=\"fabirkamopensource\"") << opt("fab") << opt("fabirkamopensource");
        QTest::newRow("5") << Q("Basic enterprise_hint=\"iana\" realm=\"GitHub\" domain_hint=\"example\"") << opt("example") << opt("iana");
        QTest::newRow("6") << Q("Basic domain_hint=\"test\" enterprise_hint=\"test-inc\" realm=\"GitHub\"") << opt("test") << opt("test-inc");
    }

    void fromHeaders_Hints_ReturnsWithHints()
    {
        QFETCH(QString, header);
        QFETCH(OptString, domain);
        QFETCH(OptString, enterprise);
        const QList<GitHubAuthChallenge> challenges = GitHubAuthChallenge::fromHeaders({header});
        QCOMPARE(challenges.size(), 1);
        QCOMPARE(challenges[0].domain(), domain);
        QCOMPARE(challenges[0].enterprise(), enterprise);
    }

    void fromHeaders_EmptyHeaders_ReturnsEmpty()
    {
        QVERIFY(GitHubAuthChallenge::fromHeaders({}).isEmpty());
    }

    void isDomainMember_NoHint_data()
    {
        QTest::addColumn<OptString>("userName");
        QTest::addColumn<bool>("expected");
        QTest::newRow("null") << opt(nullptr) << false;
        QTest::newRow("empty") << opt("") << false;
        QTest::newRow("space") << opt(" ") << false;
        QTest::newRow("alice") << opt("alice") << true;
        QTest::newRow("alice_contoso") << opt("alice_contoso") << false;
        QTest::newRow("alice_CONTOSO") << opt("alice_CONTOSO") << false;
        QTest::newRow("alice_contoso_alt") << opt("alice_contoso_alt") << false;
        QTest::newRow("pj_nitin") << opt("pj_nitin") << true;
        QTest::newRow("up_the_irons") << opt("up_the_irons") << true;
    }

    void isDomainMember_NoHint()
    {
        QFETCH(OptString, userName);
        QFETCH(bool, expected);
        QCOMPARE(GitHubAuthChallenge().isDomainMember(userName), expected);
    }

    void isDomainMember_DomainHint_data()
    {
        QTest::addColumn<OptString>("userName");
        QTest::addColumn<bool>("expected");
        QTest::newRow("null") << opt(nullptr) << false;
        QTest::newRow("empty") << opt("") << false;
        QTest::newRow("space") << opt(" ") << false;
        QTest::newRow("alice") << opt("alice") << false;
        QTest::newRow("alice_contoso") << opt("alice_contoso") << true;
        QTest::newRow("alice_CONTOSO") << opt("alice_CONTOSO") << true;
        QTest::newRow("alice_contoso_alt") << opt("alice_contoso_alt") << false;
        QTest::newRow("pj_nitin") << opt("pj_nitin") << false;
        QTest::newRow("up_the_irons") << opt("up_the_irons") << false;
    }

    void isDomainMember_DomainHint()
    {
        QFETCH(OptString, userName);
        QFETCH(bool, expected);
        QCOMPARE(GitHubAuthChallenge(Q("contoso"), Q("contoso-corp")).isDomainMember(userName), expected);
    }

    void equals_Null_ReturnsFalse()
    {
        // A C++ value cannot be null; the closest is a challenge without hints
        QVERIFY(!(GitHubAuthChallenge(Q("contoso"), Q("contoso-corp")) == GitHubAuthChallenge()));
    }

    void equals_SameInstance_ReturnsTrue()
    {
        const GitHubAuthChallenge challenge(Q("contoso"), Q("contoso-corp"));
        QVERIFY(challenge == challenge);
    }

    void equals_DifferentInstance_ReturnsTrue()
    {
        QVERIFY(GitHubAuthChallenge(Q("contoso"), Q("constoso-corp")) == GitHubAuthChallenge(Q("contoso"), Q("constoso-corp")));
    }

    void equals_DifferentCase_ReturnsTrue()
    {
        QVERIFY(GitHubAuthChallenge(Q("contoso"), Q("contoso-corp")) == GitHubAuthChallenge(Q("CONTOSO"), Q("CONTOSO-CORP")));
    }

    void equals_DifferentShortCode_ReturnsFalse()
    {
        QVERIFY(!(GitHubAuthChallenge(Q("contoso"), Q("constoso-corp")) == GitHubAuthChallenge(Q("fab"), Q("fabrikamopensource"))));
    }
};

GCM_REGISTER_TEST(GitHubAuthChallengeTests)
#include "githubauthchallengetests.moc"
