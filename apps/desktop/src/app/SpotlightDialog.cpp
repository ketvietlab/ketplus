#include "app/SpotlightDialog.h"
#include "ui/ChromeGeometry.h"

#include "ui/TypeScale.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace ketplus {
namespace {
enum SpotlightRole {
    EntryIdRole = Qt::UserRole,
    TitleRole,
    SubtitleRole,
    CategoryRole,
    GroupStartRole,
    KindRole,
};

// Spotlight finds files and Atlases only; repositories are switched from the sidebar.
QString displayCategory(const QString& category) {
    const auto upper = category.toUpper();
    if (upper == QStringLiteral("ATLASES"))
        return QStringLiteral("ATLAS");
    return upper;
}

QIcon iconForKind(const QString& kind) {
    if (kind == QStringLiteral("atlas"))
        return QIcon(QStringLiteral(":/agent/icons/network.svg"));
    return QIcon(QStringLiteral(":/agent/icons/file-code.svg"));
}

class SpotlightDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const auto group = enabled ? QPalette::Active : QPalette::Disabled;
        const bool groupStart = index.data(GroupStartRole).toBool();
        const int headingHeight = groupStart ? 26 : 0;
        const QRect rowRect = option.rect.adjusted(0, headingHeight + 1, 0, -1);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (groupStart) {
            QFont headingFont = option.font;
            headingFont.setPixelSize(11);
            headingFont.setWeight(QFont::DemiBold);
            headingFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
            painter->setFont(headingFont);
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            painter->drawText(option.rect.adjusted(12, 6, -12, 0), Qt::AlignTop,
                              index.data(CategoryRole).toString());
        }
        if (selected && enabled) {
            painter->setBrush(option.palette.color(QPalette::Highlight));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(rowRect, 5, 5);
        }

        auto icon = iconForKind(index.data(KindRole).toString()).pixmap(16, 16);
        if (!icon.isNull()) {
            QPainter tint(&icon);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(icon.rect(),
                          option.palette.color(
                              enabled ? (selected ? QPalette::Highlight : QPalette::PlaceholderText)
                                      : QPalette::Mid));
            tint.end();
            painter->drawPixmap(rowRect.left() + 12, rowRect.center().y() - 8, icon);
        }

        const QRect textRect = rowRect.adjusted(50, 7, -42, -7);
        QFont titleFont = option.font;
        titleFont.setPixelSize(14);
        titleFont.setWeight(QFont::Medium);
        painter->setFont(titleFont);
        painter->setPen(option.palette.color(group, QPalette::Text));
        painter->drawText(textRect.adjusted(0, 0, 0, -20), Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(index.data(TitleRole).toString(),
                                                             Qt::ElideRight, textRect.width()));
        QFont pathFont = option.font;
        pathFont.setPixelSize(11);
        painter->setFont(pathFont);
        painter->setPen(option.palette.color(group, QPalette::PlaceholderText));
        painter->drawText(textRect.adjusted(0, 20, 0, 0), Qt::AlignVCenter,
                          QFontMetrics(pathFont).elidedText(index.data(SubtitleRole).toString(),
                                                            Qt::ElideMiddle, textRect.width()));
        QFont affordanceFont = option.font;
        affordanceFont.setPixelSize(13);
        painter->setFont(affordanceFont);
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(rowRect.adjusted(0, 0, -13, 0), Qt::AlignRight | Qt::AlignVCenter,
                          selected ? QStringLiteral("↵") : QStringLiteral("↗"));
        painter->restore();
    }
};
} // namespace

