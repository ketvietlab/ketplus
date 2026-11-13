#pragma once
#include <QJsonArray>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;
namespace ketplus::remote {
class MachineBackend;
class MachineAttachmentUpload;
} // namespace ketplus::remote
namespace ketplus {
class MachineAttachmentPanel final : public QWidget {
    Q_OBJECT
  public:
    MachineAttachmentPanel(remote::MachineBackend* backend, QString sessionId, qint64 generation,
                           QWidget* parent = nullptr);
    void chooseFiles();
    void addFiles(const QStringList& paths);
    bool pasteImage();
    [[nodiscard]] bool hasAttachments() const { return !rows_.isEmpty(); }
    [[nodiscard]] bool allReady() const;
    [[nodiscard]] QJsonArray attachmentIds() const;
    void setSending(bool sending);
    void sent(bool success);
    [[nodiscard]] bool discardable() const;
  signals:
    void changed();
    void inputRejected(const QString& message);

  private:
    struct Row {
        QPointer<QWidget> widget;
        QPointer<remote::MachineAttachmentUpload> upload;
        qint64 bytes;
    };
    bool addBytes(QString name, QString type, QByteArray bytes, const QString& source = {});
    void rejectInput(const QString& message);
    void remove(remote::MachineAttachmentUpload* upload);
    QPointer<remote::MachineBackend> backend_;
    QString sessionId_;
    qint64 generation_;
    bool sending_{false};
    QVBoxLayout* layout_;
    QList<Row> rows_;
};
} // namespace ketplus
