// Port of TestInfrastructure/Objects/NullTrace.cs
#pragma once

#include "core/trace.h"

namespace gcm::test {

class NullTrace : public ITrace
{
public:
    bool hasListeners() const override { return false; }
    bool isSecretTracingEnabled() const override { return m_secrets; }
    void setSecretTracingEnabled(bool value) override { m_secrets = value; }
    void addListener(std::shared_ptr<TextWriter>) override {}
    void flush() override {}
    void writeException(const std::exception &, const Loc & = Loc::current()) override {}
    void writeDictionary(const StringDictionary &, const Loc & = Loc::current()) override {}
    void writeDictionarySecrets(const MultiDictionary &, const QStringList &, Qt::CaseSensitivity = Qt::CaseSensitive,
                                const Loc & = Loc::current()) override {}
    void writeLine(const QString &, const Loc & = Loc::current()) override {}
    void writeLineSecrets(const QString &, const QStringList &, const Loc & = Loc::current()) override {}

private:
    bool m_secrets = false;
};

} // namespace gcm::test