SpotlightDialog::SpotlightDialog(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint), query_(new QLineEdit(this)),
      results_(new QListWidget(this)),
      empty_(new QLabel(tr("No matching files or Atlases"), this)) {
    setObjectName("spotlightDialog");
    setWindowTitle(tr("KetPlus Spotlight"));
    setFixedWidth(640);
    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 0, 0, 0, 0);
    chrome_geometry::spacing(layout, 0);

    auto* searchHeader = new QWidget(this);
    searchHeader->setObjectName(QStringLiteral("spotlightSearchHeader"));
    chrome_geometry::fixedHeight(searchHeader, 54);
    auto* searchLayout = new QHBoxLayout(searchHeader);
    chrome_geometry::margins(searchLayout, 18, 0, 18, 0);
    chrome_geometry::spacing(searchLayout, 11);
    auto* searchGlyph = new QLabel(searchHeader);
    searchGlyph->setObjectName(QStringLiteral("spotlightSearchGlyph"));
    auto searchIcon = QIcon(":/agent/icons/search.svg").pixmap(20, 20);
    if (!searchIcon.isNull()) {
        QPainter painter(&searchIcon);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(searchIcon.rect(), palette().color(QPalette::Highlight));
        painter.end();
    }
    searchGlyph->setPixmap(searchIcon);
    chrome_geometry::fixedSize(searchGlyph, 20, 20);
    query_->setObjectName("spotlightQuery");
    query_->setPlaceholderText(tr("Search files and Atlases…"));
    query_->setAccessibleName(tr("Spotlight search"));
    query_->setClearButtonEnabled(false);
    chrome_geometry::minimumHeight(query_, 40);
    auto* escape = new QLabel(tr("esc"), searchHeader);
    escape->setObjectName(QStringLiteral("spotlightEscape"));
    escape->setProperty("kvRole", QStringLiteral("spotlightFooter"));
    searchLayout->addWidget(searchGlyph);
    searchLayout->addWidget(query_, 1);
    searchLayout->addWidget(escape);
    query_->installEventFilter(this);
    results_->setObjectName("spotlightResults");
    results_->setItemDelegate(new SpotlightDelegate(results_));
    results_->setFrameShape(QFrame::NoFrame);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setTextElideMode(Qt::ElideMiddle);
    results_->setFocusPolicy(Qt::NoFocus);
    chrome_geometry::iconSize(results_, QSize(16, 16));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setProperty("kvRole", QStringLiteral("spotlightEmpty"));
    layout->addWidget(searchHeader);

    auto* hint = new QWidget(this);
    hint->setObjectName(QStringLiteral("spotlightHint"));
    chrome_geometry::fixedHeight(hint, 34);
    auto* hintLayout = new QHBoxLayout(hint);
    chrome_geometry::margins(hintLayout, 20, 0, 20, 0);
    auto* scope = new QLabel(tr("FILES & ATLASES"), hint);
    scope->setObjectName(QStringLiteral("spotlightAnywhere"));
    scope->setProperty("kvRole", QStringLiteral("spotlightHint"));
    hintLayout->addWidget(scope);
    hintLayout->addStretch();
    layout->addWidget(hint);
    layout->addWidget(results_, 1);
    layout->addWidget(empty_, 1);

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("spotlightFooter"));
    chrome_geometry::fixedHeight(footer, 40);
    auto* footerLayout = new QHBoxLayout(footer);
    chrome_geometry::margins(footerLayout, 19, 0, 19, 0);
    chrome_geometry::spacing(footerLayout, 18);
    auto* navigate = new QLabel(tr("\u2191 \u2193  navigate"), footer);
    auto* open = new QLabel(tr("\u21b5  open"), footer);
    auto* workspace = new QLabel(tr("Search this workspace"), footer);
    for (auto* label : {navigate, open, workspace})
        label->setProperty("kvRole", QStringLiteral("spotlightFooter"));
    footerLayout->addWidget(navigate);
    footerLayout->addWidget(open);
    footerLayout->addStretch();
    footerLayout->addWidget(workspace);
    layout->addWidget(footer);

    const auto colors = palette();
    const int basePixels = QApplication::font().pixelSize() > 0
                               ? QApplication::font().pixelSize()
                               : InterfaceZoomMetrics::mediumPixels;
    const QString sheet = TypeScale::apply(QStringLiteral(R"(
        QDialog#spotlightDialog { border: 1px solid %1; border-radius: 8px; background: %2; }
        QWidget#spotlightSearchHeader { border-bottom: 1px solid %3; }
        QLineEdit#spotlightQuery { padding: 0; border: 0; background: transparent; color: %4; font-size: {{reading}}px; }
        QWidget#spotlightHint { border-bottom: 0; }
        QLabel[kvRole="spotlightHint"] { color: %5; font-size: {{micro}}px; }
        QListWidget#spotlightResults { padding: 0 10px 12px 10px; border: 0; background: transparent; outline: 0; }
        QListWidget#spotlightResults::item { border: 0; background: transparent; }
        QListWidget#spotlightResults::item:selected { background: transparent; color: %4; }
        QLabel[kvRole="spotlightEmpty"] { color: %5; font-size: {{reading}}px; }
        QWidget#spotlightFooter { border-top: 1px solid %3; }
        QLabel[kvRole="spotlightFooter"] { color: %5; font-size: {{micro}}px; }
    )"),
                                           basePixels);
    setStyleSheet(sheet.arg(colors.color(QPalette::Mid).name(), colors.color(QPalette::Base).name(),
                            colors.color(QPalette::AlternateBase).name(),
                            colors.color(QPalette::Text).name(),
                            colors.color(QPalette::PlaceholderText).name()));
    chrome_geometry::apply(this, basePixels);
    connect(query_, &QLineEdit::textChanged, this, &SpotlightDialog::filter);
    connect(query_, &QLineEdit::returnPressed, this, &SpotlightDialog::activateCurrent);
    connect(results_, &QListWidget::itemClicked, this, [this] { activateCurrent(); });
    setFocusProxy(query_);
}

