#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QWidget>

class QLabel;

namespace ket {
class Checkbox;
class Radio;
class Select;
class TextArea;
class TextField;
} // namespace ket

namespace ketplus {

// The New team "Schedule" section, and the team page's Change time dialog.
// Start now with repeat off adds nothing to team.create. A scheduled start is
// start_at {at, timezone}. A repeat is {rule, work_order}; rule is the daemon object
// with time_zone.
class TeamScheduleFields final : public QWidget {
    Q_OBJECT

  public:
    explicit TeamScheduleFields(QWidget* parent = nullptr);

    void setGoal(const QString& goal);
    // Change time: the start fields only, already scheduled.
    void setStartOnly(bool startOnly);
    void setScheduled(bool scheduled);
    void setStart(const QString& date, const QString& time, const QString& zone);

    [[nodiscard]] QJsonObject startAt() const;
    [[nodiscard]] QJsonObject repeat() const;
    // Empty when the section can be sent. `now` is UTC.
    [[nodiscard]] QString problem(const QDateTime& now) const;
    void showProblem(const QDateTime& now);

  private:
    void applyStartMode();
    void applyRepeatMode();
    void preset(bool tomorrow);
    [[nodiscard]] QString zone() const;
    [[nodiscard]] QString repeatZone() const;

    ket::Radio* now_{nullptr};
    ket::Radio* scheduled_{nullptr};
    QWidget* startDetails_{nullptr};
    ket::TextField* date_{nullptr};
    ket::TextField* time_{nullptr};
    ket::Select* zone_{nullptr};
    ket::Select* repeat_{nullptr};
    QWidget* repeatDetails_{nullptr};
    ket::TextField* repeatTime_{nullptr};
    ket::Select* repeatZone_{nullptr};
    ket::Select* hours_{nullptr};
    QWidget* days_{nullptr};
    ket::Checkbox* day_[7]{};
    QWidget* orderRow_{nullptr};
    ket::TextArea* order_{nullptr};
    QLabel* problem_{nullptr};
    bool orderEdited_{false};
    bool startOnly_{false};
};
} // namespace ketplus
