// EntraAuthentication: the port of EntraAuthentication{,.PublicClient,.ConfidentialClient,.Caching}.cs with the MSAL
// calls replaced by our own implementation of the Microsoft identity platform protocols (v2.0 endpoints).
#include "entra.h"

#include "core/commandcontext.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace gcm::entra {

namespace {

namespace CredCfg = Constants::GitConfiguration::Credential;
namespace Envars = Constants::EnvironmentVariables;

QString trace2Category() { return QStringLiteral("entra"); }

/// Scopes MSAL adds to every user token request.
QStringList reservedScopes() { return {QStringLiteral("openid"), QStringLiteral("profile"), QStringLiteral("offline_access")}; }

/// Tenants that are not a specific directory.
bool isTenantless(const QString &tenant)
{
    return tenant.compare(QLatin1String("common"), Qt::CaseInsensitive) == 0 ||
           tenant.compare(QLatin1String("organizations"), Qt::CaseInsensitive) == 0 ||
           tenant.compare(QLatin1String("consumers"), Qt::CaseInsensitive) == 0;
}

/// An authority URL split into the cloud instance ("https://login.microsoftonline.com") and the tenant.
struct Authority
{
    QString instance;
    QString environment;
    QString tenant;

    static Authority parse(const QString &authority)
    {
        const std::optional<QUrl> uri = tryCreateUri(authority);
        if (!uri) {
            throw ArgumentException(QStringLiteral("The authority '%1' is not a valid URL. (Parameter 'authority')").arg(authority));
        }
        Authority a;
        a.environment = uri->host().toLower();
        a.instance = uri->scheme().toLower() + QStringLiteral("://") + uri->authority().toLower();
        const QStringList segments = uri->path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        a.tenant = segments.value(0, QStringLiteral("common"));
        return a;
    }
    QUrl endpoint(const QString &tenant, const QString &name) const
    {
        return QUrl(QStringLiteral("%1/%2/oauth2/v2.0/%3").arg(instance, tenant, name));
    }
};

QString scopesKey(const QStringList &scopes)
{
    QStringList s;
    for (const QString &scope : scopes) {
        if (!reservedScopes().contains(scope, Qt::CaseInsensitive)) {
            s.append(scope.toLower());
        }
    }
    s.sort();
    s.removeDuplicates();
    return s.join(QLatin1Char(' '));
}

QStringList withReservedScopes(const QStringList &scopes)
{
    QStringList all = scopes;
    const QStringList reserved = reservedScopes();
    for (const QString &r : reserved) {
        if (!all.contains(r, Qt::CaseInsensitive)) {
            all.append(r);
        }
    }
    return all;
}

QByteArray base64UrlDecode(const QString &s)
{
    return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString base64Url(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// The payload of a JWT (no signature validation, as MSAL for ID tokens of public clients).
QJsonObject jwtPayload(const QString &jwt)
{
    const QStringList parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    return QJsonDocument::fromJson(base64UrlDecode(parts[1])).object();
}

/// "k1=v1&k2=v2" with the values escaped like Uri.EscapeDataString.
QString queryString(const QList<std::pair<QString, QString>> &items)
{
    QStringList parts;
    for (const auto &[key, value] : items) {
        parts.append(key + QLatin1Char('=') + escapeDataString(value));
    }
    return parts.join(QLatin1Char('&'));
}

/// Waits `ms` milliseconds in a nested event loop.
void delay(int ms)
{
    if (ms <= 0) {
        return;
    }
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// The response of a token request.
struct TokenResponse
{
    QString accessToken;
    OptString refreshToken;
    OptString idToken;
    OptString clientInfo;
    qint64 expiresIn = 3600;
};

/// The account described by a token response (client_info + id_token), if any.
std::optional<TokenCache::Account> accountFromResponse(const TokenResponse &r, const QString &environment)
{
    if (!r.idToken && !r.clientInfo) {
        return std::nullopt;
    }
    TokenCache::Account a;
    a.environment = environment;
    const QJsonObject idClaims = r.idToken ? jwtPayload(*r.idToken) : QJsonObject();
    if (r.clientInfo) {
        const QJsonObject ci = QJsonDocument::fromJson(base64UrlDecode(*r.clientInfo)).object();
        const QString uid = ci.value(QStringLiteral("uid")).toString();
        const QString utid = ci.value(QStringLiteral("utid")).toString();
        if (!uid.isEmpty() && !utid.isEmpty()) {
            a.homeAccountId = uid + QLatin1Char('.') + utid;
            a.tenantId = utid;
        }
    }
    if (a.homeAccountId.isEmpty()) {
        // Without client_info: <oid>.<tid> from the ID token
        const QString oid = idClaims.value(QStringLiteral("oid")).toString();
        const QString tid = idClaims.value(QStringLiteral("tid")).toString();
        if (oid.isEmpty() || tid.isEmpty()) {
            return std::nullopt;
        }
        a.homeAccountId = oid + QLatin1Char('.') + tid;
        a.tenantId = tid;
    }
    for (const char *claim : {"preferred_username", "upn", "email", "unique_name"}) {
        const QString v = idClaims.value(QLatin1String(claim)).toString();
        if (!v.isEmpty()) {
            a.userName = v;
            break;
        }
    }
    return a;
}

} // namespace

// =============================================================================================
// Private
// =============================================================================================

struct EntraAuthentication::Private
{
    Private(EntraAuthentication &q, ICommandContext &context) : q(q), context(context) {}

    EntraAuthentication &q;
    ICommandContext &context;
    TokenCacheStorageFactory storageFactory;
    std::shared_ptr<IOAuth2CodeGenerator> codeGenerator = std::make_shared<OAuth2CryptographicCodeGenerator>();
    std::shared_ptr<IOAuth2WebBrowser> browser;
    std::function<qint64()> clock = [] { return QDateTime::currentSecsSinceEpoch(); };
    std::optional<HttpClient> httpClient;
    bool publicAppCreated = false;
    /// The storage of each cache once registered (primary or fallback); null when the cache could not be set up.
    std::map<TokenCacheKind, std::unique_ptr<ITokenCacheStorage>> storages;
    std::map<TokenCacheKind, bool> registered;

    ITrace &trace() { return context.trace(); }

    HttpClient &http()
    {
        if (!httpClient) {
            httpClient = context.httpClientFactory().createClient();
        }
        return *httpClient;
    }

    /// WithTraceLogging: MSAL's verbose log, written to the GCM trace when GCM_TRACE_MSAUTH is enabled.
    void msalTrace(const QString &message)
    {
        if (context.settings().isMsalTracingEnabled()) {
            trace().writeLine(QStringLiteral("[Verbose] %1").arg(message));
        }
    }

    // --- public client application (GetPublicAppBuilder) ----------------------------------------

    const PublicClientConfig &publicApp()
    {
        if (!q.m_publicClientConfig) {
            throw InvalidOperationException(QStringLiteral("Public client configuration is required for user authentication."));
        }
        const PublicClientConfig &config = *q.m_publicClientConfig;
        if (!publicAppCreated) {
            publicAppCreated = true;
            trace().writeLine(QStringLiteral("Creating public client application builder..."));
            Trace2::writeData(trace2Category(), QStringLiteral("app/client_id"), config.clientId);

            // The broker needs MSAL's native runtime, which this port does not have: report it as MSAL does when
            // the broker is enabled and supported by the app but not installed.
            QString brokerStatus = QStringLiteral("disabled");
            if (context.sessionManager().isDesktopSession() && q.isBrokerEnabled()) {
                if (config.supportsLinuxBroker) {
                    trace().writeLine(QStringLiteral("Broker is supported by the app and enabled by the user."));
                    trace().writeLine(QStringLiteral("Broker authentication is not available."));
                    brokerStatus = QStringLiteral("unavailable");
                } else {
                    brokerStatus = QStringLiteral("unsupported");
                    trace().writeLine(QStringLiteral("Broker is not supported by the app on this platform."));
                }
            }
            Trace2::writeData(trace2Category(), QStringLiteral("broker/status"), brokerStatus);
        }
        return config;
    }

    // --- token cache (RegisterCacheAsync) -------------------------------------------------------

    /// The storage of the cache, set up on first use: the Secret Service, or the plaintext file when that fails.
    ITokenCacheStorage *cacheStorage(TokenCacheKind kind)
    {
        if (registered[kind]) {
            return storages[kind].get();
        }
        registered[kind] = true;
        Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("cache_init"));
        trace().writeLine(QStringLiteral("Configuring MSAL token cache..."));
        if (kind == TokenCacheKind::User) {
            const bool shared = q.m_publicClientConfig && q.m_publicClientConfig->useSharedCache;
            Trace2::writeData(trace2Category(), QStringLiteral("cache/type"), shared ? QStringLiteral("msdevtools") : QStringLiteral("gcm"));
            if (shared) {
                trace().writeLine(QStringLiteral("The shared Microsoft Developer MSAL cache is not supported; using the Git Credential Manager cache."));
            }
        }

        QString cacheResult = QStringLiteral("ok");
        std::unique_ptr<ITokenCacheStorage> storage;
        try {
            storage = storageFactory(kind, false);
            // Test that cache access is working correctly
            storage->load();
        } catch (const std::exception &ex) {
            storage.reset();
            const QString message = QStringLiteral("Cannot persist Entra authentication data securely!");
            context.console().writeWarning(QStringLiteral("cannot persist Entra authentication token cache securely!"));
            trace().writeLine(message);
            trace().writeException(ex);
            Trace2::writeError(message);
            // On Linux the SecretService/keyring might not be available so we must fall-back to a plaintext file.
            context.console().writeWarning(QStringLiteral("using plain-text fallback token cache"));
            trace().writeLine(QStringLiteral("Using fall-back plaintext token cache on Linux."));
            cacheResult = QStringLiteral("linux_fallback");
            try {
                storage = storageFactory(kind, true);
            } catch (const std::exception &fallbackEx) {
                trace().writeException(fallbackEx);
                storage.reset();
            }
        }

        if (!storage) {
            Trace2::writeData(trace2Category(), QStringLiteral("result"), QStringLiteral("failed"));
            context.console().writeError(QStringLiteral("failed to set up token cache!"));
            trace().writeLine(QStringLiteral("Failed to integrate with token cache!"));
        } else {
            Trace2::writeData(trace2Category(), QStringLiteral("result"), cacheResult);
            trace().writeLine(QStringLiteral("Token cache configured."));
        }
        storages[kind] = std::move(storage);
        return storages[kind].get();
    }

    TokenCache loadCache(TokenCacheKind kind)
    {
        ITokenCacheStorage *storage = cacheStorage(kind);
        if (!storage) {
            return {};
        }
        try {
            return TokenCache::fromJson(storage->load());
        } catch (const std::exception &ex) {
            trace().writeLine(QStringLiteral("Failed to read the token cache."));
            trace().writeException(ex);
            return {};
        }
    }

    /// Load, modify and save the cache under the cache lock.
    void updateCache(TokenCacheKind kind, const std::function<void(TokenCache &)> &update)
    {
        ITokenCacheStorage *storage = cacheStorage(kind);
        if (!storage) {
            return;
        }
        storage->lock();
        try {
            TokenCache cache = TokenCache::fromJson(storage->load());
            update(cache);
            storage->save(cache.toJson());
        } catch (const std::exception &ex) {
            trace().writeLine(QStringLiteral("Failed to update the token cache."));
            trace().writeException(ex);
        }
        storage->unlock();
    }

    /// Stores the tokens of a response (and the account of a user response).
    void cacheTokens(TokenCacheKind kind, const QString &clientId, const Authority &authority, const QString &tenant,
                     const QStringList &scopes, const TokenResponse &response, const std::optional<TokenCache::Account> &account)
    {
        const qint64 expiresOn = clock() + response.expiresIn;
        // The tenant that issued the access token, so silent requests for the same tenant find it
        QString tokenTenant = tenant;
        if (response.idToken) {
            const QString tid = jwtPayload(*response.idToken).value(QStringLiteral("tid")).toString();
            if (!tid.isEmpty()) {
                tokenTenant = tid;
            }
        }
        if (isTenantless(tokenTenant) && account) {
            tokenTenant = account->tenantId;
        }
        updateCache(kind, [&](TokenCache &cache) {
            const QString homeAccountId = account ? account->homeAccountId : QString();
            if (account) {
                cache.accounts.removeIf([&](const TokenCache::Account &a) { return a.homeAccountId == account->homeAccountId && a.environment == account->environment; });
                cache.accounts.append(*account);
                if (response.refreshToken) {
                    cache.refreshTokens.removeIf([&](const TokenCache::RefreshToken &r) {
                        return r.homeAccountId == homeAccountId && r.environment == authority.environment && r.clientId == clientId;
                    });
                    cache.refreshTokens.append({homeAccountId, authority.environment, clientId, *response.refreshToken});
                }
            }
            const QString key = scopesKey(scopes);
            cache.accessTokens.removeIf([&](const TokenCache::AccessToken &t) {
                return t.homeAccountId == homeAccountId && t.environment == authority.environment && t.clientId == clientId &&
                       t.tenantId.compare(tokenTenant, Qt::CaseInsensitive) == 0 && t.scopes == key;
            });
            // Drop expired access tokens on the way
            const qint64 now = clock();
            cache.accessTokens.removeIf([&](const TokenCache::AccessToken &t) { return t.expiresOn <= now; });
            cache.accessTokens.append({homeAccountId, authority.environment, clientId, tokenTenant, key, response.accessToken, expiresOn});
        });
    }

    /// A cached access token valid for at least five more minutes.
    OptString cachedAccessToken(TokenCacheKind kind, const QString &homeAccountId, const QString &environment, const QString &clientId,
                                const QString &tenant, const QStringList &scopes)
    {
        const TokenCache cache = loadCache(kind);
        const QString key = scopesKey(scopes);
        const qint64 now = clock();
        for (const TokenCache::AccessToken &t : cache.accessTokens) {
            if (t.homeAccountId == homeAccountId && t.environment == environment && t.clientId == clientId &&
                t.tenantId.compare(tenant, Qt::CaseInsensitive) == 0 && t.scopes == key && t.expiresOn > now + 300) {
                return t.secret;
            }
        }
        return std::nullopt;
    }

    // --- HTTP ------------------------------------------------------------------------------------

    /// The error of an identity service response as an exception (MsalServiceException and friends).
    [[noreturn]] void throwServiceError(const HttpResponse &response)
    {
        const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
        const QString error = obj.value(QStringLiteral("error")).toString(QStringLiteral("unknown_error"));
        QString description = obj.value(QStringLiteral("error_description")).toString();
        if (description.isEmpty()) {
            description = response.contentString().isEmpty() ? QStringLiteral("HTTP %1").arg(response.statusCode) : response.contentString();
        }
        const OptString claims = obj.contains(QStringLiteral("claims")) ? OptString(obj.value(QStringLiteral("claims")).toString()) : std::nullopt;
        const QString suberror = obj.value(QStringLiteral("suberror")).toString();
        if (suberror == QLatin1String("protection_policy_required")) {
            throw IntuneAppProtectionPolicyRequiredException(error, description, response.statusCode, claims);
        }
        throw EntraServiceException(error, description, response.statusCode, claims);
    }

    TokenResponse postToken(const QUrl &endpoint, const StringDictionary &form)
    {
        msalTrace(QStringLiteral("POST %1").arg(absoluteUri(endpoint)));
        const HttpResponse response = http().postForm(endpoint, form);
        msalTrace(QStringLiteral("Response %1").arg(response.statusCode));
        const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
        if (!response.isSuccessStatusCode() || !obj.value(QStringLiteral("access_token")).isString()) {
            throwServiceError(response);
        }
        TokenResponse r;
        r.accessToken = obj.value(QStringLiteral("access_token")).toString();
        if (obj.value(QStringLiteral("refresh_token")).isString()) {
            r.refreshToken = obj.value(QStringLiteral("refresh_token")).toString();
        }
        if (obj.value(QStringLiteral("id_token")).isString()) {
            r.idToken = obj.value(QStringLiteral("id_token")).toString();
        }
        if (obj.value(QStringLiteral("client_info")).isString()) {
            r.clientInfo = obj.value(QStringLiteral("client_info")).toString();
        }
        const QJsonValue expiresIn = obj.value(QStringLiteral("expires_in"));
        if (expiresIn.isDouble()) {
            r.expiresIn = qint64(expiresIn.toDouble());
        } else if (expiresIn.isString()) {
            r.expiresIn = expiresIn.toString().toLongLong();
        }
        return r;
    }

    // --- public client flows ---------------------------------------------------------------------

    StringDictionary userTokenForm(const PublicClientConfig &config, const QStringList &scopes)
    {
        return StringDictionary {
            { QStringLiteral("client_id"), config.clientId },
            { QStringLiteral("scope"), withReservedScopes(scopes).join(QLatin1Char(' ')) },
            { QStringLiteral("client_info"), QStringLiteral("1") },
        };
    }

    std::optional<EntraAuthResult> silent(const PublicClientConfig &config, const Authority &authority, const QStringList &scopes,
                                          const TokenCache::Account &account)
    {
        Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("token_silent"));
        Trace2::writeData(trace2Category(), QStringLiteral("account/kind"), QStringLiteral("cached"));
        trace().writeLine(QStringLiteral("Attempting silent authentication using account '%1'").arg(account.homeAccountId));

        // A tenant-less authority uses the account's home tenant. WithMsaPassthroughTransfer: Microsoft accounts
        // (home tenant 9188040d-...) use the MSA transfer tenant.
        QString tenant = isTenantless(authority.tenant) ? account.tenantId : authority.tenant;
        if (config.isMsaPassthroughEnabled && account.tenantId.compare(Constants::MsaHomeTenantId, Qt::CaseInsensitive) == 0) {
            tenant = Constants::MsaTransferTenantId;
        }

        const EntraAccount entraAccount{account.homeAccountId, account.userName};
        if (const OptString at = cachedAccessToken(TokenCacheKind::User, account.homeAccountId, authority.environment, config.clientId, tenant, scopes)) {
            msalTrace(QStringLiteral("Found a valid access token in the cache."));
            return EntraAuthResult{*at, entraAccount};
        }

        OptString refreshToken;
        const TokenCache cache = loadCache(TokenCacheKind::User);
        for (const TokenCache::RefreshToken &r : cache.refreshTokens) {
            if (r.homeAccountId == account.homeAccountId && r.environment == authority.environment && r.clientId == config.clientId) {
                refreshToken = r.secret;
            }
        }
        if (!refreshToken) {
            Trace2::writeData(trace2Category(), QStringLiteral("result"), QStringLiteral("ui_required"));
            trace().writeLine(QStringLiteral("Silent authentication failed; interaction required!"));
            return std::nullopt;
        }

        StringDictionary form = userTokenForm(config, scopes);
        form.set(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        form.set(QStringLiteral("refresh_token"), *refreshToken);
        TokenResponse response;
        try {
            response = postToken(authority.endpoint(tenant, QStringLiteral("token")), form);
        } catch (const EntraServiceException &ex) {
            if (ex.errorCode() != EntraError::InvalidGrant && ex.errorCode() != EntraError::InteractionRequired) {
                throw;
            }
            // MsalUiRequiredException
            Trace2::writeData(trace2Category(), QStringLiteral("result"), QStringLiteral("ui_required"));
            trace().writeLine(QStringLiteral("Silent authentication failed; interaction required!"));
            return std::nullopt;
        }
        std::optional<TokenCache::Account> refreshed = accountFromResponse(response, authority.environment);
        if (!refreshed) {
            refreshed = account;
        }
        if (refreshed->userName.isEmpty()) {
            refreshed->userName = account.userName;
        }
        cacheTokens(TokenCacheKind::User, config.clientId, authority, tenant, scopes, response, refreshed);
        return EntraAuthResult{response.accessToken, EntraAccount{refreshed->homeAccountId, refreshed->userName}};
    }

    EntraAuthResult finishUserFlow(const PublicClientConfig &config, const Authority &authority, const QStringList &scopes, const TokenResponse &response)
    {
        const std::optional<TokenCache::Account> account = accountFromResponse(response, authority.environment);
        cacheTokens(TokenCacheKind::User, config.clientId, authority, authority.tenant, scopes, response, account);
        EntraAuthResult result{response.accessToken, std::nullopt};
        if (account) {
            result.account = EntraAccount{account->homeAccountId, account->userName};
        }
        return result;
    }

    EntraAuthResult systemWebView(const PublicClientConfig &config, const Authority &authority, const QStringList &scopes)
    {
        context.console().writeInfo(QStringLiteral("opening browser to complete authentication..."));

        std::shared_ptr<IOAuth2WebBrowser> webBrowser = browser;
        if (!webBrowser) {
            webBrowser = std::make_shared<OAuth2SystemWebBrowser>(context.sessionManager(), OAuth2WebBrowserOptions());
        }
        // MSAL's default redirect URI for public clients: http://localhost with a free port
        const QUrl redirectUri = webBrowser->updateRedirectUri(QUrl(QStringLiteral("http://localhost/")));
        const QString state = codeGenerator->createNonce();
        const QString verifier = codeGenerator->createPkceCodeVerifier();
        const QString challenge = codeGenerator->createPkceCodeChallenge(OAuth2PkceChallengeMethod::Sha256, verifier);

        const QString query = queryString({
            { QStringLiteral("client_id"), config.clientId },
            { QStringLiteral("response_type"), QStringLiteral("code") },
            { QStringLiteral("redirect_uri"), redirectUri.toString(QUrl::FullyEncoded) },
            { QStringLiteral("scope"), withReservedScopes(scopes).join(QLatin1Char(' ')) },
            { QStringLiteral("state"), state },
            { QStringLiteral("code_challenge"), challenge },
            { QStringLiteral("code_challenge_method"), QStringLiteral("S256") },
            { QStringLiteral("prompt"), QStringLiteral("select_account") },
            { QStringLiteral("client_info"), QStringLiteral("1") },
        });
        QUrl authorizeUri = authority.endpoint(authority.tenant, QStringLiteral("authorize"));
        authorizeUri.setQuery(query, QUrl::StrictMode);

        const StringDictionary response = webBrowser->getAuthenticationResponse(authorizeUri, redirectUri, OAuth2ResponseMode::Default, CancellationToken());
        if (const OptString error = response.tryGet(QStringLiteral("error"))) {
            const QString description = valueOr(response.tryGet(QStringLiteral("error_description")), *error);
            if (*error == EntraError::AccessDenied) {
                throw EntraClientException(EntraError::AuthenticationCanceled, QStringLiteral("User canceled authentication. %1").arg(description));
            }
            throw EntraServiceException(*error, description);
        }
        if (response.tryGet(QStringLiteral("state")) != OptString(state)) {
            throw EntraClientException(QStringLiteral("state_mismatch"), QStringLiteral("The state returned by the server does not match the state sent."));
        }
        const OptString code = response.tryGet(QStringLiteral("code"));
        if (!code) {
            throw EntraClientException(QStringLiteral("authentication_ui_failed"), QStringLiteral("The authorization response contains no code."));
        }

        StringDictionary form = userTokenForm(config, scopes);
        form.set(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
        form.set(QStringLiteral("code"), *code);
        form.set(QStringLiteral("redirect_uri"), redirectUri.toString(QUrl::FullyEncoded));
        form.set(QStringLiteral("code_verifier"), verifier);
        return finishUserFlow(config, authority, scopes, postToken(authority.endpoint(authority.tenant, QStringLiteral("token")), form));
    }

    EntraAuthResult deviceCode(const PublicClientConfig &config, const Authority &authority, const QStringList &scopes)
    {
        const StringDictionary request{
            {QStringLiteral("client_id"), config.clientId},
            {QStringLiteral("scope"), withReservedScopes(scopes).join(QLatin1Char(' '))},
        };
        const HttpResponse dcResponse = http().postForm(authority.endpoint(authority.tenant, QStringLiteral("devicecode")), request);
        const QJsonObject dc = QJsonDocument::fromJson(dcResponse.content).object();
        if (!dcResponse.isSuccessStatusCode() || !dc.value(QStringLiteral("device_code")).isString()) {
            throwServiceError(dcResponse);
        }
        const QString code = dc.value(QStringLiteral("device_code")).toString();
        QString message = dc.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = QStringLiteral("To sign in, use a web browser to open the page %1 and enter the code %2 to authenticate.").arg(dc.value(QStringLiteral("verification_uri")).toString(), dc.value(QStringLiteral("user_code")).toString());
        }
        // ShowDeviceCodeAsync
        context.console().writeLine(message);

        int intervalMs = int(dc.value(QStringLiteral("interval")).toDouble(5)) * 1000;
        const qint64 expiresAt = clock() + qint64(dc.value(QStringLiteral("expires_in")).toDouble(900));
        StringDictionary form {
            { QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code") },
            { QStringLiteral("device_code"), code },
            { QStringLiteral("client_id"), config.clientId },
            { QStringLiteral("client_info"), QStringLiteral("1") },
        };
        while (true) {
            delay(intervalMs);
            try {
                return finishUserFlow(config, authority, scopes, postToken(authority.endpoint(authority.tenant, QStringLiteral("token")), form));
            } catch (const EntraServiceException &ex) {
                if (ex.errorCode() == QLatin1String("authorization_pending") && clock() < expiresAt) {
                    continue;
                }
                if (ex.errorCode() == QLatin1String("slow_down") && clock() < expiresAt) {
                    intervalMs += 5000;
                    continue;
                }
                if (ex.errorCode() == QLatin1String("expired_token") || ex.errorCode() == QLatin1String("authorization_pending")) {
                    throw EntraClientException(EntraError::CodeExpired, QStringLiteral("Verification code expired before contacting the server"));
                }
                throw;
            }
        }
    }

    bool tryGetModePreference(InteractionMode &mode)
    {
        QString valueStr;
        if (context.settings().tryGetSetting(Envars::MsAuthFlow, CredCfg::SectionName, CredCfg::MsAuthFlow, valueStr)) {
            trace().writeLine(QStringLiteral("Interaction mode overriden to '%1'.").arg(valueStr));
            const QString v = valueStr.toLower();
            if (v == QLatin1String("auto")) {
                mode = InteractionMode::Auto;
            } else if (v == QLatin1String("embedded") || v == QLatin1String("embeddedwebview")) {
                mode = InteractionMode::EmbeddedWebView;
            } else if (v == QLatin1String("system") || v == QLatin1String("systemwebview")) {
                mode = InteractionMode::SystemWebView;
            } else if (v == QLatin1String("device") || v == QLatin1String("devicecode")) {
                mode = InteractionMode::DeviceCode;
            } else {
                bool isNumber = false;
                const int n = v.toInt(&isNumber);
                if (isNumber && n >= 0 && n <= 3) {
                    mode = InteractionMode(n);
                } else {
                    context.console().writeWarning(QStringLiteral("unknown interaction mode '%1'; using 'auto'").arg(valueStr));
                    mode = InteractionMode::Auto;
                }
            }
            return true;
        }
        mode = InteractionMode::Auto;
        return false;
    }

    // Embedded web views are not available (as upstream today)
    bool isEmbeddedWebViewAvailable() { return false; }
    bool isSystemWebViewAvailable() { return context.sessionManager().isWebBrowserAvailable(); }
    bool isDeviceCodeAvailable() { return context.settings().isTerminalPromptsEnabled(); }

    EntraAuthResult interactive(const PublicClientConfig &config, const Authority &authority, const QStringList &scopes, InteractionMode interactionMode)
    {
        Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("token_interactive"));
        // Check for a stored preference if we've not been given a specific mode from the caller
        InteractionMode mode;
        if (interactionMode == InteractionMode::Auto && tryGetModePreference(mode)) {
            trace().writeLine(QStringLiteral("Interaction mode overriden to '%1'.").arg(modeNameForTrace(mode)));
            interactionMode = mode;
        }
        Trace2::writeData(trace2Category(), QStringLiteral("mode/requested"), modeName(interactionMode));

        if (interactionMode == InteractionMode::Auto) {
            trace().writeLine(QStringLiteral("Resolving interactive mode auto..."));
            if (isEmbeddedWebViewAvailable()) {
                interactionMode = InteractionMode::EmbeddedWebView;
            } else if (isSystemWebViewAvailable()) {
                interactionMode = InteractionMode::SystemWebView;
            } else if (isDeviceCodeAvailable()) {
                interactionMode = InteractionMode::DeviceCode;
            } else {
                throw InvalidOperationException(QStringLiteral("No available interaction modes."));
            }
        }

        Trace2::writeData(trace2Category(), QStringLiteral("mode/resolved"), modeName(interactionMode));
        switch (interactionMode) {
        case InteractionMode::EmbeddedWebView:
            trace().writeLine(QStringLiteral("Performing interactive authentication via embedded webview..."));
            throw EntraClientException(QStringLiteral("webview_unavailable"), QStringLiteral("An embedded web view is not available on this platform; use the system web view or device code flow instead."));
        case InteractionMode::SystemWebView:
            trace().writeLine(QStringLiteral("Performing interactive authentication via system webview..."));
            return systemWebView(config, authority, scopes);
        case InteractionMode::DeviceCode:
            trace().writeLine(QStringLiteral("Performing interactive authentication via device code..."));
            return deviceCode(config, authority, scopes);
        default:
            throw ArgumentOutOfRangeException(QStringLiteral("Unexpected interaction mode. (Parameter 'interactionMode')"));
        }
    }

    /// Enum.ToString of InteractionMode for trace lines ("SystemWebView").
    static QString modeNameForTrace(InteractionMode mode)
    {
        switch (mode) {
            case InteractionMode::Auto: return QStringLiteral("Auto");
            case InteractionMode::EmbeddedWebView: return QStringLiteral("EmbeddedWebView");
            case InteractionMode::SystemWebView: return QStringLiteral("SystemWebView");
            case InteractionMode::DeviceCode: return QStringLiteral("DeviceCode");
        }
        return QString();
    }

    /// The accounts of the user cache that hold a refresh token for the client.
    QList<TokenCache::Account> userAccounts(const QString &clientId)
    {
        const TokenCache cache = loadCache(TokenCacheKind::User);
        QList<TokenCache::Account> result;
        for (const TokenCache::Account &a : cache.accounts) {
            const bool hasToken = std::any_of(cache.refreshTokens.begin(), cache.refreshTokens.end(), [&](const TokenCache::RefreshToken &r) {
                return r.homeAccountId == a.homeAccountId && r.environment == a.environment && r.clientId == clientId;
            });
            if (hasToken) {
                result.append(a);
            }
        }
        return result;
    }

    /// ResolveAccountAsync: by home account ID first, then by UPN.
    std::optional<TokenCache::Account> resolveAccount(const QString &clientId, const EntraAccount &account)
    {
        Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("resolve_account"));
        trace().writeLine(QStringLiteral("Getting all cached accounts..."));
        const QList<TokenCache::Account> accounts = userAccounts(clientId);
        Trace2::writeData(trace2Category(), QStringLiteral("accounts/cached_count"), qint64(accounts.size()));
        if (accounts.isEmpty()) {
            Trace2::writeData(trace2Category(), QStringLiteral("match/kind"), QStringLiteral("none"));
            trace().writeLine(QStringLiteral("No cached accounts available."));
            return std::nullopt;
        }
        trace().writeLine(QStringLiteral("Found %1 cached accounts.").arg(accounts.size()));
        trace().writeLine(QStringLiteral("Checking cached accounts for ID '%1'...").arg(valueOr(account.homeAccountId)));
        if (!isNullOrWhiteSpace(account.homeAccountId)) {
            std::optional<TokenCache::Account> byId;
            for (const TokenCache::Account &a : accounts) {
                if (a.homeAccountId.compare(*account.homeAccountId, Qt::CaseInsensitive) == 0) {
                    byId = a;
                    break;
                }
            }
            if (byId && !isNullOrWhiteSpace(account.userName) && byId->userName.compare(*account.userName, Qt::CaseInsensitive) != 0) {
                trace().writeLine(QStringLiteral("Cached account UPN '%1' differs from supplied UPN '%2' for HomeAccountId '%3'; using HomeAccountId.").arg(byId->userName, *account.userName, *account.homeAccountId));
            }
            Trace2::writeData(trace2Category(), QStringLiteral("match/kind"), byId ? QStringLiteral("id") : QStringLiteral("none"));
            trace().writeLine(QStringLiteral("Matched account by ID '%1' (%2).)").arg(byId ? byId->homeAccountId : QString(), byId ? byId->userName : QString()));
            return byId;
        }
        trace().writeLine(QStringLiteral("Checking cached accounts for UPN '%1'...").arg(valueOr(account.userName)));
        if (!isNullOrWhiteSpace(account.userName)) {
            QList<TokenCache::Account> matched;
            for (const TokenCache::Account &a : accounts) {
                if (a.userName.compare(*account.userName, Qt::CaseInsensitive) == 0) {
                    matched.append(a);
                }
            }
            if (matched.size() > 1) {
                trace().writeLine(QStringLiteral("%1 cached accounts share UPN '%2'; using the first (provide a HomeAccountId to disambiguate).").arg(QString::number(matched.size()), *account.userName));
            }
            if (!matched.isEmpty()) {
                Trace2::writeData(trace2Category(), QStringLiteral("match/kind"), QStringLiteral("upn"));
                trace().writeLine(QStringLiteral("Matched account by UPN '%1' (%2).)").arg(matched.constFirst().homeAccountId, matched.constFirst().userName));
                return matched.constFirst();
            }
        }
        Trace2::writeData(trace2Category(), QStringLiteral("match/kind"), QStringLiteral("none"));
        trace().writeLine(QStringLiteral("No cached account found."));
        return std::nullopt;
    }

    // --- confidential clients -------------------------------------------------------------------

    /// AcquireTokenForClient: the application cache first, then the client credentials grant.
    EntraAuthResult clientCredentials(const QString &clientId, const QString &tenantId, const QStringList &scopes,
                                      const std::function<void(StringDictionary &, const QUrl &)> &addCredential)
    {
        const Authority authority = Authority::parse(QStringLiteral("https://login.microsoftonline.com/") + tenantId);
        if (const OptString at = cachedAccessToken(TokenCacheKind::App, QString(), authority.environment, clientId, tenantId, scopes)) {
            msalTrace(QStringLiteral("Found a valid application token in the cache."));
            return EntraAuthResult{*at, std::nullopt};
        }
        const QUrl tokenEndpoint = authority.endpoint(tenantId, QStringLiteral("token"));
        StringDictionary form {
            { QStringLiteral("grant_type"), QStringLiteral("client_credentials") },
            { QStringLiteral("client_id"), clientId },
            { QStringLiteral("scope"), scopes.join(QLatin1Char(' ')) },
        };
        addCredential(form, tokenEndpoint);
        const TokenResponse response = postToken(tokenEndpoint, form);
        cacheTokens(TokenCacheKind::App, clientId, authority, tenantId, scopes, response, std::nullopt);
        return EntraAuthResult{response.accessToken, std::nullopt};
    }

    /// A client assertion (RFC 7523) signed with the certificate's private key (RS256), as MSAL's WithCertificate.
    QString certificateAssertion(const QString &clientId, const QUrl &audience, const ServicePrincipalIdentity::Certificate &cert, bool sendX5C)
    {
        QJsonObject header{
            { QStringLiteral("alg"), QStringLiteral("RS256") },
            { QStringLiteral("typ"), QStringLiteral("JWT") },
            { QStringLiteral("x5t"), base64Url(QCryptographicHash::hash(cert.der, QCryptographicHash::Sha1)) },
        };
        if (sendX5C) {
            header.insert(QStringLiteral("x5c"), QJsonArray{QString::fromLatin1(cert.der.toBase64())});
        }
        const qint64 now = clock();
        const QJsonObject payload{
            { QStringLiteral("aud"), absoluteUri(audience) },
            { QStringLiteral("iss"), clientId },
            { QStringLiteral("sub"), clientId },
            { QStringLiteral("jti"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
            { QStringLiteral("nbf"), double(now) },
            { QStringLiteral("iat"), double(now) },
            { QStringLiteral("exp"), double(now + 600) },
        };
        const QString signingInput = base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + QLatin1Char('.') +
                                     base64Url(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        return signingInput + QLatin1Char('.') + base64Url(signRs256(cert.privateKeyPem, signingInput.toLatin1()));
    }

    static QByteArray signRs256(const QByteArray &privateKeyPem, const QByteArray &data)
    {
        BIO *bio = BIO_new_mem_buf(privateKeyPem.constData(), int(privateKeyPem.size()));
        EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!key) {
            ERR_clear_error();
            throw EntraClientException(QStringLiteral("invalid_client_certificate"), QStringLiteral("The certificate has no usable private key."));
        }
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        QByteArray signature;
        size_t length = 0;
        if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
            EVP_DigestSign(ctx, nullptr, &length, reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size())) == 1) {
            signature.resize(qsizetype(length));
            if (EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(signature.data()), &length,
                               reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size())) == 1) {
                signature.resize(qsizetype(length));
            } else {
                signature.clear();
            }
        }
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        ERR_clear_error();
        if (signature.isEmpty()) {
            throw EntraClientException(QStringLiteral("invalid_client_certificate"), QStringLiteral("Failed to sign the client assertion with the certificate."));
        }
        return signature;
    }

    // --- managed identity -------------------------------------------------------------------------

    EntraAuthResult managedIdentity(const QString &resource, const ManagedIdentity &mi);

    // --- workload federation ---------------------------------------------------------------------

    QString clientAssertion(const WorkloadFederationOptions &fedOpts)
    {
        switch (fedOpts.scenario) {
        case WorkloadFederationScenario::Generic:
            trace().writeLine(QStringLiteral("Getting client assertion for generic workload federation scenario..."));
            if (isNullOrWhiteSpace(fedOpts.genericClientAssertion)) {
                throw InvalidOperationException(QStringLiteral("Client assertion must be provided for generic workload federation scenario."));
            }
            return fedOpts.genericClientAssertion;
        case WorkloadFederationScenario::ManagedIdentity: {
            trace().writeLine(QStringLiteral("Getting client assertion for managed identity workload federation scenario..."));
            const ManagedIdentity mi = ManagedIdentity::create(fedOpts.managedIdentityId);
            return q.getTokenForManagedIdentity(fedOpts.audience(), mi).accessToken;
        }
        case WorkloadFederationScenario::GitHubActions:
            trace().writeLine(QStringLiteral("Getting client assertion for GitHub Actions workload federation scenario..."));
            return gitHubOidcToken(fedOpts.gitHubTokenRequestUrl, fedOpts.audience(), fedOpts.gitHubTokenRequestToken);
        }
        throw ArgumentOutOfRangeException(QStringLiteral("Unsupported workload federation scenario. (Parameter 'Scenario')"));
    }

    QString gitHubOidcToken(const QUrl &requestUri, const QString &audience, const QString &requestToken)
    {
        Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("github_oidc"));
        QUrl uri = requestUri;
        QString query = uri.query(QUrl::FullyEncoded);
        if (!query.isEmpty()) {
            query += QLatin1Char('&');
        }
        query += QStringLiteral("audience=") + escapeDataString(audience);
        uri.setQuery(query, QUrl::StrictMode);

        HttpRequest request;
        request.url = uri;
        request.setBearerAuthorization(requestToken);
        trace().writeLine(QStringLiteral("Requesting GitHub OIDC token from '%1'...").arg(absoluteUri(uri)));
        trace().writeLineSecrets(QStringLiteral("OIDC request token: {0}"), {requestToken});
        const HttpResponse response = context.httpClientFactory().createClient().send(request);
        if (!response.isSuccessStatusCode()) {
            trace().writeLine(QStringLiteral("Failed to acquire GitHub OIDC token [%1 %2]: %3").arg(QString::number(response.statusCode), httpStatusCodeName(response.statusCode), response.contentString()));
            response.ensureSuccessStatusCode();
        }
        const QString json = response.contentString();
        try {
            QJsonParseError error{};
            const QJsonDocument doc = QJsonDocument::fromJson(response.content, &error);
            if (error.error != QJsonParseError::NoError) {
                throw Exception(QStringLiteral("'%1' is an invalid JSON response.").arg(json.left(20)));
            }
            const QJsonObject obj = doc.object();
            if (!obj.contains(QStringLiteral("value"))) {
                throw InvalidOperationException(QStringLiteral("Invalid response from GitHub OIDC token endpoint: 'value' property not found."));
            }
            if (!obj.value(QStringLiteral("value")).isString()) {
                throw InvalidOperationException(QStringLiteral("Invalid response from GitHub OIDC token endpoint: 'value' property is null."));
            }
            return obj.value(QStringLiteral("value")).toString();
        } catch (const std::exception &ex) {
            trace().writeException(ex);
            trace().writeLine(QStringLiteral("OIDC token response: %1").arg(json));
            throw;
        }
    }
};

