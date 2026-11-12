#include "RemoteControlFixture.h"
#include <QSet>
#include <QUrlQuery>

// Sealed session/worktree projection and transcript paging over the synthetic loopback relay.
namespace {
QString hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString b64(const QByteArray& bytes) { return QString::fromLatin1(CloudCrypto::base64Url(bytes)); }

QJsonObject sealedMetadata(CloudCrypto& host, const QString& id, int generation, int cursor,
                           const QJsonObject& metadata, int senderKeyVersion = 1) {
    const auto keyId = QStringLiteral("%1:%2:1").arg(id).arg(generation);
    const auto aad =
        CloudCrypto::canonical({"ketplus", 2, "session-metadata", "workspace", "linux-host", id,
                                generation, cursor, 1, keyId, 1, "linux-host", senderKeyVersion});
    return {{"cursor", cursor},
            {"turnSequence", 1},
            {"content", host.encrypt(QJsonDocument(metadata).toJson(QJsonDocument::Compact), aad,
                                     id, generation, 1, "linux-host", senderKeyVersion)}};
}
QJsonObject indexRow(const QString& id, int generation, const QString& machine,
                     const QString& worktree) {
    return {{"id", id},
            {"generation", generation},
            {"workspaceId", "workspace"},
            {"machineId", machine},
            {"adapter", "codex"},
            {"title", "Relay title"},
            {"state", "ready"},
            {"lastEventCursor", 9},
            {"currentTurnSequence", 1},
            {"repositoryId", "repo-1"},
            {"worktreeId", worktree},
            // As the relay's sessionDto when the host sealed nothing.
            {"encryptedMetadata", QJsonValue::Null}};
}
const QJsonObject metadata{{"branch", "feature/sealed"},
                           {"machineName", "builder"},
                           {"worktreeExists", true},
                           {"lastTurnStartedAt", "2026-11-12T08:00:00.000Z"},
                           {"lastTurnOutcome", "completed"},
                           {"title", "Sealed title"},
                           {"state", "running"},
                           {"repositoryId", "repo-1"},
                           {"repositoryLabel", "ketplus"},
                           {"worktreeId", "wt-a"},
                           {"worktreeLabel", "ketplus"}};

// Plays the paired host behind the relay: opens each sealed restore request and answers with a
// fragmented, host-signed page, as CloudControlClient::executeRestoreWindow does.
struct RestoreHost {
    explicit RestoreHost(EncryptedFixture& host) : fixture(host) {}
    EncryptedFixture& fixture;
    QByteArray clientSigning;
    QByteArray large;
    QHash<QString, QJsonObject> requests;
    QHash<QString, QJsonArray> responses;
    QHash<QString, QString> digests;
    QList<QJsonObject> payloads;
    bool tamper{false};
    int senderKeyVersion{1};
    // The host refuses a root page with more events, as it does one too large for its byte cap.
    int maximumEvents{128};
    QSet<QString> failed;
    // The relay answers a failed restore request 410 restore_window_unavailable
    // (readTranscriptRestoreResponses); false serves the 200 page with status "failed" instead.
    bool failedAs410{true};
    QStringList cancelled;
    // As the relay's commandDto: a missing optional field is null, never absent.
    static QJsonValue orNull(const QJsonObject& command, const char* key) {
        return command.contains(QLatin1String(key)) ? command.value(QLatin1String(key))
                                                    : QJsonValue(QJsonValue::Null);
    }

