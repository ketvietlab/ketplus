#include "workspace/SearchPanel.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

// Draws the row from the markup the panel built, so one row can carry several colors.
class ResultDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

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
        painter->setClipRect(text);

        painter->save();
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
    : QWidget(parent), queryEdit_(new QLineEdit(this)), replaceEdit_(new QLineEdit(this)),
      includeEdit_(new QLineEdit(this)),
      matchCaseCheck_(new QCheckBox(QStringLiteral("Match case"), this)),
      wholeWordCheck_(new QCheckBox(QStringLiteral("Whole word"), this)),
      regexCheck_(new QCheckBox(QStringLiteral("Regex"), this)),
      searchButton_(new QPushButton(QStringLiteral("Search"), this)),
      replaceAllButton_(new QPushButton(QStringLiteral("Replace All"), this)),
      statusLabel_(new QLabel(this)), results_(new QTreeWidget(this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* title = new QLabel(QStringLiteral("Search"), this);
    title->setProperty("kvRole", QStringLiteral("fieldLabel"));
    auto* closeButton = new QToolButton(this);
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setAutoRaise(true);
    closeButton->setToolTip(QStringLiteral("Hide search"));
    closeButton->setAccessibleName(QStringLiteral("Hide search"));
    auto* header = new QHBoxLayout;
    header->addWidget(title, 1);
    header->addWidget(closeButton);

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
    replaceAllButton_->setEnabled(false);

    statusLabel_->setProperty("kvRole", QStringLiteral("searchStatus"));
    statusLabel_->setWordWrap(true);
    results_->setHeaderHidden(true);
    results_->setColumnCount(1);
    results_->setUniformRowHeights(true);
    results_->setTextElideMode(Qt::ElideRight);
    results_->setAccessibleName(QStringLiteral("Search results"));
    results_->setItemDelegate(new ResultDelegate(results_));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(queryEdit_);
    layout->addWidget(replaceEdit_);
    layout->addWidget(includeEdit_);
    layout->addLayout(optionsRow);
    layout->addLayout(buttonsRow);
    layout->addWidget(statusLabel_);
    layout->addWidget(results_, 1);

    connect(queryEdit_, &QLineEdit::returnPressed, this, &SearchPanel::searchRequested);
    connect(includeEdit_, &QLineEdit::returnPressed, this, &SearchPanel::searchRequested);
    connect(searchButton_, &QPushButton::clicked, this, [this] {
        if (searching_) {
            emit cancelRequested();
        } else {
            emit searchRequested();
        }
    });
    connect(replaceAllButton_, &QPushButton::clicked, this, &SearchPanel::replaceAllRequested);
    connect(closeButton, &QToolButton::clicked, this, &SearchPanel::hideRequested);
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
    replaceAllButton_->setEnabled(false);
}

void SearchPanel::addFileResult(const WorkspaceSearchFileResult& result) {
    auto* fileItem = new QTreeWidgetItem(results_);
    const qsizetype separator = result.relativePath.lastIndexOf(QLatin1Char('/'));
    const QString folder =
        separator < 0 ? QString() : result.relativePath.left(separator + 1);
    const QString fileName = result.relativePath.mid(separator + 1);
    fileItem->setText(0, QStringLiteral("%1  (%2)")
                             .arg(result.relativePath)
                             .arg(result.matches.size()));
    fileItem->setData(0, markupRole,
                      colored(folder, colors_.folder) +
                          colored(fileName, colors_.fileName, true) +
                          colored(QStringLiteral("  ·  %1").arg(result.matches.size()),
                                  colors_.lineNumber));
    fileItem->setToolTip(0, result.path);
    fileItem->setData(0, pathRole, result.path);
    for (const auto& match : result.matches) {
        auto* matchItem = new QTreeWidgetItem(fileItem);
        matchItem->setText(0, QStringLiteral("%1: %2").arg(match.line).arg(match.preview.trimmed()));
        matchItem->setData(0, markupRole, matchMarkup(match));
        matchItem->setToolTip(0, match.preview);
        matchItem->setData(0, pathRole, result.path);
        matchItem->setData(0, lineRole, match.line);
        matchItem->setData(0, columnRole, match.column);
        matchItem->setData(0, lengthRole, match.length);
    }
    fileItem->setExpanded(true);
    matchCount_ += static_cast<int>(result.matches.size());
    replaceAllButton_->setEnabled(!searching_);
}

void SearchPanel::setSearching(const bool searching) {
    searching_ = searching;
    searchButton_->setText(searching ? QStringLiteral("Cancel") : QStringLiteral("Search"));
    replaceAllButton_->setEnabled(!searching && matchCount_ > 0);
}

void SearchPanel::setStatusText(const QString& text) { statusLabel_->setText(text); }

void SearchPanel::setResultColors(const QString& folder, const QString& fileName,
                                  const QString& lineNumber, const QString& text,
                                  const QString& match) {
    colors_ = {folder, fileName, lineNumber, text, match};
}

QString SearchPanel::matchMarkup(const WorkspaceSearchMatch& match) const {
    // The preview is the whole line; leading indentation is dropped for the row.
    const QString line = match.preview;
    qsizetype indent = 0;
    while (indent < line.size() && line.at(indent).isSpace()) {
        ++indent;
    }
    const qsizetype start =
        qMax(indent, qMin(static_cast<qsizetype>(match.column), line.size()));
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
    emit matchActivated(item->data(0, pathRole).toString(), line,
                        item->data(0, columnRole).toInt(), item->data(0, lengthRole).toInt());
}

} // namespace ketplus
