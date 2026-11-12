#pragma once

#include "teams/TeamGoal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QScrollArea;
class QTableWidget;
class QVBoxLayout;

namespace ket {
class Dialog;
} // namespace ket

namespace ketplus {

class TeamDirectory;
namespace team_page {
class TaskRows;
class Text;
} // namespace team_page

// #466: where a team stands and what it needs from the user, in that order: what waits for
// the user, the tasks, the members, what happened. It follows team.changed without a reload,
// keeping its scroll position and any dialog it opened. Without the team (deleted, or a
// repository that has none) it explains teams and offers New team.
//
// The page is one column, at most 1120 px wide, that never scrolls sideways: text wraps or
// elides, a decision's choices continue on the next line, and the task columns and the member
// status that do not fit are left out (TeamPageParts.h).
class TeamPage final : public QWidget {
    Q_OBJECT

  public:
    TeamPage(TeamDirectory* teams, const QString& teamId, const QString& repository,
             QWidget* parent = nullptr);

    [[nodiscard]] QString teamId() const { return teamId_; }
    [[nodiscard]] QScrollArea* scrollArea() const { return scroll_; }
    [[nodiscard]] QTableWidget* tasksTable() const { return tasks_; }
    [[nodiscard]] QJsonArray updates() const { return updates_; }
    void setTaskFilter(const QString& state);
    // Names the goal's mentions and marks the numbers not on this computer.
    void setConversationDirectory(team_goal::ConversationDirectory directory);
    // Opens the task dialog (description, criteria, dependencies, history, Hand over…).
    ket::Dialog* openTask(const QString& taskId);

  signals:
    void conversationRequested(const QString& sessionId);
    // A mention in the goal: `number` is the conversation's `@agent/` number.
    void mentionRequested(const QString& number);
    void addMemberRequested(const QString& teamId);
    // T-23: the banner's "Add a second reviewer".
    void addReviewerRequested(const QString& teamId);
    void newTeamRequested(const QString& repository);
    void deleteRequested(const QString& teamId);
    void removeMemberRequested(const QString& teamId, const QString& sessionId);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void rebuild();
    void readUpdates();
    // ADR 0115: the page of Updates before the oldest one shown.
    void readOlderUpdates();
    // Where the keyboard focus is in the content, by object names: a rebuild puts it back.
    struct FocusPlace final {
        QString name;
        QString row;
        int index{-1};
        // Where the focus was inside the named widget, when it was on a part without a name.
        int part{-1};
    };
    [[nodiscard]] FocusPlace focusPlace() const;
    void restoreFocus(const FocusPlace& place);
    void buildHeading(const QJsonObject& team);
    void buildBanners(const QJsonObject& team);
    void buildDecisions(const QJsonObject& team);
    void buildTasks(const QJsonObject& team);
    void buildMembers(const QJsonObject& team);
    // ADR 0114 §10 (TeamPageStorage.cpp): the disk under the team's worktrees and the worktrees
    // of its done tasks, read again with the team and every minute.
    void readStorage();
    // The done tasks and their worktrees, as one string to compare.
    [[nodiscard]] static QString doneTasks(const QJsonObject& team);
    void buildDiskBanner();
    void buildDoneWorktrees(const QJsonObject& team);
    void buildUpdates();
    void buildEmpty();
    void fillTasks();
    void addSection(const QString& title, const QString& count, const char* name,
                    QWidget* trailing = nullptr);
    [[nodiscard]] static QString memberName(const QJsonObject& team, const QString& sessionId);
    // Wide enough for the lane column and the members' status.
    [[nodiscard]] bool roomy() const;
    // Too narrow for a member's lane beside its name.
    [[nodiscard]] bool compact() const;
    [[nodiscard]] int columnWidth() const;
    [[nodiscard]] bool filterFits() const;
    void showMenu(QWidget* anchor, const QJsonObject& team);
    // Sends a command under `what` (TeamCommands): nothing while the same one is pending.
    void send(const QString& what, const QString& source, const QString& type, QJsonObject payload);
    [[nodiscard]] QString commandKey(const QString& what) const;

    TeamDirectory* teams_;
    QString teamId_;
    QString repository_;
    QString filter_;
    // The newest page of Updates from team.get, then the older pages the user asked for.
    QJsonArray updates_;
    QJsonArray olderUpdates_;
    bool newestHaveMore_{false};
    bool olderHaveMore_{false};
    bool olderPending_{false};
    // The team the content was built from; empty for the page without a team.
    QJsonObject shown_;
    QScrollArea* scroll_;
    QWidget* content_{nullptr};
    // The column of sections, centred in content_.
    QWidget* column_{nullptr};
    QVBoxLayout* layout_{nullptr};
    QTableWidget* tasks_{nullptr};
    team_page::TaskRows* taskRows_{nullptr};
    team_page::Text* taskHint_{nullptr};
    // What the last rebuild laid out for; a resize across either rebuilds.
    bool roomy_{false};
    // The goal stays open across rebuilds once the user opened it.
    bool goalExpanded_{false};
    team_goal::ConversationDirectory conversations_;
    bool compact_{false};
    bool segmented_{true};
    int segmentedWidth_{0};
    bool readPending_{false};
    // team.storage.get's last answer; empty when the daemon has none.
    QJsonObject storage_;
    bool storagePending_{false};
    QString storageTasks_{QStringLiteral("-")};
};

} // namespace ketplus
