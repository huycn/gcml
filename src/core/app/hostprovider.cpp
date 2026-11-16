// Port of HostProvider.cs and HostProviderRegistry.cs
#include "core/hostprovider.h"

#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/strings.h"
#include "core/trace2.h"

namespace gcm {

// =============================================================================================
// HostProvider
// =============================================================================================

QString HostProvider::getServiceName(const GitRequest &request)
{
    // By default the service name is the absolute URI of the request, without any user info.
    const std::optional<QUrl> uri = request.getRemoteUri(false);
    if (!uri) {
        throw InvalidOperationException(QStringLiteral("Unable to determine the remote URL of the request."));
    }
    QString service = absoluteUri(*uri);
    while (service.endsWith(QLatin1Char('/'))) {
        service.chop(1);
    }
    return service;
}

GitResponse HostProvider::getCredential(const GitRequest &request)
{
    // Try and locate an existing credential in the OS credential store
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();
    trace.writeLine(QStringLiteral("Looking for existing credential in store with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));

    std::optional<Credential> credential = m_context.credentialStore().get(service, request.userName());
    if (!credential) {
        trace.writeLine(QStringLiteral("No existing credentials found."));
        // No existing credential was found, create a new one
        trace.writeLine(QStringLiteral("Creating new credential..."));
        credential = generateCredential(request);
        trace.writeLine(QStringLiteral("Credential created."));
    } else {
        trace.writeLine(QStringLiteral("Existing credential found."));
    }
    return GitResponse(*credential);
}

void HostProvider::storeCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();

    // WIA authentication is signalled to Git as an empty user name/password pair, which we are then asked to
    // 'store'. Empty credentials are not stored.
    if (isNullOrWhiteSpace(request.userName()) && isNullOrWhiteSpace(request.password())) {
        trace.writeLine(QStringLiteral("Not storing empty credential."));
    } else {
        // Add or update the credential in the store.
        trace.writeLine(QStringLiteral("Storing credential with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));
        m_context.credentialStore().addOrUpdate(service, request.userName().value_or(QString()), request.password().value_or(QString()));
        trace.writeLine(QStringLiteral("Credential was successfully stored."));
    }
}

void HostProvider::eraseCredential(const GitRequest &request)
{
    const QString service = getServiceName(request);
    ITrace &trace = m_context.trace();

    trace.writeLine(QStringLiteral("Erasing stored credential in store with service=%1 account=%2...").arg(service, request.userName().value_or(QString())));
    if (m_context.credentialStore().remove(service, request.userName())) {
        trace.writeLine(QStringLiteral("Credential was successfully erased."));
    } else {
        trace.writeLine(QStringLiteral("No credential was erased."));
    }
}

// =============================================================================================
// HostProviderRegistry
// =============================================================================================

HostProviderRegistry::HostProviderRegistry(ICommandContext &context)
:   m_context(context)
{
}

HostProviderRegistry::~HostProviderRegistry() = default;

void HostProviderRegistry::registerProvider(std::shared_ptr<IHostProvider> provider, HostProviderPriority priority)
{
    EnsureArgument::notNull(provider, "hostProvider");

    if (equalsIgnoreCase(provider->id(), Constants::ProviderIdAuto)) {
        throw ArgumentException(QStringLiteral("A host provider cannot be registered with the ID '%1' (Parameter 'hostProvider')").arg(Constants::ProviderIdAuto));
    }

    for (const QStringList authorities = provider->supportedAuthorityIds(); const QString &authority : authorities) {
        if (equalsIgnoreCase(authority, Constants::AuthorityIdAuto)) {
            throw ArgumentException(QStringLiteral("A host provider cannot be registered with the legacy authority ID '%1' (Parameter 'hostProvider')").arg(Constants::AuthorityIdAuto));
        }
    }

    if (!m_priorityOrder.contains(int(priority))) {
        m_priorityOrder.append(int(priority));
    }
    m_providers[int(priority)].append(std::move(provider));
}

std::shared_ptr<IHostProvider> HostProviderRegistry::getProvider(const GitRequest &request)
{
    ITrace &trace = m_context.trace();
    IConsoleService &console = m_context.console();
    ISettings &settings = m_context.settings();

    auto allProviders = [this] {
        QList<std::shared_ptr<IHostProvider>> all;
        for (int p : std::as_const(m_priorityOrder)) {
            all.append(m_providers[p]);
        }
        return all;
    };

    // A specified provider
    if (const OptString providerId = settings.providerOverride()) {
        trace.writeLine(QStringLiteral("Host provider override was set id='%1'").arg(*providerId));
        if (!equalsIgnoreCase(Constants::ProviderIdAuto, *providerId)) {
            for (const auto providers = allProviders(); const auto &provider : providers) {
                if (equalsIgnoreCase(provider->id(), *providerId)) {
                    return provider;
                }
            }
            trace.writeLine(QStringLiteral("No host provider was found with ID '%1'.. falling back to auto-detection.").arg(*providerId));
            console.writeWarning(QStringLiteral("a host provider override was set but no such provider '%1' was found. Falling back to auto-detection.").arg(*providerId));
        }
    }
    // A provider by supported authorities
    else if (const OptString authority = settings.legacyAuthorityOverride()) {
        trace.writeLine(QStringLiteral("Host provider authority override was set authority='%1'").arg(*authority));
        console.writeWarning(QStringLiteral("the `credential.authority` and `GCM_AUTHORITY` settings are deprecated."));
        console.writeWarning(QStringLiteral("see %1 for more information.").arg(Constants::HelpUrls::GcmAuthorityDeprecated));
        if (!equalsIgnoreCase(Constants::AuthorityIdAuto, *authority)) {
            for (const auto providers = allProviders(); const auto &provider : providers) {
                for (const QStringList ids = provider->supportedAuthorityIds(); const QString &id : ids) {
                    if (equalsIgnoreCase(id, *authority)) {
                        return provider;
                    }
                }
            }
            trace.writeLine(QStringLiteral("No host provider was found with authority '%1'.. falling back to auto-detection.").arg(*authority));
            console.writeWarning(QStringLiteral("a supported authority override was set but no such provider supporting authority '%1' was found. Falling back to auto-detection.").arg(*authority));
        }
    }

    // Auto-detection, with a network probe whose result is remembered
    trace.writeLine(QStringLiteral("Performing auto-detection of host provider."));
    const std::optional<QUrl> uri = request.getRemoteUri();
    if (!uri) {
        throw Exception(QStringLiteral("Unable to detect host provider without a remote URL"));
    }

    // Only HTTP(S) URLs can be probed (not SMTP, IMAP, ...)
    const bool canProbeUri = equalsIgnoreCase(uri->scheme(), QStringLiteral("http")) ||
                             equalsIgnoreCase(uri->scheme(), QStringLiteral("https"));

    const int probeTimeoutMs = settings.autoDetectProviderTimeout();
    trace.writeLine(QStringLiteral("Auto-detect probe timeout is %1 ms.").arg(QString::number(probeTimeoutMs / 1000.0)));

    std::optional<HttpResponse> probeResponse;
    bool probed = false;

    auto matchProvider = [&](HostProviderPriority priority, bool probe) -> std::shared_ptr<IHostProvider> {
        const auto &providers = m_providers[int(priority)];
        if (providers.isEmpty()) {
            return nullptr;
        }
        trace.writeLine(QStringLiteral("Checking against %1 host providers registered with priority '%2'.")
                            .arg(QString::number(providers.size()),
                                 priority == HostProviderPriority::High     ? QStringLiteral("High")
                                 : priority == HostProviderPriority::Normal ? QStringLiteral("Normal")
                                                                            : QStringLiteral("Low")));

        // Static matching on the Git request arguments first (cheap)
        for (const auto &provider : providers) {
            if (provider->isSupported(request)) {
                return provider;
            }
        }

        // Then the response to one query of the remote URL (expensive), shared by all providers. A zero or negative
        // timeout disables the probe.
        if (probe && probeTimeoutMs > 0) {
            if (!probed) {
                probed = true;
                trace.writeLine(QStringLiteral("Querying remote URL for host provider auto-detection."));
                HttpClient client = m_context.httpClientFactory().createClient();
                client.setTimeoutMs(probeTimeoutMs);
                try {
                    probeResponse = client.head(*uri);
                } catch (const TimeoutException &) {
                    console.writeWarning(QStringLiteral("auto-detection of host provider took too long (>%1ms)").arg(probeTimeoutMs));
                    console.writeWarning(QStringLiteral("see %1 for more information.").arg(Constants::HelpUrls::GcmAutoDetect));
                } catch (const std::exception &ex) {
                    // The probe failed for some other reason; do not crash
                    console.writeWarning(QStringLiteral("failed to probe '%1' to detect provider").arg(absoluteUri(*uri)));
                    console.writeWarning(QString::fromUtf8(ex.what()));
                    console.writeWarning(QStringLiteral("see %1 for more information.").arg(Constants::HelpUrls::GcmAutoDetect));
                }
            }
            for (const auto &provider : providers) {
                if (provider->isSupported(probeResponse ? &*probeResponse : nullptr)) {
                    return provider;
                }
            }
        }
        return nullptr;
    };

    // Match providers starting with the highest priority
    std::shared_ptr<IHostProvider> match = matchProvider(HostProviderPriority::High, canProbeUri);
    if (!match) {
        match = matchProvider(HostProviderPriority::Normal, canProbeUri);
    }
    if (!match) {
        match = matchProvider(HostProviderPriority::Low, canProbeUri);
    }
    if (!match) {
        throw Exception(QStringLiteral("No host provider available to service this request."));
    }

    // A network call was made: remember the provider to avoid future calls
    if (probeResponse) {
        QString url = absoluteUri(*uri);
        while (url.endsWith(QLatin1Char('/'))) {
            url.chop(1);
        }
        const QString keyName = QStringLiteral("%1.%2.%3").arg(Constants::GitConfiguration::Credential::SectionName, url, Constants::GitConfiguration::Credential::Provider);
        try {
            trace.writeLine(QStringLiteral("Remembering host provider for '%1' as '%2'...").arg(absoluteUri(*uri), match->id()));
            m_context.git().getConfiguration().set(GitConfigurationLevel::Global, keyName, match->id());
        } catch (const std::exception &ex) {
            const QString message = QStringLiteral("Failed to set host provider!");
            trace.writeLine(message);
            trace.writeException(ex);
            Trace2::writeError(message);
            console.writeWarning(QStringLiteral("failed to remember result of host provider detection!"));
            console.writeWarning(QStringLiteral("try setting this manually: `git config --global %1 %2`").arg(keyName, match->id()));
        }
    }

    return match;
}

} // namespace gcm
