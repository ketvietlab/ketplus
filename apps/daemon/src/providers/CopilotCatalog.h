#pragma once
#include "providers/ProviderAdapter.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTemporaryDir>

class QProcess;
class QTimer;
namespace ketplus::daemon {
namespace copilot_catalog {
inline constexpr int maximumModels = 256;
inline constexpr int maximumIdLength = 256;
inline constexpr int maximumLabelLength = 256;
inline constexpr int maximumDescriptionLength = 2000;
inline constexpr qsizetype maximumOutputBytes = 1024 * 1024;
inline constexpr qsizetype maximumCatalogBytes = 256 * 1024;
struct Snapshot {
    QJsonArray models;
    QString configId;
    QString current;
    bool legacy{false};
};
Snapshot parse(const QJsonObject& session);
QJsonArray fallback();
struct Selection {
    QString method;
    QJsonObject params;
    QString error;
};
Selection select(const Snapshot& catalog, const QString& sessionId, const QString& requested);
} // namespace copilot_catalog

// A disposable setup-only ACP probe. It never receives user prompts or host MCP servers.
class CopilotCatalog final : public QObject {
  public:
    explicit CopilotCatalog(QObject* parent = nullptr, int timeoutMs = 8000, int freshMs = 60000);
    ~CopilotCatalog() override;
    void get(const QString& executable, ProviderAdapter::Callback done);
    void stop();

  private:
    void drain();
    void line(const QByteArray& bytes);
    void send(int id, const QString& method, const QJsonObject& params);
    void finish(bool success);
    void publish();
    QProcess* process_;
    QTimer* timer_;
    QTemporaryDir directory_;
    QElapsedTimer fetched_;
    QString executable_;
    QJsonArray cached_;
    QJsonArray candidate_;
    QList<ProviderAdapter::Callback> waiters_;
    QByteArray buffer_;
    qsizetype received_{0};
    int expected_{0};
    int timeoutMs_;
    int freshMs_;
    bool finishing_{false};
    bool successful_{false};
    bool drainingFinished_{false};
};
} // namespace ketplus::daemon
