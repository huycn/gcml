// Port of Authentication/AuthenticationBase.cs.
//
// UI model of this port: the main executable never links a GUI toolkit. Every graphical prompt is shown by the
// separate `git-credential-manager-ui` helper (QtWidgets), invoked as a child process that speaks upstream's
// helper protocol: options on the command line, an optional payload on stdin, a `key=value` dictionary terminated
// by a blank line on stdout. On failure the helper exits non-zero with `error=<message>`; when the user dismissed
// the dialog it additionally writes `cancelled=1`, which invokeHelper turns into OperationCanceledException.
#pragma once

#include "core/commandcontext.h"

namespace gcm {

class AuthenticationBase
{
public:
    explicit AuthenticationBase(ICommandContext &context) : m_context(context) {}
    virtual ~AuthenticationBase() = default;

protected:
    /// Runs the helper and returns its result dictionary (keys case-insensitive).
    /// Throws Exception("helper error (<code>): <error>") on failure, OperationCanceledException on user cancel.
    /// When `ct` is cancelled the helper is killed and OperationCanceledException is thrown.
    virtual StringDictionary invokeHelper(const QString &path, const QString &args, const OptString &standardInput = std::nullopt, const CancellationToken &ct = {});

    /// Throws InvalidOperationException("Cannot prompt because user interactivity has been disabled.")
    void throwIfUserInteractionDisabled();
    void throwIfGuiPromptsDisabled();
    /// Throws InvalidOperationException("Cannot prompt because terminal prompts have been disabled.")
    void throwIfTerminalPromptsDisabled();

    /// True when a graphical prompt should be used: GUI prompts enabled, desktop session, and the UI helper found.
    /// On success `command` is the helper executable and `args` a (possibly empty) argument prefix ending in a
    /// space when non-empty.
    bool tryFindUiHelper(QString &command, QString &args);

    /// Resolution order: GCM_UI_HELPER / credential.uiHelper (an explicit empty value disables the UI helper),
    /// else `defaultValue`. A rooted path must exist; a bare name is looked up in the installation directory and
    /// then on PATH. Missing helpers produce a warning and return false.
    bool tryFindHelperCommand(const QString &envar, const QString &configName, const QString &defaultValue, QString &command, QString &args);

    ICommandContext &m_context;
};

} // namespace gcm
