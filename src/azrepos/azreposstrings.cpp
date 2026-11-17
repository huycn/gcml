// Definitions of the string constants of azrepos.h and entra.h (see GCM_STR in core/constants.h).
#define GCM_DEFINE_STRINGS_AZREPOS
#define GCM_DEFINE_STRINGS_ENTRA
#include "azrepos/azrepos.h"
#include "azrepos/entra.h"

namespace gcm {

const QStringList azrepos::AzureDevOpsConstants::AzureDevOpsDefaultScopes = {QStringLiteral("499b84ac-1321-427f-aa17-267ca6975798/.default")};
const QString azrepos::AzureReposBinding::NoInherit = QString(QLatin1String(""));

// The same value as Constants::DefaultWorkloadFederationAudience (defined in another file, so not used here: the
// order of initialisation between files is unspecified)
const QString entra::WorkloadFederationOptions::DefaultAudience = QStringLiteral("api://AzureADTokenExchange");
const QStringList entra::EntraAuthentication::AuthorityIds = {
    QStringLiteral("msa"),  QStringLiteral("microsoft"),   QStringLiteral("microsoftaccount"),
    QStringLiteral("aad"),  QStringLiteral("azure"),       QStringLiteral("azuredirectory"),
    QStringLiteral("live"), QStringLiteral("liveconnect"), QStringLiteral("liveid"),
};
const QString entra::EntraAuthentication::DefaultAuthority = QStringLiteral("https://login.microsoftonline.com/common");

} // namespace gcm
