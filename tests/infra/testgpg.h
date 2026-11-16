// Port of TestInfrastructure/Objects/TestGpg.cs: "encrypts" by storing the plain text in a TestFileSystem.
#pragma once

#include "core/credential.h"
#include "infra/testfilesystem.h"

namespace gcm::test {

class TestGpg : public IGpg
{
public:
    explicit TestGpg(TestFileSystem &fs) : m_fs(fs) {}

    QString decryptFile(const QString &path) override { return QString::fromUtf8(m_fs.readAllBytes(path)); }
    void encryptFile(const QString &path, const QString &gpgId, const QString &contents) override
    {
        if (!m_keys.contains(gpgId.toCaseFolded())) {
            throw Exception(QStringLiteral("No GPG key found for '%1'.").arg(gpgId));
        }
        m_fs.Files.set(path, contents.toUtf8());
    }
    void generateKeys(const QString &userId) { m_keys.insert(userId.toCaseFolded()); }

private:
    TestFileSystem &m_fs;
    QSet<QString> m_keys;
};

} // namespace gcm::test
