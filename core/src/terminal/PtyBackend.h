#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

namespace ketplus {

// Platform pseudo-terminal backend behind PtyProcess. POSIX uses forkpty, Windows uses ConPTY.
// Implementations emit outputReceived/exited on the owner's thread and are no longer running
// by the time exited is emitted.
class PtyBackend : public QObject {
    Q_OBJECT

  public:
    explicit PtyBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~PtyBackend() override = default;

    // Creates the backend for the current platform.
    static std::unique_ptr<PtyBackend> create(QObject* parent);

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] QString shellPath() const { return shellPath_; }

    // On failure returns false and fills `error` with a user-facing message.
    virtual bool start(const QString& workingDirectory, int rows, int columns, QString& error) = 0;
    virtual void write(const QByteArray& bytes) = 0;
    virtual void resize(int rows, int columns) = 0;
    virtual void stop() = 0;

  signals:
    void outputReceived(const QByteArray& bytes);
    void exited(int exitCode);

  protected:
    QString shellPath_;
};

} // namespace ketplus
