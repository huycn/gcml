// Generic interactive authentication flows shared by host providers.
// Ports Authentication/BasicAuthentication.cs, OAuthAuthentication.cs, WindowsIntegratedAuthentication.cs.
#pragma once

#include "core/auth/authbase.h"
#include "core/auth/oauth2.h"

namespace gcm {

// --- basic --------------------------------------------------------------------------------
class IBasicAuthentication
{
public:
    virtual ~IBasicAuthentication() = default;

    /// Prompts for username/password for `resource` (GUI helper `basic` command, else terminal).
    virtual Credential getCredentials(const QString &resource, const OptString &userName = std::nullopt) = 0;
};

class BasicAuthentication : public AuthenticationBase, public IBasicAuthentication
{
public:
    static const QStringList AuthorityIds;

    explicit BasicAuthentication(ICommandContext &context) : AuthenticationBase(context) {}

    Credential getCredentials(const QString &resource, const OptString &userName = std::nullopt) override;
};

// --- OAuth --------------------------------------------------------------------------------
enum class OAuthAuthenticationModes : int
{
    None = 0,
    Browser = 1 << 0,
    DeviceCode = 1 << 1,
    All = Browser | DeviceCode,
};

constexpr OAuthAuthenticationModes operator|(OAuthAuthenticationModes a, OAuthAuthenticationModes b)
{
    return OAuthAuthenticationModes(int(a) | int(b));
}

constexpr OAuthAuthenticationModes operator&(OAuthAuthenticationModes a, OAuthAuthenticationModes b)
{
    return OAuthAuthenticationModes(int(a) & int(b));
}

constexpr OAuthAuthenticationModes operator~(OAuthAuthenticationModes a)
{
    return OAuthAuthenticationModes(~int(a) & int(OAuthAuthenticationModes::All));
}

constexpr bool hasFlag(OAuthAuthenticationModes value, OAuthAuthenticationModes flag)
{
    return (int(value) & int(flag)) != 0;
}

class IOAuthAuthentication
{
public:
    virtual ~IOAuthAuthentication() = default;

    /// Picks one mode out of `modes`, asking the user (helper `oauth` command, else terminal menu) when several apply.
    virtual OAuthAuthenticationModes getAuthenticationMode(const QString &resource, OAuthAuthenticationModes modes) = 0;
    virtual OAuth2TokenResult getTokenByBrowser(IOAuth2Client &client, const QStringList &scopes) = 0;
    /// Shows the device code (helper `device` command, else terminal) while polling for the token.
    virtual OAuth2TokenResult getTokenByDeviceCode(IOAuth2Client &client, const QStringList &scopes) = 0;
};

class OAuthAuthentication : public AuthenticationBase, public IOAuthAuthentication
{
public:
    explicit OAuthAuthentication(ICommandContext &context) : AuthenticationBase(context) {}

    OAuthAuthenticationModes getAuthenticationMode(const QString &resource, OAuthAuthenticationModes modes) override;
    OAuth2TokenResult getTokenByBrowser(IOAuth2Client &client, const QStringList &scopes) override;
    OAuth2TokenResult getTokenByDeviceCode(IOAuth2Client &client, const QStringList &scopes) override;
};

// --- Windows Integrated Authentication (detection only on Linux) --------------------------
enum class NtlmSupport
{
    Once,
    Always,
    Disabled,
};

enum class WindowsAuthenticationTypes : int
{
    None = 0,
    Ntlm = 1,
    Negotiate = 2,
    All = Ntlm | Negotiate,
};

constexpr WindowsAuthenticationTypes operator|(WindowsAuthenticationTypes a, WindowsAuthenticationTypes b)
{
    return WindowsAuthenticationTypes(int(a) | int(b));
}

constexpr bool hasFlag(WindowsAuthenticationTypes value, WindowsAuthenticationTypes flag)
{
    return (int(value) & int(flag)) != 0;
}

class IWindowsIntegratedAuthentication
{
public:
    virtual ~IWindowsIntegratedAuthentication() = default;

    virtual NtlmSupport askEnableNtlm(const QUrl &uri) = 0;
    /// Probes `uri` (HEAD) and reports which of NTLM / Negotiate the server offers in WWW-Authenticate.
    virtual WindowsAuthenticationTypes getAuthenticationTypes(const QUrl &uri) = 0;
};

class WindowsIntegratedAuthentication : public AuthenticationBase, public IWindowsIntegratedAuthentication
{
public:
    static const QStringList AuthorityIds;

    explicit WindowsIntegratedAuthentication(ICommandContext &context) : AuthenticationBase(context) {}

    NtlmSupport askEnableNtlm(const QUrl &uri) override;
    WindowsAuthenticationTypes getAuthenticationTypes(const QUrl &uri) override;
};

} // namespace gcm
