// Port of Interop/Linux/SecretServiceCollection.cs over the libsecret C API (same schema and attributes, so
// credentials stored by the .NET version stay readable).
#include "core/stores.h"

#include "core/constants.h"
#include "core/strings.h"

#include <QScopeGuard>
#include <libsecret/secret.h>

namespace gcm {

namespace {

const char *SchemaName = "com.microsoft.GitCredentialManager";
const char *ServiceAttributeName = "service";
const char *AccountAttributeName = "account";
const char *PlainTextContentType = "text/plain";

const SecretSchema *schema()
{
    static const SecretSchema s = {
        SchemaName,
        SECRET_SCHEMA_DONT_MATCH_NAME,
        {
            {ServiceAttributeName, SECRET_SCHEMA_ATTRIBUTE_STRING},
            {AccountAttributeName, SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SecretSchemaAttributeType(0)},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &s;
}

/// Owns a GHashTable of strdup'ed key/value strings.
struct Attributes
{
    Q_DISABLE_COPY_MOVE(Attributes)
    Attributes() : table(g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free)) {}
    ~Attributes() { g_hash_table_destroy(table); }
    void insert(const char *key, const QString &value)
    {
        g_hash_table_insert(table, g_strdup(key), g_strdup(value.toUtf8().constData()));
    }
    GHashTable *table;
};

InteropException exceptionFromError(SecretServiceCollection::Operation operation, GError *error)
{
    if (!error) {
        return SecretServiceCollection::createException(operation, std::nullopt, -1, std::nullopt);
    }
    return SecretServiceCollection::createException(operation, QString::fromUtf8(g_quark_to_string(error->domain)), error->code, QString::fromUtf8(error->message));
}

/// Diagnoses bus connectivity separately from Secret Service availability.
SecretService *getSecretService()
{
    GError *error = nullptr;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error || !connection) {
        const InteropException ex = exceptionFromError(SecretServiceCollection::Operation::ConnectBus, error);
        if (error) {
            g_error_free(error);
        }
        throw ex;
    }
    g_object_unref(connection);

    SecretService *service = secret_service_get_sync(SecretServiceFlags(SECRET_SERVICE_OPEN_SESSION | SECRET_SERVICE_LOAD_COLLECTIONS), nullptr, &error);
    if (error || !service) {
        const InteropException ex = exceptionFromError(SecretServiceCollection::Operation::OpenService, error);
        if (error) {
            g_error_free(error);
        }
        throw ex;
    }
    return service;
}

void throwIfError(SecretServiceCollection::Operation operation, GError *error)
{
    if (error) {
        const InteropException ex = exceptionFromError(operation, error);
        g_error_free(error);
        throw ex;
    }
}

} // namespace

SecretServiceCollection::SecretServiceCollection(OptString ns) : m_namespace(std::move(ns)) {}

QString SecretServiceCollection::createServiceName(const QString &service) const
{
    return isNullOrWhiteSpace(m_namespace) ? service : *m_namespace + QLatin1Char(':') + service;
}

QStringList SecretServiceCollection::getAccounts(const OptString &service)
{
    QStringList accounts;
    for (const auto credentials = enumerate(service, std::nullopt); const auto &[svc, credential] : credentials) {
        if (!accounts.contains(credential.account)) {
            accounts.append(credential.account);
        }
    }
    return accounts;
}

std::optional<Credential> SecretServiceCollection::get(const OptString &service, const OptString &account)
{
    const auto results = enumerate(service, account);
    if (results.isEmpty()) {
        return std::nullopt;
    }
    return results.first().second;
}

QList<std::pair<OptString, Credential>> SecretServiceCollection::enumerate(const OptString &service, const OptString &account)
{
    SecretService *secService = getSecretService();
    Attributes query;
    if (!isNullOrWhiteSpace(service)) {
        query.insert(ServiceAttributeName, createServiceName(*service));
    }
    if (!isNullOrWhiteSpace(account)) {
        query.insert(AccountAttributeName, *account);
    }

    GError *error = nullptr;
    GList *results = secret_service_search_sync(secService, schema(), query.table, SecretSearchFlags(SECRET_SEARCH_UNLOCK | SECRET_SEARCH_ALL), nullptr, &error);
    const auto guard = qScopeGuard([results, secService] {
        if (results) {
            g_list_free_full(results, g_object_unref);
        }
        g_object_unref(secService);
    });
    throwIfError(Operation::Search, error);

    QList<std::pair<OptString, Credential>> credentials;
    for (GList *itemPtr = results; itemPtr && itemPtr->data; itemPtr = itemPtr->next) {
        auto *item = static_cast<SecretItem *>(itemPtr->data);

        // Searches can return locked items even when unlocking was requested
        if (secret_item_get_locked(item)) {
            GList toUnlock{item, nullptr, nullptr};
            const int numUnlocked = secret_service_unlock_sync(secService, &toUnlock, nullptr, nullptr, &error);
            if (error || numUnlocked != 1) {
                const InteropException ex = exceptionFromError(Operation::Unlock, error);
                if (error) {
                    g_error_free(error);
                }
                throw ex;
            }
        }

        // Attributes
        GHashTable *attrs = secret_item_get_attributes(item);
        auto lookup = [attrs](const char *key) -> OptString {
            const auto *value = static_cast<const char *>(g_hash_table_lookup(attrs, key));
            return value ? OptString(QString::fromUtf8(value)) : std::nullopt;
        };
        const OptString svc = lookup(ServiceAttributeName);
        const OptString acct = lookup(AccountAttributeName);
        g_hash_table_unref(attrs);

        // The secret
        const gboolean loaded = secret_item_load_secret_sync(item, nullptr, &error);
        if (error || !loaded) {
            const InteropException ex = exceptionFromError(Operation::Load, error);
            if (error) {
                g_error_free(error);
            }
            throw ex;
        }
        SecretValue *value = secret_item_get_secret(item);
        if (!value) {
            throw exceptionFromError(Operation::Load, nullptr);
        }
        gsize length = 0;
        const gchar *password = secret_value_get(value, &length);
        Credential credential{acct.value_or(QString()), QString::fromUtf8(password, qsizetype(length))};
        secret_value_unref(value);

        credentials.append({svc, credential});
    }
    return credentials;
}

void SecretServiceCollection::addOrUpdate(const QString &service, const QString &account, const QString &secret)
{
    // Nothing to write when an identical credential exists
    if (const std::optional<Credential> existing = get(service, account);
        existing && existing->account == account && existing->password == secret) {
        return;
    }

    SecretService *secService = getSecretService();
    const auto serviceGuard = qScopeGuard([secService] { g_object_unref(secService); });

    const QString fullServiceName = createServiceName(service);
    Attributes attributes;
    attributes.insert(ServiceAttributeName, fullServiceName);
    if (!isNullOrWhiteSpace(account)) {
        attributes.insert(AccountAttributeName, account);
    }

    const QByteArray secretBytes = secret.toUtf8();
    SecretValue *secretValue = secret_value_new(secretBytes.constData(), secretBytes.size(), PlainTextContentType);
    GError *error = nullptr;
    // The full service name is the label
    const gboolean result = secret_service_store_sync(secService, schema(), attributes.table, nullptr,
                                                      fullServiceName.toUtf8().constData(), secretValue, nullptr, &error);
    secret_value_unref(secretValue);
    throwIfError(Operation::Store, error);
    if (!result) {
        throw exceptionFromError(Operation::Store, nullptr);
    }
}

bool SecretServiceCollection::remove(const OptString &service, const OptString &account)
{
    SecretService *secService = getSecretService();
    const auto serviceGuard = qScopeGuard([secService] { g_object_unref(secService); });

    Attributes query;
    if (!isNullOrWhiteSpace(service)) {
        query.insert(ServiceAttributeName, createServiceName(*service));
    }
    if (!isNullOrWhiteSpace(account)) {
        query.insert(AccountAttributeName, *account);
    }

    GError *error = nullptr;
    const gboolean result = secret_service_clear_sync(secService, schema(), query.table, nullptr, &error);
    throwIfError(Operation::Erase, error);
    return result;
}

InteropException SecretServiceCollection::createException(Operation operation, const OptString &errorDomain, int errorCode,
                                                          const OptString &errorMessage)
{
    QString message;
    switch (operation) {
        case Operation::ConnectBus: message = QStringLiteral("Failed to connect to the D-Bus session bus."); break;
        case Operation::OpenService: message = QStringLiteral("Failed to open the Secret Service session."); break;
        case Operation::Search: message = QStringLiteral("Failed to search for credentials."); break;
        case Operation::Unlock: message = QStringLiteral("Failed to unlock the secret item."); break;
        case Operation::Store: message = QStringLiteral("Failed to store credentials."); break;
        case Operation::Erase: message = QStringLiteral("Failed to erase credentials."); break;
        case Operation::Load: message = QStringLiteral("Failed to load the secret."); break;
    }

    const QString dbusErrorDomain = QString::fromUtf8(g_quark_to_string(G_DBUS_ERROR));
    const QString ioErrorDomain = QString::fromUtf8(g_quark_to_string(G_IO_ERROR));
    const QString secretErrorDomain = QString::fromUtf8(g_quark_to_string(SECRET_ERROR));

    const bool serviceUnavailable = errorDomain == dbusErrorDomain &&
                                    (errorCode == G_DBUS_ERROR_SERVICE_UNKNOWN || errorCode == G_DBUS_ERROR_NAME_HAS_NO_OWNER);
    OptString hint;
    if (errorDomain == ioErrorDomain && errorCode == G_IO_ERROR_CANCELLED) {
        hint = QStringLiteral("The operation was cancelled.");
    } else if (errorDomain == secretErrorDomain && errorCode == SECRET_ERROR_IS_LOCKED) {
        hint = QStringLiteral("The secret item or collection is locked. Unlock it using your Secret Service provider before retrying.");
    } else if (operation == Operation::ConnectBus) {
        hint = QStringLiteral("Ensure a D-Bus session bus is running and accessible. In a headless session, start your shell or workload with 'dbus-run-session'.");
    } else if (operation == Operation::OpenService) {
        hint = (serviceUnavailable ? QStringLiteral("No Secret Service provider is available. ") : QString()) +
               QStringLiteral("Ensure a Secret Service provider, such as GNOME Keyring or KWallet, is installed and running on the same D-Bus session bus.");
    } else if (serviceUnavailable) {
        hint = QStringLiteral("A required D-Bus service is unavailable. Ensure your Secret Service provider and its prompting components are installed and running.");
    } else if (operation == Operation::Unlock) {
        hint = QStringLiteral("Unlock the item or its collection using your Secret Service provider before retrying. An unlock prompt may have been dismissed or cancelled.");
    } else if (operation == Operation::Store && !errorMessage) {
        hint = QStringLiteral("The collection could not be created or unlocked. A Secret Service prompt may have been dismissed or cancelled.");
    } else if (operation == Operation::Load && !errorMessage) {
        hint = QStringLiteral("No secret value was returned. Ensure the item and its collection are unlocked.");
    }

    if (hint) {
        message += QLatin1Char('\n') + *hint;
    }
    message += QStringLiteral("\nSee %1 for more information.").arg(Constants::HelpUrls::GcmCredentialStores);

    std::shared_ptr<const Exception> inner;
    if (errorMessage) {
        inner = std::make_shared<Exception>(QStringLiteral("%1: %2").arg(errorDomain.value_or(QString()), *errorMessage));
    }
    return InteropException(message, errorCode, inner);
}

} // namespace gcm
