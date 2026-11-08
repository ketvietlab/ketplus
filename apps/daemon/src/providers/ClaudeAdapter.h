#pragma once

#include "providers/ProviderAdapter.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <functional>

class QProcess;
class QTimer;

namespace ketplus::daemon {

class ClaudeCredentials;

class ClaudeAdapter final : public ProviderAdapter {
    Q_OBJECT

  public:
    explicit ClaudeAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~ClaudeAdapter() override;

    [[nodiscard]] QString name() const override { return QStringLiteral("claude"); }
    [[nodiscard]] QString missingMessage() const override;
    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] bool isReady() const override;
    [[nodiscard]] QString executable() const override;

    void call(const QString& method, const QJsonObject& params, Callback callback) override;
    // The bundled sidecar: its staged SDK versions and the policy baseline capabilities.
    void runtimeStatus(std::function<void(const compat::Runtime&)> callback) override;
    [[nodiscard]] bool supports(const QString& capability) const override;
    void configurationCatalog(Callback callback) override;
    void respond(const QJsonValue& requestId, const QJsonObject& result) override;
    void respondError(const QJsonValue& requestId, const QString& code,
                      const QString& message) override;
    void stop() override;
    void setCredentials(ClaudeCredentials* credentials) { credentials_ = credentials; }

    [[nodiscard]] static QString missingSidecarMessage();
    [[nodiscard]] static QJsonArray modelCatalog();
    [[nodiscard]] static QJsonArray permissionProfiles();
    [[nodiscard]] static QString permissionMode(const QString& profile);

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
    bool malformedOutput_{false};
    QTimer* restartTimer_{nullptr};
    QDeadlineTimer restartNotBefore_;
    QElapsedTimer readySince_;
    int restartDelayMs_{0};
};

} // namespace ketplus::daemon