// =============================================================================================
// Managed identity (ManagedIdentityApplication)
// =============================================================================================

EntraAuthResult EntraAuthentication::Private::managedIdentity(const QString &resource, const ManagedIdentity &mi)
{
    const StringDictionary &env = context.environment().variables();
    const OptString identityEndpoint = env.tryGet(QStringLiteral("IDENTITY_ENDPOINT"));
    const OptString identityHeader = env.tryGet(QStringLiteral("IDENTITY_HEADER"));
    const OptString imdsEndpoint = env.tryGet(QStringLiteral("IMDS_ENDPOINT"));
    const OptString msiEndpoint = env.tryGet(QStringLiteral("MSI_ENDPOINT"));

    HttpRequest request;
    QList<std::pair<QString, QString>> query;
    QString source;
    auto addUserAssigned = [&](const QString &clientIdParam, const QString &resourceIdParam) {
        if (mi.kind() == ManagedIdentity::Kind::ClientId) {
            query.append({clientIdParam, mi.value()});
        } else if (mi.kind() == ManagedIdentity::Kind::ResourceId) {
            query.append({resourceIdParam, mi.value()});
        }
    };

    if (identityEndpoint && identityHeader && env.contains(QStringLiteral("IDENTITY_SERVER_THUMBPRINT"))) {
        throw EntraClientException(EntraError::ManagedIdentityRequestFailed, QStringLiteral("[Managed Identity] Service Fabric managed identities are not supported."));
    }
    if (identityEndpoint && identityHeader) {
        // App Service
        source = QStringLiteral("AppService");
        request.url = QUrl(*identityEndpoint);
        query.append({QStringLiteral("api-version"), QStringLiteral("2019-08-01")});
        query.append({QStringLiteral("resource"), resource});
        addUserAssigned(QStringLiteral("client_id"), QStringLiteral("mi_res_id"));
        request.headers.set(QStringLiteral("X-IDENTITY-HEADER"), *identityHeader);
    } else if (msiEndpoint) {
        // Cloud Shell
        source = QStringLiteral("CloudShell");
        if (mi.kind() != ManagedIdentity::Kind::System) {
            throw EntraClientException(EntraError::UserAssignedManagedIdentityNotSupported, QStringLiteral("[Managed Identity] User assigned identity is not supported by the Cloud Shell managed identity source."));
        }
        request.method = QStringLiteral("POST");
        request.url = QUrl(*msiEndpoint);
        request.headers.set(QStringLiteral("Metadata"), QStringLiteral("true"));
        request.setFormContent(StringDictionary{{QStringLiteral("resource"), resource}});
    } else if (identityEndpoint && imdsEndpoint) {
        // Azure Arc
        source = QStringLiteral("AzureArc");
        if (mi.kind() != ManagedIdentity::Kind::System) {
            throw EntraClientException(EntraError::UserAssignedManagedIdentityNotSupported, QStringLiteral("[Managed Identity] User assigned identity is not supported by the Azure Arc managed identity source."));
        }
        request.url = QUrl(*identityEndpoint);
        query.append({QStringLiteral("api-version"), QStringLiteral("2019-11-01")});
        query.append({QStringLiteral("resource"), resource});
        request.headers.set(QStringLiteral("Metadata"), QStringLiteral("true"));
    } else {
        // IMDS (the base address can be overridden like MSAL, e.g. for AKS pod identity)
        source = QStringLiteral("Imds");
        const QString base = env.tryGet(QStringLiteral("AZURE_POD_IDENTITY_AUTHORITY_HOST")).value_or(QStringLiteral("http://169.254.169.254"));
        request.url = QUrl(base + (base.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + QStringLiteral("metadata/identity/oauth2/token"));
        query.append({QStringLiteral("api-version"), QStringLiteral("2018-02-01")});
        query.append({QStringLiteral("resource"), resource});
        addUserAssigned(QStringLiteral("client_id"), QStringLiteral("msi_res_id"));
        request.headers.set(QStringLiteral("Metadata"), QStringLiteral("true"));
    }
    if (!query.isEmpty()) {
        request.url.setQuery(queryString(query), QUrl::StrictMode);
    }
    Trace2::writeData(trace2Category(), QStringLiteral("mi/source"), source.toLower());
    msalTrace(QStringLiteral("[Managed Identity] Sending request to %1 (%2).").arg(absoluteUri(request.url.adjusted(QUrl::RemoveQuery)), source));

    HttpClient client = context.httpClientFactory().createClient();
    client.setTimeoutMs(30 * 1000);
    HttpResponse response;
    try {
        response = client.send(request);
        if (source == QLatin1String("AzureArc") && response.statusCode == 401) {
            // Azure Arc: the challenge names a key file that proves local access to the machine
            const QString challenge = response.headers.value(QStringLiteral("WWW-Authenticate")).value_or(QString());
            const qsizetype eq = challenge.indexOf(QLatin1Char('='));
            const QString keyPath = eq < 0 ? QString() : challenge.mid(eq + 1).trimmed();
            if (!keyPath.startsWith(QLatin1String("/var/opt/azcmagent/tokens/")) || !keyPath.endsWith(QLatin1String(".key")) ||
                !context.fileSystem().fileExists(keyPath)) {
                throw EntraServiceException(EntraError::ManagedIdentityRequestFailed, QStringLiteral("[Managed Identity] The Azure Arc challenge did not name a valid key file."), 401);
            }
            const QString key = context.fileSystem().readAllText(keyPath);
            request.headers.set(QStringLiteral("Authorization"), QStringLiteral("Basic ") + key.trimmed());
            response = client.send(request);
        }
    } catch (const HttpRequestException &ex) {
        throw EntraServiceException(EntraError::ManagedIdentityUnreachableNetwork, QStringLiteral("[Managed Identity] %1").arg(ex.message()));
    } catch (const TimeoutException &ex) {
        throw EntraServiceException(EntraError::ManagedIdentityUnreachableNetwork, QStringLiteral("[Managed Identity] %1").arg(ex.message()));
    }

    const QJsonObject obj = QJsonDocument::fromJson(response.content).object();
    if (!response.isSuccessStatusCode()) {
        QString message = obj.value(QStringLiteral("error_description")).toString();
        if (message.isEmpty()) {
            message = obj.value(QStringLiteral("message")).toString(response.contentString());
        }
        throw EntraServiceException(EntraError::ManagedIdentityRequestFailed, QStringLiteral("[Managed Identity] %1").arg(message), response.statusCode);
    }
    if (!obj.value(QStringLiteral("access_token")).isString()) {
        throw EntraServiceException(EntraError::InvalidManagedIdentityResponse, QStringLiteral("[Managed Identity] Invalid response, the authentication response received did not contain the expected fields."), response.statusCode);
    }
    return EntraAuthResult{obj.value(QStringLiteral("access_token")).toString(), std::nullopt};
}

// =============================================================================================
// EntraAuthentication
// =============================================================================================

EntraAuthentication::EntraAuthentication(ICommandContext &context, std::optional<PublicClientConfig> publicClientConfig,
                                         TokenCacheStorageFactory cacheStorageFactory)
    : AuthenticationBase(context), m_publicClientConfig(std::move(publicClientConfig)), d(std::make_unique<Private>(*this, context))
{
    d->storageFactory = cacheStorageFactory ? std::move(cacheStorageFactory)
                                            : TokenCacheStorageFactory([&context](TokenCacheKind kind, bool fallback) {
                                                  return createDefaultTokenCacheStorage(context, kind, fallback);
                                              });
}

EntraAuthentication::~EntraAuthentication() = default;

void EntraAuthentication::setCodeGenerator(std::shared_ptr<IOAuth2CodeGenerator> generator) { d->codeGenerator = std::move(generator); }
void EntraAuthentication::setWebBrowser(std::shared_ptr<IOAuth2WebBrowser> browser) { d->browser = std::move(browser); }
void EntraAuthentication::setClock(std::function<qint64()> clock) { d->clock = std::move(clock); }

bool EntraAuthentication::isBrokerEnabled()
{
    // Default to using the OS broker
    const bool defaultValue = true;
    QString valueStr;
    if (m_context.settings().tryGetSetting(Envars::MsAuthUseBroker, CredCfg::SectionName, CredCfg::MsAuthUseBroker, valueStr)) {
        return toBooleanyOrDefault(valueStr, defaultValue);
    }
    return defaultValue;
}

InteractionMode EntraAuthentication::getInteractionMode()
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("get_interaction_mode"));
    // The broker is never available here, so there is no early "Auto" answer for it.
    d->publicApp();

    // Check for a stored user preference
    InteractionMode mode;
    if (d->tryGetModePreference(mode)) {
        Trace2::writeData(trace2Category(), QStringLiteral("mode/source"), QStringLiteral("preference"));
        Trace2::writeData(trace2Category(), QStringLiteral("mode/selected"), modeName(mode));
        return mode;
    }

    // Determine the set of available modes
    QList<InteractionMode> available{InteractionMode::Auto};
    if (d->isEmbeddedWebViewAvailable()) {
        available.append(InteractionMode::EmbeddedWebView);
    }
    if (d->isSystemWebViewAvailable()) {
        available.append(InteractionMode::SystemWebView);
    }
    if (d->isDeviceCodeAvailable()) {
        available.append(InteractionMode::DeviceCode);
    }
    QStringList names;
    QStringList labels;
    for (const InteractionMode m : std::as_const(available)) {
        names.append(modeName(m));
        labels.append(displayName(m));
    }
    Trace2::writeData(trace2Category(), QStringLiteral("mode/available"), names.join(QLatin1Char(',')));

    // Show the mode prompt with the UI helper
    QString command, args;
    if (m_context.settings().isGuiPromptsEnabled() && m_context.sessionManager().isDesktopSession() && tryFindUiHelper(command, args)) {
        const QString promptArgs = args + QStringLiteral("select-interaction-mode --available %1").arg(quoteCmdArg(names.join(QLatin1Char(','))));
        const StringDictionary result = invokeHelper(command, promptArgs);
        const OptString str = result.tryGet(QStringLiteral("interaction_mode"));
        if (str) {
            for (const InteractionMode m : {InteractionMode::Auto, InteractionMode::EmbeddedWebView, InteractionMode::SystemWebView, InteractionMode::DeviceCode}) {
                if (str->compare(modeName(m), Qt::CaseInsensitive) == 0) {
                    Trace2::writeData(trace2Category(), QStringLiteral("mode/source"), QStringLiteral("helper"));
                    Trace2::writeData(trace2Category(), QStringLiteral("mode/selected"), modeName(m));
                    return m;
                }
            }
        }
        throw Exception(QStringLiteral("Missing or invalid interaction_mode in response"));
    }

    // Show prompt in tty
    const int choice = m_context.console().promptSelection(QStringLiteral("Select an authentication flow"), labels);
    const InteractionMode selected = available.value(choice, InteractionMode::Auto);
    Trace2::writeData(trace2Category(), QStringLiteral("mode/source"), QStringLiteral("terminal"));
    Trace2::writeData(trace2Category(), QStringLiteral("mode/selected"), modeName(selected));
    return selected;
}

