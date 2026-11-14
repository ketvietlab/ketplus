#pragma once

#include "schedules/ScheduleRules.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QWidget>

class QLabel;

namespace ket {
class Button;
class Checkbox;
class Dialog;
class FieldRow;
class SegmentedControl;
class Select;
class TextArea;
class TextField;
} // namespace ket

namespace ketplus {

class ScheduleClient;

// Edit-only. The target is shown and sent back unchanged for a team start; it is not a choice.
class ScheduleEditor final : public QObject {
    Q_OBJECT

  public:
    ScheduleEditor(ScheduleClient* client, const QJsonObject& schedule, QWidget* anchor);
    ~ScheduleEditor() override;

    void open();
    [[nodiscard]] ket::Dialog* dialog() const;
    [[nodiscard]] QString scheduleId() const;

  private:
    void fill();
    void applyKind();
    void updateSummary();
    void save();
    void showIssue(const QString& code, const QString& message, const QString& field = {});
    [[nodiscard]] ScheduleRule collect() const;

    ScheduleClient* client_;
    QJsonObject schedule_;
    bool teamStart_{false};
    bool preserved_{false};
    QString preservedRule_;

    QPointer<ket::Dialog> dialog_;
    ket::FieldRow* nameRow_{nullptr};
    ket::TextField* name_{nullptr};
    ket::FieldRow* promptRow_{nullptr};
    ket::TextArea* prompt_{nullptr};
    QLabel* promptNote_{nullptr};
    ket::FieldRow* ruleRow_{nullptr};
    ket::SegmentedControl* repeat_{nullptr};
    ket::TextField* date_{nullptr};
    ket::TextField* time_{nullptr};
    ket::Checkbox* days_[7]{};
    ket::Select* hours_{nullptr};
    ket::TextField* anchorDate_{nullptr};
    ket::TextField* anchorTime_{nullptr};
    ket::Select* timezone_{nullptr};
    ket::FieldRow* busyRow_{nullptr};
    ket::Select* busy_{nullptr};
    ket::FieldRow* missedRow_{nullptr};
    ket::Select* missed_{nullptr};
    QWidget* onceRow_{nullptr};
    QWidget* timeRow_{nullptr};
    QWidget* daysRow_{nullptr};
    QWidget* hoursRow_{nullptr};
    QWidget* anchorRow_{nullptr};
    QWidget* zoneRow_{nullptr};
    QLabel* summary_{nullptr};
    QLabel* banner_{nullptr};
    QLabel* raw_{nullptr};
};

} // namespace ketplus
