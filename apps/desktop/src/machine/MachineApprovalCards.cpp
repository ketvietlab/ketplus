#include "machine/MachineApprovalCards.h"

#include "machine/MachineConversationSession.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

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
    const QJsonObject question = questions.isEmpty() ? QJsonObject{} : questions.first().toObject();
    QString prompt = question.value(QLatin1String("question")).toString();
    if (prompt.isEmpty())
        prompt = describe(request);
    layout->addWidget(new QLabel(prompt, card));
    if (expired) {
        layout->addWidget(
            new QLabel(tr("Expired: %1").arg(request.value("expired_reason").toString()), card));
        return card;
    }
    // One answer to the first question: an option, or free text where the question allows it.
    const auto answer = [session = session_, request, question, id](const QJsonArray& optionIds,
                                                                    const QString& custom) {
        QJsonObject normalized{{QStringLiteral("question_id"), question.value(QLatin1String("id"))},
                               {QStringLiteral("question_revision"),
                                question.value(QLatin1String("revision")).toInt(1)},
                               {QStringLiteral("option_ids"), optionIds}};
        if (!custom.isEmpty())
            normalized.insert(QStringLiteral("custom_answer"), custom);
        session->answerQuestion(
            id, {{QStringLiteral("request_id"), id},
                 {QStringLiteral("revision"), request.value(QLatin1String("revision")).toInt(1)},
                 {QStringLiteral("action"), QStringLiteral("answer")},
                 {QStringLiteral("answers"), QJsonArray{normalized}}});
    };
    auto* row = new QHBoxLayout;
    for (const QJsonValue& value : question.value(QLatin1String("options")).toArray()) {
        const QJsonObject option = value.toObject();
        auto* button = new QPushButton(option.value(QLatin1String("label")).toString(), card);
        button->setObjectName(QStringLiteral("machineQuestionOption"));
        button->setEnabled(canAct);
        const QString optionId = option.value(QLatin1String("id")).toString();
        connect(button, &QPushButton::clicked, session_,
                [answer, optionId] { answer(QJsonArray{optionId}, {}); });
        row->addWidget(button);
    }
    if (question.value(QLatin1String("options")).toArray().isEmpty() ||
        question.value(QLatin1String("allow_custom")).toBool()) {
        auto* custom = new QLineEdit(card);
        custom->setObjectName(QStringLiteral("machineQuestionAnswer"));
        custom->setEnabled(canAct);
        connect(custom, &QLineEdit::returnPressed, session_, [answer, custom] {
            if (!custom->text().trimmed().isEmpty())
                answer({}, custom->text().trimmed());
        });
        row->addWidget(custom, 1);
    }
    layout->addLayout(row);
    return card;
}

} // namespace ketplus
