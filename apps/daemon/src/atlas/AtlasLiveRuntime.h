#pragma once
#include "atlas/AtlasLiveTunnel.h"
#include "workspace/AtlasIndex.h"
#include <QProcess>
#include <QTimer>
#include <functional>

namespace ketplus::daemon {
class AtlasLiveRuntime final : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const QJsonObject&)>;
    AtlasLiveRuntime(QObject* parent = nullptr);
    ~AtlasLiveRuntime() override;
    void start(const AtlasDescriptor& atlas, QUrl relay, const QString& bearer, qint64 revision,
               Callback callback);
    void attach(QUrl relay, const QString& bearer, Callback callback);
    bool reusable() const { return launched_ && !stopped_; }
    void stop();
    QString channel() const { return channel_; }

  private:
    void launch();
    void fail(const QString& code);
    void inspectOutput();
    AtlasDescriptor atlas_;
    QProcess process_;
    QTimer timeout_;
    AtlasLiveTunnel* tunnel_{nullptr};
    Callback callback_;
    QByteArray output_;
    QByteArray key_;
    QUrl relay_;
    QString bearer_, channel_, cli_;
    quint16 viewerPort_{0}, rendererPort_{0};
    qint64 revision_{0};
    bool checkingVersion_{true}, stopped_{false}, launched_{false};
};
} // namespace ketplus::daemon
