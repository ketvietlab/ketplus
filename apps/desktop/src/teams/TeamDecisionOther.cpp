// T-55: a decision's "Other" answer, written on the card. The daemon takes free text as an
// answer (TeamCoordinator::answerDecision trims it to 4000 characters and refuses it empty), so
// the card offers a field after its choices instead of sending the user to the conversation.
// The text is kept on the page, not in the widgets: the page rebuilds with every team change,
// and a refusal leaves the text as it was typed.
#include "KetButton.h"
#include "KetTextArea.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include <functional>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

// The daemon's limit for a written answer.
constexpr int kAnswerCharacters = 4000;

// Enter sends, Shift+Enter adds a line, Esc cancels: the composer's keys.
class DraftKeys final : public QObject {
  public:
    DraftKeys(QTextEdit* editor, std::function<void()> send, std::function<void()> cancel)
        : QObject(editor), editor_(editor), send_(std::move(send)), cancel_(std::move(cancel)) {
        editor->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != editor_ || event->type() != QEvent::KeyPress)
            return QObject::eventFilter(watched, event);
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            cancel_();
            return true;
        }
        if (key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter)
            return QObject::eventFilter(watched, event);
        if (key->modifiers() & Qt::ShiftModifier)
            editor_->textCursor().insertText(QStringLiteral("\n"));
        else
            send_();
        return true;
    }

  private:
    QTextEdit* editor_;
    std::function<void()> send_;
    std::function<void()> cancel_;
};

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
    field->setText(decisionDrafts_.value(id));
    field->editor()->moveCursor(QTextCursor::End);
    column->addWidget(field);

    auto* foot = new QHBoxLayout;
    foot->setContentsMargins(0, 0, 0, 0);
    foot->setSpacing(px(8));
    auto* keys =
        new Text(tr("Enter sends · Shift+Enter adds a line · Esc cancels"), box, Text::Fit::Elide);
    keys->setObjectName(QStringLiteral("teamDecisionOtherKeys"));
    keys->setTone(QStringLiteral("text-muted"));
    keys->setType(12);
    foot->addWidget(keys, 1);
    auto* cancel = new ket::Button(tr("Cancel"), ket::ButtonVariant::Ghost, box);
    cancel->setObjectName(QStringLiteral("teamDecisionOtherCancel"));
    foot->addWidget(cancel);
    auto* send = new ket::Button(tr("Send"), ket::ButtonVariant::Primary, box);
    send->setObjectName(QStringLiteral("teamDecisionOtherSend"));
    foot->addWidget(send);
    column->addLayout(foot);

    const QPointer<ket::TextArea> alive(field);
    const QPointer<ket::Button> sending(send);
    // Send waits for words, and for the answer already on its way (TeamCommands disables it).
    const auto ready = [this, alive, sending, key] {
        if (!alive || !sending || teams_->commands()->isPending(key))
            return;
        sending->setEnabled(!alive->text().trimmed().isEmpty());
    };
    ready();
    teams_->commands()->bind(send, key, QStringLiteral("teamDecisionOther"));
    teams_->commands()->bind(cancel, key);
    connect(field, &ket::TextArea::textChanged, this, [this, alive, id, ready] {
        if (alive)
            decisionDrafts_.insert(id, alive->text());
        ready();
    });
    connect(teams_->commands(), &TeamCommands::pendingChanged, send,
            [ready, key](const QString& changed) {
                if (changed == key)
                    ready();
            });
    const auto submit = [this, alive, id, what, key] {
        if (!alive || teams_->commands()->isPending(key))
            return;
        const QString text = alive->text().trimmed();
        if (text.isEmpty())
            return;
        TeamPage::send(what, QStringLiteral("teamDecisionOther"),
                       QStringLiteral("team.decision.answer"),
                       {{QStringLiteral("decision_id"), id}, {QStringLiteral("choice"), text}});
    };
    // Cancel puts the card back as it was, with its choices and Other.
    const auto close = [this, id, key] {
        if (teams_->commands()->isPending(key))
            return;
        decisionDrafts_.remove(id);
        QMetaObject::invokeMethod(this, &TeamPage::rebuild, Qt::QueuedConnection);
    };
    connect(send, &QAbstractButton::clicked, this, submit);
    connect(cancel, &QAbstractButton::clicked, this, close);
    new DraftKeys(field->editor(), submit, close);
    if (focusDraft_ == id) {
        focusDraft_.clear();
        draftFocus_ = field->editor();
    }
    return box;
}

} // namespace ketplus
