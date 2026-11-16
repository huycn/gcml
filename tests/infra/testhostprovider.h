// Port of TestInfrastructure/Objects/TestHostProvider.cs and TestHostProviderRegistry.cs
#pragma once

#include "core/hostprovider.h"

namespace gcm::test {

class TestHostProvider : public HostProvider
{
public:
    explicit TestHostProvider(ICommandContext &context) : HostProvider(context) {}

    std::function<bool(const GitRequest &)> IsSupportedFunc;
    QString LegacyAuthorityIdValue;
    std::function<Credential(const GitRequest &)> GenerateCredentialFunc;

    QString id() const override { return QStringLiteral("test-provider"); }
    QString name() const override { return QStringLiteral("TestHostProvider"); }
    QString legacyAuthorityId() const { return LegacyAuthorityIdValue; }
    using HostProvider::isSupported;
    bool isSupported(const GitRequest &request) override { return IsSupportedFunc(request); }
    Credential generateCredential(const GitRequest &request) override { return GenerateCredentialFunc(request); }
};

class TestHostProviderRegistry : public IHostProviderRegistry
{
public:
    std::shared_ptr<IHostProvider> Provider;

    void registerProvider(std::shared_ptr<IHostProvider>, HostProviderPriority) override {}
    std::shared_ptr<IHostProvider> getProvider(const GitRequest &) override { return Provider; }
};

} // namespace gcm::test
