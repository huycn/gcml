// Port of GitCredentialManager.Constants. Names and values are kept identical to upstream.
#pragma once

#include "core/common.h"
#include "core/gitcapabilities.h"

// String constants. A QString cannot be constexpr, and an inline variable would make every file that includes the header
// construct it at start-up, so GCM_STR declares the constant and exactly one .cpp per header defines it: the one that
// defines GCM_DEFINE_STRINGS_<HEADER> before its includes (see constants.cpp). Each header with GCM_STR constants
// selects the form below right before using it. Do not initialise a global object of another file from these
// constants: the order of initialisation between files is unspecified.
#define GCM_STR_DECLARE(name, value) extern const QString name
#define GCM_STR_DEFINE(name, value) extern const QString name; const QString name = QStringLiteral(value)
#undef GCM_STR
#ifdef GCM_DEFINE_STRINGS_CONSTANTS
#define GCM_STR GCM_STR_DEFINE
#else
#define GCM_STR GCM_STR_DECLARE
#endif

namespace gcm::Constants {

    GCM_STR(DefaultWindowTitle, "Git Credential Manager");
    GCM_STR(PersonalAccessTokenUserName, "PersonalAccessToken");
    GCM_STR(DefaultCredentialNamespace, "git");
    inline constexpr int DefaultAutoDetectProviderTimeoutMs = 2000;
    GCM_STR(DefaultUiHelper, "git-credential-manager-ui");

    GCM_STR(ProviderIdAuto, "auto");
    GCM_STR(AuthorityIdAuto, "auto");

    GCM_STR(GcmDataDirectoryName, ".gcm");
    GCM_STR(LinuxAppDefaultsDirectoryPath, "/etc/git-credential-manager/config.d");

    /// Home tenant ID for Microsoft Accounts (MSA).
    GCM_STR(MsaHomeTenantId, "9188040d-6c67-4c5b-b112-36a304b66dad");
    /// Special tenant ID for transferring between MSA native tokens and AAD tokens.
    GCM_STR(MsaTransferTenantId, "f8cdef31-a31e-4b4a-93e4-5f571e91255a");
    GCM_STR(DefaultWorkloadFederationAudience, "api://AzureADTokenExchange");

    /// The set of Git credential protocol capabilities supported by Git Credential Manager.
    inline constexpr GitCapabilities SupportedCapabilities = GitCapabilities::State | GitCapabilities::AuthType;

    namespace CredentialProtocol {
        GCM_STR(NtlmKey, "ntlm");
        GCM_STR(NtlmSuppressed, "suppressed");
        GCM_STR(NtlmAllow, "allow");
        GCM_STR(StateKey, "state");
        GCM_STR(ContinueKey, "continue");
        GCM_STR(AuthTypeKey, "authtype");
        GCM_STR(CredentialKey, "credential");
        GCM_STR(EphemeralKey, "ephemeral");
        GCM_STR(PasswordKey, "password");
        GCM_STR(UserNameKey, "username");
        /// Prefix GCM reserves on every `state[]` entry.
        GCM_STR(GcmStatePrefix, "gcm.");
    } // namespace CredentialProtocol

    namespace CredentialStoreNames {
        GCM_STR(Gpg, "gpg");
        GCM_STR(SecretService, "secretservice");
        GCM_STR(Plaintext, "plaintext");
        GCM_STR(Cache, "cache");
        GCM_STR(None, "none");
        // Recognised only to produce a helpful "not available on Linux" error:
        GCM_STR(WindowsCredentialManager, "wincredman");
        GCM_STR(Dpapi, "dpapi");
        GCM_STR(MacOSKeychain, "keychain");
    } // namespace CredentialStoreNames

    namespace RegexPatterns {
        GCM_STR(Any, ".*");
        GCM_STR(None, "$.+");
        GCM_STR(Empty, "^$");
    } // namespace RegexPatterns

