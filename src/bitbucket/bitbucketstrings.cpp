// Definitions of the string constants of bitbucket.h (see GCM_STR in core/constants.h).
#define GCM_DEFINE_STRINGS_BITBUCKET
#include "bitbucket/bitbucket.h"

namespace gcm::bitbucket {

// After BitbucketConstants::Id, which is defined above by the include (same file, so initialised first)
const QStringList BitbucketAuthentication::AuthorityIds = {BitbucketConstants::Id};

} // namespace gcm::bitbucket