void SpotlightDialog::setEntries(const QVector<SpotlightEntry>& entries) {
    entries_ = entries;
    filter();
    query_->setFocus();
}

void SpotlightDialog::setAvailableHeight(const int height) {
    availableHeight_ = qBound(220, height, 680);
    filter();
}

void SpotlightDialog::setQuery(const QString& query) {
    query_->setText(query);
    query_->setFocus();
}

void SpotlightDialog::filter() {
    const auto terms = query_->text().simplified().split(' ', Qt::SkipEmptyParts);
    QVector<QPair<int, SpotlightEntry>> matches;
    for (const auto& entry : entries_) {
        const QString text =
            entry.title + " " + entry.subtitle + " " + entry.category + " " + entry.searchText;
        int score = 0;
        bool match = true;
        for (const auto& term : terms) {
            if (!text.contains(term, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
            if (entry.title.startsWith(term, Qt::CaseInsensitive))
                score += 2;
            else if (entry.title.contains(term, Qt::CaseInsensitive))
                ++score;
        }
        if (match)
            matches.append({score, entry});
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    results_->clear();
    QString previousCategory;
    for (const auto& match : matches) {
        if (results_->count() >= 200)
            break;
        const auto& entry = match.second;
        const auto category = displayCategory(entry.category);
        const bool groupStart = category != previousCategory;
        previousCategory = category;
        auto* item = new QListWidgetItem(results_);
        item->setData(EntryIdRole, entry.id);
        item->setData(TitleRole, entry.title + (entry.available ? "" : tr(" (Unavailable)")));
        item->setData(SubtitleRole, entry.subtitle);
        item->setData(CategoryRole, category);
        item->setData(GroupStartRole, groupStart);
        item->setData(KindRole, entry.kind);
        item->setToolTip(entry.subtitle);
        item->setSizeHint(QSize(0, groupStart ? 80 : 54));
        if (!entry.available)
            item->setFlags(Qt::NoItemFlags);
    }
    results_->setVisible(!matches.isEmpty());
    empty_->setVisible(matches.isEmpty());
    for (int row = 0; row < results_->count(); ++row) {
        if (results_->item(row)->flags().testFlag(Qt::ItemIsEnabled)) {
            results_->setCurrentRow(row);
            break;
        }
    }
    int resultHeight = 0;
    for (int row = 0; row < results_->count(); ++row)
        resultHeight += results_->item(row)->sizeHint().height();
    const int emptyHeight = matches.isEmpty() ? 230 : 0;
    setFixedHeight(
        qMin(availableHeight_, qMax(220, 54 + 34 + 40 + 2 + qMax(resultHeight, emptyHeight))));
}

void SpotlightDialog::activateCurrent() {
    auto* item = results_->currentItem();
    if (!item || !item->flags().testFlag(Qt::ItemIsEnabled))
        return;
    const auto id = item->data(Qt::UserRole).toString();
    accept();
    emit entryActivated(id);
}

bool SpotlightDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == query_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space &&
            key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            reject();
            return true;
        }
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int step = key->key() == Qt::Key_Down ? 1 : -1;
            for (int row = results_->currentRow() + step; row >= 0 && row < results_->count();
                 row += step) {
                if (!results_->item(row)->flags().testFlag(Qt::ItemIsEnabled))
                    continue;
                results_->setCurrentRow(row);
                break;
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace ketplus
