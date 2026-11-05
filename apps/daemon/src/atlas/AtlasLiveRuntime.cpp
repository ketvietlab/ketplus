#include "atlas/AtlasLiveRuntime.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUrlQuery>
#include <QUuid>

namespace ketplus::daemon {
AtlasLiveRuntime::AtlasLiveRuntime(QObject* parent) : QObject(parent) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail("atlas_start_timeout"); });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &AtlasLiveRuntime::inspectOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        process_.readAllStandardError(); // Project output is never forwarded or persisted.
    });
    connect(&process_, &QProcess::errorOccurred, this, [this] { fail("atlas_process_failed"); });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (stopped_)
            return;
        if (checkingVersion_ && code == 0 && status == QProcess::NormalExit &&
            output_.trimmed() == "0.5.2") {
            checkingVersion_ = false;
            launch();
        } else
            fail(checkingVersion_ ? "atlas_cli_version" : "atlas_process_exited");
    });
}
AtlasLiveRuntime::~AtlasLiveRuntime() { stop(); }
void AtlasLiveRuntime::start(const AtlasDescriptor& atlas, QUrl relay, const QString& bearer,
                             qint64 revision, Callback callback) {
    atlas_ = atlas;
    relay_ = std::move(relay);
    bearer_ = bearer;
    revision_ = revision;
    callback_ = std::move(callback);
    key_ = AtlasTunnelCodec::randomKey();
    channel_ = QUuid::createUuid().toString(QUuid::Id128);
    cli_ = qEnvironmentVariable("KETPLUS_KETATLAS_CLI");
    if (cli_.isEmpty())
        cli_ = QStandardPaths::findExecutable("ketatlas");
    if (cli_.isEmpty() || key_.size() != 32) {
        fail("atlas_cli_missing");
        return;
    }
    process_.setWorkingDirectory(atlas_.directory);
    process_.start(cli_, {"--version"});
    timeout_.start(30000);
}
void AtlasLiveRuntime::launch() {
    QTcpServer viewer, renderer;
    if (!viewer.listen(QHostAddress::LocalHost, 0) ||
        !renderer.listen(QHostAddress::LocalHost, 0)) {
        fail("atlas_port_unavailable");
        return;
    }
    viewerPort_ = viewer.serverPort();
    rendererPort_ = renderer.serverPort();
    QStringList args{"serve",          atlas_.path,   "--root",
                     atlas_.directory, "--port",      QString::number(viewerPort_),
                     "--read-only",    "--no-preload"};
    if (QFileInfo::exists(QDir(atlas_.directory).filePath("atlas.renderer.json")))
        args << "--renderer" << "--html-port" << QString::number(rendererPort_);
    else
        rendererPort_ = 0;
    viewer.close();
    renderer.close();
    output_.clear();
    process_.start(cli_, args);
}
void AtlasLiveRuntime::inspectOutput() {
    output_ += process_.readAllStandardOutput();
    if (output_.size() > 32768)
        output_ = output_.right(32768);
    if (checkingVersion_ || launched_ ||
        !output_.contains("KetAtlas: http://127.0.0.1:" + QByteArray::number(viewerPort_)))
        return;
    launched_ = true;
    timeout_.stop();
    attach(relay_, bearer_, std::move(callback_));
}
void AtlasLiveRuntime::attach(QUrl relay, const QString& bearer, Callback callback) {
    relay_ = std::move(relay);
    bearer_ = bearer;
    callback_ = std::move(callback);
    timeout_.stop();
    if (tunnel_) {
        tunnel_->disconnect(this);
        tunnel_->stop();
        tunnel_->deleteLater();
    }
    key_ = AtlasTunnelCodec::randomKey();
    channel_ = QUuid::createUuid().toString(QUuid::Id128);
    if (key_.size() != 32) {
        fail("atlas_key_failed");
        return;
    }
    QHash<QString, quint16> origins{{"viewer", viewerPort_}};
    QJsonArray descriptors{QJsonObject{{"id", "viewer"}, {"port", int(viewerPort_)}}};
    if (rendererPort_) {
        origins.insert("renderer", rendererPort_);
        descriptors.append(QJsonObject{{"id", "renderer"}, {"port", int(rendererPort_)}});
    }
    tunnel_ = new AtlasLiveTunnel(key_, channel_, origins, this);
    QUrlQuery query(relay_);
    query.addQueryItem("channel", channel_);
    query.addQueryItem("repositoryId", property("repositoryId").toString());
    query.addQueryItem("worktreeId", property("worktreeId").toString());
    relay_.setQuery(query);
    connect(tunnel_, &AtlasLiveTunnel::closed, this, [this] {
        // Keep this owned process for a bounded idle lease; reconnect gets a new key.
        if (!stopped_)
            timeout_.start(5 * 60 * 1000);
    });
    tunnel_->connectRelay(relay_, bearer_);
    bearer_.clear();
    if (stopped_)
        return;
    if (callback_) {
        auto callback = std::move(callback_);
        callback({{"ok", true},
                  {"repository_id", property("repositoryId").toString()},
                  {"worktree_id", property("worktreeId").toString()},
                  {"channel", channel_},
                  {"key", QString::fromLatin1(key_.toBase64())},
                  {"origins", descriptors},
                  {"context_revision", revision_},
                  {"title", atlas_.title}});
    }
    // A client that never attaches must not leave a renderer behind.
    timeout_.start(30000);
    connect(tunnel_, &AtlasLiveTunnel::ready, &timeout_, &QTimer::stop);
}
void AtlasLiveRuntime::fail(const QString& code) {
    if (callback_) {
        auto callback = std::move(callback_);
        callback({{"ok", false}, {"error_code", code}});
    }
    stop();
    deleteLater();
}
void AtlasLiveRuntime::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    timeout_.stop();
    if (tunnel_)
        tunnel_->stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        if (!process_.waitForFinished(1500)) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }
    key_.fill(0);
    bearer_.clear();
}
} // namespace ketplus::daemon
