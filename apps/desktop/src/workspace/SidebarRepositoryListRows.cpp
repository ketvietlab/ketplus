// A conversation of the sidebar tree is one line (ket::SidebarRow): the state's own mark and
// colour, the title and #number; a finished, unread one is a dot and a semibold title. The role,
// the state in words, what started the turn and the worktree are in the hint and the accessible
// description. Conversations carry no user tags (ADR 0117).
#include "KetSidebarTree.h"
#include "workspace/SidebarRepositoryList.h"
#include "workspace/SidebarRepositoryListDetail.h"

#include <QFileInfo>

namespace ketplus {
namespace sidebar_list {

ket::ConversationStatus conversationStatus(const QVariantMap& conversation) {
    using Status = ket::ConversationStatus;
    const auto status = conversation.value(QStringLiteral("status")).toString().toLower();
    if (conversation.value(QStringLiteral("needsInput")).toBool() || status.contains("input") ||
        status.contains("question") || status == QStringLiteral("waiting for your answer…"))
        return Status::NeedsInput;
    if (conversation.value(QStringLiteral("awaitingReply")).toBool())
        return Status::AwaitingReply;
    if (conversation.value(QStringLiteral("running")).toBool())
        return Status::Working;
    if (status.contains("fail") || status.contains("error") || status.contains("disconnect") ||
        status.contains("missing") || status.contains("not found"))
        return Status::Error;
    if (status.contains("stop"))
        return Status::Stopped;
    return conversation.value(QStringLiteral("started")).toBool() ? Status::Completed
                                                                  : Status::NotStarted;
}

QString runningAgentSender(const QVariantMap& conversation) {
    return conversation.value(QStringLiteral("running")).toBool()
               ? conversation.value(QStringLiteral("agentTurnNumber")).toString()
               : QString();
}

bool waitsForUser(const QVariantMap& conversation, const bool seen) {
    using Status = ket::ConversationStatus;
    const Status status = conversationStatus(conversation);
    return status == Status::NeedsInput || status == Status::AwaitingReply ||
           status == Status::Error || (status == Status::Completed && !seen);
}

} // namespace sidebar_list

namespace {

// A team member's role is the label of its locked tag; the team sets it (#467).
QString roleOf(const QVariantMap& conversation) {
    for (const QVariant& value : conversation.value(QStringLiteral("tags")).toList()) {
        const QVariantMap tag = value.toMap();
        if (tag.value(QStringLiteral("locked")).toBool())
            return tag.value(QStringLiteral("label")).toString().trimmed();
    }
    return {};
}

// What started the running turn: "Message from #12 · Title", or a reply.
QString originOf(const QVariantMap& conversation) {
    const QString sender = sidebar_list::runningAgentSender(conversation);
    if (sender.isEmpty())
        return {};
    const QString title = conversation.value(QStringLiteral("agentTurnTitle")).toString();
    return (conversation.value(QStringLiteral("agentTurnReply")).toBool()
                ? QObject::tr("Reply from #%1")
                : QObject::tr("Message from #%1"))
               .arg(sender) +
           (title.isEmpty() ? QString() : QStringLiteral(" · ") + title);
}

} // namespace

ket::SidebarRow* SidebarRepositoryList::createRow(QWidget* parent, const QVariantMap& conversation,
                                                  const int level) {
    const QString id = conversation.value(QStringLiteral("id")).toString();
    const QString worktreePath = conversation.value(QStringLiteral("path")).toString();
    QString title = conversation.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        title = tr("New conversation");

    auto* row = new ket::SidebarRow(title, parent);
    row->setObjectName(QStringLiteral("productConversationButton"));
    row->setProperty("kvRole", QStringLiteral("productConversation"));
    row->setProperty("worktreePath", worktreePath);
    row->setProperty("conversationId", id);
    row->setProperty("sessionId", conversation.value(QStringLiteral("sessionId")));
    row->setLevel(level);
    row->installEventFilter(this);
    row->setCheckable(true);
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    // ADR 0027: agents and the user call a conversation by this number.
    row->setNumber(conversation.value(QStringLiteral("number")).toString());
    // The worktree is an attribute of the conversation; name it without a machine path.
    row->setWorktree(QFileInfo(worktreePath).fileName());
    updateRow(row, conversation);
    connect(row, &QAbstractButton::clicked, this, [this, row, worktreePath, id] {
        markConversationSeen(id);
        // Selection follows the session manager; the refresh re-checks the row.
        row->setChecked(row->property("selectedConversation").toBool());
        emit conversationRequested(worktreePath, id);
    });
    connect(row, &QWidget::customContextMenuRequested, this,
            [this, worktreePath, id] { emit conversationContextMenuRequested(worktreePath, id); });
    rows_.append(row);
    return row;
}

void SidebarRepositoryList::updateRow(ket::SidebarRow* row, const QVariantMap& conversation) const {
    const QString id = conversation.value(QStringLiteral("id")).toString();
    const bool selected = conversation.value(QStringLiteral("selected")).toBool();
    const bool seen =
        conversation.value(QStringLiteral("seen")).toBool() || seenConversationIds_.contains(id);
    const auto status = sidebar_list::conversationStatus(conversation);
    row->setProperty("selectedConversation", selected);
    row->setChecked(selected);
    row->setStatus(status);
    row->setUnread(status == ket::ConversationStatus::Completed && !seen);
    row->setProperty("unread", row->isUnread());
    const QString role = roleOf(conversation);
    row->setRole(role);
    const QString origin = originOf(conversation);
    row->setStatusDetail(origin);

    const QString number = conversation.value(QStringLiteral("number")).toString();
    const QString title = row->text();
    const QString numbered =
        number.isEmpty() ? title : QLatin1Char('#') + number + QLatin1Char(' ') + title;
    // The row's accessible name and description are the kit's: "title, #number", then the role,
    // the state in words with what started the turn, and the worktree. The hint of a resting
    // pointer says the same on lines of their own.
    QStringList hint{numbered};
    hint << (role.isEmpty() ? row->stateText() : role + QStringLiteral(" · ") + row->stateText());
    hint << QFileInfo(row->property("worktreePath").toString()).fileName();
    row->setToolTip(hint.join(QLatin1Char('\n')));
}

} // namespace ketplus