QList<EntraAccount> EntraAuthentication::getUserAccounts()
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("get_accounts"));
    const PublicClientConfig &config = d->publicApp();
    QList<EntraAccount> result;
    const QList<TokenCache::Account> accounts = d->userAccounts(config.clientId);
    for (const TokenCache::Account &a : accounts) {
        result.append(EntraAccount{a.homeAccountId, a.userName});
    }
    Trace2::writeData(trace2Category(), QStringLiteral("cached/count"), qint64(result.size()));
    return result;
}

bool EntraAuthentication::removeUserAccount(const EntraAccount &account)
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("remove_account"));
    const PublicClientConfig &config = d->publicApp();
    const std::optional<TokenCache::Account> cached = d->resolveAccount(config.clientId, account);
    if (!cached) {
        Trace2::writeData(trace2Category(), QStringLiteral("was_removed"), QStringLiteral("false"));
        return false;
    }
    m_context.trace().writeLine(QStringLiteral("Removing account '%1' (%2) from the cache...").arg(cached->homeAccountId, cached->userName));
    d->updateCache(TokenCacheKind::User, [&](TokenCache &cache) {
        cache.accounts.removeIf([&](const TokenCache::Account &a) { return a.homeAccountId == cached->homeAccountId; });
        cache.refreshTokens.removeIf([&](const TokenCache::RefreshToken &r) { return r.homeAccountId == cached->homeAccountId; });
        cache.accessTokens.removeIf([&](const TokenCache::AccessToken &t) { return t.homeAccountId == cached->homeAccountId; });
    });
    Trace2::writeData(trace2Category(), QStringLiteral("was_removed"), QStringLiteral("true"));
    return true;
}

