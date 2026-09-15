#include "workspace/SearchPanel.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr int pathRole = Qt::UserRole + 1;
constexpr int lineRole = Qt::UserRole + 2;
constexpr int columnRole = Qt::UserRole + 3;
constexpr int lengthRole = Qt::UserRole + 4;

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
    fileItem->setText(0, QStringLiteral("%1  (%2)")
                             .arg(result.relativePath)
                             .arg(result.matches.size()));
    fileItem->setToolTip(0, result.path);
    fileItem->setData(0, pathRole, result.path);
    for (const auto& match : result.matches) {
        auto* matchItem = new QTreeWidgetItem(fileItem);
        matchItem->setText(0, QStringLiteral("%1: %2").arg(match.line).arg(match.preview.trimmed()));
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
