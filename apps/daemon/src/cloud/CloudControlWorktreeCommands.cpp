#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/ControlEndpoint.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include <QUrlQuery>

#include "KetPlusProductVersion.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "workspace/ReadOnlyWorkspaceFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThreadPool>

#include <algorithm>
#include <utility>

// Commands scoped to a catalog worktree rather than a session: creating a conversation and
// reading branches or configuration.

namespace ketplus::daemon {
using cloud_control::sourceContext;

namespace {

// Branch names up to a budget that keeps the sealed list under the relay's 512 KiB result.
QJsonArray boundedNames(const QStringList& names, qsizetype* budget, bool* truncated) {
    QJsonArray bounded;
    for (const QString& name : names) {
        const qsizetype size = name.toUtf8().size() + 4;
        if (size > *budget) {
            *truncated = true;
            break;
        }
        *budget -= size;
        bounded.append(name);
    }
    return bounded;
}

} // namespace

void CloudControlClient::executeWorktreeCommand(const QJsonObject& command,
                                                const QJsonObject& payload,
                                                const QByteArray& senderAgreementKey) {
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    const QString repositoryId = command.value(QStringLiteral("repositoryId")).toString();
    const QString worktreeId = command.value(QStringLiteral("worktreeId")).toString();
    const QByteArray salt = ensurePathIdSalt();
    QString worktreePath;
    for (const QString& candidate : catalogWorktreePaths()) {
        const QJsonObject scope = sourceContext(candidate, salt);
        if (scope.value(QStringLiteral("repositoryId")).toString() == repositoryId &&
            scope.value(QStringLiteral("worktreeId")).toString() == worktreeId) {
            worktreePath = candidate;
            break;
        }
    }
    const bool listing =
        command.value(QStringLiteral("kind")) == QStringLiteral("worktree.branches.read");
    const bool configuring =
        command.value(QStringLiteral("kind")) == QStringLiteral("worktree.configuration.read");
    const QJsonObject newWorktree = payload.value(QStringLiteral("new_worktree")).toObject();
    const QString branch = newWorktree.value(QStringLiteral("branch")).toString().trimmed();
    const QString base = newWorktree.value(QStringLiteral("base")).toString().trimmed();
    if (command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        !command.value(QStringLiteral("sessionId")).toString().isEmpty() ||
        command.value(QStringLiteral("sessionGeneration")).toInteger() != 0 ||
        content.value(QStringLiteral("recipientDeviceId")).toString() != machineId_ ||
        content.value(QStringLiteral("recipientKeyVersion")).toInt() != machineKeyVersion_ ||
        command.value(QStringLiteral("expectedCatalogRevision")).toInteger(-1) != 0 ||
        command.value(QStringLiteral("expectedLifecycleRevision")).toInteger(-1) != 0 ||
        worktreePath.isEmpty() || !QFileInfo(worktreePath).isDir() ||
        (!listing && !configuring &&
         payload.value(QStringLiteral("prompt")).toString().trimmed().isEmpty()) ||
        (!newWorktree.isEmpty() && (branch.isEmpty() || base.isEmpty()))) {
        acknowledgeCommand(
            command,
            {{QStringLiteral("ok"), false},
             {QStringLiteral("error_code"), QStringLiteral("worktree_scope_changed")},
             {QStringLiteral("error_message"),
              QStringLiteral("The enrolled worktree scope changed.")}},
            senderAgreementKey);
        return;
    }
    if (configuring) {
        // The catalog scope this host holds for the worktree, inside the signed result: a paired
        // Desktop verifies a worktree index entry with no conversation by it (ADR 0119).
        const QJsonObject scope = sourceContext(worktreePath, salt);
        QJsonObject worktree;
        for (const auto* key : {"repositoryId", "worktreeId", "repositoryLabel", "worktreeLabel"})
            worktree.insert(QLatin1String(key), scope.value(QLatin1String(key)));
        worktree.insert(QStringLiteral("catalogRevision"), 0);
        worktree.insert(QStringLiteral("lifecycleRevision"), 0);
        // What the phone can choose before the first message: the models, efforts and
        // permissions a conversation started here would begin with.
        host_->handleProviderQuery(
            QJsonObject{
                {QStringLiteral("message_type"), QStringLiteral("provider.configuration_catalog")},
                {QStringLiteral("payload"), QJsonObject{}}},
            [this, command, senderAgreementKey, worktree](const QJsonObject& models,
                                                          const QString& providerError) {
                QJsonObject result =
                    providerError.isEmpty()
                        ? host_->newConversationConfiguration(
                              models.value(QStringLiteral("catalog")).toArray())
                        : QJsonObject{
                              {QStringLiteral("ok"), false},
                              {QStringLiteral("error_code"),
                               QStringLiteral("provider_capabilities_unavailable")},
                              {QStringLiteral("error_message"),
                               QStringLiteral("The available settings could not be read.")}};
                result.insert(QStringLiteral("worktree"), worktree);
                acknowledgeCommand(command, result, senderAgreementKey);
            });
        return;
    }
    if (!listing && newWorktree.isEmpty()) {
        startCreatedConversation(command, payload, worktreePath, senderAgreementKey);
        return;
    }
    const QString repository = worktreeRepository(worktreePath);
    if (!listing) {
        // A command redelivered after its acknowledgement was lost answers with what the first
        // attempt journaled, instead of failing on the branch that attempt created (#86).
        if (const QJsonObject cached =
                journal_->commandResult(command.value(QStringLiteral("id")).toString());
            !cached.isEmpty()) {
            acknowledgeCommand(command, cached, senderAgreementKey);
            return;
        }
        // Every checkout of a repository shares its allowance, including linked checkouts of a
        // bare repository, which have no common main checkout.
        const QString commonDirectory = worktreeCommonDirectory(worktreePath);
        const QString refusal = worktreeQuota_.reserve(
            commonDirectory.isEmpty() ? repository : commonDirectory,
            QDateTime::currentMSecsSinceEpoch(),
            QStorageInfo(repository).isValid() ? QStorageInfo(repository).bytesAvailable() : -1);
        if (!refusal.isEmpty()) {
            qWarning("command %s refused: remote worktree quota",
                     qUtf8Printable(command.value(QStringLiteral("id")).toString()));
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("worktree_quota_exceeded")},
                 {QStringLiteral("error_message"), refusal}},
                senderAgreementKey);
            return;
        }
    }
    // Git can take seconds on a large repository, so it runs off the main thread; the
    // answer is dropped if the Mac left the account meanwhile.
    const quint64 requestEpoch = accountEpoch_;
    const QPointer<CloudControlClient> guard(this);
    workspaceFilesPool_.start([guard, command, payload, senderAgreementKey, repository, listing,
                               branch, base, salt, requestEpoch]() {
        QJsonObject result;
        QString createdPath;
        if (listing) {
            const RepositoryBranches branches = readRepositoryBranches(repository);
            if (!branches.ok) {
                result = {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), QStringLiteral("branches_unavailable")},
                          {QStringLiteral("error_message"),
                           QStringLiteral("Could not read the repository's branches.")}};
            } else {
                // Each checkout's branch, which the relay's catalog does not carry.
                QJsonArray worktrees;
                for (const QString& checkout : repositoryCheckouts(repository)) {
                    const QJsonObject scope = sourceContext(checkout, salt);
                    worktrees.append(QJsonObject{
                        {QStringLiteral("worktree_id"), scope.value(QStringLiteral("worktreeId"))},
                        {QStringLiteral("branch"), checkoutBranch(checkout)},
                        {QStringLiteral("folder"), QFileInfo(checkout).fileName()},
                        {QStringLiteral("primary"), checkout == repository}});
                }
                qsizetype budget = 256 * 1024;
                bool truncated = false;
                const QJsonArray local = boundedNames(branches.local, &budget, &truncated);
                const QJsonArray remote = boundedNames(branches.remote, &budget, &truncated);
                result = {{QStringLiteral("ok"), true},
                          {QStringLiteral("local"), local},
                          {QStringLiteral("remote"), remote},
                          {QStringLiteral("truncated"), truncated},
                          {QStringLiteral("worktrees"), worktrees}};
            }
        } else {
            const AddedWorktree added = addWorktree(repository, branch, base);
            createdPath = added.path;
            if (!added.errorCode.isEmpty())
                result = {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), added.errorCode},
                          {QStringLiteral("error_message"), added.message}};
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, command, payload, senderAgreementKey, result, createdPath, requestEpoch]() {
                if (!guard || guard->accountEpoch_ != requestEpoch)
                    return;
                if (!createdPath.isEmpty())
                    guard->startCreatedConversation(command, payload, createdPath,
                                                    senderAgreementKey);
                else
                    guard->acknowledgeCommand(command, result, senderAgreementKey);
            },
            Qt::QueuedConnection);
    });
}

void CloudControlClient::startCreatedConversation(const QJsonObject& command,
                                                  const QJsonObject& payload,
                                                  const QString& worktreePath,
                                                  const QByteArray& senderAgreementKey) {
    const QJsonObject local{
        {QStringLiteral("protocol_version"), 0},
        {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
        {QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
        {QStringLiteral("sent_at"), protocol::timestamp()},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("prompt"),
              payload.value(QStringLiteral("prompt")).toString().trimmed()},
             {QStringLiteral("worktree_path"), worktreePath},
             {QStringLiteral("options"), payload.value(QStringLiteral("options")).toObject()}}}};
    acknowledgeCommand(command, host_->handleCommand(local, QStringLiteral("cloud-control-v2")),
                       senderAgreementKey);
}

} // namespace ketplus::daemon
