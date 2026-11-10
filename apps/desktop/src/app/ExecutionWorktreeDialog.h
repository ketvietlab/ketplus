#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <functional>

namespace ketplus {
using ExecutionWorktreeRequest = std::function<void(
    const QString&, const QJsonObject&, std::function<void(const QJsonObject&, const QString&)>)>;
void showExecutionWorktreeDialog(QWidget* parent, ExecutionWorktreeRequest request,
                                 std::function<void(const QString&)> selected,
                                 const QString& fileContextPath = {},
                                 const QStringList& repositories = {},
                                 const QString& currentRepository = {},
                                 // Create worktree… for this conversation: the repository the
                                 // picker lists and the context revision it last read.
                                 std::function<void(const QString&, qint64)> createWorktree = {});
} // namespace ketplus
