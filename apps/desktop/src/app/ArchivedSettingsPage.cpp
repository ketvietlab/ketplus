#include "app/ArchivedSettingsPage.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetIcons.h"
#include "KetTokens.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

// The mock's archived row: a panel with a hairline border and 7 px corners.
class ArchivedCard final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = ket::Appearance::instance().theme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(ket::Tokens::color(u"panel-border", theme), 1));
        painter.setBrush(ket::Tokens::color(u"panel-bg", theme));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
    }
};

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    return value;
}

// "@agent/1022 · ketviet · archived 28 Oct", leaving out what is not known.
QString details(const QVariantMap& item) {
    QStringList parts;
    const QString number = item.value(QStringLiteral("number")).toString();
    if (!number.isEmpty())
        parts.append(QStringLiteral("@agent/") + number);
    const QString kind =
        item.value(QStringLiteral("kind"), QStringLiteral("conversation")).toString();
    if (kind == QStringLiteral("worktree"))
        parts.append(ArchivedSettingsPage::tr("Worktree"));
    const QString path = item.value(QStringLiteral("path")).toString();
    if (!path.isEmpty())
        parts.append(QFileInfo(path).fileName());
    const QDateTime archived = item.value(QStringLiteral("archived_at")).toDateTime();
    if (archived.isValid())
        parts.append(
            ArchivedSettingsPage::tr("archived %1")
                .arg(QLocale().toString(archived.toLocalTime().date(), QStringLiteral("d MMM"))));
    if (item.value(QStringLiteral("running")).toBool())
        parts.append(ArchivedSettingsPage::tr("Running"));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

ArchivedSettingsPage::ArchivedSettingsPage(QVariantList conversations, RestoreConversation restore,
                                           StopConversation stop, DeleteConversation remove,
                                           QWidget* parent)
    : SettingsPage(QStringLiteral("archived"), QStringLiteral("Archived"), parent),
      conversations_(std::move(conversations)), restore_(std::move(restore)),
      stop_(std::move(stop)), delete_(std::move(remove)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Archived"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(14);
    layout->addWidget(textLabel(tr("Archived conversations keep their transcript and worktree. "
                                   "Delete removes them from this Mac."),
                                QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(14);
    emptyState_ = new ket::EmptyState(QStringLiteral("archive"), tr("Nothing archived"),
                                      tr("Archive a conversation from its context menu."), this);
    emptyState_->setObjectName(QStringLiteral("archivedEmptyState"));
    layout->addWidget(emptyState_);
    auto* list = new QWidget(this);
    list->setObjectName(QStringLiteral("archivedConversationList"));
    cards_ = new QVBoxLayout(list);
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(14);
    layout->addWidget(list);
    layout->addStretch(1);
    refresh();
}

void ArchivedSettingsPage::setConversations(QVariantList conversations) {
    conversations_ = std::move(conversations);
    refresh();
}

void ArchivedSettingsPage::refresh() {
    while (QLayoutItem* item = cards_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide(); // its button may still be handling the click
            widget->deleteLater();
        }
        delete item;
    }
    for (const QVariant& value : std::as_const(conversations_)) {
        const QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        const QString id = item.value(QStringLiteral("id")).toString();
        auto* card = new ArchivedCard(cards_->parentWidget());
        card->setObjectName(QStringLiteral("archivedItem"));
        card->setProperty("conversationId", id);
        card->setMinimumHeight(64);
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(16, 10, 12, 10);
        row->setSpacing(8);
        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        text->addWidget(textLabel(item.value(QStringLiteral("title")).toString(),
                                  QStringLiteral("archivedItemTitle"), card));
        text->addWidget(textLabel(details(item), QStringLiteral("settingsWorkspaceHelp"), card));
        row->addLayout(text, 1);

        const auto button = [card, row](const QString& label, ket::ButtonVariant variant,
                                        const char* name) {
            auto* result = new ket::Button(label, variant, card);
            result->setControlSize(ket::ControlSize::Small);
            result->setObjectName(QLatin1String(name));
            row->addWidget(result, 0, Qt::AlignVCenter);
            return result;
        };
        if (item.value(QStringLiteral("running")).toBool()) {
            auto* stop =
                button(tr("Stop turn"), ket::ButtonVariant::GhostDanger, "stopArchivedTurnButton");
            connect(stop, &QAbstractButton::clicked, this, [this, path, id] {
                if (!stop_ || !stop_(path, id))
                    return;
                for (QVariant& entry : conversations_) {
                    QVariantMap map = entry.toMap();
                    if (map.value(QStringLiteral("path")).toString() == path &&
                        map.value(QStringLiteral("id")).toString() == id) {
                        map.insert(QStringLiteral("running"), false);
                        entry = map;
                    }
                }
                refresh();
            });
        }
        auto* restore =
            button(tr("Restore"), ket::ButtonVariant::Secondary, "restoreArchivedItemButton");
        restore->setIcon(ket::Icons::icon(u"restore"));
        connect(restore, &QAbstractButton::clicked, this, [this, path, id] {
            if (!restore_ || !restore_(path, id))
                return;
            for (qsizetype index = 0; index < conversations_.size(); ++index) {
                const QVariantMap map = conversations_.at(index).toMap();
                if (map.value(QStringLiteral("path")).toString() == path &&
                    map.value(QStringLiteral("id")).toString() == id) {
                    conversations_.removeAt(index);
                    break;
                }
            }
            refresh();
        });
        // Worktrees have no conversation to delete; their own menu removes them.
        if (delete_ && !id.isEmpty()) {
            auto* remove =
                button(tr("Delete…"), ket::ButtonVariant::GhostDanger, "deleteArchivedItemButton");
            connect(remove, &QAbstractButton::clicked, this,
                    [this, path, id] { delete_(path, id); });
        }
        cards_->addWidget(card);
    }
    const bool empty = conversations_.isEmpty();
    emptyState_->setVisible(empty);
    cards_->parentWidget()->setVisible(!empty);
}

} // namespace ketplus
