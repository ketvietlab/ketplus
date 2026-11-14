#include "schedules/ScheduleWindows.h"

#include "schedules/ScheduleWindow.h"

#include <QAction>
#include <QKeySequence>
#include <QWidget>

namespace ketplus {

ScheduleWindows::ScheduleWindows(QObject* parent)
    : QObject(parent), client_(new ScheduleClient(this)) {}

ScheduleWindow* ScheduleWindows::window() const { return window_; }

QAction* ScheduleWindows::createAction(QObject* parent) {
    auto* action = new QAction(tr("Schedules…"), parent);
    action->setObjectName(QStringLiteral("schedulesMenuAction"));
    // Qt maps Ctrl to Command on macOS, so this is ⇧⌘J, the mock's shortcut.
    action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+J")));
    connect(action, &QAction::triggered, this, [this] { show(); });
    return action;
}

void ScheduleWindows::show(const QString& scheduleId) {
    if (window_ == nullptr) {
        window_ = new ScheduleWindow(client_, qobject_cast<QWidget*>(parent()));
        window_->setAttribute(Qt::WA_DeleteOnClose);
        connect(window_, &ScheduleWindow::conversationRequested, this,
                &ScheduleWindows::conversationRequested);
    }
    window_->present(scheduleId);
    window_->show();
    window_->raise();
    window_->activateWindow();
    window_->noteRaised();
}

void ScheduleWindows::edit(const QString& scheduleId) {
    show(scheduleId);
    if (window_ != nullptr)
        window_->edit(scheduleId);
}

} // namespace ketplus
