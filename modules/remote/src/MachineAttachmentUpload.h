#pragma once

#include "MachineBackend.h"
#include <QByteArray>
#include <QPointer>

namespace ketplus::remote {

// One generation-fenced upload. Every step waits for the daemon result, including when
// the Relay initially answers queued. Bytes are never put in a transport outside MachineBackend.
class MachineAttachmentUpload final : public QObject {
    Q_OBJECT
  public:
    MachineAttachmentUpload(MachineBackend* backend, QString sessionId, qint64 generation,
                            QObject* parent = nullptr);
    void start(QString name, QString mediaType, QByteArray bytes);
    void cancel();
    [[nodiscard]] QString attachmentId() const { return id_; }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] bool terminal() const { return done_; }
  signals:
    void progress(qint64 acknowledgedBytes, qint64 totalBytes);
    void finished(const QString& attachmentId, const QString& error);

  private:
    void advance();
    void accept(const QJsonObject& result, const QString& error);
    void fail(const QString& error);
    QPointer<MachineBackend> backend_;
    QString sessionId_, id_, name_, mediaType_, queuedCommand_;
    QByteArray bytes_;
    qint64 generation_;
    int step_{-1}, chunk_{0};
    bool sentCancel_{false};
    bool started_{false}, pending_{false}, cancelling_{false}, ready_{false}, done_{false};
};
} // namespace ketplus::remote
