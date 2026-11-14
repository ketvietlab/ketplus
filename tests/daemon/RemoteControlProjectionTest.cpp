#include "RestoreHost.h"
#include <QSet>
#include <QUrlQuery>

// Sealed session/worktree projection and transcript paging over the synthetic loopback relay.
namespace {

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

} // namespace

class RemoteControlProjectionTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void projectionShowsOnlyHostVerifiedConversationsAndReportsTheRest_data() {
        QTest::addColumn<QString>("adapter");
        QTest::newRow("codex") << "codex";
        QTest::newRow("copilot") << "copilot";
    }
    void projectionShowsOnlyHostVerifiedConversationsAndReportsTheRest() {
        QFETCH(QString, adapter);
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
        verified.insert("adapter", adapter);
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
        QCOMPARE(shown.value("adapter").toString(), adapter);
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