    namespace EnvironmentVariables {
        GCM_STR(GcmTrace, "GCM_TRACE");
        GCM_STR(GcmTraceSecrets, "GCM_TRACE_SECRETS");
        GCM_STR(GcmTraceMsAuth, "GCM_TRACE_MSAUTH");
        GCM_STR(GcmDebug, "GCM_DEBUG");
        GCM_STR(GcmProvider, "GCM_PROVIDER");
        GCM_STR(GcmAuthority, "GCM_AUTHORITY");
        GCM_STR(GitTerminalPrompts, "GIT_TERMINAL_PROMPT");
        GCM_STR(GcmAllowWia, "GCM_ALLOW_WINDOWSAUTH");
        GCM_STR(GitTrace2Event, "GIT_TRACE2_EVENT");
        GCM_STR(GitTrace2Normal, "GIT_TRACE2");
        GCM_STR(GitTrace2Performance, "GIT_TRACE2_PERF");
        // Lowercase variants take precedence; libcurl does not look for uppercase HTTP_PROXY.
        GCM_STR(CurlNoProxy, "no_proxy");
        GCM_STR(CurlNoProxyUpper, "NO_PROXY");
        GCM_STR(CurlHttpsProxy, "https_proxy");
        GCM_STR(CurlHttpsProxyUpper, "HTTPS_PROXY");
        GCM_STR(CurlHttpProxy, "http_proxy");
        GCM_STR(CurlAllProxy, "all_proxy");
        GCM_STR(CurlAllProxyUpper, "ALL_PROXY");
        GCM_STR(GcmHttpProxy, "GCM_HTTP_PROXY");
        GCM_STR(GitSslNoVerify, "GIT_SSL_NO_VERIFY");
        GCM_STR(GitSslCaInfo, "GIT_SSL_CAINFO");
        GCM_STR(GcmInteractive, "GCM_INTERACTIVE");
        GCM_STR(GcmParentWindow, "GCM_MODAL_PARENTHWND");
        GCM_STR(MsAuthFlow, "GCM_MSAUTH_FLOW");
        GCM_STR(MsAuthUseBroker, "GCM_MSAUTH_USEBROKER");
        GCM_STR(MsAuthUseDefaultAccount, "GCM_MSAUTH_USEDEFAULTACCOUNT");
        GCM_STR(GcmCredNamespace, "GCM_NAMESPACE");
        GCM_STR(GcmCredentialStore, "GCM_CREDENTIAL_STORE");
        GCM_STR(GcmCredCacheOptions, "GCM_CREDENTIAL_CACHE_OPTIONS");
        GCM_STR(GcmPlaintextStorePath, "GCM_PLAINTEXT_STORE_PATH");
        GCM_STR(GitExecutablePath, "GIT_EXEC_PATH");
        GCM_STR(GpgExecutablePath, "GCM_GPG_PATH");
        GCM_STR(GcmAutoDetectTimeout, "GCM_AUTODETECT_TIMEOUT");
        GCM_STR(GcmGuiPromptsEnabled, "GCM_GUI_PROMPT");
        GCM_STR(GcmUiHelper, "GCM_UI_HELPER");
        GCM_STR(OAuthAuthenticationModes, "GCM_OAUTH_AUTHMODES");
        GCM_STR(OAuthClientId, "GCM_OAUTH_CLIENTID");
        GCM_STR(OAuthClientSecret, "GCM_OAUTH_CLIENTSECRET");
        GCM_STR(OAuthRedirectUri, "GCM_OAUTH_REDIRECTURI");
        GCM_STR(OAuthScopes, "GCM_OAUTH_SCOPES");
        GCM_STR(OAuthAuthzEndpoint, "GCM_OAUTH_AUTHORIZE_ENDPOINT");
        GCM_STR(OAuthTokenEndpoint, "GCM_OAUTH_TOKEN_ENDPOINT");
        GCM_STR(OAuthDeviceEndpoint, "GCM_OAUTH_DEVICE_ENDPOINT");
        GCM_STR(OAuthClientAuthHeader, "GCM_OAUTH_USE_CLIENT_AUTH_HEADER");
        GCM_STR(OAuthDefaultUserName, "GCM_OAUTH_DEFAULT_USERNAME");
        GCM_STR(OAuthResponseMode, "GCM_OAUTH_RESPONSE_MODE");
        GCM_STR(GcmGuiSoftwareRendering, "GCM_GUI_SOFTWARE_RENDERING");
        GCM_STR(GcmAllowUnsafeRemotes, "GCM_ALLOW_UNSAFE_REMOTES");
        GCM_STR(GitHubActionsTokenRequestUrl, "ACTIONS_ID_TOKEN_REQUEST_URL");
        GCM_STR(GitHubActionsTokenRequestToken, "ACTIONS_ID_TOKEN_REQUEST_TOKEN");
    } // namespace EnvironmentVariables

    namespace Http {
        GCM_STR(WwwAuthenticateBasicScheme, "Basic");
        GCM_STR(WwwAuthenticateBearerScheme, "Bearer");
        GCM_STR(WwwAuthenticateNegotiateScheme, "Negotiate");
        GCM_STR(WwwAuthenticateNtlmScheme, "NTLM");
        GCM_STR(MimeTypeJson, "application/json");
        GCM_STR(MimeTypeFormUrlEncoded, "application/x-www-form-urlencoded");
        GCM_STR(MethodGet, "GET");
        GCM_STR(MethodPost, "POST");
    } // namespace Http

