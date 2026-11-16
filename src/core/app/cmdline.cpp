// The sub-command command-line parser standing in for System.CommandLine (core/cmdline.h).
#include "core/cmdline.h"

#include "core/strings.h"

namespace gcm {

std::shared_ptr<Command> Command::findCommand(const QString &name) const
{
    for (const auto &command : m_commands) {
        if (command->name() == name) {
            return command;
        }
    }
    return nullptr;
}

CommandLineParser::CommandLineParser(std::shared_ptr<Command> root, QString executableName, QString version, TextWriter &out, TextWriter &err)
    : m_root(std::move(root)), m_executableName(std::move(executableName)), m_version(std::move(version)), m_out(out),
      m_err(err)
{
}

namespace {

// Function-local statics rather than globals: built on first use instead of at load time.
const Option &helpOption()
{
    static const Option option {
        { QStringLiteral("-?"), QStringLiteral("-h"), QStringLiteral("--help") },
        QStringLiteral("Show help and usage information"),
    };
    return option;
}

const Option &versionOption()
{
    static const Option option {
        { QStringLiteral("--version") },
        QStringLiteral("Show version information"),
    };
    return option;
}

bool isHelpToken(const QString &token)
{
    return token == QLatin1String("-?") ||
           token == QLatin1String("-h") ||
           token == QLatin1String("--help") ||
           token == QLatin1String("/?") ||
           token == QLatin1String("/h");
}

/// The options a command accepts: its own, then the global options of itself and its ancestors.
QList<Option> availableOptions(const QList<const Command *> &path)
{
    QList<Option> options = path.last()->options();
    for (const Command *c : path) {
        options.append(c->globalOptions());
    }
    return options;
}

const Option *findOption(const QList<Option> &options, const QString &alias)
{
    for (const Option &o : options) {
        if (o.aliases.contains(alias)) {
            return &o;
        }
    }
    return nullptr;
}

QString optionLabel(const Option &option)
{
    // Short aliases first, like System.CommandLine: "-o, --output <output>"
    QStringList aliases = option.aliases;
    std::stable_sort(aliases.begin(), aliases.end(), [](const QString &a, const QString &b) {
        return !a.startsWith(QLatin1String("--")) && b.startsWith(QLatin1String("--"));
    });
    QString label = aliases.join(QStringLiteral(", "));
    if (option.takesValue) {
        QString valueName = option.valueName;
        if (valueName.isEmpty()) {
            valueName = option.aliases.first();
            for (const QString &alias : option.aliases) {
                if (alias.size() > valueName.size()) {
                    valueName = alias;
                }
            }
            while (valueName.startsWith(QLatin1Char('-'))) {
                valueName.remove(0, 1);
            }
        }
        label += QStringLiteral(" <%1>").arg(valueName);
    }
    if (option.required) {
        label += QStringLiteral(" (REQUIRED)");
    }
    return label;
}

void appendTable(QString &out, const QList<std::pair<QString, QString>> &rows)
{
    int width = 0;
    for (const auto &[left, right] : rows) {
        width = std::max(width, int(left.size()));
    }
    for (const auto &[left, right] : rows) {
        out += QStringLiteral("  ") + left;
        if (!right.isEmpty()) {
            out += QString(width - left.size() + 2, QLatin1Char(' ')) + right;
        }
        out += QLatin1Char('\n');
    }
}

} // namespace

QString CommandLineParser::helpText(const QList<const Command *> &commandPath) const
{
    const Command *command = commandPath.last();
    QString text;

    // System.CommandLine prints nothing for a hidden command and always prints the description section.
    if (command->isHidden()) {
        return text;
    }
    text += command->description().isEmpty() ? QStringLiteral("Description:\n\n")
                                             : QStringLiteral("Description:\n  %1\n\n").arg(command->description());

    // Usage
    QStringList usage{m_executableName};
    for (qsizetype i = 1; i < commandPath.size(); ++i) {
        usage.append(commandPath[i]->name());
    }
    for (const Argument &argument : command->arguments()) {
        usage.append(argument.required ? QStringLiteral("<%1>").arg(argument.name) : QStringLiteral("[<%1>]").arg(argument.name));
    }

    const bool hasVisibleCommands = std::any_of(command->commands().begin(), command->commands().end(), [](const auto &c) { return !c->isHidden(); });
    if (hasVisibleCommands) {
        usage.append(QStringLiteral("[command]"));
    }
    usage.append(QStringLiteral("[options]"));
    text += QStringLiteral("Usage:\n  %1\n\n").arg(usage.join(QLatin1Char(' ')));

    if (!command->arguments().isEmpty()) {
        QList<std::pair<QString, QString>> rows;
        for (const Argument &argument : command->arguments()) {
            rows.append({QStringLiteral("<%1>").arg(argument.name), argument.description});
        }
        text += QStringLiteral("Arguments:\n");
        appendTable(text, rows);
        text += QLatin1Char('\n');
    }

    QList<std::pair<QString, QString>> optionRows;
    const QList<Option> options = availableOptions(commandPath);
    for (const Option &option : options) {
        optionRows.append({optionLabel(option), option.description});
    }
    if (commandPath.size() == 1) {
        optionRows.append({optionLabel(versionOption()), versionOption().description});
    }
    optionRows.append({optionLabel(helpOption()), helpOption().description});
    text += QStringLiteral("Options:\n");
    appendTable(text, optionRows);

    if (hasVisibleCommands) {
        QList<std::pair<QString, QString>> rows;
        for (const auto &c : command->commands()) {
            if (c->isHidden()) {
                continue;
            }
            QString label = c->name();
            for (const Argument &argument : c->arguments()) {
                label += argument.required ? QStringLiteral(" <%1>").arg(argument.name) : QStringLiteral(" [<%1>]").arg(argument.name);
            }
            rows.append({label, c->description()});
        }
        text += QStringLiteral("\nCommands:\n");
        appendTable(text, rows);
    } else {
        // (System.CommandLine's empty sub-commands section)
        text += QLatin1Char('\n');
    }
    return text + QLatin1Char('\n');
}

int CommandLineParser::invoke(const QStringList &args)
{
    QList<const Command *> path{m_root.get()};
    ParseResult result;
    QStringList errors;
    bool helpRequested = false;
    bool versionRequested = false;
    qsizetype nextArgument = 0;
    bool onlyPositionals = false;

    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString &token = args[i];
        const Command *current = path.last();

        if (!onlyPositionals && token == QLatin1String("--")) {
            onlyPositionals = true;
            continue;
        }

        if (!onlyPositionals && isHelpToken(token)) {
            helpRequested = true;
            continue;
        }
        if (!onlyPositionals && token == QLatin1String("--version") && path.size() == 1) {
            versionRequested = true;
            continue;
        }

        if (!onlyPositionals && token.startsWith(QLatin1Char('-')) && token.size() > 1) {
            QString alias = token;
            OptString inlineValue;
            const qsizetype eq = token.indexOf(QLatin1Char('='));
            if (eq > 0) {
                alias = token.left(eq);
                inlineValue = token.mid(eq + 1);
            }
            const QList<Option> options = availableOptions(path);
            const Option *option = findOption(options, alias);
            if (!option) {
                errors.append(QStringLiteral("Unrecognized command or argument '%1'.").arg(token));
                continue;
            }
            const QString canonical = option->aliases.first();
            if (!option->takesValue) {
                if (inlineValue && !isTruthy(inlineValue) && !isFalsey(inlineValue)) {
                    errors.append(QStringLiteral("Cannot parse argument '%1' for option '%2' as expected type 'System.Boolean'.")
                                      .arg(*inlineValue, alias));
                    continue;
                }
                if (!inlineValue || isTruthy(inlineValue)) {
                    result.m_options[canonical];
                }
                continue;
            }
            QString value;
            if (inlineValue) {
                value = *inlineValue;
            } else if (i + 1 < args.size() && !(args[i + 1].startsWith(QLatin1Char('-')) && args[i + 1].size() > 1)) {
                value = args[++i];
            } else {
                errors.append(QStringLiteral("Required argument missing for option: '%1'.").arg(alias));
                continue;
            }
            QStringList &values = result.m_options[canonical];
            if (option->allowMultiple) {
                values.append(value);
            } else {
                values = {value};
            }
            continue;
        }

        // A sub-command (only before any positional argument of the current command)
        if (!onlyPositionals && nextArgument == 0) {
            if (const std::shared_ptr<Command> sub = current->findCommand(token)) {
                path.append(sub.get());
                continue;
            }
        }

        // A positional argument
        if (nextArgument < current->arguments().size()) {
            result.m_arguments.set(current->arguments()[nextArgument++].name, token);
            result.m_positionals.append(token);
            continue;
        }
        errors.append(QStringLiteral("Unrecognized command or argument '%1'.").arg(token));
    }