EntraAuthResult EntraAuthentication::getTokenForUser(const QStringList &scopes, const OptString &authority,
                                                     const std::optional<EntraAccount> &account, InteractionMode interactionMode)
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("get_token_user"));
    const PublicClientConfig &config = d->publicApp();
    if (!isNullOrWhiteSpace(authority)) {
        Trace2::writeData(trace2Category(), QStringLiteral("app/authority"), *authority);
    }
    const Authority auth = Authority::parse(isNullOrWhiteSpace(authority) ? DefaultAuthority : *authority);
    d->cacheStorage(TokenCacheKind::User);

    // If we've been given an account, try to resolve it to one in the cache
    std::optional<TokenCache::Account> cachedAccount;
    if (account) {
        cachedAccount = d->resolveAccount(config.clientId, *account);
    }

    // Try silent authentication first if we have a cached account
    if (cachedAccount) {
        if (std::optional<EntraAuthResult> result = d->silent(config, auth, scopes, *cachedAccount)) {
            Trace2::writeData(trace2Category(), QStringLiteral("flow"), QStringLiteral("silent"));
            return *result;
        }
    }

    throwIfUserInteractionDisabled();

    // Interactive authentication (the broker is not available)
    Trace2::writeData(trace2Category(), QStringLiteral("flow"), QStringLiteral("interactive"));
    return d->interactive(config, auth, scopes, interactionMode);
}