    namespace GitConfiguration {
        namespace Credential {
            GCM_STR(SectionName, "credential");
            GCM_STR(Helper, "helper");
            GCM_STR(Trace, "trace");
            GCM_STR(TraceSecrets, "traceSecrets");
            GCM_STR(TraceMsAuth, "traceMsAuth");
            GCM_STR(Debug, "debug");
            GCM_STR(Provider, "provider");
            GCM_STR(Authority, "authority");
            GCM_STR(AllowWia, "allowWindowsAuth");
            GCM_STR(HttpProxy, "httpProxy");
            GCM_STR(HttpsProxy, "httpsProxy");
            GCM_STR(UseHttpPath, "useHttpPath");
            GCM_STR(Interactive, "interactive");
            GCM_STR(MsAuthFlow, "msauthFlow");
            GCM_STR(MsAuthUseBroker, "msauthUseBroker");
            GCM_STR(CredNamespace, "namespace");
            GCM_STR(CredentialStore, "credentialStore");
            GCM_STR(CredCacheOptions, "cacheOptions");
            GCM_STR(PlaintextStorePath, "plaintextStorePath");
            GCM_STR(UserName, "username");
            GCM_STR(AutoDetectTimeout, "autoDetectTimeout");
            GCM_STR(GuiPromptsEnabled, "guiPrompt");
            GCM_STR(UiHelper, "uiHelper");
            GCM_STR(MsAuthUseDefaultAccount, "msauthUseDefaultAccount");
            GCM_STR(GuiSoftwareRendering, "guiSoftwareRendering");
            GCM_STR(GpgPassStorePath, "gpgPassStorePath");
            GCM_STR(AllowUnsafeRemotes, "allowUnsafeRemotes");
            GCM_STR(OAuthAuthenticationModes, "oauthAuthModes");
            GCM_STR(OAuthClientId, "oauthClientId");
            GCM_STR(OAuthClientSecret, "oauthClientSecret");
            GCM_STR(OAuthRedirectUri, "oauthRedirectUri");
            GCM_STR(OAuthScopes, "oauthScopes");
            GCM_STR(OAuthAuthzEndpoint, "oauthAuthorizeEndpoint");
            GCM_STR(OAuthTokenEndpoint, "oauthTokenEndpoint");
            GCM_STR(OAuthDeviceEndpoint, "oauthDeviceEndpoint");
            GCM_STR(OAuthClientAuthHeader, "oauthUseClientAuthHeader");
            GCM_STR(OAuthDefaultUserName, "oauthDefaultUserName");
            GCM_STR(OAuthResponseMode, "oauthResponseMode");
        } // namespace Credential

        namespace Http {
            GCM_STR(SectionName, "http");
            GCM_STR(Proxy, "proxy");
            GCM_STR(SchannelUseSslCaInfo, "schannelUseSSLCAInfo");
            GCM_STR(SslBackend, "sslBackend");
            GCM_STR(SslVerify, "sslVerify");
            GCM_STR(SslCaInfo, "sslCAInfo");
            GCM_STR(SslAutoClientCert, "sslAutoClientCert");
            GCM_STR(CookieFile, "cookieFile");
            GCM_STR(AllowNtlmAuth, "allowNTLMAuth");
        } // namespace Http

        namespace Remote {
            GCM_STR(SectionName, "remote");
            GCM_STR(FetchUrl, "url");
            GCM_STR(PushUrl, "pushUrl");
        } // namespace Remote

        namespace Trace2 {
            GCM_STR(SectionName, "trace2");
            GCM_STR(EventTarget, "eventtarget");
            GCM_STR(NormalTarget, "normaltarget");
            GCM_STR(PerformanceTarget, "perftarget");
        } // namespace Trace2
    } // namespace GitConfiguration

    namespace HelpUrls {
        GCM_STR(GcmProjectUrl, "https://aka.ms/gcm");
        GCM_STR(GcmNewIssue, "https://aka.ms/gcm/bug");
        GCM_STR(GcmAuthorityDeprecated, "https://aka.ms/gcm/authority");
        GCM_STR(GcmHttpProxyGuide, "https://aka.ms/gcm/httpproxy");
        GCM_STR(GcmTlsVerification, "https://aka.ms/gcm/tlsverify");
        GCM_STR(GcmCredentialStores, "https://aka.ms/gcm/credstores");
        GCM_STR(GcmAutoDetect, "https://aka.ms/gcm/autodetect");
        GCM_STR(GcmDefaultAccount, "https://aka.ms/gcm/defaultaccount");
        GCM_STR(GcmMultipleUsers, "https://aka.ms/gcm/multipleusers");
        GCM_STR(GcmUnsafeRemotes, "https://aka.ms/gcm/unsaferemotes");
        GCM_STR(GcmNtlm, "https://aka.ms/gcm/ntlm");
    } // namespace HelpUrls

    /// Version of this build (from the top-level VERSION file / CMake project version).
    QString gcmVersion();
    /// "<version>+<commit>" when built from a Git checkout (AssemblyInformationalVersion upstream): `--version`, the
    /// TRACE2 version event and the diagnose log.
    QString gcmInformationalVersion();

    /// HTTP user-agent, e.g. `Git-Credential-Manager/3.0.2 (Linux; x86_64) Qt/6.12.0`.
    QString getHttpUserAgent();

} // namespace gcm::Constants
