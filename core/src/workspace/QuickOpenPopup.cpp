#include "workspace/QuickOpenPopup.h"
#include "KetCommandPalette.h"
#include "KetIcons.h"
#include "workspace/FuzzyMatcher.h"
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <algorithm>

namespace ketplus {
QuickOpenPopup::QuickOpenPopup(QWidget* parent)
    : QFrame(parent), palette_(new ket::CommandPalette(parent)) {
    setAttribute(Qt::WA_NoSystemBackground);
    palette_->setParent(this);
    queryEdit_ = palette_->searchField();
    queryEdit_->setAccessibleName(tr("Quick open query"));
    queryEdit_->installEventFilter(this);
    parent->installEventFilter(this);
    connect(palette_, &ket::CommandPalette::queryChanged, this, &QuickOpenPopup::refilter);
    connect(palette_, &ket::CommandPalette::closed, this, [this] {
        hide();
        emit dismissed();
    });
    connect(palette_, &ket::CommandPalette::activated, this, [this](const ket::CommandItem& item) {
        bool valid = false;
        const qsizetype index = item.id.toLongLong(&valid);
        if (valid && index >= 0 && index < items_.size())
            emit itemActivated(items_.at(index).data);
    });
    hide();
}
void QuickOpenPopup::open(const QString& placeholder, const QList<QuickOpenItem>& items) {
    items_ = items;
    statusText_.clear();
    palette_->setPlaceholderText(placeholder);
    reposition();
    show();
    raise();
    palette_->open();
    refilter();
}
void QuickOpenPopup::setItems(const QList<QuickOpenItem>& items) {
    items_ = items;
    refilter();
}
void QuickOpenPopup::setStatusText(const QString& text) {
    statusText_ = text;
    refilter();
}
QString QuickOpenPopup::query() const { return queryEdit_->text(); }
QList<QuickOpenItem> QuickOpenPopup::visibleItems() const {
    QList<QuickOpenItem> result;
    for (const auto index : visibleIndexes_)
        result.append(items_.at(index));
    return result;
}
bool QuickOpenPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        reposition();
    if (watched == queryEdit_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_PageDown || key == Qt::Key_PageUp || key == Qt::Key_Down ||
            key == Qt::Key_Up) {
            moveCurrentRow(key == Qt::Key_PageDown ? maximumVisibleRows
                           : key == Qt::Key_PageUp ? -maximumVisibleRows
                           : key == Qt::Key_Down   ? 1
                                                   : -1);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
void QuickOpenPopup::refilter() {
    const QString query = queryEdit_->text().trimmed();
    struct RankedItem final {
        int score;
        qsizetype index;
    };
    QList<RankedItem> ranked;
    ranked.reserve(items_.size());
    for (qsizetype index = 0; index < items_.size(); ++index) {
        int score = fuzzyMatchScore(query, items_.at(index).label);
        if (score < 0) {
            const int detailScore = fuzzyMatchScore(query, items_.at(index).detail);
            score = detailScore < 0 ? -1 : detailScore / 2;
        }
        if (score >= 0) {
            ranked.append({score, index});
        }
    }
    if (!query.isEmpty()) {
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const RankedItem& left, const RankedItem& right) {
                             return left.score > right.score;
                         });
    }

    visibleIndexes_.clear();
    ket::CommandGroup group;
    group.title = statusText_;
    const qsizetype count = qMin<qsizetype>(ranked.size(), maximumVisibleItems);
    for (qsizetype row = 0; row < count; ++row) {
        const auto index = ranked.at(row).index;
        const auto& item = items_.at(index);
        group.items.append(
            {QString::number(index), item.label, item.detail, ket::Icons::codeForLucide(u"file")});
        visibleIndexes_.append(index);
    }
    palette_->setEmptyText(statusText_.isEmpty() ? tr("No results") : statusText_,
                           tr("Try another name or command."));
    palette_->setGroups({group});
    palette_->setHighlightedIndex(0);
}
void QuickOpenPopup::moveCurrentRow(int offset) {
    if (!visibleIndexes_.isEmpty())
        palette_->setHighlightedIndex(
            qBound(0, palette_->highlightedIndex() + offset, int(visibleIndexes_.size()) - 1));
}
void QuickOpenPopup::reposition() { setGeometry(parentWidget()->rect()); }
} // namespace ketplus
