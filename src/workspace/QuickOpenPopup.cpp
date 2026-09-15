#include "workspace/QuickOpenPopup.h"

#include "workspace/FuzzyMatcher.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {

QuickOpenPopup::QuickOpenPopup(QWidget* parent)
    : QFrame(parent), queryEdit_(new QLineEdit(this)), statusLabel_(new QLabel(this)),
      list_(new QListWidget(this)) {
    setProperty("kvRole", QStringLiteral("quickOpen"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::StyledPanel);

    queryEdit_->setAccessibleName(QStringLiteral("Quick open query"));
    statusLabel_->setProperty("kvRole", QStringLiteral("quickOpenStatus"));
    statusLabel_->hide();
    // Keep keyboard focus in the query field; the list is driven from there.
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setTextElideMode(Qt::ElideMiddle);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(queryEdit_);
    layout->addWidget(statusLabel_);
    layout->addWidget(list_);

    connect(queryEdit_, &QLineEdit::textChanged, this, &QuickOpenPopup::refilter);
    connect(list_, &QListWidget::itemClicked, this, &QuickOpenPopup::activateCurrent);
    queryEdit_->installEventFilter(this);
    parent->installEventFilter(this);
    hide();
}

void QuickOpenPopup::open(const QString& placeholder, const QList<QuickOpenItem>& items) {
    items_ = items;
    queryEdit_->setPlaceholderText(placeholder);
    setStatusText({});
    {
        const QSignalBlocker blocker(queryEdit_);
        queryEdit_->clear();
    }
    refilter();
    show();
    raise();
    queryEdit_->setFocus();
}

void QuickOpenPopup::setItems(const QList<QuickOpenItem>& items) {
    items_ = items;
    refilter();
}

void QuickOpenPopup::setStatusText(const QString& text) {
    statusLabel_->setText(text);
    statusLabel_->setVisible(!text.isEmpty());
    if (isVisible()) {
        reposition();
    }
}

QString QuickOpenPopup::query() const { return queryEdit_->text(); }

QList<QuickOpenItem> QuickOpenPopup::visibleItems() const {
    QList<QuickOpenItem> visible;
    visible.reserve(visibleIndexes_.size());
    for (const qsizetype index : visibleIndexes_) {
        visible.append(items_.at(index));
    }
    return visible;
}

bool QuickOpenPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget()) {
        if (event->type() == QEvent::Resize && isVisible()) {
            reposition();
        }
        return QFrame::eventFilter(watched, event);
    }
    if (watched != queryEdit_) {
        return QFrame::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // Navigation keys belong to the picker, not to window shortcuts.
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter ||
            key == Qt::Key_Up || key == Qt::Key_Down) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::KeyPress:
        switch (static_cast<QKeyEvent*>(event)->key()) {
        case Qt::Key_Escape:
            dismiss();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activateCurrent();
            return true;
        case Qt::Key_Down:
            moveCurrentRow(1);
            return true;
        case Qt::Key_Up:
            moveCurrentRow(-1);
            return true;
        case Qt::Key_PageDown:
            moveCurrentRow(maximumVisibleRows);
            return true;
        case Qt::Key_PageUp:
            moveCurrentRow(-maximumVisibleRows);
            return true;
        default:
            break;
        }
        break;
    case QEvent::FocusOut:
        if (isVisible()) {
            dismiss();
        }
        break;
    default:
        break;
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

    list_->clear();
    visibleIndexes_.clear();
    const qsizetype visibleCount = qMin<qsizetype>(ranked.size(), maximumVisibleItems);
    for (qsizetype row = 0; row < visibleCount; ++row) {
        const auto& item = items_.at(ranked.at(row).index);
        auto* listItem = new QListWidgetItem(
            item.detail.isEmpty() ? item.label
                                  : QStringLiteral("%1    %2").arg(item.label, item.detail),
            list_);
        listItem->setToolTip(item.detail);
        visibleIndexes_.append(ranked.at(row).index);
    }
    if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    reposition();
}

void QuickOpenPopup::activateCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || row >= visibleIndexes_.size()) {
        return;
    }
    const QVariant data = items_.at(visibleIndexes_.at(row)).data;
    hide();
    emit itemActivated(data);
}

void QuickOpenPopup::dismiss() {
    hide();
    emit dismissed();
}

void QuickOpenPopup::moveCurrentRow(const int offset) {
    if (list_->count() == 0) {
        return;
    }
    list_->setCurrentRow(qBound(0, list_->currentRow() + offset, list_->count() - 1));
}

void QuickOpenPopup::reposition() {
    const QWidget* parent = parentWidget();
    const int width = qBound(360, parent->width() / 2, 720);
    const int rows = qMin(list_->count(), maximumVisibleRows);
    const int rowHeight = list_->count() > 0 ? list_->sizeHintForRow(0) : 0;
    list_->setVisible(rows > 0);
    list_->setFixedHeight(rows * rowHeight + 2 * list_->frameWidth() + 4);
    const int height = sizeHint().height();
    setGeometry((parent->width() - width) / 2, 56, width, height);
}

} // namespace ketplus
