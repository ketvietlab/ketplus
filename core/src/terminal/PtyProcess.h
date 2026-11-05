#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;

namespace ketplus {

class PtyProcess final : public QObject {
    Q_OBJECT

  public:
    explicit PtyProcess(QObject* parent = nullptr);
    ~PtyProcess() override;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] QString shellPath() const;

    bool start(const QString& workingDirectory, int rows, int columns);
    void send(const QByteArray& bytes);
    void resizeTerminal(int rows, int columns);
    void stop();

  signals:
    void outputReceived(const QByteArray& bytes);
    void started();
    void exited(int exitCode);
    void errorOccurred(const QString& message);

  private:
    void readAvailable();
    void flushPendingWrite();
    void checkChild();
    void closeMaster();

    int masterFileDescriptor_{-1};
    qint64 childProcessId_{-1};
    QSocketNotifier* readNotifier_{nullptr};
    QSocketNotifier* writeNotifier_{nullptr};
    QTimer* childTimer_{nullptr};
    QByteArray pendingWrite_;
    QString shellPath_;
};

} // namespace ketplus
