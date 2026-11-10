#include "agent/TranscriptNetworkPolicy.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>

namespace ketplus {
namespace {

class LocalOnlyNetwork final : public QNetworkAccessManager {
  public:
    using QNetworkAccessManager::QNetworkAccessManager;

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* data) override {
        const QString scheme = request.url().scheme().toLower();
        if (scheme == QLatin1String("file") || scheme == QLatin1String("qrc") ||
            scheme == QLatin1String("data"))
            return QNetworkAccessManager::createRequest(operation, request, data);
        // A request with no address fails at once with ProtocolUnknownError, before any lookup.
        QNetworkRequest refused(request);
        refused.setUrl(QUrl());
        return QNetworkAccessManager::createRequest(operation, refused, nullptr);
    }
};

class LocalOnlyNetworkFactory final : public QQmlNetworkAccessManagerFactory {
  public:
    QNetworkAccessManager* create(QObject* parent) override { return new LocalOnlyNetwork(parent); }
};

} // namespace

void installLocalOnlyNetwork(QQmlEngine* engine) {
    // The engine keeps the factory for its lifetime; one instance serves every engine.
    static LocalOnlyNetworkFactory factory;
    engine->setNetworkAccessManagerFactory(&factory);
}

} // namespace ketplus
