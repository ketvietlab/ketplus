#include "ui/FindReplaceBar.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace ketplus {

FindReplaceBar::FindReplaceBar(QWidget* parent)
    : QWidget(parent), findEdit_(new QLineEdit(this)), replaceEdit_(new QLineEdit(this)),
      matchCaseCheck_(new QCheckBox(QStringLiteral("Match case"), this)),
      wholeWordCheck_(new QCheckBox(QStringLiteral("Whole word"), this)),
      resultLabel_(new QLabel(this)), replaceRow_(new QWidget(this)) {
    setProperty("kvRole", QStringLiteral("findBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    findEdit_->setPlaceholderText(QStringLiteral("Find"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->setAccessibleName(QStringLiteral("Find text"));
    replaceEdit_->setPlaceholderText(QStringLiteral("Replace with"));
    replaceEdit_->setClearButtonEnabled(true);
    replaceEdit_->setAccessibleName(QStringLiteral("Replacement text"));
    resultLabel_->setProperty("kvRole", QStringLiteral("searchStatus"));
    resultLabel_->setMinimumWidth(76);

    auto* previousButton = new QPushButton(QStringLiteral("Previous"), this);
    auto* nextButton = new QPushButton(QStringLiteral("Next"), this);
    auto* closeButton = new QPushButton(QString::fromUtf8("×"), this);
    closeButton->setProperty("kvRole", QStringLiteral("findClose"));
    closeButton->setToolTip(QStringLiteral("Close search"));
    closeButton->setAccessibleName(QStringLiteral("Close search"));

    auto* findLabel = new QLabel(QStringLiteral("Find"), this);
    findLabel->setProperty("kvRole", QStringLiteral("fieldLabel"));

    auto* findRow = new QHBoxLayout;
    findRow->setContentsMargins(10, 8, 8, 4);
    findRow->setSpacing(7);
    findRow->addWidget(findLabel);
    findRow->addWidget(findEdit_, 1);
    findRow->addWidget(previousButton);
    findRow->addWidget(nextButton);
    findRow->addWidget(matchCaseCheck_);
    findRow->addWidget(wholeWordCheck_);
    findRow->addWidget(resultLabel_);
    findRow->addWidget(closeButton);

    auto* replaceLabel = new QLabel(QStringLiteral("Replace"), replaceRow_);
    replaceLabel->setProperty("kvRole", QStringLiteral("fieldLabel"));
    auto* replaceButton = new QPushButton(QStringLiteral("Replace"), replaceRow_);
    auto* replaceAllButton = new QPushButton(QStringLiteral("Replace All"), replaceRow_);
    auto* replaceLayout = new QHBoxLayout(replaceRow_);
    replaceLayout->setContentsMargins(10, 4, 8, 8);
    replaceLayout->setSpacing(7);
    replaceLayout->addWidget(replaceLabel);
    replaceLayout->addWidget(replaceEdit_, 1);
    replaceLayout->addWidget(replaceButton);
    replaceLayout->addWidget(replaceAllButton);
    replaceLayout->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(findRow);
    layout->addWidget(replaceRow_);

    connect(previousButton, &QPushButton::clicked, this, [this] { emit findRequested(true); });
    connect(nextButton, &QPushButton::clicked, this, [this] { emit findRequested(false); });
    connect(replaceButton, &QPushButton::clicked, this, &FindReplaceBar::replaceRequested);
    connect(replaceAllButton, &QPushButton::clicked, this, &FindReplaceBar::replaceAllRequested);
    connect(closeButton, &QPushButton::clicked, this, &FindReplaceBar::dismiss);
    connect(findEdit_, &QLineEdit::textChanged, this, [this] { resultLabel_->clear(); });

    findEdit_->installEventFilter(this);
    replaceEdit_->installEventFilter(this);
    hide();
}

QString FindReplaceBar::query() const { return findEdit_->text(); }

QString FindReplaceBar::replacement() const { return replaceEdit_->text(); }

bool FindReplaceBar::matchCase() const { return matchCaseCheck_->isChecked(); }

bool FindReplaceBar::wholeWord() const { return wholeWordCheck_->isChecked(); }

void FindReplaceBar::open(const bool showReplace, const QString& selectedText) {
    replaceRow_->setVisible(showReplace);
    setFixedHeight(showReplace ? 92 : 52);
    if (!selectedText.isEmpty() && selectedText.size() <= 256 &&
        !selectedText.contains(QLatin1Char('\n')) && !selectedText.contains(QLatin1Char('\r'))) {
        findEdit_->setText(selectedText);
    }
    resultLabel_->clear();
    show();
    updateGeometry();
    if (parentWidget() != nullptr && parentWidget()->layout() != nullptr) {
        parentWidget()->layout()->invalidate();
        parentWidget()->layout()->activate();
    }
    raise();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void FindReplaceBar::showSearchResult(const bool found, const bool wrapped) {
    if (!found) {
        resultLabel_->setText(QStringLiteral("No results"));
        resultLabel_->setProperty("searchState", QStringLiteral("empty"));
    } else if (wrapped) {
        resultLabel_->setText(QStringLiteral("Wrapped"));
        resultLabel_->setProperty("searchState", QStringLiteral("wrapped"));
    } else {
        resultLabel_->clear();
        resultLabel_->setProperty("searchState", QString());
    }
    resultLabel_->style()->unpolish(resultLabel_);
    resultLabel_->style()->polish(resultLabel_);
}

void FindReplaceBar::showReplacementCount(const int count) {
    resultLabel_->setText(count == 1 ? QStringLiteral("1 replaced")
                                     : QStringLiteral("%1 replaced").arg(count));
    resultLabel_->setProperty("searchState", count == 0 ? QStringLiteral("empty") : QString());
    resultLabel_->style()->unpolish(resultLabel_);
    resultLabel_->style()->polish(resultLabel_);
}

bool FindReplaceBar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (watched == replaceEdit_) {
                emit replaceRequested();
            } else {
                emit findRequested(keyEvent->modifiers().testFlag(Qt::ShiftModifier));
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindReplaceBar::dismiss() {
    hide();
    emit closeRequested();
}

} // namespace ketplus
