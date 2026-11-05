#include "app/AgentConversationWidth.h"

#include "agent/AgentWorkspaceView.h"

#include <QCoreApplication>
#include <QSettings>

namespace ketplus {
namespace {

const auto settingsKey = QStringLiteral("agent/conversationWidth");
// Wide was 960 px before it was narrowed to Claude's wide reading column.
constexpr int legacyWidePixels = 960;
constexpr int widePixels = 864;

} // namespace

QList<AgentConversationWidth::Choice> AgentConversationWidth::choices() {
    return {{AgentWorkspaceView::defaultConversationWidth,
             QCoreApplication::translate("AgentConversationWidth", "Default")},
            {widePixels, QCoreApplication::translate("AgentConversationWidth", "Wide")},
            {1200, QCoreApplication::translate("AgentConversationWidth", "Extra wide")},
            {0, QCoreApplication::translate("AgentConversationWidth", "Full width")}};
}

int AgentConversationWidth::load() {
    bool valid = false;
    int pixels = QSettings().value(settingsKey).toInt(&valid);
    if (valid && pixels == legacyWidePixels)
        pixels = widePixels;
    for (const Choice& choice : choices()) {
        if (valid && choice.pixels == pixels)
            return pixels;
    }
    return AgentWorkspaceView::defaultConversationWidth;
}

void AgentConversationWidth::save(const int pixels) {
    if (pixels == AgentWorkspaceView::defaultConversationWidth)
        QSettings().remove(settingsKey);
    else
        QSettings().setValue(settingsKey, pixels);
    emit notifier() -> changed(load());
}

AgentConversationWidth* AgentConversationWidth::notifier() {
    static auto* instance = new AgentConversationWidth(QCoreApplication::instance());
    return instance;
}

} // namespace ketplus
