// Port of TestInfrastructure/Objects/TestConsoleService.cs.
// Upstream drives Spectre.Console prompts through a TestConsole; here the answers are queued directly.
#pragma once

#include "core/console.h"

namespace gcm::test {

class TestConsoleService : public IConsoleService
{
public:
    /// Every message written with writeInfo/Warning/Error/Fatal/Line, without prefix.
    QStringList WrittenMessages;
    /// Labels / titles of the prompts shown, in order.
    QStringList Prompts;
    /// Choices offered by the most recent selection prompt.
    QStringList LastChoices;

    /// Queue the answer for the next text (or secret) prompt.
    void pushText(const QString &text) { m_texts.append(text); }
    /// Queue the zero-based choice for the next selection prompt.
    void pushSelection(int index) { m_selections.append(index); }

    void writeInfo(const QString &message) override { WrittenMessages.append(message); }
    void writeWarning(const QString &message) override { WrittenMessages.append(message); }
    void writeError(const QString &message) override { WrittenMessages.append(message); }
    void writeFatal(const QString &message) override { WrittenMessages.append(message); }
    void writeLine(const QString &message) override { WrittenMessages.append(message); }

    QString promptText(const QString &label) override
    {
        Prompts.append(label);
        if (m_texts.isEmpty()) {
            throw InvalidOperationException(QStringLiteral("No input queued for prompt '%1'.").arg(label));
        }
        return m_texts.takeFirst();
    }
    QString promptSecret(const QString &label, std::optional<QChar> = std::nullopt) override { return promptText(label); }
    int promptSelection(const QString &title, const QStringList &choices) override
    {
        Prompts.append(title);
        LastChoices = choices;
        if (m_selections.isEmpty()) {
            throw InvalidOperationException(QStringLiteral("No selection queued for prompt '%1'.").arg(title));
        }
        const int index = m_selections.takeFirst();
        if (index < 0 || index >= choices.size()) {
            throw ArgumentOutOfRangeException(QStringLiteral("Queued selection %1 is out of range.").arg(index));
        }
        return index;
    }

private:
    QStringList m_texts;
    QList<int> m_selections;
};

} // namespace gcm::test
