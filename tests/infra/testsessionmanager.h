// Port of TestInfrastructure/Objects/TestSessionManager.cs
#pragma once

#include "core/platform.h"

namespace gcm::test {

class TestSessionManager : public ISessionManager
{
public:
    std::optional<bool> IsWebBrowserAvailableOverride;
    bool IsDesktopSession = false;
    std::function<void(const QUrl &)> OpenBrowserFunc = [](const QUrl &) {};

    bool isDesktopSession() const override { return IsDesktopSession; }
    bool isWebBrowserAvailable() const override { return IsWebBrowserAvailableOverride.value_or(IsDesktopSession); }
    void openBrowser(const QUrl &uri) override { OpenBrowserFunc(uri); }
    using ISessionManager::openBrowser;
};

} // namespace gcm::test