EntraAuthResult EntraAuthentication::getTokenForServicePrincipal(const QStringList &scopes, const ServicePrincipalIdentity &sp)
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("token_service_principal"));
    m_context.trace().writeLine(QStringLiteral("Creating confidential client for service principal '%1' in tenant '%2'...").arg(sp.id, sp.tenantId));

    std::function<void(StringDictionary &, const QUrl &)> addCredential;
    if (sp.certificate) {
        Trace2::writeData(trace2Category(), QStringLiteral("credential/type"), QStringLiteral("certificate"));
        m_context.trace().writeLine(QStringLiteral("Using service principal certificate: %1").arg(sp.certificate->thumbprint));
        const ServicePrincipalIdentity::Certificate cert = *sp.certificate;
        const bool sendX5C = sp.sendX5C;
        const QString clientId = sp.id;
        addCredential = [this, cert, sendX5C, clientId](StringDictionary &form, const QUrl &tokenEndpoint) {
            form.set(QStringLiteral("client_assertion_type"), QStringLiteral("urn:ietf:params:oauth:client-assertion-type:jwt-bearer"));
            form.set(QStringLiteral("client_assertion"), d->certificateAssertion(clientId, tokenEndpoint, cert, sendX5C));
        };
    } else if (!isNullOrWhiteSpace(sp.clientSecret)) {
        Trace2::writeData(trace2Category(), QStringLiteral("credential/type"), QStringLiteral("secret"));
        m_context.trace().writeLineSecrets(QStringLiteral("Using service principal secret: {0}"), {*sp.clientSecret});
        const QString secret = *sp.clientSecret;
        addCredential = [secret](StringDictionary &form, const QUrl &) { form.set(QStringLiteral("client_secret"), secret); };
    } else {
        throw ArgumentException(QStringLiteral("Service principal '%1' must have either a certificate or client secret. (Parameter 'sp')").arg(sp.id));
    }

    Trace2::writeData(trace2Category(), QStringLiteral("send_x5c"), sp.sendX5C ? QStringLiteral("true") : QStringLiteral("false"));
    m_context.trace().writeLine(QStringLiteral("SendX5C is '%1'").arg(sp.sendX5C ? QStringLiteral("True") : QStringLiteral("False")));
    d->cacheStorage(TokenCacheKind::App);
    m_context.trace().writeLine(QStringLiteral("Acquiring token for service principal with scopes '%1'...").arg(scopes.join(QStringLiteral(", "))));
    return d->clientCredentials(sp.id, sp.tenantId, scopes, addCredential);
}

