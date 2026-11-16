// Port of ConfigurationService.cs
#include "core/commandcontext.h"
#include "core/git.h"

namespace gcm {

ConfigurationService::ConfigurationService(ICommandContext &context) : m_context(context) {}

void ConfigurationService::addComponent(IConfigurableComponent *component) { m_components.append(component); }

void ConfigurationService::configure(ConfigurationTarget target)
{
    for (IConfigurableComponent *component : std::as_const(m_components)) {
        const QString message = QStringLiteral("Configuring component '%1'...").arg(component->componentName());
        m_context.trace().writeLine(message);
        m_context.console().writeLine(message);
        component->configure(target);
    }
}

void ConfigurationService::unconfigure(ConfigurationTarget target)
{
    for (IConfigurableComponent *component : std::as_const(m_components)) {
        const QString message = QStringLiteral("Unconfiguring component '%1'...").arg(component->componentName());
        m_context.trace().writeLine(message);
        m_context.console().writeLine(message);
        component->unconfigure(target);
    }
}

} // namespace gcm
