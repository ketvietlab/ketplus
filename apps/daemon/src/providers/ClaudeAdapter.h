#pragma once

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>

#include <functional>

class QProcess;
class QTimer;

namespace ketplus::daemon {

class ClaudeCredentials;

class ClaudeAdapter final : public QObject {
    Q_OBJECT

  public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;

    explicit ClaudeAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~ClaudeAdapter() override;

    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString executable() const;

    void call(const QString& method, const QJsonObject& params, Callback callback);
    void configurationCatalog(Callback callback);
    void respond(const QJsonValue& requestId, const QJsonObject& result);
    void respondError(const QJsonValue& requestId, const QString& code, const QString& message);
    void stop();
    void setCredentials(ClaudeCredentials* credentials) { credentials_ = credentials; }

    [[nodiscard]] static QJsonArray modelCatalog();
    [[nodiscard]] static QJsonArray permissionProfiles();
    [[nodiscard]] static QString permissionMode(const QString& profile);

  signals:
    void canonicalEvent(const QString& sessionId, const QString& type, const QJsonObject& payload);
    void requestReceived(const QJsonValue& requestId, const QString& method,
                         const QJsonObject& params);
    void stopped(const QString& reason);

  private:
    struct QueuedCall final {
        QString method;
        QJsonObject params;
        Callback callback;
    };

    void ensureStarted();
    void initialize();
    void sendCall(const QString& method, const QJsonObject& params, Callback callback);
    void sendObject(const QJsonObject& object);
    void consumeStdout();
    void consumeLine(const QByteArray& line);
    void failPending(const QString& reason);
    void setReady(bool ready);
    void noteUnexpectedStop();
    [[nodiscard]] static QString findExecutable();
    [[nodiscard]] static QString findNode();
    [[nodiscard]] static bool sidecarLooksRunnable(const QString& indexPath);
    void configureProcess();

    ClaudeCredentials* credentials_{nullptr};
    QString executable_;
    QProcess* process_{nullptr};
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QHash<quint64, Callback> pending_;
    QList<QueuedCall> queued_;
    quint64 nextRequestId_{1};
    bool starting_{false};
    bool ready_{false};
    bool stopping_{false};
    QTimer* restartTimer_{nullptr};
    QDeadlineTimer restartNotBefore_;
    QElapsedTimer readySince_;
    int restartDelayMs_{0};
};

} // namespace ketplus::daemon
