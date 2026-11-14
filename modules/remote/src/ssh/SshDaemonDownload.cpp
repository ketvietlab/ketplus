#include "ssh/SshDaemonInstaller.h"
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <memory>

namespace ketplus::remote::ssh {
void SshDaemonInstaller::fetch(const QUrl& url, qint64 limit, std::function<void(QByteArray)> done,
                               bool archive, int redirects) {
    if (terminal_ || cancelled_)
        return;
    if (!channel_.configured() || !channel_.permits(url) || redirects > 5) {
        fail(tr("The release download URL or redirect is not trusted."));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    auto* reply = network_->get(request);
    reply_ = reply;
    reply->setReadBufferSize(65536);
    const int generation = generation_;
    auto bytes = std::make_shared<QByteArray>();
    auto count = std::make_shared<qint64>(0);
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto file = std::make_shared<QFile>(work_->filePath("runtime.tar.gz"));
    if (archive && (!file->open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                    !file->setPermissions(QFile::ReadOwner | QFile::WriteOwner))) {
        reply->abort();
        reply->deleteLater();
        fail(tr("Unable to create the private release download."));
        return;
    }
    const auto drain = [this, reply, generation, limit, bytes, count, file, hash, archive] {
        if (generation != generation_)
            return;
        // Redirect/error bodies are not release bytes and are never executed or presented.
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            *count += reply->readAll().size();
            if (*count > daemonManifestLimit) {
                reply->abort();
                fail(tr("The release redirect or error response exceeded its size limit."));
            }
            return;
        }
        const auto chunk = reply->readAll();
        *count += chunk.size();
        if (*count > limit || (archive && file->write(chunk) != chunk.size())) {
            reply->abort();
            fail(tr("The release download exceeded its size limit or could not be saved."));
            return;
        }
        if (!archive)
            bytes->append(chunk);
        else
            hash->addData(chunk);
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, limit, url, done = std::move(done), archive, redirects, bytes,
             file, hash, drain] {
                reply->deleteLater();
                if (generation != generation_ || terminal_ || cancelled_)
                    return;
                drain();
                if (file->isOpen())
                    file->close();
                if (terminal_)
                    return;
                const auto next =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
                if (!next.isEmpty()) {
                    fetch(url.resolved(next), limit, done, archive, redirects + 1);
                    return;
                }
                if (reply->error() != QNetworkReply::NoError ||
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                    fail(tr("Unable to download the trusted release. Nothing was installed."));
                    return;
                }
                done(archive ? hash->result() : *bytes);
            });
}
} // namespace ketplus::remote::ssh
