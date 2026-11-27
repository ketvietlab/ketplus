// The team page stages a user's written answer locally while it waits for batch submission.
// Keep the cursor and selection when the card has to be rebuilt.
#include "KetButton.h"
#include "KetTextArea.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"

#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ketplus {
namespace {

using team_page::px;

// The daemon's limit for a written answer.
constexpr int kAnswerCharacters = 4000;

} // namespace

QWidget* TeamPage::decisionOther(QWidget* card, const QString& id, const QString& what,
                                 const bool sentences) {
    const QString label = tr("Other…");
    QAbstractButton* other = nullptr;
    if (sentences)
        other = new team_page::ChoiceRow(label, false, card);
    else
        other = new team_page::ChoiceButton(label, ket::ButtonVariant::Secondary, card);
    other->setObjectName(QStringLiteral("teamDecisionOther"));
    other->setAccessibleName(tr("Write another answer"));
    teams_->commands()->bind(other, commandKey(what));
    connect(other, &QAbstractButton::clicked, this, [this, id] {
        decisionDrafts_.insert(id, decisionDrafts_.value(id));
        focusDraft_ = id;
        QMetaObject::invokeMethod(this, &TeamPage::rebuild, Qt::QueuedConnection);
    });
    return other;
}

QWidget* TeamPage::decisionDraft(QWidget* card, const QString& id, const QString& what) {
    const QString key = commandKey(what);
    auto* box = new QWidget(card);
    box->setObjectName(QStringLiteral("teamDecisionOtherBox"));
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(0, px(4), 0, 0);
    column->setSpacing(px(8));
    auto* field = new ket::TextArea(box);
    field->setObjectName(QStringLiteral("teamDecisionOtherText"));
    field->setAccessibleName(tr("Your answer"));
    field->setPlaceholderText(tr("Write your answer…"));
    field->setMinimumLines(2);
    field->setMaximumLines(8);
    field->setMaxLength(kAnswerCharacters);
    // Read before setText, which reports the change and would record the cursor it leaves.
    const DecisionDraft draft = decisionDrafts_.value(id);
    field->setText(draft.text);
    if (draft.position < 0) {
        field->editor()->moveCursor(QTextCursor::End);
    } else {
        const int end = static_cast<int>(field->text().size());
        QTextCursor cursor = field->editor()->textCursor();
        cursor.setPosition(qBound(0, draft.anchor, end));
        cursor.setPosition(qBound(0, draft.position, end), QTextCursor::KeepAnchor);
        field->editor()->setTextCursor(cursor);
    }
    // Where the user is in the text, for the next rebuild.
    const auto remember = [this, editor = QPointer<QTextEdit>(field->editor()), id] {
        if (!editor || !decisionDrafts_.contains(id))
            return;
        const QTextCursor cursor = editor->textCursor();
        DecisionDraft& kept = decisionDrafts_[id];
        kept.anchor = cursor.anchor();
        kept.position = cursor.position();
    };
    connect(field->editor(), &QTextEdit::cursorPositionChanged, this, remember);
    connect(field->editor(), &QTextEdit::selectionChanged, this, remember);
    column->addWidget(field);

    const QPointer<ket::TextArea> alive(field);
    connect(field, &ket::TextArea::textChanged, this, [this, alive, id] {
        if (!alive)
            return;
        decisionDrafts_[id].text = alive->text();
        stageDecisionAnswer(id, alive->text(), true);
    });
    field->setEnabled(!teams_->commands()->isPending(key));
    connect(teams_->commands(), &TeamCommands::pendingChanged, field,
            [this, alive, key](const QString& changed) {
                if (alive && changed == key)
                    alive->setEnabled(!teams_->commands()->isPending(key));
            });
    if (focusDraft_ == id) {
        focusDraft_.clear();
        draftFocus_ = field->editor();
    }
    return box;
}

} // namespace ketplus
