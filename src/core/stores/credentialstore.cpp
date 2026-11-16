// Port of CredentialStore.cs (Linux): selection and validation of the backing store.
#include "core/commandcontext.h"
#include "core/constants.h"
#include "core/stores.h"
#include "core/strings.h"
#include "core/trace2.h"

#include <QDir>

namespace gcm {

namespace StoreNames = Constants::CredentialStoreNames;

namespace {

Exception storeException(const QString &message)
{
    return Exception(message + QStringLiteral("\nSee %1 for more information.").arg(Constants::HelpUrls::GcmCredentialStores));
}

QString availableStoreList()
{
    auto row = [](const QString &name, const QString &description) {
        return QStringLiteral("  %1 : %2\n").arg(name.leftJustified(13), description);
    };
    return row(StoreNames::SecretService, QStringLiteral("freedesktop.org Secret Service (requires D-Bus and a Secret Service provider)")) +
           row(StoreNames::Gpg, QStringLiteral("GNU `pass` compatible credential storage (requires GPG and `pass`)")) +
           row(StoreNames::Cache, QStringLiteral("Git's in-memory credential cache")) +
           row(StoreNames::Plaintext, QStringLiteral("store credentials in plain-text files (UNSECURE)")) +
           row(StoreNames::None, QStringLiteral("disable internal credential storage"));
}

} // namespace

CredentialStore::CredentialStore(ICommandContext &context) : m_context(context) {}

CredentialStore::~CredentialStore() = default;

QString CredentialStore::name()
{
    ensureBackingStore();
    return m_backingStore->name();
}

int CredentialStore::maxCredentialSize()
{
    ensureBackingStore();
    return m_backingStore->maxCredentialSize();
}

QStringList CredentialStore::getAccounts(const OptString &service)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cred_store"), QStringLiteral("get_accounts"));
    ensureBackingStore();
    return m_backingStore->getAccounts(service);
}

std::optional<Credential> CredentialStore::get(const OptString &service, const OptString &account)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cred_store"), QStringLiteral("get"));
    ensureBackingStore();
    return m_backingStore->get(service, account);
}

void CredentialStore::addOrUpdate(const QString &service, const QString &account, const QString &secret)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cred_store"), QStringLiteral("add"));
    ensureBackingStore();
    m_backingStore->addOrUpdate(service, account, secret);
}

bool CredentialStore::remove(const OptString &service, const OptString &account)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cred_store"), QStringLiteral("remove"));
    ensureBackingStore();
    return m_backingStore->remove(service, account);
}

