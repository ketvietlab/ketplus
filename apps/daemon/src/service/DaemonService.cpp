#include "service/DaemonService.h"
#include "DaemonPaths.h"
#include "providers/ClaudeAccountUsage.h"
#include "providers/ClaudeCredentials.h"

#include "cloud/CloudControlClient.h"
#include "ipc/LocalIpcServer.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/JournalSpace.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {

constexpr QFileDevice::Permissions ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

#if defined(Q_OS_UNIX)
void restrictDirectory(const QString& path) {
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
#endif

} // namespace

DaemonService::DaemonService(DaemonConfiguration configuration, QObject* parent)
    : QObject(parent), configuration_(std::move(configuration)) {}

DaemonService::~DaemonService() {
    // Provider callbacks capture SessionHost and, for queries, LocalIpcServer.
    // Drain them while both owners are still alive instead of relying on the
    // reverse member-destruction order.
    if (adapter_) {
        adapter_->stop();
    }
    if (claude_) {
        claude_->stop();
    }
    if (cursor_) {
        cursor_->stop();
    }
    ipc_.reset();
    cloud_.reset();
    host_.reset();
    cursor_.reset();
    claude_.reset();
    adapter_.reset();
    journal_.reset();
}

bool DaemonService::start() {
    const QFileInfo stateInfo(configuration_.statePath);
    const QFileInfo tokenInfo(configuration_.tokenPath);
    if (!QDir().mkpath(stateInfo.absolutePath()) || !QDir().mkpath(tokenInfo.absolutePath())) {
        error_ = QStringLiteral("Unable to create the daemon state directory.");
        return false;
    }
#if defined(Q_OS_UNIX)
    restrictDirectory(stateInfo.absolutePath());
    restrictDirectory(tokenInfo.absolutePath());
#endif
    const QByteArray token = loadOrCreateToken();
    if (token.isEmpty()) {
        return false;
    }
    // One daemon per journal: the startup compaction below replaces the file, which must not
    // happen under another daemon's open connection. A daemon being replaced may still be
    // closing.
    journalLock_ = std::make_unique<QLockFile>(configuration_.statePath + QStringLiteral(".lock"));
    if (!journalLock_->tryLock(5'000)) {
        error_ = QStringLiteral("Another KetPlus Daemon uses this journal.");
        journalLock_.reset();
        return false;
    }
    compactJournal();
    journal_ = std::make_unique<LocalJournal>(configuration_.statePath);
    if (!journal_->isOpen()) {
        error_ = journal_->errorString();
        return false;
    }
#if defined(Q_OS_UNIX)
    if (!QFile::setPermissions(configuration_.statePath, ownerFile)) {
        error_ = QStringLiteral("Unable to protect the local daemon journal.");
        return false;
    }
    // WAL mode keeps recent transcript pages in sidecar files created with the process umask.
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString sidecar = configuration_.statePath + suffix;
        if (QFileInfo::exists(sidecar) && !QFile::setPermissions(sidecar, ownerFile)) {
            error_ = QStringLiteral("Unable to protect the local daemon journal.");
            return false;
        }
    }
#endif
    adapter_ = std::make_unique<CodexAdapter>(configuration_.codexExecutable);
    claudeCredentials_ = std::make_unique<ClaudeCredentials>(
        ClaudeCredentials::keychainStorage(QFileInfo(configuration_.statePath).absoluteFilePath()));
    auto* accountUsage = new ClaudeAccountUsage(
        ClaudeCredentials::keychainStorage(QFileInfo(configuration_.statePath).absoluteFilePath() +
                                           "#admin-analytics"),
        claudeCredentials_.get());
    claudeCredentials_->setAccountUsage(accountUsage);
    claude_ = std::make_unique<ClaudeAdapter>(configuration_.claudeExecutable);
    claude_->setCredentials(claudeCredentials_.get());
    cursor_ = std::make_unique<CursorAdapter>(configuration_.cursorExecutable);
    host_ =
        std::make_unique<SessionHost>(journal_.get(), adapter_.get(), claude_.get(), cursor_.get());
    host_->setClaudeAccountUsage(accountUsage);
    cloud_ = std::make_unique<CloudControlClient>(journal_.get(), host_.get());
    ipc_ = std::make_unique<LocalIpcServer>(host_.get(), configuration_.socketName, token,
                                            cloud_.get());
    ipc_->setClaudeCredentials(claudeCredentials_.get());
    connect(ipc_.get(), &LocalIpcServer::shutdownRequested, QCoreApplication::instance(),
            &QCoreApplication::quit, Qt::QueuedConnection);
    if (!ipc_->listen()) {
        error_ = QStringLiteral("Unable to listen on the local daemon endpoint: %1")
                     .arg(ipc_->errorString());
        return false;
    }
    // ADR 0027: agents reach this endpoint through `ketplus_daemon --mcp-agents`.
    host_->setAgentEndpoint(ipc_->fullServerName(), QCoreApplication::applicationFilePath());
    cloud_->start();
    spaceTimer_ = new QTimer(this);
    spaceTimer_->setInterval(reclaimIntervalMs);
    connect(spaceTimer_, &QTimer::timeout, this, &DaemonService::reclaimJournalSpace);
    spaceTimer_->start();
    return true;
}

