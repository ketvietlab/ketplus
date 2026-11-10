#include "app/SettingsWorkspacePage.h"

#include "KetSelect.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentConversationWidth.h"
#include "app/SettingsBrowserPage.h"
#include "app/SettingsComputerPage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsUpdatesPage.h"
#include "ui/InterfaceFont.h"
#include "update/AppUpdater.h"

#include <QListWidget>
#include <QSignalBlocker>
#include <QStackedWidget>

namespace ketplus {
namespace {

// Holds the chosen conversation width until Save, so the public dialog's Save
// and Reset cover it like every other field. It is registered with the dialog but
// never shown there: the segmented buttons on the Typography page edit it.
class ConversationWidthState final : public SettingsPage {
  public:
    ConversationWidthState()
        : SettingsPage(QStringLiteral("agent-conversation-width"),
                       QStringLiteral("Conversation width")),
          saved_(AgentConversationWidth::load()), pixels_(saved_) {}

    [[nodiscard]] int pixels() const noexcept { return pixels_; }
    void select(const int pixels) {
        if (pixels_ == pixels)
            return;
        pixels_ = pixels;
        emit changed();
    }
    [[nodiscard]] bool hasChanges() const override { return pixels_ != saved_; }
    void resetToDefaults() override { select(AgentWorkspaceView::defaultConversationWidth); }
    void apply() override {
        AgentConversationWidth::save(pixels_);
        saved_ = pixels_;
    }

  private:
    int saved_;
    int pixels_;
};

} // namespace

SettingsWorkspacePage::SettingsWorkspacePage(SettingsDialog* dialog,
                                             SettingsProvidersPage::Fetcher providers,
                                             QWidget* parent,
                                             SettingsClaudeCredentials::Requester credentials,
                                             SettingsStoragePage::Requester storageRequester)
    : SettingsWorkspace(dialog, QStringLiteral("⌘ ,"), InterfaceUiFont::settingsLabel(), parent) {
    addConversationWidth();
    addLink(tr("Account & devices"), QStringLiteral("account-devices"));
    addLink(tr("Agent models"), QStringLiteral("agent-models"));
    addSection(tr("Browser"), QStringLiteral("browser"),
               new SettingsBrowserPage({}, {}, browser_tools::systemProbes(), this));
    addSection(tr("Computer use"), QStringLiteral("computer"),
               new SettingsComputerPage({}, {}, this));
    addSection(tr("Providers"), QStringLiteral("providers"),
               new SettingsProvidersPage(std::move(providers), this, std::move(credentials)));
    // Archived conversations are a page of the dialog (DesktopSettingsPages); it moves here.
    auto* publicNavigation = dialog->findChild<QListWidget*>(QStringLiteral("settingsNavigation"));
    auto* publicPages = dialog->findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
    if (publicNavigation != nullptr && publicPages != nullptr) {
        for (int index = 0; index < publicNavigation->count(); ++index) {
            if (publicNavigation->item(index)->data(Qt::UserRole).toString() !=
                QStringLiteral("archived"))
                continue;
            QWidget* archived = publicPages->widget(index);
            publicPages->removeWidget(archived);
            addSection(tr("Archived"), QStringLiteral("archived"), archived);
            break;
        }
    }
    // Storage has its own controls; Restore defaults does not apply to it.
    addSection(tr("Storage"), QStringLiteral("storage"),
               new SettingsStoragePage(std::move(storageRequester), this), false);
    addSection(tr("Updates"), QStringLiteral("updates"),
               new SettingsUpdatesPage(AppUpdater::instance(), this));
    addStyleRules(QStringLiteral(R"QSS(
QLabel#storageUsage, QLabel#storageStatus, QComboBox#storageDetailDays,
QComboBox#computerWorkDisplay { color:%2; font-size:{{reading}}px; }
)QSS"));
    connect(this, &SettingsWorkspace::linkActivated, this, [this](const QString& id) {
        if (id == QStringLiteral("account-devices"))
            emit accountDevicesRequested();
        else if (id == QStringLiteral("agent-models"))
            emit agentModelsRequested();
    });
}

// A select like the mock's, "Default · 760 px", between the interface and editor rows. It
// applies as it is chosen, like the fields around it.
void SettingsWorkspacePage::addConversationWidth() {
    auto* widthState = new ConversationWidthState;
    dialog()->addPage(widthState);
    auto* conversationWidth = new ket::Select(this);
    for (const auto& choice : AgentConversationWidth::choices())
        conversationWidth->addItem(choice.pixels > 0
                                       ? tr("%1 · %2 px").arg(choice.label).arg(choice.pixels)
                                       : choice.label,
                                   choice.pixels);
    conversationWidth->setObjectName(QStringLiteral("agentConversationWidth"));
    conversationWidth->setAccessibleName(tr("Conversation width"));
    connect(conversationWidth, &QComboBox::currentIndexChanged, widthState,
            [widthState](int index) {
                const auto choices = AgentConversationWidth::choices();
                if (index >= 0 && index < choices.size())
                    widthState->select(choices[index].pixels);
            });
    const auto syncWidth = [conversationWidth, widthState] {
        const QSignalBlocker blocker(conversationWidth);
        const auto choices = AgentConversationWidth::choices();
        for (qsizetype index = 0; index < choices.size(); ++index)
            if (choices[index].pixels == widthState->pixels())
                conversationWidth->setCurrentIndex(int(index));
    };
    connect(widthState, &SettingsPage::changed, conversationWidth, syncWidth);
    syncWidth();
    addTypographyRow(2, tr("Conversation width"),
                     tr("Applies to Agent Workspace. Atlas keeps its compact reading column."),
                     conversationWidth);
}

} // namespace ketplus
