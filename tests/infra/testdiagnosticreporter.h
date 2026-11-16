// Port of TestInfrastructure/Objects/TestDiagnosticReporter.cs
#pragma once

#include "core/application.h"

namespace gcm::test {

class TestDiagnosticReporter : public IDiagnosticReporter
{
public:
    struct Error
    {
        QString message;
        /// what() of the reported exception, if any (the exception object itself does not outlive the call).
        OptString exception;
    };

    QStringList Progress;
    QStringList Info;
    QStringList Warnings;
    QList<Error> Errors;
    QStringList Files;

    void reportProgress(const QString &message) override { Progress.append(message); }
    void reportInfo(const QString &message) override { Info.append(message); }
    void reportWarning(const QString &message) override { Warnings.append(message); }
    void reportError(const QString &message, const std::exception *exception = nullptr) override
    {
        Errors.append({message, exception ? OptString(QString::fromUtf8(exception->what())) : std::nullopt});
    }
    void addFile(const QString &path) override { Files.append(path); }
};

} // namespace gcm::test
