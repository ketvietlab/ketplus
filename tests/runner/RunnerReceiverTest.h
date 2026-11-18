#pragma once

#include "protocol/Frames.h"
#include "snapshot/SnapshotStoreReceiver.h"
#include "state/StateDirectory.h"

#include <QObject>
#include <QSemaphore>
#include <QStringList>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <optional>

namespace runner_receiver_test {

using namespace ketplus::runner;

struct Packed {
    QString commit;
    QByteArray bytes;
    QString sha256;
};

// A 32-character key from a short name.
QString key(const QString& name);

class ReceiverTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();

    // RunnerReceiverTest.cpp: layout, keys, want.
    void nestedDirectoriesAreRefused();
    void unsafeKeysTouchNothing();
    void fullPackIsAppliedAndHeld();
    void haveRestoresTheSnapshotFromTheMirror();
    void needResumesAfterARestart();
    void sameOfferResumesAndAnotherPackStartsOver();
    void truncatedPartialResumesAtAChunkBoundary();

    // RunnerReceiverChunkTest.cpp.
    void framesAndPayloadsPairInAnyOrder();
    void earlyChunksWaitAndRepeatsAreAckedAgain();
    void badChunksAreCorrupt();
    void tooManyHeldChunksAreCorrupt();
    void eightCorruptChunksEndThePack();
    void chunksWithoutAnOfferAreDropped();
    void chunksOutsideNeedAreDroppedAndCountsRestart();
    void oversizedPackIsRefused();
    void diskFullEndsTheJob();

    // RunnerReceiverApplyTest.cpp.
    void badPacksLeaveTheWorkspace();
    void checkoutTooLargeLeavesTheWorkspace();
    void unsafePathLeavesTheWorkspace();
    void incrementalApplyKeepsUntrackedFiles();
    void cleanRemovesUntrackedFilesOnly();
    void missingBaseAsksForAFullPack();
    void unindexableIncrementalPackAsksOnce();
    void foreignOwnerIsRefused();
    void cancelWhileWriting();
    void cancelWhileApplying();
    void cancelAfterApplying();

    // RunnerReceiverKeepTest.cpp.
    void partialPacksExpireAfterADay();
    void idleEvictsTheLeastRecentlyUsed();
    void offerEvictsOtherWorkspacesOnly();
    void heldListsTheMostRecent256();

  private:
    QString path(const QString& name) const;
    bool writeFile(const QString& file, const QByteArray& content) const;
    QByteArray readFile(const QString& file) const;
    QString snapshot();  // of the source repository as it is now
    bool writeBigFile(); // a random file of two chunks and a bit: a pack of three chunks
    Packed pack(const QString& commit, const QString& base = {});
    void makeReceiver(qint64 budget = receiver::ReceiverOptions{}.budgetBytes);
    QString workspace(const QString& workspaceName = QStringLiteral("ws")) const;
    QString partialPack(const QString& workspaceName = QStringLiteral("ws")) const;

    void begin(const QString& snapshotId, const QString& workspaceName = QStringLiteral("ws"),
               bool clean = false, const QString& machine = QStringLiteral("machine-a"));
    void offer(const Packed& packed, const std::optional<QString>& base = std::nullopt,
               const QStringList& warnings = {});
    void frameOnly(const Packed& packed, qint64 offset); // a chunk frame, payload never sent
    void chunk(const Packed& packed, qint64 offset, qint64 bytes, bool payloadFirst = false,
               std::optional<QByteArray> payload = std::nullopt);
    // Sends from `from` chunk by chunk, each after the previous one was acked.
    bool sendPack(const Packed& packed, qint64 from = 0);
    // begin, offer and every chunk; the answer: "ready" or the failure code.
    QString deliver(const Packed& packed, const std::optional<QString>& base = std::nullopt,
                    const QString& workspaceName = QStringLiteral("ws"), bool clean = false,
                    const QString& machine = QStringLiteral("machine-a"));

    template <typename F> QList<F> sentOf() const {
        QList<F> frames;
        for (const protocol::Frame& frame : m_sent)
            if (const F* typed = std::get_if<F>(&frame))
                frames.append(*typed);
        return frames;
    }
    std::optional<protocol::SnapshotWant> nextWant(); // the next want sent, waiting for it
    std::optional<qint64> lastAck() const;
    QStringList errorsSent() const;
    bool waitAnswer(); // until one more ready or failure arrives
    QString lastAnswer() const;
    void holdAt(receiver::WorkStep step); // the worker stops there next time
    bool held();                          // waits until it does
    void release();

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_repo;
    QString m_store;
    QString m_work;
    std::optional<app::StateDirectory> m_state;
    std::unique_ptr<receiver::SnapshotStoreReceiver> m_receiver;
    std::optional<snapshot::ApplyLimits> m_limits;
    std::atomic<qint64> m_nowMs{0};
    std::atomic_bool m_diskFull{false};
    std::atomic_int m_holdAt{-1};
    QSemaphore m_reached;
    QSemaphore m_released;
    QList<protocol::Frame> m_sent;
    qsizetype m_wantsSeen = 0;
    QStringList m_answers; // "ready" or the failure code, in order
    QString m_readyWorkspace;
    QStringList m_readyWarnings;
    int m_transfer = 0;
};

} // namespace runner_receiver_test
