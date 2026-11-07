#include "workspace/SearchPanel.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetChrome.h"
#include "KetControlRecipes.h"
#include "KetIcons.h"
#include "KetSurfaceRecipes.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "ui/Theme.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>

#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextOption>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr int pathRole = Qt::UserRole + 1;
constexpr int lineRole = Qt::UserRole + 2;
constexpr int columnRole = Qt::UserRole + 3;
constexpr int lengthRole = Qt::UserRole + 4;
constexpr int markupRole = Qt::UserRole + 5;
constexpr int relativePathRole = Qt::UserRole + 6;

// Draws the row from the markup the panel built, so one row can carry several colors.
class ResultDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        auto& a = ket::Appearance::instance();
        size.setHeight(qRound(a.scaled(index.parent().isValid()
                                           ? ket::fileRowRecipe(false, {}, a.theme()).height
                                           : ket::groupHeaderRecipe(false, {}, a.theme()).height)));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const QString markup = index.data(markupRole).toString();
        if (markup.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem view(option);
        initStyleOption(&view, index);
        view.text.clear();
        auto& a = ket::Appearance::instance();
        view.font.setPixelSize(qRound(a.scaled(
            index.parent().isValid() ? ket::fileRowRecipe(false, {}, a.theme()).fontPixels
                                     : ket::groupHeaderRecipe(false, {}, a.theme()).fontPixels)));
        const QWidget* widget = view.widget;
        QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &view, painter, widget);

        QTextDocument document;
        document.setDefaultFont(view.font);
        document.setDocumentMargin(0);
        QTextOption layout;
        // Rows keep one line and are clipped, so every result is the same height.
        layout.setWrapMode(QTextOption::NoWrap);
        document.setDefaultTextOption(layout);
        document.setHtml(markup);
        const QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &view, widget);
        document.setTextWidth(text.width());

        painter->save();
        // Clipping inside the saved state, so the next row is not clipped to this one.
        painter->setClipRect(text);
        painter->translate(text.topLeft() +
                           QPoint(0, (text.height() - document.size().height()) / 2));
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = view.palette;
        document.documentLayout()->draw(painter, context);
        painter->restore();
    }
};

QString escaped(const QString& text) { return text.toHtmlEscaped(); }

QString colored(const QString& text, const QString& color, const bool bold = false) {
    if (text.isEmpty()) {
        return {};
    }
    return QStringLiteral("<span style=\"color:%1;%2\">%3</span>")
        .arg(color, bold ? QStringLiteral("font-weight:600;") : QString(), escaped(text));
}

} // namespace

