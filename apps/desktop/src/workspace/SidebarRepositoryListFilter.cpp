#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetTokens.h"
#include "workspace/SidebarFilterText.h"
#include "workspace/SidebarRepositoryList.h"
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
namespace ketplus {
void SidebarRepositoryList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (filterEmpty_ && event->size().width() != event->oldSize().width())
        applyPresentation();
}
void SidebarRepositoryList::setFilterText(const QString& text) {
    if (filterText_ == text)
        return;
    filterText_ = text;
    applyPresentation();
}
// Filtering changes visibility only. The full model, pins, selection and saved folds stay intact,
// and both local and selected-machine projections take this same path on every refresh.
void SidebarRepositoryList::applyFilter() {
    const auto& appearance = ket::Appearance::instance();
    auto palette = filterEmpty_->palette();
    palette.setColor(QPalette::WindowText, ket::Tokens::color(u"text-muted", appearance.theme()));
    filterEmpty_->setPalette(palette);
    filterEmpty_->setMargin(qRound(appearance.scaled(8)));
    auto font = filterEmpty_->font();
    font.setPixelSize(qRound(appearance.scaled(12)));
    filterEmpty_->setFont(font);
    const QString query = filterText_.trimmed();
    filterEmpty_->hide();
    if (query.isEmpty())
        return;
    bool any = false;
    for (qsizetype i = 0; i < groups_.size(); ++i) {
        const auto& repository = repositories_.at(i);
        auto* group = groups_.at(i);
        const bool repositoryMatch = repository.name.contains(query, Qt::CaseInsensitive) ||
                                     repository.path.contains(query, Qt::CaseInsensitive);
        auto* held = group->findChild<ket::SidebarTreeGuide*>(
            QStringLiteral("productRepositoryHeld"), Qt::FindDirectChildrenOnly);
        int conversations = 0;
        for (auto* row : held->findChildren<ket::SidebarRow*>(
                 QStringLiteral("productConversationButton"), Qt::FindDirectChildrenOnly)) {
            const bool match = repositoryMatch || row->text().contains(query, Qt::CaseInsensitive);
            row->setVisible(match);
            conversations += match;
        }
        if (auto* section = held->findChild<ket::SidebarSectionLabel*>(
                QStringLiteral("productConversationsSection"), Qt::FindDirectChildrenOnly)) {
            section->setCount(conversations);
            section->setVisible(conversations > 0);
        }
        bool hasContent = conversations > 0;
        for (auto* folder : held->findChildren<QWidget*>(QStringLiteral("productTeamFolder"),
                                                         Qt::FindDirectChildrenOnly)) {
            int members = 0;
            for (auto* row : folder->findChildren<ket::SidebarRow*>()) {
                const bool match =
                    repositoryMatch || row->text().contains(query, Qt::CaseInsensitive);
                row->setVisible(match);
                members += match;
            }
            const bool match = repositoryMatch || members > 0;
            folder->setVisible(match);
            hasContent = hasContent || match;
            auto* branch = folder->findChild<ket::SidebarTeamBranch*>();
            branch->setExpanded(true);
            // The guide is the container of the team's conversation rows.
            auto* guide = folder->findChild<ket::SidebarTreeGuide*>();
            guide->setVisible(members > 0);
        }
        if (auto* more =
                held->findChild<QPushButton*>(QStringLiteral("productShowMoreConversations")))
            more->hide();
        const bool match = repositoryMatch || hasContent;
        group->setVisible(match);
        headers_.at(i)->setExpanded(hasContent);
        held->setVisible(hasContent);
        any = any || match;
    }
    filterEmpty_->setVisible(!any);
}
} // namespace ketplus
