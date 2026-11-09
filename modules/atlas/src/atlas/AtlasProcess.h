#pragma once
#include "workspace/AtlasIndex.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

namespace ketplus {
class AtlasProcess final : public QObject {
    Q_OBJECT
  public:
    explicit AtlasProcess(QObject* parent = nullptr);
    ~AtlasProcess() override;
    void start(const AtlasDescriptor& atlas);
    void stop();
    bool running() const { return process_.state() != QProcess::NotRunning; }
    static QString cliPath();
    // PATH, then Homebrew, then each nvm Node version from newest to oldest.
    static QStringList toolSearchPaths();
  signals:
    void ready(const QUrl& url);
    void failed(const QString& message);

  private:
    void launchAttempt();
    bool claimPort(QString& error);
    void releasePort();

    QProcess process_;
    QTimer timeout_;
    AtlasDescriptor atlas_;
    QByteArray output_;
    quint16 port_{0};
    int startAttempts_{0};
    quint64 generation_{0};
    bool stopping_{false};
    bool ready_{false};
};
} // namespace ketplus