SearchPanel::SearchPanel(QWidget* parent)
    : QWidget(parent), queryEdit_(new ket::TextField(this)), replaceEdit_(new ket::TextField(this)),
      includeEdit_(new ket::TextField(this)),
      matchCaseCheck_(new ket::Checkbox(QStringLiteral("Match case"), this)),
      wholeWordCheck_(new ket::Checkbox(QStringLiteral("Whole word"), this)),
      regexCheck_(new ket::Checkbox(QStringLiteral("Regex"), this)),
      searchButton_(new ket::Button(QStringLiteral("Search"), ket::ButtonVariant::Secondary, this)),
      replaceAllButton_(
          new ket::Button(QStringLiteral("Replace All"), ket::ButtonVariant::Secondary, this)),
      statusLabel_(new QLabel(this)), results_(new QTreeWidget(this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* header = new ket::PaneHeader(tr("Search"), ket::Icons::codeForLucide(u"search"), this);
    auto* closeButton = new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"x")),
                                            tr("Hide search"), header);
    header->actions()->addWidget(closeButton);

    queryEdit_->setPlaceholderText(QStringLiteral("Search in files"));
    queryEdit_->setClearButtonEnabled(true);
    queryEdit_->setAccessibleName(QStringLiteral("Search in files"));
    replaceEdit_->setPlaceholderText(QStringLiteral("Replace with"));
    replaceEdit_->setClearButtonEnabled(true);
    replaceEdit_->setAccessibleName(QStringLiteral("Replace in files with"));
    includeEdit_->setPlaceholderText(QStringLiteral("Files to include, e.g. *.cpp, src/**"));
    includeEdit_->setClearButtonEnabled(true);
    includeEdit_->setAccessibleName(QStringLiteral("Files to include"));

    auto* optionsRow = new QHBoxLayout;
    optionsRow->addWidget(matchCaseCheck_);
    optionsRow->addWidget(wholeWordCheck_);
    optionsRow->addWidget(regexCheck_);
    optionsRow->addStretch();

    auto* buttonsRow = new QHBoxLayout;
    buttonsRow->addWidget(searchButton_);
    buttonsRow->addWidget(replaceAllButton_);
    buttonsRow->addStretch();
    replaceAllButton_->setObjectName(QStringLiteral("searchReplaceAllButton"));
    replaceAllButton_->setEnabled(false);

    statusLabel_->setProperty("kvRole", QStringLiteral("searchStatus"));
    statusLabel_->setWordWrap(true);
    results_->setHeaderHidden(true);
    results_->setColumnCount(1);
    results_->setUniformRowHeights(false);
    results_->setTextElideMode(Qt::ElideRight);
    results_->setAccessibleName(QStringLiteral("Search results"));
    results_->setItemDelegate(new ResultDelegate(results_));
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, results_,
            [this] { results_->doItemsLayout(); });
    const auto palette = currentThemePalette();
    setResultColors(palette.textSecondary, palette.textMain, palette.textMuted, palette.textMain,
                    palette.accentText);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    layout->addWidget(header);
    layout->addWidget(queryEdit_);
    layout->addWidget(replaceEdit_);
    layout->addWidget(includeEdit_);
    layout->addLayout(optionsRow);
    layout->addLayout(buttonsRow);
    layout->addWidget(statusLabel_);
    layout->addWidget(results_, 1);

    connect(queryEdit_, &QLineEdit::returnPressed, this, &SearchPanel::searchRequested);
    connect(includeEdit_, &QLineEdit::returnPressed, this, &SearchPanel::searchRequested);
    connect(searchButton_, &QAbstractButton::clicked, this, [this] {
        if (searching_) {
            emit cancelRequested();
        } else {
            emit searchRequested();
        }
    });
    connect(replaceAllButton_, &QAbstractButton::clicked, this, &SearchPanel::replaceAllRequested);
    connect(closeButton, &QAbstractButton::clicked, this, &SearchPanel::hideRequested);
    connect(results_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item) { activateItem(item); });
    connect(results_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item) { activateItem(item); });
}

WorkspaceSearchOptions SearchPanel::options() const {
    return {.query = queryEdit_->text(),
            .matchCase = matchCaseCheck_->isChecked(),
            .wholeWord = wholeWordCheck_->isChecked(),
            .regex = regexCheck_->isChecked(),
            .includePatterns = includeEdit_->text()};
}

void SearchPanel::setOptions(const WorkspaceSearchOptions& options) {
    const QSignalBlocker queryBlocker(queryEdit_);
    const QSignalBlocker matchCaseBlocker(matchCaseCheck_);
    const QSignalBlocker wholeWordBlocker(wholeWordCheck_);
    const QSignalBlocker regexBlocker(regexCheck_);
    const QSignalBlocker includeBlocker(includeEdit_);
    queryEdit_->setText(options.query);
    matchCaseCheck_->setChecked(options.matchCase);
    wholeWordCheck_->setChecked(options.wholeWord);
    regexCheck_->setChecked(options.regex);
    includeEdit_->setText(options.includePatterns);
}

QString SearchPanel::replacement() const { return replaceEdit_->text(); }

QStringList SearchPanel::resultPaths() const {
    QStringList paths;
    for (int index = 0; index < results_->topLevelItemCount(); ++index) {
        paths.append(results_->topLevelItem(index)->data(0, pathRole).toString());
    }
    return paths;
}

int SearchPanel::resultMatchCount() const noexcept { return matchCount_; }

bool SearchPanel::resultsTruncated() const noexcept { return truncated_; }

bool SearchPanel::isSearching() const noexcept { return searching_; }

void SearchPanel::focusQuery(const QString& seed) {
    if (!seed.isEmpty() && seed.size() <= 256 && !seed.contains(QLatin1Char('\n'))) {
        queryEdit_->setText(seed);
    }
    queryEdit_->setFocus();
    queryEdit_->selectAll();
}

void SearchPanel::clearResults() {
    results_->clear();
    matchCount_ = 0;
    truncated_ = false;
    replaceAllButton_->setEnabled(false);
}