EntraAuthResult EntraAuthentication::getTokenForManagedIdentity(const QString &resource, const ManagedIdentity &mi)
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("token_managed_identity"));
    // Record whether the identity is system- or user-assigned, but not the client or resource ID itself
    Trace2::writeData(trace2Category(), QStringLiteral("mi/kind"), mi.id().section(QStringLiteral("://"), 0, 0));
    m_context.trace().writeLine(QStringLiteral("Creating confidential client for managed identity '%1'...").arg(mi.id()));
    m_context.trace().writeLine(QStringLiteral("Acquiring token for managed identity with resource '%1'...").arg(resource));
    return d->managedIdentity(resource, mi);
}

EntraAuthResult EntraAuthentication::getTokenUsingWorkloadFederation(const QStringList &scopes, const WorkloadFederationOptions &fedOpts)
{
    Trace2::Scope region = Trace2::startRegion(trace2Category(), QStringLiteral("token_workload_federation"));
    Trace2::writeData(trace2Category(), QStringLiteral("scenario"), scenarioName(fedOpts.scenario).toLower());
    m_context.trace().writeLine(QStringLiteral("Creating confidential client for federation with client ID '%1' and tenant ID '%2'...").arg(fedOpts.clientId, fedOpts.tenantId));
    m_context.trace().writeLine(QStringLiteral("Federation scenario: %1").arg(scenarioName(fedOpts.scenario)));
    d->cacheStorage(TokenCacheKind::App);
    // The assertion is only computed when a token must be requested (MSAL's assertion callback)
    return d->clientCredentials(fedOpts.clientId, fedOpts.tenantId, scopes, [this, fedOpts](StringDictionary &form, const QUrl &) {
        form.set(QStringLiteral("client_assertion_type"), QStringLiteral("urn:ietf:params:oauth:client-assertion-type:jwt-bearer"));
        form.set(QStringLiteral("client_assertion"), d->clientAssertion(fedOpts));
    });
}

} // namespace gcm::entra
