// Definitions of the string constants of github.h (see GCM_STR in core/constants.h).
#define GCM_DEFINE_STRINGS_GITHUB
#include "github/github.h"

namespace gcm::github {

const QStringList GitHubConstants::InvalidUnderscoreLogins = {QStringLiteral("pj_nitin"), QStringLiteral("up_the_irons")};
const QStringList GitHubAuthentication::AuthorityIds = {QStringLiteral("github")};

} // namespace gcm::github