void SearchPanel::addFileResult(const WorkspaceSearchFileResult& result) {
    auto* fileItem = new QTreeWidgetItem(results_);
    const qsizetype separator = result.relativePath.lastIndexOf(QLatin1Char('/'));
    const QString folder = separator < 0 ? QString() : result.relativePath.left(separator + 1);
    const QString fileName = result.relativePath.mid(separator + 1);
    fileItem->setText(
        0, QStringLiteral("%1  (%2)").arg(result.relativePath).arg(result.matches.size()));
    fileItem->setData(
        0, markupRole,
        colored(folder, colors_.folder) + colored(fileName, colors_.fileName, true) +
            colored(QStringLiteral("  ·  %1").arg(result.matches.size()), colors_.lineNumber));
    fileItem->setToolTip(0, result.path);
    fileItem->setData(0, pathRole, result.path);
    fileItem->setData(0, relativePathRole, result.relativePath);
    for (const auto& match : result.matches) {
        auto* matchItem = new QTreeWidgetItem(fileItem);
        matchItem->setText(0,
                           QStringLiteral("%1: %2").arg(match.line).arg(match.preview.trimmed()));
        matchItem->setData(0, markupRole, matchMarkup(match));
        matchItem->setToolTip(0, match.preview);
        matchItem->setData(0, pathRole, result.path);
        matchItem->setData(0, lineRole, match.line);
        matchItem->setData(0, columnRole, match.column);
        matchItem->setData(0, lengthRole, match.length);
    }
    fileItem->setExpanded(true);
    matchCount_ += static_cast<int>(result.matches.size());
    replaceAllButton_->setEnabled(!searching_ && !truncated_);
}

void SearchPanel::setSearching(const bool searching) {
    searching_ = searching;
    searchButton_->setText(searching ? QStringLiteral("Cancel") : QStringLiteral("Search"));
    replaceAllButton_->setEnabled(!searching && matchCount_ > 0 && !truncated_);
}

void SearchPanel::setResultsTruncated(const bool truncated) {
    truncated_ = truncated;
    replaceAllButton_->setEnabled(!searching_ && matchCount_ > 0 && !truncated_);
}

void SearchPanel::setStatusText(const QString& text) { statusLabel_->setText(text); }

void SearchPanel::setResultColors(const QString& folder, const QString& fileName,
                                  const QString& lineNumber, const QString& text,
                                  const QString& match) {
    colors_ = {folder, fileName, lineNumber, text, match};
    for (int group = 0; group < results_->topLevelItemCount(); ++group) {
        auto* item = results_->topLevelItem(group);
        const QString path = item->data(0, relativePathRole).toString();
        const auto separator = path.lastIndexOf('/');
        item->setData(
            0, markupRole,
            colored(path.left(separator + 1), colors_.folder) +
                colored(path.mid(separator + 1), colors_.fileName, true) +
                colored(QStringLiteral("  ·  %1").arg(item->childCount()), colors_.lineNumber));
        for (int row = 0; row < item->childCount(); ++row) {
            auto* child = item->child(row);
            WorkspaceSearchMatch value;
            value.line = child->data(0, lineRole).toInt();
            value.column = child->data(0, columnRole).toInt();
            value.length = child->data(0, lengthRole).toInt();
            value.preview = child->toolTip(0);
            child->setData(0, markupRole, matchMarkup(value));
        }
    }
}

QString SearchPanel::matchMarkup(const WorkspaceSearchMatch& match) const {
    // The preview is the whole line; leading indentation is dropped for the row.
    const QString line = match.preview;
    qsizetype indent = 0;
    while (indent < line.size() && line.at(indent).isSpace()) {
        ++indent;
    }
    const qsizetype start = qMax(indent, qMin(static_cast<qsizetype>(match.column), line.size()));
    const qsizetype length =
        qMax<qsizetype>(0, qMin(static_cast<qsizetype>(match.length), line.size() - start));

    return colored(QStringLiteral("%1  ").arg(match.line), colors_.lineNumber) +
           colored(line.mid(indent, start - indent), colors_.text) +
           colored(line.mid(start, length), colors_.match, true) +
           colored(line.mid(start + length), colors_.text);
}

void SearchPanel::activateItem(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const int line = item->data(0, lineRole).toInt();
    if (line <= 0) {
        return;
    }
    emit matchActivated(item->data(0, pathRole).toString(), line, item->data(0, columnRole).toInt(),
                        item->data(0, lengthRole).toInt());
}

} // namespace ketplus