    const Command *command = path.last();
    result.command = command;

    // --version wins over parse errors, as in System.CommandLine
    if (versionRequested) {
        m_out.writeLine(m_version);
        return 0;
    }
    if (helpRequested) {
        m_out.write(helpText(path));
        return 0;
    }

    if (errors.isEmpty()) {
        for (qsizetype a = nextArgument; a < command->arguments().size(); ++a) {
            if (command->arguments()[a].required) {
                errors.append(QStringLiteral("Required argument missing for command: '%1'.").arg(command->name()));
            }
        }
        const QList<Option> options = availableOptions(path);
        for (const Option &option : options) {
            if (option.required && !result.has(option.aliases.first())) {
                errors.append(QStringLiteral("Option '%1' is required.").arg(option.aliases.first()));
            }
        }
    }
    if (!command->handler()) {
        errors.prepend(QStringLiteral("Required command was not provided."));
    }

    if (!errors.isEmpty()) {
        for (const QString &error : std::as_const(errors)) {
            m_err.writeLine(error);
        }
        m_err.writeLine();
        m_out.write(helpText(path));
        return 1;
    }

    try {
        for (const auto &middleware : std::as_const(m_middleware)) {
            middleware(result);
        }
        return command->handler()(result);
    } catch (const std::exception &ex) {
        if (!m_exceptionHandler) {
            throw;
        }
        return m_exceptionHandler(ex);
    }
}

} // namespace gcm
