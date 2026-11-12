#include "machine/MachineRichTranscript.h"

#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "machine/MachineConversationSession.h"

#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace ketplus {

namespace {

QString keyOf(const QVariant& row) {
    return row.toMap().value(QStringLiteral("messageKey")).toString();
}

bool sameKeys(const QVariantList& left, qsizetype leftStart, const QVariantList& right,
              qsizetype rightStart, qsizetype count) {
    for (qsizetype index = 0; index < count; ++index)
        if (keyOf(left.at(leftStart + index)) != keyOf(right.at(rightStart + index)))
            return false;
    return true;
}

} // namespace

MachineRichTranscript::MachineRichTranscript(QWidget* parent) : QObject(parent) {
    view_ = new AgentWorkspaceView(parent);
    view_->setObjectName(QStringLiteral("machineRichTranscript"));
    view_->setTranscriptOnly(
        tr("This file is on the other machine. Open it there; this Mac cannot open, diff or "
           "reveal it."));
    view_->resetMessages({}, {});
    connect(view_, &AgentWorkspaceView::composerErrorOccurred, this,
            &MachineRichTranscript::actionRefused);
    connect(view_, &AgentWorkspaceView::olderMessagesRequested, this,
            [this](double, double, const qint64 token) {
                if (token > 0)
                    requestToken_ = token;
                if (session_ != nullptr && session_->hasOlder() && !session_->loadingOlder())
                    session_->loadOlder();
                // A request the session declines (nothing older, or one in flight) still
                // completes for the renderer, so its paging never waits on it.
                publishHistory();
            });
    connect(view_, &AgentWorkspaceView::conversationCopyRequested, this,
            [this] { view_->copyText(view_->transcriptCopyText()); });
}

void MachineRichTranscript::show(MachineConversationSession* session) {
    session_ = session;
    if (session == nullptr) {
        sync({}, {});
    } else {
        const QString scope = session->machineId() + QLatin1Char('\n') + session->sessionId();
        sync(scope, agent_message::daemonWindowMessages(
                        {{QStringLiteral("events"), session->transcript().events()}}));
    }
    publishHistory();
}

void MachineRichTranscript::sync(const QString& scope, const QVariantList& rows) {
    if (scope != scope_ || rows_.isEmpty()) {
        scope_ = scope;
        rows_ = rows;
        view_->resetMessages(rows, scope);
        return;
    }
    if (rows == rows_)
        return;
    const QVariantList before = std::exchange(rows_, rows);
    const qsizetype added = rows.size() - before.size();
    // An older page: what was shown is the tail of the new rows.
    if (added > 0 && std::equal(before.cbegin(), before.cend(), rows.cbegin() + added)) {
        view_->prependMessages(rows.mid(0, added));
        return;
    }
    // A live event: the rows shown keep their keys; the last ones changed or new ones follow.
    if (added >= 0 && sameKeys(before, 0, rows, 0, before.size())) {
        for (qsizetype index = 0; index < before.size(); ++index)
            if (before.at(index) != rows.at(index))
                view_->updateMessage(int(index), rows.at(index).toMap());
        for (qsizetype index = before.size(); index < rows.size(); ++index)
            view_->appendMessage(rows.at(index).toMap());
        return;
    }
    // An older page that joined a turn split across pages: the renderer keeps the rows that
    // stay and replaces the rest.
    if (added > 0 && sameKeys(before, 1, rows, added + 1, before.size() - 1)) {
        view_->replaceMessageWindow(rows);
        return;
    }
    view_->resetMessages(rows, scope);
}

void MachineRichTranscript::publishHistory() {
    const bool open = session_ != nullptr;
    view_->setHistoryState({{QStringLiteral("hasOlder"), open && session_->hasOlder()},
                            {QStringLiteral("hasNewer"), false},
                            {QStringLiteral("loading"), open && session_->loadingOlder()},
                            {QStringLiteral("requestToken"), requestToken_}});
}

} // namespace ketplus
