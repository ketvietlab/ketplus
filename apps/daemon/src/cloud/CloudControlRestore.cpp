#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
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
#include <QThreadPool>

#include <algorithm>
#include <utility>

// Read-only workspace file commands and transcript restore windows for paired devices.

namespace ketplus::daemon {
using cloud_control::object;
using cloud_control::responseCode;
using cloud_control::sourceContext;

void CloudControlClient::executeWorkspaceFiles(const QJsonObject& command,
                                               const QJsonObject& payload) {
    const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
    const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
    const auto sessions = journal_->sessions();
    const auto match =
        std::find_if(sessions.cbegin(), sessions.cend(), [&](const StoredSession& s) {
            return s.id == sessionId && s.generation == generation;
        });
    // Legacy mobile resource references carry no historical context. Never reinterpret
    // their relative paths in the worktree selected by a local context switch.
    const auto requestedContext = payload.value("resource_context_revision");
    if (match != sessions.cend() && match->contextRevision > 1 && requestedContext.isUndefined()) {
        acknowledgeCommand(command, {{"ok", false}, {"error_code", "context_upgrade_required"}});
        return;
    }
    QString resourceRoot = match == sessions.cend() ? QString{} : match->worktreePath;
    if (payload.contains("resource_event_cursor") || payload.contains("resource_source_path")) {
        const auto resolved = host_->handleCommand(
            {{"protocol_version", 0},
             {"message_id", command.value("id")},
             {"message_type", "session.resource.resolve"},
             {"session_id", sessionId},
             {"session_generation", generation},
             {"sent_at", protocol::timestamp()},
             {"payload", QJsonObject{{"event_cursor", payload.value("resource_event_cursor")},
                                     {"path", payload.value("resource_source_path")}}}},
            "cloud-control-v2");
        if (!resolved.value("ok").toBool() ||
            resolved.value("resource_context_revision") != requestedContext) {
            acknowledgeCommand(command,
                               {{"ok", false}, {"error_code", "resource_context_unknown"}});
            return;
        }
        resourceRoot = resolved.value("worktree_path").toString();
    } else if (!requestedContext.isUndefined()) {
        resourceRoot.clear();
        const qint64 revision = requestedContext.toInteger(-1);
        if (revision > 0 && requestedContext.toDouble() == double(revision)) {
            for (const auto& value : journal_->sessionContexts(sessionId, generation)) {
                const auto saved = value.toObject();
                if (saved.value("context_revision").toInteger() == revision)
                    resourceRoot = saved.value("worktree_path").toString();
            }
        }
        if (resourceRoot.isEmpty() || !QFileInfo(resourceRoot).isDir()) {
            acknowledgeCommand(command,
                               {{"ok", false}, {"error_code", "resource_context_unknown"}});
            return;
        }
    }
    const QJsonObject context =
        resourceRoot.isEmpty()
            ? QJsonObject{}
            : sourceContext(resourceRoot, ensurePathIdSalt(),
                            journal_->worktreeRepositories().value(resourceRoot));
    if (match == sessions.cend() ||
        command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
        command.value(QStringLiteral("repositoryId")) !=
            context.value(QStringLiteral("repositoryId")) ||
        command.value(QStringLiteral("worktreeId")) !=
            context.value(QStringLiteral("worktreeId"))) {
        acknowledgeCommand(command,
                           {{QStringLiteral("ok"), false},
                            {QStringLiteral("error_code"), QStringLiteral("scope_forbidden")}});
        return;
    }
    constexpr int maximumPendingReads = 8;
    if (workspaceFilesPending_.fetch_add(1) >= maximumPendingReads) {
        workspaceFilesPending_.fetch_sub(1);
        acknowledgeCommand(command, {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error_code"), QStringLiteral("busy")}});
        return;
    }
    const WorkspaceFileScope scope{command.value(QStringLiteral("ownerSubjectId")).toString(),
                                   workspaceId_,
                                   machineId_,
                                   command.value(QStringLiteral("repositoryId")).toString(),
                                   command.value(QStringLiteral("worktreeId")).toString(),
                                   resourceRoot};
    QJsonObject request = payload;
    request.insert(QStringLiteral("owner_subject_id"), scope.ownerSubjectId);
    request.insert(QStringLiteral("workspace_id"), scope.workspaceId);
    request.insert(QStringLiteral("machine_id"), scope.machineId);
    request.insert(QStringLiteral("repository_id"), scope.repositoryId);
    request.insert(QStringLiteral("worktree_id"), scope.worktreeId);
    request.insert(QStringLiteral("revision"), command.value(QStringLiteral("workspaceRevision")));
    const QString kind = command.value(QStringLiteral("kind")).toString();
    // The relay only lists the revision this daemon last published, which trails the worktree
    // by up to a heartbeat while an agent edits it. A revision published recently for this
    // session is served as the worktree is now instead of failing as stale; any other one is
    // still checked against the worktree with git.
    const auto fence =
        revisionPublished(sessionId, command.value(QStringLiteral("workspaceRevision")).toString())
            ? ReadOnlyWorkspaceFiles::RevisionFence::Published
            : ReadOnlyWorkspaceFiles::RevisionFence::Live;
    const QPointer<CloudControlClient> guard(this);
    const qint64 resourceContextRevision = requestedContext.toInteger(0);
    workspaceFilesPool_.start([guard, command, request, scope, kind, fence,
                               resourceContextRevision]() {
        ReadOnlyWorkspaceFiles files(scope, fence);
        QJsonObject result;
        if (kind == QStringLiteral("workspace.files.tree"))
            result = files.tree(request);
        else if (kind == QStringLiteral("workspace.files.search"))
            result = files.search(request);
        else if (kind == QStringLiteral("workspace.files.file"))
            result = files.file(request);
        else if (kind == QStringLiteral("workspace.files.diff"))
            result = files.diff(request);
        else
            result = {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("unsupported")}};
        if (!guard)
            return;
        guard->workspaceFilesPending_.fetch_sub(1);
        QMetaObject::invokeMethod(
            guard,
            [guard, command, result, scope, request, resourceContextRevision]() {
                if (!guard)
                    return;
                const auto current = guard->journal_->sessions();
                const auto live =
                    std::find_if(current.cbegin(), current.cend(), [&](const StoredSession& s) {
                        return s.id == command.value(QStringLiteral("sessionId")).toString() &&
                               s.generation ==
                                   command.value(QStringLiteral("sessionGeneration")).toInteger() &&
                               (resourceContextRevision > 0 || s.worktreePath == scope.rootPath);
                    });
                if (live == current.cend()) {
                    guard->acknowledgeCommand(command, {{QStringLiteral("ok"), false},
                                                        {QStringLiteral("error_code"),
                                                         QStringLiteral("stale_revision")}});
                    return;
                }
                if (resourceContextRevision > 0) {
                    bool known = false;
                    for (const auto& value :
                         guard->journal_->sessionContexts(live->id, live->generation)) {
                        const auto context = value.toObject();
                        known |= context.value("context_revision").toInteger() ==
                                     resourceContextRevision &&
                                 context.value("worktree_path").toString() == scope.rootPath;
                    }
                    if (request.contains("resource_event_cursor")) {
                        const auto verified = guard->host_->handleCommand(
                            {{"protocol_version", 0},
                             {"message_id", command.value("id")},
                             {"message_type", "session.resource.resolve"},
                             {"session_id", live->id},
                             {"session_generation", live->generation},
                             {"sent_at", protocol::timestamp()},
                             {"payload",
                              QJsonObject{{"event_cursor", request.value("resource_event_cursor")},
                                          {"path", request.value("resource_source_path")}}}},
                            "cloud-control-v2");
                        known = verified.value("ok").toBool() &&
                                verified.value("resource_context_revision").toInteger() ==
                                    resourceContextRevision &&
                                verified.value("worktree_path").toString() == scope.rootPath;
                    }
                    if (!known) {
                        guard->acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "resource_context_unknown"}});
                        return;
                    }
                }
                guard->acknowledgeCommand(command, result);
            },
            Qt::QueuedConnection);
    });
}

