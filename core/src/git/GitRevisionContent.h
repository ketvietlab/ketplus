#pragma once
#include "git/GitTypes.h"
#include <QObject>
#include <QPointer>
class QProcess;
namespace ketplus {
// Read the right-hand snapshot represented by a diff, never stage or modify it.
class GitRevisionContent final : public QObject {
    Q_OBJECT
  public:
    explicit GitRevisionContent(QObject* parent = nullptr) : QObject(parent) {}
    void load(const QString& path, GitDiffMode mode);
  signals:
    void ready(const QString& content, const QString& error);

  private:
    QPointer<QProcess> process_;
    quint64 generation_{0};
};
} // namespace ketplus
