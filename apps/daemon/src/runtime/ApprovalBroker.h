#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

namespace ketplus::daemon {

struct ApprovalResolution final {
    bool accepted{false};
    QString errorCode;
    QString message;
    QString resultCode;
    QJsonObject transcriptAnswers{};
    QJsonArray transcriptQuestions{};
};

// Approval details with `request_type` "mcp_tool_call" came from a Codex
// `mcpServer/elicitation/request` and are answered with an MCP elicitation action; every other
// approval is answered with a command/file-change `decision`.
[[nodiscard]] QJsonObject approvalProviderResult(const QJsonObject& details, bool accepted);

class ApprovalBroker final : public QObject {
    Q_OBJECT

  public:
    explicit ApprovalBroker(QObject* parent = nullptr);

    [[nodiscard]] QString request(const QString& sessionId, qint64 generation,
                                  const QJsonValue& providerRequestId, const QString& kind,
                                  const QJsonObject& details, int timeoutMs = 0);
    [[nodiscard]] ApprovalResolution resolve(const QString& approvalId, const QString& sessionId,
                                             qint64 generation, const QString& expectedKind,
                                             const QJsonObject& resolution);
    [[nodiscard]] QJsonObject pendingPage(const QString& sessionId, qint64 generation,
                                          const QString& afterId = {}) const;
    // The provider process exited. Its JSON-RPC request ids are reused by the next process, so
    // pending approvals expire without ever being answered to any provider.
    // Forgets pending requests without answering them: their provider process is
    // gone. Only requests of sessions `ofSession` accepts, or all without one.
    void cancelPending(const QString& sessionId, qint64 generation);
    // #126: true only while a pending request can expire.
    [[nodiscard]] bool expiryTimerActive() const { return expiryTimer_.isActive(); }
    // Async assistant cards belong to one turn; closing them never answers a provider.
    void expireAssistantQuestions(const QString& sessionId, qint64 generation);
    void abandonPending(const std::function<bool(const QString& sessionId)>& ofSession = {});

  signals:
    void requested(const QString& approvalId, const QString& sessionId, qint64 generation,
                   const QString& kind, const QJsonObject& details, const QString& expiresAt);
    void providerResolutionReady(const QJsonValue& providerRequestId, const QJsonObject& result);
    void providerRequestRejected(const QJsonValue& providerRequestId, const QString& code,
                                 const QString& message);
    void expired(const QString& approvalId, const QString& sessionId, qint64 generation,
                 const QString& kind, int revision);

  private:
    struct Pending final {
        QString sessionId;
        qint64 generation{0};
        QJsonValue providerRequestId;
        QString kind;
        qint64 expiresAtMs{0};
        QJsonObject details;
        int revision{1};
    };

    void expireRequests();
    void updateExpiryTimer();
    void rememberResolved(const QString& approvalId, const QString& kind);

    QHash<QString, Pending> pending_;
    QSet<QString> deferred_;
    QSet<QString> resolved_;
    QHash<QString, QString> resolvedKinds_;
    QList<QString> resolvedOrder_;
    QTimer expiryTimer_;
};

} // namespace ketplus::daemon