void CloudControlClient::executeRestoreWindow(const QJsonObject& command,
                                              const QJsonObject& payload) {
    const QByteArray scope = CloudCrypto::canonical(
        // Scope 3: pages are cloud-projected; never replay a raw page cached under scope 2.
        {QStringLiteral("ketplus"), 3, QStringLiteral("transcript-restore-cache"),
         command.value(QStringLiteral("id")), command.value(QStringLiteral("ownerSubjectId")),
         command.value(QStringLiteral("workspaceId")), command.value(QStringLiteral("machineId")),
         command.value(QStringLiteral("sessionId")),
         command.value(QStringLiteral("sessionGeneration")),
         command.value(QStringLiteral("credentialEpoch")),
         command.value(QStringLiteral("snapshotCursor")),
         command.value(QStringLiteral("requestedByteCap")),
         command.value(QStringLiteral("continuationOfRequestId")).toString(),
         command.value(QStringLiteral("continuationSequence")).toInteger(0),
         command.value(QStringLiteral("continuationDigest")).toString(),
         command.value(QStringLiteral("deadlineAt")), machineKeyVersion_});
    const QString cacheKey =
        QStringLiteral("restore-response:%1")
            .arg(QString::fromLatin1(
                QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex()));
    const qint64 snapshotCursor = command.value(QStringLiteral("snapshotCursor")).toInteger(-1);
    const qint64 requestedByteCap = command.value(QStringLiteral("requestedByteCap")).toInteger(-1);
    const QString continuationParent =
        command.value(QStringLiteral("continuationOfRequestId")).toString();
    const qint64 continuationSequence =
        command.value(QStringLiteral("continuationSequence")).toInteger(0);
    const QString continuationDigest =
        command.value(QStringLiteral("continuationDigest")).toString();
    const QJsonValue continuationSequenceValue =
        command.value(QStringLiteral("continuationSequence"));
    const bool continuation = continuationSequence > 0;
    const QDateTime deadline =
        QDateTime::fromString(command.value(QStringLiteral("deadlineAt")).toString(), Qt::ISODate);
    if (command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
        workspaceId_.isEmpty() ||
        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
        command.value(QStringLiteral("sessionId")).toString().isEmpty() ||
        command.value(QStringLiteral("sessionGeneration")).toInteger() < 1 ||
        command.value(QStringLiteral("credentialEpoch")).toInteger() < 1 || snapshotCursor < 0 ||
        requestedByteCap < 4096 || requestedByteCap > 512 * 1024 || !deadline.isValid() ||
        deadline <= QDateTime::currentDateTimeUtc() ||
        ((!continuationSequenceValue.isUndefined() && !continuationSequenceValue.isNull()) &&
         (!continuationSequenceValue.isDouble() || continuationSequence < 0 ||
          continuationSequenceValue.toDouble() != static_cast<double>(continuationSequence))) ||
        (continuation ? continuationParent.size() < 16 || continuationParent.size() > 128 ||
                            continuationSequence > 4096 ||
                            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$"))
                                 .match(continuationDigest)
                                 .hasMatch()
                      : !continuationParent.isEmpty() || !continuationDigest.isEmpty())) {
        acknowledgeCommand(
            command, {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")},
                      {QStringLiteral("error_message"),
                       QStringLiteral("The restore request boundary is unavailable.")}});
        return;
    }
    const QJsonObject cached = journal_->commandResult(cacheKey);
    if (!cached.isEmpty()) {
        uploadRestoreResponses(command, cached.value(QStringLiteral("responses")).toArray(),
                               cached.value(QStringLiteral("summary")).toObject());
        return;
    }
    const auto sessions = journal_->sessions();
    const auto match = std::find_if(sessions.cbegin(), sessions.cend(), [&](const auto& session) {
        return session.id == command.value(QStringLiteral("sessionId")).toString() &&
               session.generation == command.value(QStringLiteral("sessionGeneration")).toInteger();
    });
    if (match == sessions.cend()) {
        acknowledgeCommand(command,
                           {{QStringLiteral("ok"), false},
                            {QStringLiteral("error_code"), QStringLiteral("snapshot_changed")},
                            {QStringLiteral("error_message"),
                             QStringLiteral("The local transcript snapshot changed.")}});
        return;
    }

    QJsonObject localResult;
    if (continuation) {
        if (payload.keys() != QStringList{QStringLiteral("large_event")} ||
            !payload.value(QStringLiteral("large_event")).isObject()) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The restore continuation selector is invalid.")}});
            return;
        }
        const QJsonObject selector = payload.value(QStringLiteral("large_event")).toObject();
        QStringList selectorKeys = selector.keys();
        selectorKeys.sort();
        QStringList expectedKeys{QStringLiteral("digest_sha256"), QStringLiteral("item_id"),
                                 QStringLiteral("offset"),        QStringLiteral("revision"),
                                 QStringLiteral("total_bytes"),   QStringLiteral("transfer_id"),
                                 QStringLiteral("version")};
        expectedKeys.sort();
        const QString transferId = selector.value(QStringLiteral("transfer_id")).toString();
        const QString itemId = selector.value(QStringLiteral("item_id")).toString();
        const qint64 revision = selector.value(QStringLiteral("revision")).toInteger(-1);
        const QString digest = selector.value(QStringLiteral("digest_sha256")).toString();
        const qint64 totalBytes = selector.value(QStringLiteral("total_bytes")).toInteger(-1);
        const qint64 offset = selector.value(QStringLiteral("offset")).toInteger(-1);
        const auto safeInteger = [](const QJsonValue& value, const qint64 minimum,
                                    const qint64 maximum) {
            return value.isDouble() && value.toDouble() >= static_cast<double>(minimum) &&
                   value.toDouble() <= static_cast<double>(maximum) &&
                   value.toDouble() == static_cast<double>(value.toInteger(-1));
        };
        if (selectorKeys != expectedKeys ||
            !safeInteger(selector.value(QStringLiteral("version")), 1, 1) || transferId.isEmpty() ||
            transferId.size() > 128 || itemId.isEmpty() || itemId.size() > 256 ||
            !safeInteger(selector.value(QStringLiteral("revision")), 1, snapshotCursor) ||
            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(digest).hasMatch() ||
            !safeInteger(selector.value(QStringLiteral("total_bytes")), 1, 32 * 1024 * 1024) ||
            !safeInteger(selector.value(QStringLiteral("offset")), 0, totalBytes - 1)) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The restore continuation selector is invalid.")}});
            return;
        }
        // Raw large-event bytes are served only for items whose cloud projection is the raw
        // payload; any other item was inlined as its projection in the root page.
        if (largeEventCloudProjection(
                command.value(QStringLiteral("sessionId")).toString(),
                command.value(QStringLiteral("sessionGeneration")).toInteger(), transferId,
                selector)
                .has_value()) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("restore_continuation_conflict")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The retained large transcript item no longer matches.")}});
            return;
        }
        const QJsonObject local{
            {QStringLiteral("protocol_version"), 0},
            {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("message_type"), QStringLiteral("session.transcript_large_event")},
            {QStringLiteral("session_id"), command.value(QStringLiteral("sessionId"))},
            {QStringLiteral("session_generation"),
             command.value(QStringLiteral("sessionGeneration"))},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("payload"),
             QJsonObject{
                 {QStringLiteral("transfer_id"), transferId},
                 {QStringLiteral("offset"), offset},
                 {QStringLiteral("maximum_bytes"),
                  qBound(qint64{1}, ((requestedByteCap - 2048) * 9) / 16, qint64{128 * 1024})}}}};
        localResult = host_->handleCommand(local, QStringLiteral("cloud-control-v2"));
        const QByteArray fragmentData =
            CloudCrypto::fromBase64Url(localResult.value(QStringLiteral("data")).toString());
        const qint64 nextOffset = localResult.value(QStringLiteral("next_offset")).toInteger(-1);
        if (!localResult.value(QStringLiteral("ok")).toBool() ||
            localResult.value(QStringLiteral("session_id")) !=
                command.value(QStringLiteral("sessionId")) ||
            localResult.value(QStringLiteral("session_generation")) !=
                command.value(QStringLiteral("sessionGeneration")) ||
            localResult.value(QStringLiteral("transfer_id")).toString() != transferId ||
            localResult.value(QStringLiteral("item_id")).toString() != itemId ||
            localResult.value(QStringLiteral("revision")).toInteger(-1) != revision ||
            localResult.value(QStringLiteral("digest_sha256")).toString() != digest ||
            localResult.value(QStringLiteral("total_bytes")).toInteger(-1) != totalBytes ||
            localResult.value(QStringLiteral("offset")).toInteger(-1) != offset ||
            fragmentData.isEmpty() || nextOffset != offset + fragmentData.size() ||
            nextOffset > totalBytes ||
            localResult.value(QStringLiteral("complete")).toBool() != (nextOffset == totalBytes)) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("restore_continuation_conflict")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The retained large transcript item no longer matches.")}});
            return;
        }
    } else {
        if (payload.contains(QStringLiteral("large_event"))) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral(
                      "A root restore request cannot contain a continuation selector.")}});
            return;
        }
        QJsonObject windowPayload = payload;
        windowPayload.remove(QStringLiteral("projection_revision"));
        windowPayload.insert(QStringLiteral("snapshot_cursor"), snapshotCursor);
        windowPayload.insert(
            QStringLiteral("event_limit"),
            qBound(1, payload.value(QStringLiteral("event_limit")).toInt(128), 128));
        windowPayload.insert(QStringLiteral("turn_limit"),
                             qBound(1, payload.value(QStringLiteral("turn_limit")).toInt(3), 3));
        const QJsonObject local{
            {QStringLiteral("protocol_version"), 0},
            {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("message_type"), QStringLiteral("session.transcript_window")},
            {QStringLiteral("session_id"), command.value(QStringLiteral("sessionId"))},
            {QStringLiteral("session_generation"),
             command.value(QStringLiteral("sessionGeneration"))},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("payload"), windowPayload}};
        localResult = host_->restoreTranscriptWindow(local);
        if (localResult.value(QStringLiteral("ok")).toBool()) {
            // The restore page leaves the machine: apply the same cloud projection as event
            // upload before fragmenting, so budgets and digests cover the projected bytes.
            const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
            const qint64 generation =
                command.value(QStringLiteral("sessionGeneration")).toInteger();
            localResult = projectTranscriptWindowForCloud(
                localResult, [&](const QString& transferId, const QJsonObject& manifest) {
                    return largeEventCloudProjection(sessionId, generation, transferId, manifest);
                });
        }
    }
    if (!localResult.value(QStringLiteral("ok")).toBool()) {
        acknowledgeCommand(command, localResult);
        return;
    }
    const QByteArray page = QJsonDocument(localResult).toJson(QJsonDocument::Compact);
    const QString digest =
        QString::fromLatin1(QCryptographicHash::hash(page, QCryptographicHash::Sha256).toHex());
    constexpr qsizetype fragmentBytes = 48 * 1024;
    QJsonArray responses;
    qint64 encryptedBytes = 0;
    QString cryptoError;
    for (qsizetype offset = 0; offset < page.size(); offset += fragmentBytes) {
        const QByteArray fragment = page.mid(offset, fragmentBytes);
        const bool terminal = offset + fragment.size() == page.size();
        const qint64 cursor = responses.size() + 1;
        const QJsonObject plain{{QStringLiteral("restore_response_version"), 1},
                                {QStringLiteral("encoding"), QStringLiteral("json-utf8-fragment")},
                                {QStringLiteral("offset"), offset},
                                {QStringLiteral("total_bytes"), page.size()},
                                {QStringLiteral("data"), QString::fromLatin1(fragment.toBase64(
                                                             QByteArray::Base64UrlEncoding |
                                                             QByteArray::OmitTrailingEquals))}};
        const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
        const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
        const QJsonObject shell{
            {QStringLiteral("keyId"), QStringLiteral("%1:%2:1").arg(sessionId).arg(generation)},
            {QStringLiteral("keyEpoch"), 1},
            {QStringLiteral("senderDeviceId"), machineId_},
            {QStringLiteral("senderKeyVersion"), machineKeyVersion_}};
        const QByteArray aad = restoreResponseAssociatedData(command, cursor, terminal,
                                                             terminal ? digest : QString{}, shell);
        const QJsonObject encrypted =
            crypto_.encrypt(QJsonDocument(plain).toJson(QJsonDocument::Compact), aad, sessionId,
                            generation, 1, machineId_, machineKeyVersion_, &cryptoError);
        if (encrypted.isEmpty()) {
            lastError_ = cryptoError;
            return;
        }
        encryptedBytes +=
            CloudCrypto::fromBase64Url(encrypted.value(QStringLiteral("ciphertext")).toString())
                .size();
        if (encryptedBytes > requestedByteCap) {
            acknowledgeCommand(
                command, {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")},
                          {QStringLiteral("error_message"),
                           QStringLiteral("The bounded restore page is too large.")}});
            return;
        }
        responses.append(
            QJsonObject{{QStringLiteral("responseCursor"), cursor},
                        {QStringLiteral("terminal"), terminal},
                        {QStringLiteral("responseDigest"), terminal ? digest : QString{}},
                        {QStringLiteral("content"), encrypted}});
    }
    if (responses.isEmpty()) {
        acknowledgeCommand(
            command, {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")}});
        return;
    }
    const QJsonObject summary{{QStringLiteral("ok"), true},
                              {QStringLiteral("response_cursor"), responses.size()},
                              {QStringLiteral("snapshot_cursor"), snapshotCursor},
                              {QStringLiteral("response_digest"), digest}};
    if (!journal_->storeCommandResult(cacheKey, {{QStringLiteral("responses"), responses},
                                                 {QStringLiteral("summary"), summary}})) {
        lastError_ = journal_->errorString();
        return;
    }
    uploadRestoreResponses(command, responses, summary);
}

std::optional<QJsonObject>

CloudControlClient::largeEventCloudProjection(const QString& sessionId, const qint64 generation,
                                              const QString& transferId,
                                              const QJsonObject& manifest) {
    constexpr qsizetype maximumInlineProjectionBytes = 64 * 1024;
    const QString itemId = manifest.value(QStringLiteral("item_id")).toString();
    const QJsonObject omitted{{QStringLiteral("item_id"), itemId},
                              {QStringLiteral("cloud_omitted"), true}};
    // Reassemble from the journal (bounded by the 32 MiB large-event cap) and verify the digest.
    QByteArray bytes;
    QString eventType;
    QString digest;
    for (qint64 offset = 0;;) {
        const LargeEventFragment fragment =
            journal_->largeEventFragment(sessionId, generation, transferId, offset);
        if (!fragment.errorCode.isEmpty() || fragment.data.isEmpty() || fragment.offset != offset ||
            (offset > 0 && fragment.digestSha256 != digest))
            return omitted;
        if (offset == 0) {
            const QString key = QStringLiteral("%1:%2:%3:%4")
                                    .arg(sessionId)
                                    .arg(generation)
                                    .arg(transferId, fragment.digestSha256);
            const auto cached = largeEventCloudDecisions_.constFind(key);
            if (cached != largeEventCloudDecisions_.constEnd())
                return cached.value();
            eventType = fragment.eventType;
            digest = fragment.digestSha256;
            bytes.reserve(fragment.totalBytes);
        }
        bytes.append(fragment.data);
        offset = fragment.nextOffset;
        if (fragment.complete)
            break;
    }
    const QString key =
        QStringLiteral("%1:%2:%3:%4").arg(sessionId).arg(generation).arg(transferId, digest);
    std::optional<QJsonObject> decision;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            digest ||
        !document.isObject()) {
        decision = omitted;
    } else {
        const QJsonObject raw = document.object();
        const QJsonObject projected = projectEventPayloadForCloud(eventType, raw);
        if (projected != raw) {
            decision = QJsonDocument(projected).toJson(QJsonDocument::Compact).size() <=
                               maximumInlineProjectionBytes
                           ? projected
                           : omitted;
        }
    }
    largeEventCloudDecisions_.insert(key, decision);
    return decision;
}