void DaemonService::compactJournal() {
    const auto megabytes = [](const qint64 bytes) { return double(bytes) / (1024.0 * 1024.0); };
    const auto result = journal_space::migrateToIncrementalVacuum(configuration_.statePath);
    switch (result.outcome) {
    case journal_space::Migration::NotNeeded:
        return;
    case journal_space::Migration::Migrated:
        qInfo("journal: compacted %.0f MB to %.0f MB; free pages are now reclaimed while idle",
              megabytes(result.before.pageCount * result.before.pageSize),
              megabytes(result.after.pageCount * result.after.pageSize));
        return;
    case journal_space::Migration::Deferred:
    case journal_space::Migration::Failed:
        // The journal keeps working as it was; the next start tries again.
        qWarning("journal: compaction %s: %s",
                 result.outcome == journal_space::Migration::Deferred ? "deferred" : "failed",
                 qPrintable(result.error));
        return;
    }
}

void DaemonService::reclaimJournalSpace() {
    if (!journal_ || (host_ && host_->hasActiveSessions()))
        return;
    journal_->reclaimFreePages(reclaimBatchPages);
}

QString DaemonService::errorString() const { return error_; }

DaemonConfiguration DaemonService::defaultConfiguration() {
    return {remote::DaemonPaths::state(),
            remote::DaemonPaths::token(),
            remote::DaemonPaths::socket(),
            {},
            {},
            {}};
}

QByteArray DaemonService::loadOrCreateToken() {
    QFile existing(configuration_.tokenPath);
    if (existing.exists()) {
#if defined(Q_OS_UNIX)
        constexpr QFileDevice::Permissions unsafePermissions =
            QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        const QFileInfo tokenInfo(configuration_.tokenPath);
        // A symlink or another account's file is not a credential this daemon created.
        if (tokenInfo.isSymLink() || !tokenInfo.isFile() || tokenInfo.ownerId() != ::getuid() ||
            (tokenInfo.permissions() & unsafePermissions) != 0) {
            error_ = QStringLiteral("The local daemon credential has unsafe permissions.");
            return {};
        }
#endif
        if (!existing.open(QIODevice::ReadOnly)) {
            error_ = QStringLiteral("Unable to read the local daemon credential.");
            return {};
        }
        const QByteArray token = existing.readAll().trimmed();
        if (token.size() < 32) {
            error_ = QStringLiteral("The local daemon credential is invalid.");
            return {};
        }
        return token;
    }

    QByteArray random(32, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < random.size(); offset += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        const qsizetype count = qMin<qsizetype>(4, random.size() - offset);
        for (qsizetype byte = 0; byte < count; ++byte) {
            random[offset + byte] = static_cast<char>((value >> (byte * 8)) & 0xffU);
        }
    }
    const QByteArray token =
        random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QSaveFile file(configuration_.tokenPath);
    // Restrict the temporary file before the token is written, not after it is renamed in.
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(ownerFile) ||
        file.write(token) != token.size() || !file.commit() ||
        !QFile::setPermissions(configuration_.tokenPath, ownerFile)) {
        error_ = QStringLiteral("Unable to create the local daemon credential.");
        return {};
    }
    return token;
}

} // namespace ketplus::daemon