void CredentialStore::ensureBackingStore()
{
    if (m_backingStore) {
        return;
    }

    Trace2::Scope region = Trace2::startRegion(QStringLiteral("cred_store"), QStringLiteral("init"));
    ISettings &settings = m_context.settings();
    const QString ns = settings.credentialNamespace();
    const OptString configured = settings.credentialBackingStore();
    const OptString credStoreName = configured ? OptString(configured->toLower()) : std::nullopt;

    // Linux has no default store
    Trace2::writeData(QStringLiteral("cred_store"), QStringLiteral("store/configured"), credStoreName);
    Trace2::writeData(QStringLiteral("cred_store"), QStringLiteral("store/default"), OptString());
    Trace2::writeData(QStringLiteral("cred_store"), QStringLiteral("store/namespace"), ns);

    const QString name = credStoreName.value_or(QString());
    if (name == StoreNames::WindowsCredentialManager || name == StoreNames::Dpapi) {
        throw storeException(QStringLiteral("Can only use the '%1' credential store on Windows.").arg(name));
    } else if (name == StoreNames::MacOSKeychain) {
        throw storeException(QStringLiteral("Can only use the '%1' credential store on macOS.").arg(name));
    } else if (name == StoreNames::SecretService) {
        m_backingStore = std::make_unique<SecretServiceCollection>(ns);
    } else if (name == StoreNames::Gpg) {
        const QString gpgExec = [&] {
            // GCM_GPG_PATH if set
            if (const OptString gpgPath = m_context.environment().getEnvironmentVariable(Constants::EnvironmentVariables::GpgExecutablePath)) {
                if (m_context.fileSystem().fileExists(*gpgPath)) {
                    m_context.trace().writeLine(QStringLiteral("Using Git executable from GCM_GPG_PATH: %1").arg(*gpgPath));
                    return *gpgPath;
                }
                throw Exception(QStringLiteral("GPG executable does not exist with path '%1'").arg(*gpgPath));
            }
            // Like `pass`: gpg2 if available, otherwise gpg
            QString gpg2Path;
            if (m_context.environment().tryLocateExecutable(QStringLiteral("gpg2"), gpg2Path)) {
                m_context.trace().writeLine(QStringLiteral("Using PATH-located GPG (gpg2) executable: %1").arg(gpg2Path));
                return gpg2Path;
            }
            const QString gpgPath = m_context.environment().locateExecutable(QStringLiteral("gpg"));
            m_context.trace().writeLine(QStringLiteral("Using PATH-located GPG (gpg) executable: %1").arg(gpgPath));
            return gpgPath;
        }();

        // Headless without GPG_TTY or SSH_TTY: pin-entry needs a TTY device path
        const StringDictionary &vars = m_context.environment().variables();
        if (!m_context.sessionManager().isDesktopSession() && !vars.contains(QStringLiteral("GPG_TTY")) &&
            !vars.contains(QStringLiteral("SSH_TTY"))) {
            throw storeException(QStringLiteral("GPG_TTY is not set; add `export GPG_TTY=$(tty)` to your profile."));
            }

        // A redirected pass store location, else ~/.password-store
        QString storeRoot;
        if (!settings.tryGetSetting(GpgPassCredentialStore::PasswordStoreDirEnvar, Constants::GitConfiguration::Credential::SectionName,
                                    Constants::GitConfiguration::Credential::GpgPassStorePath, storeRoot)) {
            storeRoot = QDir(QDir::homePath()).filePath(QStringLiteral(".password-store"));
                                    }

        m_gpg = std::make_unique<Gpg>(gpgExec, m_context.sessionManager(), m_context.processManager());
        m_backingStore = std::make_unique<GpgPassCredentialStore>(m_context.fileSystem(), *m_gpg, storeRoot, ns);
    } else if (name == StoreNames::Cache) {
        // --timeout and other options
        QString options;
        if (!settings.tryGetSetting(Constants::EnvironmentVariables::GcmCredCacheOptions,
                                    Constants::GitConfiguration::Credential::SectionName,
                                    Constants::GitConfiguration::Credential::CredCacheOptions, options)) {
            options.clear();
                                    }
        m_backingStore = std::make_unique<CredentialCacheStore>(m_context.git(), options);
    } else if (name == StoreNames::Plaintext) {
        // A redirected store location, else ~/.gcm/store
        QString storeRoot;
        if (!settings.tryGetSetting(Constants::EnvironmentVariables::GcmPlaintextStorePath,
                                    Constants::GitConfiguration::Credential::SectionName,
                                    Constants::GitConfiguration::Credential::PlaintextStorePath, storeRoot)) {
            storeRoot = m_context.fileSystem().userDataDirectoryPath() + QStringLiteral("/store");
                                    }
        m_backingStore = std::make_unique<PlaintextCredentialStore>(m_context.fileSystem(), storeRoot, ns);
    } else if (name == StoreNames::None) {
        m_backingStore = std::make_unique<NullCredentialStore>();
    } else {
        QString message = isNullOrWhiteSpace(credStoreName) ? QStringLiteral("No credential store has been selected.\n")
                                                            : QStringLiteral("Unknown credential store '%1'.\n").arg(name);
        message += QStringLiteral("\nSet the %1 environment variable or the %2.%3 Git configuration setting to one of the following options:\n\n")
                       .arg(Constants::EnvironmentVariables::GcmCredentialStore, Constants::GitConfiguration::Credential::SectionName, Constants::GitConfiguration::Credential::CredentialStore);
        message += availableStoreList();
        message += QStringLiteral("\nSee %1 for more information.\n").arg(Constants::HelpUrls::GcmCredentialStores);
        throw Exception(message);
    }
}

} // namespace gcm
