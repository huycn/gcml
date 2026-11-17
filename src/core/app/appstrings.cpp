// Definitions of the constants of core/generic.h.
#include "core/generic.h"

namespace gcm {

const QString GenericOAuthConfig::WellKnown::GiteaClientId = QStringLiteral("e90ee53c-94e2-48ac-9358-a874fb9e0662");
const QString GenericOAuthConfig::WellKnown::GiteaAuthzEndpoint = QStringLiteral("login/oauth/authorize");
const QString GenericOAuthConfig::WellKnown::GiteaTokenEndpoint = QStringLiteral("login/oauth/access_token");
const QUrl GenericOAuthConfig::WellKnown::LocalIPv4RedirectUri = QUrl(QStringLiteral("http://127.0.0.1"));

} // namespace gcm