    QJsonObject event(int cursor, int turn, const QJsonObject& payload,
                      const QString& type = "assistant.message") const {
        return {{"session_id", "opaque-session"}, {"session_generation", 3}, {"cursor", cursor},
                {"turn_sequence", turn},          {"event_type", type},      {"payload", payload}};
    }
    QJsonObject reference() const {
        return {{"version", 1},
                {"encoding", "json-utf8"},
                {"transfer_id", "t-big"},
                {"event_type", "item.completed"},
                {"item_id", "item-big"},
                {"digest_sha256", hex(large)},
                {"total_bytes", large.size()},
                {"revision", 8}};
    }
    QJsonObject page(const QJsonObject& payload) const {
        if (payload.contains("large_event")) {
            const auto selector = payload.value("large_event").toObject();
            const auto offset = selector.value("offset").toInteger();
            const auto data = large.mid(offset, 100);
            const auto next = offset + data.size();
            return {{"ok", true},
                    {"large_event_fragment_version", 1},
                    {"session_id", "opaque-session"},
                    {"session_generation", 3},
                    {"transfer_id", "t-big"},
                    {"event_type", "item.completed"},
                    {"item_id", "item-big"},
                    {"revision", 8},
                    {"digest_sha256", hex(large)},
                    {"total_bytes", large.size()},
                    {"offset", offset},
                    {"next_offset", next},
                    {"complete", next == large.size()},
                    {"data", b64(data)}};
        }
        if (payload.value("before_cursor").toInteger() == 0)
            return {
                {"ok", true},
                {"transcript_window_version", 1},
                {"session_id", "opaque-session"},
                {"session_generation", 3},
                {"snapshot_cursor", 9},
                {"projection_revision", 1},
                {"before_cursor", 7},
                {"has_more_before", true},
                {"events", QJsonArray{event(7, 2, {{"text", "newest prompt"}}),
                                      event(8, 2, {{"large_event", reference()}}, "item.completed"),
                                      event(9, 2, {{"text", "newest answer"}})}},
                {"partial_items", QJsonArray{QJsonObject{{"transfer_id", "t-big"}}}}};
        return {{"ok", true},
                {"transcript_window_version", 1},
                {"session_id", "opaque-session"},
                {"session_generation", 3},
                {"snapshot_cursor", 9},
                {"projection_revision", 1},
                {"before_cursor", 3},
                {"has_more_before", false},
                {"events", QJsonArray{event(3, 1, {{"text", "first prompt"}}),
                                      event(5, 1, {{"text", "first answer"}})}}};
    }
    std::optional<std::pair<int, QJsonObject>> answer(const QNetworkRequest& request,
                                                      const QByteArray& body) {
        const auto path = request.url().path();
        if (path == prefix + "/v2/mobile/sessions/opaque-session/commands")
            return accept(QJsonDocument::fromJson(body).object());
        const QString responsesPrefix = prefix + "/v2/mobile/restore-requests/";
        if (path.startsWith(responsesPrefix) && path.endsWith("/cancel")) {
            cancelled.append(
                path.mid(responsesPrefix.size()).chopped(QStringLiteral("/cancel").size()));
            return std::pair{200, QJsonObject{{"ok", true}}};
        }
        if (!path.startsWith(responsesPrefix) || !path.endsWith("/responses"))
            return std::nullopt;
        const auto id =
            path.mid(responsesPrefix.size()).chopped(QStringLiteral("/responses").size());
        const auto command = requests.value(id);
        const auto after = QUrlQuery(request.url()).queryItemValue("afterCursor").toLongLong();
        if (failed.contains(id) && failedAs410)
            return std::pair{410, QJsonObject{{"code", "restore_window_unavailable"},
                                              {"message", "restore_window_unavailable"}}};
        QJsonArray selected;
        for (const auto& value : responses.value(id))
            if (value.toObject().value("responseCursor").toInteger() > after)
                selected.append(value);
        return std::pair{
            200,
            QJsonObject{{"requestId", id},
                        {"ownerSubjectId", "fixture-owner"},
                        {"sessionId", "opaque-session"},
                        {"sessionGeneration", 3},
                        {"snapshotCursor", command.value("snapshotCursor")},
                        {"continuationOfRequestId", orNull(command, "continuationOfRequestId")},
                        {"continuationSequence", command.value("continuationSequence")},
                        {"continuationDigest", orNull(command, "continuationDigest")},
                        {"afterCursor", after},
                        {"nextCursor", responses.value(id).size()},
                        {"status", failed.contains(id) ? "failed" : "completed"},
                        {"responses", selected}}};
    }
    std::pair<int, QJsonObject> accept(QJsonObject command) {
        // The relay adds the routing scope and the owner it authenticated.
        command.insert("workspaceId", "workspace");
        command.insert("machineId", "linux-host");
        command.insert("sessionId", "opaque-session");
        command.insert("capability", "transcript.restore_window");
        command.insert("ownerSubjectId", "fixture-owner");
        const auto content = command.value("content").toObject();
        QString error;
        const auto plaintext =
            fixture.host.decrypt(content, ControlV2Client::commandAssociatedData(command),
                                 clientSigning, "opaque-session", 3, &error);
        if (plaintext.isEmpty())
            qFatal("Host could not open the restore request: %s", qPrintable(error));
        const auto payload = QJsonDocument::fromJson(plaintext).object();
        const auto id = command.value("id").toString();
        if (command.value("continuationSequence").toInteger() > 0) {
            const auto parent = command.value("continuationOfRequestId").toString();
            if (!requests.contains(parent) ||
                command.value("continuationDigest").toString() != digests.value(parent))
                qFatal("Continuation is not bound to its parent's response");
        }
        payloads.append(payload);
        requests.insert(id, command);
        QJsonObject receipt;
        for (const auto* key : {"id", "sessionId", "sessionGeneration", "kind", "snapshotCursor",
                                "requestedByteCap", "continuationSequence"})
            receipt.insert(QLatin1String(key), command.value(QLatin1String(key)));
        for (const auto* key : {"continuationOfRequestId", "continuationDigest"})
            receipt.insert(QLatin1String(key), orNull(command, key));
        receipt.insert("status", "queued");
        if (!payload.contains("large_event") &&
            payload.value("event_limit").toInt() > maximumEvents) {
            failed.insert(id);
            responses.insert(id, QJsonArray{});
            return {202, receipt};
        }
        const auto bytes = QJsonDocument(page(payload)).toJson(QJsonDocument::Compact);
        const auto digest = hex(bytes);
        digests.insert(id, digest);
        QJsonArray list;
        constexpr qsizetype fragment = 160;
        for (qsizetype offset = 0; offset < bytes.size(); offset += fragment) {
            const auto data = bytes.mid(offset, fragment);
            const bool terminal = offset + data.size() == bytes.size();
            const int cursor = list.size() + 1;
            const QJsonObject plain{{"restore_response_version", 1},
                                    {"encoding", "json-utf8-fragment"},
                                    {"offset", offset},
                                    {"total_bytes", bytes.size()},
                                    {"data", b64(data)}};
            const auto aad = CloudCrypto::canonical(
                {"ketplus", 2, "transcript-restore-response", id, "fixture-owner", "workspace",
                 "linux-host", "opaque-session", 3, command.value("credentialEpoch"),
                 command.value("snapshotCursor"), cursor, terminal ? 1 : 0,
                 terminal ? digest : QString{}, "opaque-session:3:1", 1, "linux-host",
                 senderKeyVersion});
            auto sealed =
                fixture.host.encrypt(QJsonDocument(plain).toJson(QJsonDocument::Compact), aad,
                                     "opaque-session", 3, 1, "linux-host", senderKeyVersion);
            if (tamper && terminal) {
                auto cipher = CloudCrypto::fromBase64Url(sealed.value("ciphertext").toString());
                cipher[0] = char(cipher[0] ^ 0x01);
                sealed.insert("ciphertext", b64(cipher));
            }
            list.append(QJsonObject{
                {"requestId", id},
                {"ownerSubjectId", "fixture-owner"},
                {"sessionId", "opaque-session"},
                {"sessionGeneration", 3},
                {"credentialEpoch", command.value("credentialEpoch")},
                {"snapshotCursor", command.value("snapshotCursor")},
                {"responseCursor", cursor},
                {"attempt", 1},
                {"terminal", terminal},
                {"responseDigest", terminal ? digest : QString{}},
                {"expiresAt",
                 QDateTime::currentDateTimeUtc().addSecs(120).toString(Qt::ISODateWithMs)},
                {"content", sealed}});
        }
        responses.insert(id, list);
        return {202, receipt};
    }
};
} // namespace

class RemoteControlProjectionTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void projectionShowsOnlyHostVerifiedConversationsAndReportsTheRest() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network);
        const auto own = client.identity();
        const QJsonObject recipient{
            {"id", "remote-client"}, {"keyVersion", 1}, {"keys", own.value("keys")}};
        network.replies.insert(prefix + "/v2/mobile/sessions/tampered-session/keys",
                               QJsonArray{fixture.host.wrapSessionKey("tampered-session", 1, 1,
                                                                      recipient, "linux-host", 1)});
        network.replies.insert(prefix + "/v2/mobile/sessions/fresh-session/keys", QJsonArray{});
        auto verified = indexRow("opaque-session", 3, "linux-host", "wt-a");
        verified.insert("encryptedMetadata",
                        sealedMetadata(fixture.host, "opaque-session", 3, 9, metadata));
        auto tampered = indexRow("tampered-session", 1, "linux-host", "wt-b");
        auto envelope = sealedMetadata(fixture.host, "tampered-session", 1, 9,
                                       QJsonObject{{"branch", "attacker"},
                                                   {"machineName", "relay"},
                                                   {"worktreeExists", true},
                                                   {"title", "Injected title"}});
        // The relay moves the sealed metadata to another cursor than the host signed.
        envelope.insert("cursor", 8);
        tampered.insert("encryptedMetadata", envelope);
        auto pending = indexRow("fresh-session", 1, "linux-host", "wt-b");
        pending.insert("encryptedMetadata",
                       sealedMetadata(fixture.host, "fresh-session", 1, 9, metadata));
        const auto stranger = indexRow("stranger-session", 1, "stranger-host", "wt-x");
        // A host before sealed metadata, and one that seals it without the title and state.
        const auto legacy = indexRow("legacy-session", 1, "linux-host", "wt-b");
        network.replies.insert(prefix + "/v2/mobile/sessions/old-host-session/keys",
                               QJsonArray{fixture.host.wrapSessionKey("old-host-session", 1, 1,
                                                                      recipient, "linux-host", 1)});
        auto oldHost = indexRow("old-host-session", 1, "linux-host", "wt-b");
        auto withoutTitle = metadata;
        withoutTitle.remove("title");
        withoutTitle.remove("state");
        oldHost.insert("encryptedMetadata",
                       sealedMetadata(fixture.host, "old-host-session", 1, 9, withoutTitle));
        network.replies.insert(prefix + "/v2/mobile/sessions",
                               QJsonArray{verified, tampered, pending, stranger, legacy, oldHost});
        network.replies.insert(prefix + "/v2/mobile/worktrees",
                               QJsonArray{QJsonObject{{"workspaceId", "workspace"},
                                                      {"machineId", "linux-host"},
                                                      {"repositoryId", "repo-1"},
                                                      {"repositoryLabel", "ketplus"},
                                                      {"worktreeId", "wt-a"},
                                                      {"worktreeLabel", "ketplus"},
                                                      {"catalogRevision", 0},
                                                      {"lifecycleRevision", 0}},
                                          QJsonObject{{"workspaceId", "workspace"},
                                                      {"machineId", "linux-host"},
                                                      {"repositoryId", "repo-1"},
                                                      {"repositoryLabel", "ketplus"},
                                                      {"worktreeId", "wt-b"},
                                                      {"worktreeLabel", "ketplus-fix"},
                                                      {"catalogRevision", 0},
                                                      {"lifecycleRevision", 0}},
                                          QJsonObject{{"workspaceId", "workspace"},
                                                      {"machineId", "stranger-host"},
                                                      {"repositoryId", "repo-x"},
                                                      {"worktreeId", "wt-x"}}});
        QJsonObject result;
        bool answered = false;
        client.projection([&](const QJsonValue& value, const QString& error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            result = value.toObject();
            answered = true;
        });
        QTRY_VERIFY(answered);
        const auto conversations = result.value("conversations").toArray();
        QCOMPARE(conversations.size(), 1);
        const auto shown = conversations.at(0).toObject();
        QCOMPARE(shown.value("id").toString(), QStringLiteral("opaque-session"));
        QCOMPARE(shown.value("title").toString(), QStringLiteral("Sealed title"));
        QCOMPARE(shown.value("state").toString(), QStringLiteral("running"));
        QCOMPARE(shown.value("updatedAt").toString(), QStringLiteral("2026-11-12T08:00:00.000Z"));
        QCOMPARE(shown.value("worktreeId").toString(), QStringLiteral("wt-a"));
        QCOMPARE(shown.value("branch").toString(), QStringLiteral("feature/sealed"));
        // Nothing the relay alone supplies reaches the projection.
        const auto serialized = QJsonDocument(result).toJson();
        for (const auto* forged : {"Relay title", "Injected title", "attacker", "ketplus-fix"})
            QVERIFY2(!serialized.contains(forged), forged);
        QStringList rejected;
        for (const auto& value : result.value("rejected").toArray())
            rejected.append(value.toObject().value("id").toString() + "=" +
                            value.toObject().value("reason").toString());
        rejected.sort();
        QCOMPARE(
            rejected,
            (QStringList{"fresh-session=session_key_pending", "legacy-session=metadata_absent",
                         "old-host-session=host_upgrade_required", "stranger-session=unpinned_host",
                         "tampered-session=unauthenticated_session_metadata",
                         "wt-b=unsigned_index_entry", "wt-x=unpinned_host"}));
        // The worktree comes from the host-sealed scope; the index only adds its revisions.
        const auto repositories = result.value("repositories").toArray();
        QCOMPARE(repositories.size(), 1);
        QCOMPARE(repositories.at(0).toObject().value("label").toString(),
                 QStringLiteral("ketplus"));
        const auto worktrees = repositories.at(0).toObject().value("worktrees").toArray();
        QCOMPARE(worktrees.size(), 1);
        const auto worktree = worktrees.at(0).toObject();
        QCOMPARE(worktree.value("worktreeId").toString(), QStringLiteral("wt-a"));
        QCOMPARE(worktree.value("branch").toString(), QStringLiteral("feature/sealed"));
        QCOMPARE(worktree.value("catalogRevision").toInt(-1), 0);
    }

    void pagingReassemblesALargeEventAndPagesBackwardsLosslessly() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network);
        RemoteControlTestAccess::restorePoll(client, 1);
        RestoreHost host(fixture);
        host.clientSigning = CloudCrypto::fromBase64Url(
            client.identity().value("keys").toObject().value("signingPublicKey").toString());
        QJsonObject item{{"item_id", "item-big"}, {"text", QString(330, 'x')}};
        host.large = QJsonDocument(item).toJson(QJsonDocument::Compact);
        network.handler = [&](const QNetworkRequest& request, const QByteArray& body) {
            return host.answer(request, body);
        };
        auto target = session;
        target.insert("lastEventCursor", 9);
        QJsonObject newest;
        QString failure = "pending";
        client.transcriptWindow(target, {{"event_limit", 128}, {"turn_limit", 3}},
                                [&](const QJsonValue& value, const QString& error) {
                                    newest = value.toObject();
                                    failure = error;
                                });
        QTRY_VERIFY(failure != "pending");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto events = newest.value("events").toArray();
        QCOMPARE(events.size(), 3);
        QCOMPARE(events.at(1).toObject().value("payload").toObject(), item);
        QCOMPARE(newest.value("partial_items").toArray().size(), 0);
        QVERIFY(newest.value("has_more_before").toBool());
        // The 4 fragments of the large item came as a chain of continuations after the root.
        QCOMPARE(host.payloads.size(), 1 + (host.large.size() + 99) / 100);
        QCOMPARE(host.payloads.first().value("before_cursor").toInteger(), 0);

        QJsonObject older;
        failure = "pending";
        client.transcriptWindow(target,
                                {{"snapshot_cursor", newest.value("snapshot_cursor")},
                                 {"before_cursor", newest.value("before_cursor")}},
                                [&](const QJsonValue& value, const QString& error) {
                                    older = value.toObject();
                                    failure = error;
                                });
        QTRY_VERIFY(failure != "pending");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(host.payloads.last().value("before_cursor").toInteger(), 7);
        QCOMPARE(older.value("events").toArray().size(), 2);
        QVERIFY(!older.value("has_more_before").toBool());
        QVERIFY(older.value("events").toArray().last().toObject().value("cursor").toInteger() <
                events.first().toObject().value("cursor").toInteger());
    }

    void anOversizedPageIsRequestedAgainWithFewerEvents() {
        for (const bool as410 : {true, false}) {
            FixtureTransport network;
            ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
            EncryptedFixture fixture(client, network);
            RemoteControlTestAccess::restorePoll(client, 1);
            RestoreHost host(fixture);
            host.clientSigning = CloudCrypto::fromBase64Url(
                client.identity().value("keys").toObject().value("signingPublicKey").toString());
            host.large = R"({"item_id":"item-big"})";
            host.maximumEvents = 32;
            host.failedAs410 = as410;
            network.handler = [&](const QNetworkRequest& request, const QByteArray& body) {
                return host.answer(request, body);
            };
            auto target = session;
            target.insert("lastEventCursor", 9);
            QString failure = "pending";
            QJsonObject page;
            client.transcriptWindow(target, {}, [&](const QJsonValue& value, const QString& error) {
                page = value.toObject();
                failure = error;
            });
            QTRY_VERIFY(failure != "pending");
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
            QCOMPARE(page.value("events").toArray().size(), 3);
            QList<int> limits;
            for (const auto& payload : std::as_const(host.payloads))
                if (!payload.contains("large_event"))
                    limits.append(payload.value("event_limit").toInt());
            QCOMPARE(limits, (QList<int>{128, 64, 32}));
            // Every root was the same snapshot at the same position.
            for (const auto& command : std::as_const(host.requests))
                QCOMPARE(command.value("snapshotCursor").toInteger(), 9);
        }
    }

    void aTamperedRestoreResponseOrChangedHostKeyVersionIsNeverDelivered() {
        for (const bool tamper : {true, false}) {
            r03::resetCredentials();
            FixtureTransport network;
            ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
            EncryptedFixture fixture(client, network);
            RemoteControlTestAccess::restorePoll(client, 1);
            RestoreHost host(fixture);
            host.clientSigning = CloudCrypto::fromBase64Url(
                client.identity().value("keys").toObject().value("signingPublicKey").toString());
            host.large = R"({"item_id":"item-big"})";
            host.tamper = tamper;
            // Same key bytes, but content claiming a host key version the pin never named.
            host.senderKeyVersion = tamper ? 1 : 2;
            network.handler = [&](const QNetworkRequest& request, const QByteArray& body) {
                return host.answer(request, body);
            };
            auto target = session;
            target.insert("lastEventCursor", 9);
            QJsonValue delivered;
            QString failure = "pending";
            client.transcriptWindow(target, {}, [&](const QJsonValue& value, const QString& error) {
                delivered = value;
                failure = error;
            });
            QTRY_VERIFY(failure != "pending");
            QVERIFY(!failure.isEmpty());
            QVERIFY(delivered.toObject().isEmpty());
            if (!tamper)
                QVERIFY2(failure.contains("signing key changed"), qPrintable(failure));
            // The refused request is cancelled at the relay instead of left to its deadline.
            QTRY_COMPARE(host.cancelled.size(), 1);
        }
    }

    void anUnpinnedHostKeyFailsProjectionAndPagingClosed() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network, false);
        // The client paired with another key for this host before; the relay now serves this one.
        CredentialStore otherStore("other-host");
        RemoteControlTestAccess::pinHost(client, "linux-host", 1,
                                         CloudCrypto(&otherStore).publicKeys());
        auto row = indexRow("opaque-session", 3, "linux-host", "wt-a");
        row.insert("encryptedMetadata",
                   sealedMetadata(fixture.host, "opaque-session", 3, 9, metadata));
        network.replies.insert(prefix + "/v2/mobile/sessions", QJsonArray{row});
        network.replies.insert(prefix + "/v2/mobile/worktrees", QJsonArray{});
        QJsonObject result;
        bool answered = false;
        client.projection([&](const QJsonValue& value, const QString& error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            result = value.toObject();
            answered = true;
        });
        QTRY_VERIFY(answered);
        QVERIFY(result.value("conversations").toArray().isEmpty());
        QCOMPARE(result.value("rejected").toArray().size(), 1);
        QVERIFY(!QJsonDocument(result).toJson().contains("Sealed title"));
        QString failure = "pending";
        auto target = session;
        target.insert("lastEventCursor", 9);
        client.transcriptWindow(target, {},
                                [&](const QJsonValue&, const QString& error) { failure = error; });
        QTRY_VERIFY(failure != "pending");
        QCOMPARE(failure, QStringLiteral("This host is not paired with this computer."));
        QVERIFY(!network.paths.contains(prefix + "/v2/mobile/sessions/opaque-session/commands"));
    }
};
QTEST_GUILESS_MAIN(RemoteControlProjectionTest)
#include "RemoteControlProjectionTest.moc"
