// Port of TestInfrastructure/Objects/TestGit.cs
#pragma once

#include "core/git.h"
#include "infra/testgitconfiguration.h"

#include <QDir>
#include <QUuid>

namespace gcm::test {

class TestGit : public IGit
{
public:
    explicit TestGit(bool insideRepo = true)
    {
        if (insideRepo) {
            CurrentRepository = getFakeRepositoryPath();
        }
    }

    GitVersion Version{QStringLiteral("2.32.0.test.0")};
    OptString CurrentRepository;
    QList<GitRemote> Remotes;
    TestGitConfiguration Configuration;

    // IGit
    GitVersion version() override { return Version; }
    std::unique_ptr<ChildProcess> createProcess(const QString &) override
    {
        throw NotSupportedException(QStringLiteral("TestGit::createProcess is not implemented"));
    }
    bool isInsideRepository() override { return CurrentRepository && !CurrentRepository->trimmed().isEmpty(); }
    OptString getCurrentRepository() override { return CurrentRepository; }
    QList<GitRemote> getRemotes() override { return Remotes; }
    IGitConfiguration &getConfiguration() override { return Configuration; }
    StringDictionary invokeHelper(const QString &, const StringDictionary * = nullptr) override
    {
        throw NotSupportedException(QStringLiteral("TestGit::invokeHelper is not implemented"));
    }

    static QString getFakeRepositoryPath(const OptString &name = std::nullopt)
    {
        const QString n = name ? *name : QUuid::createUuid().toString(QUuid::Id128).mid(8);
        return QDir::tempPath() + QStringLiteral("/fake-repo/") + n;
    }
};

} // namespace gcm::test
