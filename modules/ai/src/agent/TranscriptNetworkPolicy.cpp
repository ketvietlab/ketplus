#include "agent/TranscriptNetworkPolicy.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QUrl>

namespace ketplus {
namespace {

class LocalOnlyNetwork final : public QNetworkAccessManager {
  public:
    using QNetworkAccessManager::QNetworkAccessManager;

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* data) override {
        const QString scheme = request.url().scheme().toLower();
        const bool local = scheme == QLatin1String("file")
                               ? !namesAnotherMachine(request.url())
                               : scheme == QLatin1String("qrc") || scheme == QLatin1String("data");
        if (local)
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

bool namesAnotherMachine(const QString& path) {
    return path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"));
}

bool namesAnotherMachine(const QUrl& url) {
    return url.isLocalFile() && (!url.host().isEmpty() || namesAnotherMachine(url.toLocalFile()));
}

bool transcriptMayOpen(const QString& path, const QString& worktree) {
    if (!namesAnotherMachine(path))
        return true;
    if (worktree.isEmpty() || !namesAnotherMachine(worktree))
        return false;
    const auto slashes = [](QString value) { return value.replace(QLatin1Char('\\'), u'/'); };
    // cleanPath folds the leading "//" on Unix, so the share is compared without it.
    const QString inside = QDir::cleanPath(slashes(path).mid(1));
    QString root = QDir::cleanPath(slashes(worktree).mid(1));
    if (!root.endsWith(QLatin1Char('/')))
        root += QLatin1Char('/');
    return inside.startsWith(root, Qt::CaseInsensitive);
}

void installLocalOnlyNetwork(QQmlEngine* engine) {
    // The engine keeps the factory for its lifetime; one instance serves every engine.
    static LocalOnlyNetworkFactory factory;
    engine->setNetworkAccessManagerFactory(&factory);
}

} // namespace ketplus