void CloudControlClient::uploadRestoreResponses(const QJsonObject& command,
                                                const QJsonArray& responses,
                                                const QJsonObject& localResult) {
    const auto peerId = command.value("_peerConnection").toString();
    if (!peerId.isEmpty()) {
        if (!peers_->authorized(peerId, command))
            return;
        auto result = localResult;
        result.insert("responses", responses);
        acknowledgeCommand(command, result);
        return;
    }
    const QString path = QStringLiteral("/v2/restore-requests/%1/responses:append")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                 command.value(QStringLiteral("id")).toString())));
    const QJsonObject body{
        {QStringLiteral("attempt"), command.value(QStringLiteral("attempt"))},
        {QStringLiteral("claimToken"), command.value(QStringLiteral("claimToken"))},
        {QStringLiteral("responses"), responses}};
    request("POST", path, body, daemonToken_, {},
            [this, command, localResult](const int status, const QJsonValue& value,
                                         const QString& transportError) {
                if (status != 202 || !transportError.isEmpty()) {
                    lastError_ =
                        transportError.isEmpty()
                            ? responseCode(value, QStringLiteral("restore_response_upload_failed"))
                            : transportError;
                    return;
                }
                acknowledgeCommand(command, localResult);
            });
}

} // namespace ketplus::daemon
