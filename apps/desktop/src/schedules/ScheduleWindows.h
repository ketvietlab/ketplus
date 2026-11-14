#pragma once

#include "schedules/ScheduleClient.h"

#include <QObject>
#include <QPointer>

class QAction;
class QMenu;

namespace ketplus {

class ScheduleWindow;

// One non-modal Schedules window for the app. View › Schedules… and other tasks call show().
class ScheduleWindows final : public QObject {
    Q_OBJECT

  public:
    explicit ScheduleWindows(QObject* parent = nullptr);

    [[nodiscard]] ScheduleClient* client() const { return client_; }
    [[nodiscard]] ScheduleWindow* window() const { return window_; }
    // The View menu item. Triggering it opens the window, or raises it when it is already open.
    [[nodiscard]] QAction* createAction(QObject* parent);

    void show(const QString& scheduleId = {});
    void edit(const QString& scheduleId);

  signals:
    void conversationRequested(const QString& sessionId);

  private:
    ScheduleClient* client_;
    QPointer<ScheduleWindow> window_;
};

} // namespace ketplus
