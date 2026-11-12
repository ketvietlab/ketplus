#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QWidget;

namespace ket {
class Dialog;
}

namespace ketplus {

// ADR 0114 and #464: the sidebar's team entries. Every disabled entry says why in
// ket::Style::kDescriptionProperty, as the conversation menu does.
namespace team_roles {
inline constexpr int maximumDevelopers = 5;
// T-23: a team has one or two reviewers.
inline constexpr int maximumReviewers = 2;
[[nodiscard]] QString label(const QString& role);
[[nodiscard]] QColor color(const QString& role);
// Leader first, then developers, then the reviewer; the daemon's order within a role.
[[nodiscard]] int order(const QString& role);
} // namespace team_roles

namespace team_states {
// The task states in the order of the page's filter.
[[nodiscard]] QStringList all();
// "To do", …, "Ready to merge" (#468), "Done".
[[nodiscard]] QString label(const QString& state);
} // namespace team_states

struct TeamFolderMenuActions {
    QAction* open = nullptr;
    QAction* addMember = nullptr;
    QAction* remove = nullptr;
};
TeamFolderMenuActions addTeamFolderMenuActions(QMenu& menu, const QJsonObject& team);

// The repository's + when teams are available.
struct RepositoryAddMenuActions {
    QAction* conversation = nullptr;
    QAction* team = nullptr;
};
RepositoryAddMenuActions addRepositoryAddMenuActions(QMenu& menu);

// Runs a menu's choice once the closed menu has left the screen. A ket::Dialog over the Agent
// Workspace draws a snapshot of the screen beneath its backdrop; opened while the menu was still
// on screen, the snapshot kept the menu, which then seemed not to close.
inline constexpr int kMenuCloseMs = 150;
void afterMenuCloses(QObject* context, std::function<void()> action);

// The conversation menu's team entries: "Add to <team>" for each team of its repository while
// it is on none, or "Remove from <team>…" while it is on one. Each action carries the team id
// as its data. `sessionId` is the conversation's daemon session.
struct ConversationTeamActions {
    QList<QAction*> join;
    QAction* leave = nullptr;
};
ConversationTeamActions addConversationTeamActions(QMenu& menu, const QList<QJsonObject>& teams,
                                                   const QString& sessionId);

// Why `sessionId` cannot join `team`, or empty when it can.
[[nodiscard]] QString joinRefusal(const QJsonObject& team, const QString& sessionId);
// Why `sessionId` cannot leave `team` from the menu, or empty when it can.
[[nodiscard]] QString leaveRefusal(const QJsonObject& team, const QString& sessionId);

// Delete team… and Remove from team… ask first and say what stays. The caller connects
// accepted() and opens it.
ket::Dialog* teamDeleteConfirmation(QWidget* anchor, const QJsonObject& team);
ket::Dialog* teamLeaveConfirmation(QWidget* anchor, const QJsonObject& team,
                                   const QString& memberName);

} // namespace ketplus
