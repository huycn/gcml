// A small sub-command command-line framework standing in for System.CommandLine as used upstream:
// nested commands, `--long`/`-s` options (with or without a value, `--opt value` and `--opt=value`),
// positional arguments, global options, hidden commands, generated `--help`/`-h`/`-?` and `--version`.
#pragma once

#include "core/common.h"
#include "core/streams.h"

namespace gcm {

struct Option
{
    /// e.g. {"--url"} or {"--no-ui"} or {"--output", "-o"}; the first alias is the canonical name.
    QStringList aliases;
    QString description;
    /// False for boolean flags.
    bool takesValue = false;
    bool required = false;
    bool allowMultiple = false;
    /// Shown in help as `--url <valueName>`; empty: the option's name without dashes, as System.CommandLine.
    QString valueName = QString();

    static Option flag(const QString &name, const QString &description) { return Option{{name}, description, false}; }
    static Option value(const QString &name, const QString &description, bool required = false)
    {
        return Option{{name}, description, true, required};
    }
};

struct Argument
{
    QString name;
    QString description;
    bool required = true;
};

class Command;

class ParseResult
{
public:
    /// The command that was selected.
    const Command *command = nullptr;

    /// True when the flag/option was present (by any alias; lookup by canonical name).
    bool has(const QString &option) const { return m_options.contains(option); }

    OptString value(const QString &option) const
    {
        const QStringList v = m_options.value(option);
        return v.isEmpty() ? OptString() : OptString(v.last());
    }

    QStringList values(const QString &option) const { return m_options.value(option); }
    /// Positional argument by declared name.
    OptString argument(const QString &name) const { return m_arguments.tryGet(name); }
    const QStringList &positionals() const { return m_positionals; }

    // filled by the parser
    Dictionary<QStringList> m_options;
    Dictionary<QString> m_arguments;
    QStringList m_positionals;
};

class Command
{
public:
    /// Returns the process exit code. Exceptions propagate to CommandLineParser's exception handler.
    using Handler = std::function<int(const ParseResult &)>;

    Command(QString name, QString description) : m_name(std::move(name)), m_description(std::move(description)) {}
    virtual ~Command() = default;

    const QString &name() const { return m_name; }
    const QString &description() const { return m_description; }
    bool isHidden() const { return m_hidden; }
    void setHidden(bool hidden) { m_hidden = hidden; }

    void addOption(const Option &option) { m_options.append(option); }
    /// Global options are also accepted by every sub-command.
    void addGlobalOption(const Option &option) { m_globalOptions.append(option); }
    void addArgument(const Argument &argument) { m_arguments.append(argument); }
    void addCommand(std::shared_ptr<Command> command) { m_commands.append(std::move(command)); }
    void setHandler(Handler handler) { m_handler = std::move(handler); }

    const QList<Option> &options() const { return m_options; }
    const QList<Option> &globalOptions() const { return m_globalOptions; }
    const QList<Argument> &arguments() const { return m_arguments; }
    const QList<std::shared_ptr<Command>> &commands() const { return m_commands; }
    const Handler &handler() const { return m_handler; }
    std::shared_ptr<Command> findCommand(const QString &name) const;

private:
    QString m_name;
    QString m_description;
    bool m_hidden = false;
    QList<Option> m_options;
    QList<Option> m_globalOptions;
    QList<Argument> m_arguments;
    QList<std::shared_ptr<Command>> m_commands;
    Handler m_handler;
};

class CommandLineParser
{
public:
    /// `out`/`err` receive help and parse errors.
    CommandLineParser(std::shared_ptr<Command> root, QString executableName, QString version, TextWriter &out, TextWriter &err);

    /// Called with the parse result right before the handler runs (middleware, e.g. `--no-ui`, TRACE2 cmd_name).
    void addMiddleware(std::function<void(const ParseResult &)> middleware) { m_middleware.append(std::move(middleware)); }
    /// Maps an exception thrown by a handler to an exit code. Without a handler exceptions propagate.
    void setExceptionHandler(std::function<int(const std::exception &)> handler) { m_exceptionHandler = std::move(handler); }

    /// Parses and runs. Help => 0; parse error (unknown command/option, missing value/argument) => message + help, 1.
    /// A command without a handler prints its help and returns 1 ("Required command was not provided.").
    int invoke(const QStringList &args);

    /// Renders the help text of `command` (usage, description, arguments, options, visible sub-commands).
    QString helpText(const QList<const Command *> &commandPath) const;

private:
    std::shared_ptr<Command> m_root;
    QString m_executableName;
    QString m_version;
    TextWriter &m_out;
    TextWriter &m_err;
    QList<std::function<void(const ParseResult &)>> m_middleware;
    std::function<int(const std::exception &)> m_exceptionHandler;
};

} // namespace gcm
