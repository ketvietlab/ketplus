#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace ket {
class Badge;
class Button;
class Checkbox;
class EmptyState;
class Select;
class TextArea;
class TextField;
} // namespace ket

namespace ketplus {

class ScheduleClient;
class ScheduleEditor;

// Management-only window: list, empty state, detail and run history. Creating a schedule
// happens from New conversation or New team, so this window has no create action.
class ScheduleWindow final : public QWidget {
    Q_OBJECT

  public:
    ScheduleWindow(ScheduleClient* client, QWidget* parent = nullptr);

    void present(const QString& scheduleId);
    void edit(const QString& scheduleId);
    [[nodiscard]] int raiseCount() const { return raiseCount_; }
    void noteRaised() { ++raiseCount_; }

  signals:
    void conversationRequested(const QString& sessionId);

  private:
    void build();
    void reload();
    void reloadList();
    void reloadDetail();
    void refreshBanner();
    void select(const QString& id);
    void applyEnabled(const QString& id, bool enabled);
    void runNow();
    void editSelected();
    void confirmDelete();
    void showFailure(const QJsonObject& result, const QString& error);
    [[nodiscard]] bool matches(const QJsonObject& schedule) const;
    [[nodiscard]] QJsonObject selectedSchedule() const;

    ScheduleClient* client_;
    QString selectedId_;
    QString actionError_;
    int raiseCount_{0};
    bool rebuilding_{false};

    QLabel* banner_{nullptr};
    ket::Badge* count_{nullptr};
    ket::Select* filter_{nullptr};
    ket::TextField* search_{nullptr};
    QStackedWidget* stack_{nullptr};
    ket::EmptyState* empty_{nullptr};
    QListWidget* list_{nullptr};
    QWidget* detail_{nullptr};
    QLabel* detailName_{nullptr};
    ket::Checkbox* detailEnabled_{nullptr};
    QLabel* target_{nullptr};
    QLabel* repeat_{nullptr};
    QLabel* busy_{nullptr};
    QLabel* missed_{nullptr};
    QLabel* createdBy_{nullptr};
    QLabel* nextRun_{nullptr};
    ket::TextArea* prompt_{nullptr};
    QWidget* history_{nullptr};
    ket::Button* showMore_{nullptr};
    ket::Button* runNowButton_{nullptr};
    ket::Button* pause_{nullptr};
    ket::Button* edit_{nullptr};
    ket::Button* remove_{nullptr};
    QPointer<ScheduleEditor> editor_;
};

} // namespace ketplus
