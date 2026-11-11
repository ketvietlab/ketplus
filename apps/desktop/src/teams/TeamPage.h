#pragma once

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
class SegmentedControl;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// #466: where a team stands and what it needs from the user, in that order: what waits for
// the user, the tasks, the members, what happened. It follows team.changed without a reload,
// keeping its scroll position and any dialog it opened. Without the team (deleted, or a
// repository that has none) it explains teams and offers New team.
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
    // Opens the task dialog (description, criteria, dependencies, history, Hand over…).
    ket::Dialog* openTask(const QString& taskId);

  signals:
    void conversationRequested(const QString& sessionId);
    void addMemberRequested(const QString& teamId);
    void newTeamRequested(const QString& repository);
    void deleteRequested(const QString& teamId);
    void removeMemberRequested(const QString& teamId, const QString& sessionId);

  private:
    void rebuild();
    void readUpdates();
    void buildHeading(const QJsonObject& team);
    void buildBanners(const QJsonObject& team);
    void buildDecisions(const QJsonObject& team);
    void buildTasks(const QJsonObject& team);
    void buildMembers(const QJsonObject& team);
    void buildUpdates();
    void buildEmpty();
    void fillTasks();
    void showMenu(QWidget* anchor, const QJsonObject& team);
    void send(const QString& type, QJsonObject payload, QLabel* error = nullptr);

    TeamDirectory* teams_;
    QString teamId_;
    QString repository_;
    QString filter_;
    QJsonArray updates_;
    QScrollArea* scroll_;
    QWidget* content_{nullptr};
    QVBoxLayout* layout_{nullptr};
    QTableWidget* tasks_{nullptr};
    bool readPending_{false};
};

} // namespace ketplus
