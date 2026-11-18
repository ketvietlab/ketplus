#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

namespace ketplus {

class PtyBackend;

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
    std::unique_ptr<PtyBackend> backend_;
};
}; // namespace ketplus
