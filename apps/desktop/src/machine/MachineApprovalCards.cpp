#include "machine/MachineApprovalCards.h"

#include "machine/MachineConversationSession.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

namespace ketplus {

namespace {

QString describe(const QJsonObject& request) {
    for (const char* key : {"title", "command", "reason", "message", "description"}) {
        const QString text = request.value(QLatin1String(key)).toString();
        if (!text.isEmpty())
            return text;
    }
    return QObject::tr("The agent is waiting for your approval.");
}

QFrame* cardFrame(QWidget* parent, const QString& objectName) {
    auto* card = new QFrame(parent);
    card->setObjectName(objectName);
    card->setFrameShape(QFrame::StyledPanel);
    new QVBoxLayout(card);
    return card;
}

} // namespace

MachineApprovalCards::MachineApprovalCards(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    hide();
}

void MachineApprovalCards::setSession(MachineConversationSession* session) {
    session_ = session;
    refresh();
}

void MachineApprovalCards::refresh() {
    auto* cards = layout();
    while (QLayoutItem* item = cards->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (session_ == nullptr || session_->approvals().isEmpty()) {
        hide();
        return;
    }
    const bool canAct = session_->canAct();
    for (const QJsonObject& request : session_->approvals())
        cards->addWidget(request.value(QLatin1String("kind")).toString() == QLatin1String("input")
                             ? createQuestionCard(request, canAct)
                             : createCard(request, canAct));
    show();
}

QWidget* MachineApprovalCards::createCard(const QJsonObject& request, const bool canAct) {
    auto* card = cardFrame(this, QStringLiteral("machineApprovalCard"));
    const QString id = request.value(QLatin1String("approval_id")).toString();
    const bool expired = request.value(QLatin1String("expired")).toBool();
    card->setProperty("approvalId", id);
    card->setProperty("expired", expired);
    auto* layout = qobject_cast<QVBoxLayout*>(card->layout());
    layout->addWidget(new QLabel(describe(request), card));
    if (expired) {
        layout->addWidget(
            new QLabel(tr("Expired: %1").arg(request.value("expired_reason").toString()), card));
        return card;
    }
    auto* buttons = new QHBoxLayout;
    auto* accept = new QPushButton(tr("Approve"), card);
    accept->setObjectName(QStringLiteral("machineApprove"));
    auto* decline = new QPushButton(tr("Decline"), card);
    decline->setObjectName(QStringLiteral("machineDecline"));
    for (auto* button : {accept, decline}) {
        button->setEnabled(canAct);
        if (!canAct)
            button->setToolTip(session_->blockedReason());
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    layout->addLayout(buttons);
    MachineConversationSession* session = session_;
    connect(accept, &QPushButton::clicked, session,
            [session, id] { session->resolveApproval(id, true); });
    connect(decline, &QPushButton::clicked, session,
            [session, id] { session->resolveApproval(id, false); });
    return card;
}

QWidget* MachineApprovalCards::createQuestionCard(const QJsonObject& request, const bool canAct) {
    auto* card = cardFrame(this, QStringLiteral("machineQuestionCard"));
    const QString id = request.value(QLatin1String("approval_id")).toString();
    const bool expired = request.value(QLatin1String("expired")).toBool();
    card->setProperty("approvalId", id);
    card->setProperty("expired", expired);
    auto* layout = qobject_cast<QVBoxLayout*>(card->layout());
    const QJsonArray questions = request.value(QLatin1String("questions")).toArray();
    if (questions.isEmpty())
        layout->addWidget(new QLabel(describe(request), card));
    // Every question gets an answer row; the daemon refuses a request answered in part.
    struct Answer {
        QJsonObject question;
        QList<QPushButton*> options;
        QLineEdit* custom{nullptr};
    };
    auto answers = std::make_shared<QList<Answer>>();
    for (const QJsonValue& value : questions) {
        Answer answer{value.toObject(), {}, nullptr};
        const QString header = answer.question.value(QLatin1String("header")).toString();
        const QString prompt = answer.question.value(QLatin1String("prompt")).toString();
        layout->addWidget(
            new QLabel(header.isEmpty() ? prompt : header + QStringLiteral(": ") + prompt, card));
        if (expired)
            continue;
        auto* row = new QHBoxLayout;
        const bool multiple = answer.question.value(QLatin1String("selection")).toString() ==
                              QLatin1String("multiple");
        auto* group = new QButtonGroup(card);
        group->setExclusive(!multiple);
        for (const QJsonValue& optionValue :
             answer.question.value(QLatin1String("options")).toArray()) {
            const QJsonObject option = optionValue.toObject();
            auto* button = new QPushButton(option.value(QLatin1String("label")).toString(), card);
            button->setObjectName(QStringLiteral("machineQuestionOption"));
            button->setCheckable(true);
            button->setEnabled(canAct);
            button->setProperty("optionId", option.value(QLatin1String("id")).toString());
            group->addButton(button);
            row->addWidget(button);
            answer.options.append(button);
        }
        if (answer.options.isEmpty() ||
            answer.question.value(QLatin1String("allow_custom")).toBool()) {
            answer.custom = new QLineEdit(card);
            answer.custom->setObjectName(QStringLiteral("machineQuestionAnswer"));
            answer.custom->setEnabled(canAct);
            if (answer.question.value(QLatin1String("secret")).toBool())
                answer.custom->setEchoMode(QLineEdit::Password);
            row->addWidget(answer.custom, 1);
        }
        layout->addLayout(row);
        answers->append(answer);
    }
    if (expired) {
        layout->addWidget(
            new QLabel(tr("Expired: %1").arg(request.value("expired_reason").toString()), card));
        return card;
    }
    auto* submit = new QPushButton(tr("Answer"), card);
    submit->setObjectName(QStringLiteral("machineQuestionSubmit"));
    layout->addWidget(submit, 0, Qt::AlignLeft);
    const auto complete = [answers] {
        for (const Answer& answer : *answers) {
            bool given = answer.custom != nullptr && !answer.custom->text().trimmed().isEmpty();
            for (const QPushButton* option : answer.options)
                given = given || option->isChecked();
            if (!given)
                return false;
        }
        return true;
    };
    const auto update = [submit, complete, canAct] { submit->setEnabled(canAct && complete()); };
    for (const Answer& answer : *answers) {
        for (QPushButton* option : answer.options)
            connect(option, &QPushButton::toggled, submit, update);
        if (answer.custom != nullptr)
            connect(answer.custom, &QLineEdit::textChanged, submit, update);
    }
    update();
    MachineConversationSession* session = session_;
    connect(submit, &QPushButton::clicked, session, [session, request, answers, id, complete] {
        if (!complete())
            return;
        QJsonArray normalized;
        for (const Answer& answer : *answers) {
            QJsonArray optionIds;
            for (const QPushButton* option : answer.options)
                if (option->isChecked())
                    optionIds.append(option->property("optionId").toString());
            QJsonObject item{
                {QStringLiteral("question_id"), answer.question.value(QLatin1String("id"))},
                {QStringLiteral("question_revision"),
                 answer.question.value(QLatin1String("revision")).toInt(1)},
                {QStringLiteral("option_ids"), optionIds}};
            if (answer.custom != nullptr && !answer.custom->text().trimmed().isEmpty())
                item.insert(QStringLiteral("custom_answer"), answer.custom->text().trimmed());
            normalized.append(item);
        }
        session->answerQuestion(
            id, {{QStringLiteral("request_id"), id},
                 {QStringLiteral("revision"), request.value(QLatin1String("revision")).toInt(1)},
                 {QStringLiteral("action"), QStringLiteral("answer")},
                 {QStringLiteral("answers"), normalized}});
    });
    return card;
}

} // namespace ketplus
