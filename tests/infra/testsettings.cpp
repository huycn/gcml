#include "infra/testsettings.h"

#include "core/strings.h"

namespace gcm::test {

bool TestSettings::tryGetSetting(const OptString &envarName, const OptString &section, const OptString &property,
                                 QString &value)
{
    value.clear();
    if (envarName && Environment && Environment->Variables.tryGetValue(*envarName, value)) {
        return true;
    }

    const QString sec = section.value_or(QString());
    const QString prop = property.value_or(QString());
    if (RemoteUri && GitConfiguration) {
        for (const QStringList scopes = getGitConfigurationScopes(*RemoteUri); const QString &scope : scopes) {
            const QString key = QStringLiteral("%1.%2.%3").arg(sec, scope, prop);
            if (GitConfiguration->tryGet(key, false, value)) {
                return true;
            }
        }
    }

    if (GitConfiguration && GitConfiguration->tryGet(QStringLiteral("%1.%2").arg(sec, prop), false, value)) {
        return true;
    }

    value.clear();
    return false;
}

QStringList TestSettings::getSettingValues(const OptString &envarName, const OptString &section,
                                           const OptString &property, bool)
{
    QStringList result;
    QString envarValue;
    if (envarName && Environment && Environment->Variables.tryGetValue(*envarName, envarValue)) {
        result.append(envarValue);
    }

    if (!GitConfiguration) {
        return result;
    }

    const QString sec = section.value_or(QString());
    const QString prop = property.value_or(QString());
    if (RemoteUri) {
        for (const QStringList scopes = getGitConfigurationScopes(*RemoteUri); const QString &scope : scopes) {
            result.append(GitConfiguration->getAll(QStringLiteral("%1.%2.%3").arg(sec, scope, prop)));
        }
    }
    result.append(GitConfiguration->getAll(QStringLiteral("%1.%2").arg(sec, prop)));
    return result;
}

} // namespace gcm::test
