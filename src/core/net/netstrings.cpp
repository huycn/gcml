// Definitions of the string constants of core/auth/oauth2.h and core/auth/authentication.h (see GCM_STR in
// core/constants.h).
#define GCM_DEFINE_STRINGS_OAUTH2
#include "core/auth/oauth2.h"
#include "core/auth/authentication.h"

namespace gcm {

const QStringList BasicAuthentication::AuthorityIds = {QStringLiteral("basic")};
const QStringList WindowsIntegratedAuthentication::AuthorityIds = {
    QStringLiteral("integrated"), QStringLiteral("windows"), QStringLiteral("kerberos"),
    QStringLiteral("ntlm"),       QStringLiteral("tfs"),     QStringLiteral("sso"),
};

} // namespace gcm
