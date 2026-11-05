#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace ketplus {

// How wide the AI workspace lets its conversation column grow. A desktop
// preference rather than an appearance setting: Atlas embeds the same
// transcript and keeps its own layout.
class AgentConversationWidth final : public QObject {
    Q_OBJECT

  public:
    struct Choice final {
        int pixels;
        QString label;
    };

    // Unscaled pixels; 0 lets the conversation fill the pane.
    [[nodiscard]] static int load();
    [[nodiscard]] static QList<Choice> choices();
    static void save(int pixels);
    // Emits whenever a new width is saved, so a running workspace follows it.
    [[nodiscard]] static AgentConversationWidth* notifier();

  signals:
    void changed(int pixels);

  private:
    using QObject::QObject;
};

} // namespace ketplus
