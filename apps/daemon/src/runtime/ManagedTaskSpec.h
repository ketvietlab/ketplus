#pragma once

#include "storage/BackgroundTaskLedger.h"

#include <functional>

namespace ketplus::daemon {
struct ManagedTaskAdmission final {
    BackgroundTaskIntent intent;
    QJsonObject execution;
    QString errorCode;
    QString error;
};

// Context is obtained from the authenticated owner, not copied from request JSON.
// The host's catalog/workspace policy remains authoritative for every provider.
struct ManagedTaskContext final {
    BackgroundTaskOwner owner;
    QString cwd;
    QString provider;
    QString model;
    QString effort;
    std::function<bool(const QString&)> authorizeCwd;
    std::function<bool(const QString&, const QString&, const QString&)> authorizeAgent;
    QString executionPolicy{"ask"};
};

ManagedTaskAdmission validateManagedTask(const QJsonObject& payload,
                                         const ManagedTaskContext& context);
} // namespace ketplus::daemon
