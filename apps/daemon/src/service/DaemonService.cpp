#include "service/DaemonService.h"
#include "DaemonCredentialFile.h"
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
#include "storage/McpMediaStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
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
#if defined(Q_OS_MACOS)
#include <cerrno>
#include <sys/acl.h>
#endif

namespace ketplus::daemon {
namespace {

constexpr QFileDevice::Permissions ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

#if defined(Q_OS_UNIX)
bool restrictDirectory(const QString& path) {
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
}
#endif

#if defined(Q_OS_MACOS)
// An access-control entry such as "everyone allow read", inherited from a parent directory,
// grants what the owner-only mode bits deny. Daemon state must carry none.
// An ACL that cannot be read (for example behind "deny readsecurity") counts as present: the
// owner can still replace it, and a credential under it is not known to be private.
bool hasExtendedAcl(const QString& path) {
    const QByteArray native = QFile::encodeName(path);
    errno = 0;
    acl_t acl = acl_get_link_np(native.constData(), ACL_TYPE_EXTENDED);
    if (acl == nullptr) {
        const int error = errno;
        return error != ENOENT && error != EOPNOTSUPP;
    }
    acl_entry_t entry = nullptr;
    const bool any = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0;
    acl_free(acl);
    return any;
}

bool removeExtendedAcl(const QString& path) {
    if (!hasExtendedAcl(path))
        return true;
    acl_t empty = acl_init(0);
    if (empty == nullptr)
        return false;
    const bool removed =
        acl_set_link_np(QFile::encodeName(path).constData(), ACL_TYPE_EXTENDED, empty) == 0;
    acl_free(empty);
    return removed && !hasExtendedAcl(path);
}

// Media and its references were written before the state directory lost its inheritable
// entries, so each one may still carry them. Links are neither followed nor descended.
bool removeExtendedAclsBelow(const QString& directory) {
    const QFileInfo root(directory);
    if (root.isSymLink() || !root.isDir())
        return true;
    if (!removeExtendedAcl(directory))
        return false;
    QDirIterator entries(directory,
                         QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        if (!removeExtendedAcl(entries.next()))
            return false;
    }
    return true;
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
    // The journal and the credential live here; a directory others can enter or write is not
    // a place to keep or replace them.
    if (!restrictDirectory(stateInfo.absolutePath()) ||
        !restrictDirectory(tokenInfo.absolutePath())) {
        error_ = QStringLiteral("Unable to restrict the daemon state directory to its owner.");
        return false;
    }
#endif
#if defined(Q_OS_MACOS)
    // Before anything is created here, so new files inherit no entries. A linked directory is
    // handled at its target, which is where new files take their entries from.
    if (!removeExtendedAcl(QFileInfo(stateInfo.absolutePath()).canonicalFilePath()) ||
        !removeExtendedAcl(QFileInfo(tokenInfo.absolutePath()).canonicalFilePath())) {
        error_ = QStringLiteral("Unable to remove access-control entries from the daemon state.");
        return false;
    }
#endif
    // One daemon per journal: the startup compaction below replaces the file, which must not
    // happen under another daemon's open connection. A daemon being replaced may still be
    // closing.
    journalLock_ = std::make_unique<QLockFile>(configuration_.statePath + QStringLiteral(".lock"));
    if (!journalLock_->tryLock(5'000)) {
        error_ = QStringLiteral("Another KetPlus Daemon uses this journal.");
        journalLock_.reset();
        return false;
    }
    // Only the daemon that owns the journal may replace the credential a running one serves.
    const QByteArray token = loadOrCreateToken();
    if (token.isEmpty()) {
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
#if defined(Q_OS_MACOS)
    bool journalAclRemoved = removeExtendedAcl(configuration_.statePath);
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")})
        journalAclRemoved =
            journalAclRemoved && removeExtendedAcl(configuration_.statePath + suffix);
    if (!journalAclRemoved ||
        !removeExtendedAclsBelow(McpMediaStore::directoryForJournal(configuration_.statePath))) {
        error_ = QStringLiteral("Unable to remove access-control entries from the daemon state.");
        return false;
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
    connect(claudeCredentials_.get(), &ClaudeCredentials::changed, host_.get(),
            [host = host_.get()] { host->forgetProviderFailure(QStringLiteral("claude")); });
    cloud_ = std::make_unique<CloudControlClient>(journal_.get(), host_.get());
    ipc_ = std::make_unique<LocalIpcServer>(host_.get(), configuration_.socketName, token,
                                            cloud_.get());
    ipc_->setClaudeCredentials(claudeCredentials_.get());
    ipc_->setHostKeyFingerprint([cloud = cloud_.get()] {
        QString error;
        const QString fingerprint = cloud->hostKeyFingerprint(&error);
        // Without it an SSH client cannot name this machine and refuses the hello: say why.
        if (fingerprint.isEmpty())
            qWarning("host identity: key set unavailable, daemon.hello names no fingerprint: %s",
                     qPrintable(error.isEmpty() ? QStringLiteral("unknown error") : error));
        return fingerprint;
    });
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
    if (!journal_->pruneAcknowledgedEvents())
        qWarning("journal: acknowledged delivery GC failed: %s",
                 qPrintable(journal_->errorString()));
    if (!journal_->purgeExpiredAttachments())
        qWarning("journal: staged attachment GC failed: %s", qPrintable(journal_->errorString()));
    if (!journal_->pruneRetention())
        qWarning("journal: retention maintenance failed: %s", qPrintable(journal_->errorString()));
    journal_->reclaimFreePages(reclaimBatchPages);
    const auto changed = journal_->pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc());
    const bool expired = host_ && host_->expireArchivedSessions(QDateTime::currentDateTimeUtc());
    if ((changed && *changed > 0) || expired)
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
#if defined(Q_OS_MACOS)
    // A credential that carried an entry granting others access may already be known to them:
    // replace it rather than trust it. Desktop reads the file again on every connection.
    // Not even its type is checked: a denied "readsecurity" makes stat fail too.
    if (hasExtendedAcl(configuration_.tokenPath) && !QFile::remove(configuration_.tokenPath)) {
        error_ = QStringLiteral("Unable to replace the exposed local daemon credential.");
        return {};
    }
#endif
    const QFileInfo existing(configuration_.tokenPath);
    if (existing.exists() || existing.isSymLink())
        return remote::readLocalDaemonCredential(configuration_.tokenPath, &error_);

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
