#pragma once
#include <QScopeGuard>

#include "agent/TranscriptRowGeometry.h"
#include "agent/TranscriptRowMeasurer.h"
#include "agent/TranscriptWindowModel.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVariantMap>
#include <functional>

#include <memory>

namespace ketplus {

class AgentMessageModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool materializing READ materializing NOTIFY materializingChanged)
    // Advances whenever the loaded rows change, also in fields no role shows (a group's files).
    Q_PROPERTY(int sourceRevision READ sourceRevision NOTIFY sourceRevisionChanged)
    // Row geometry: every row's height and top, as a sum of measurements rather
    // than an extrapolation. The transcript view places rows from it.
    Q_PROPERTY(ketplus::TranscriptRowGeometry* geometry READ geometry CONSTANT)
    // The rows near the viewport that currently need a delegate.
    Q_PROPERTY(ketplus::TranscriptWindowModel* window READ window CONSTANT)
  public:
    explicit AgentMessageModel(QObject* parent = nullptr);
    int count() const { return int(visible_.size()); }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariantMap get(int index) const;
    Q_INVOKABLE QVariantMap source(int index) const;
    // Complete source entry that rendered row `row` belongs to (chunked messages share one).
    Q_INVOKABLE QVariantMap sourceForRow(int row) const;
    // Text searched by transcript find for rendered row `row`: the source body block of messages
    // and the copy text of activity rows.
    Q_INVOKABLE QString searchableText(int row) const;
    // Case-insensitive matches of `query` over loaded rows, in row order:
    // [{row, key, start, length}], at most `limit` entries.
    Q_INVOKABLE QVariantList findMatches(const QString& query, int limit = 1000) const;
    // Row of the leader of the activity group containing `row` (-1 when ungrouped). O(1).
    Q_INVOKABLE int groupLeaderIndex(int row) const;
    // The files the turn ending at visible `row` changed, for its changed-files card.
    Q_INVOKABLE QVariantMap turnFiles(int row) const;
    // Stable keys of the activity groups currently loaded.
    Q_INVOKABLE QStringList activityGroupKeys() const;
    Q_INVOKABLE int sourceCount() const { return int(sourceItems_.size()); }
    bool materializing() const { return materializing_; }
    int deferredOutstanding() const {
        return (deferredWorkerActive_ ? 1 : 0) + (deferredPending_ ? 1 : 0);
    }
    int maximumDeferredOutstanding() const { return maximumDeferredOutstanding_; }
    TranscriptRowGeometry* geometry() const { return geometry_; }
    TranscriptWindowModel* window() const { return window_; }
    // Asks the measurer for bodies of rows no delegate has reported, so the
    // geometry knows a window the reader has not scrolled through yet.
    // False when no off-thread work remains; resource-backed rows wait for their delegate.
    Q_INVOKABLE bool measureUnmeasuredRows(int limit, qreal width, const QFont& font);
    Q_INVOKABLE int indexOfKey(const QString& key) const;
    // The key of the row the view shows for `key`: the key itself when it is a
    // row, the group row while it is withheld in a collapsed group, and the
    // nearest block of the same message when a block key was renamed by
    // re-chunking. Empty when the message is no longer loaded. Lets a reading
    // anchor survive regrouping instead of losing its place.
    Q_INVOKABLE QString visibleKeyFor(const QString& key) const;
    // Activity-group expansion, owned here rather than in QML because it decides
    // which rows exist. A collapsed group occupies exactly one row: its members
    // are still rendered and still grouped, so summaries and streaming keep
    // working, but they are withheld from the rows the view lays out. Nothing
    // downstream has to cope with a zero-height row, and ListView's running
    // height average never sees one.
    // A group stays collapsed, even while its turn runs, until the reader opens it.
    Q_INVOKABLE bool groupExpanded(const QString& key) const;
    Q_INVOKABLE void setGroupExpanded(const QString& key, bool expanded);
    Q_INVOKABLE void toggleGroup(const QString& key);
    // The complete rendered list, including rows withheld while their group is
    // collapsed. Diagnostics and tests only: the view addresses rows through
    // count()/get(), which never expose a withheld row.
    Q_INVOKABLE int renderedCount() const { return int(items_.size()); }
    Q_INVOKABLE QVariantMap renderedRow(int row) const {
        return row >= 0 && row < items_.size() ? items_.at(row) : QVariantMap{};
    }
    // True when the reader has expanded or collapsed this group by hand.
    Q_INVOKABLE bool hasExplicitExpansion(const QString& key) const {
        return groupExpansion_.contains(key);
    }
    // The rendered row carrying `key`, whether or not a model row shows it.
    // Callers that reason about a row's identity rather than its position need
    // to reach rows withheld inside a collapsed group; indexOfKey() cannot,
    // because it addresses the rows the view lays out.
    Q_INVOKABLE QVariantMap rowForKey(const QString& key) const {
        const int row = renderedIndexes_.value(key, -1);
        return row >= 0 ? items_.at(row) : QVariantMap{};
    }
    Q_INVOKABLE int hiddenRowCount() const { return int(items_.size() - visible_.size()); }
    Q_INVOKABLE QString resolvePayload(const QString& reference);
    using PayloadSource = std::function<QVariant(const QString&, const QString&)>;
    void setPayloadSource(PayloadSource source) { payloadSource_ = std::move(source); }
    Q_INVOKABLE void reset(const QVariantList& items);
    // reset() with the reason recorded in the transcript trace.
    void resetFor(const char* reason, const QVariantList& items);
    bool resetIfChanged(const QVariantList& items);
    [[nodiscard]] qsizetype estimatedTextBytes() const;
    Q_INVOKABLE void clear() { reset({}); }
    Q_INVOKABLE void append(const QVariantMap& item);
    Q_INVOKABLE void prepend(const QVariantList& items);
    void replaceWindow(const QVariantList& items);
    Q_INVOKABLE void set(int index, const QVariantMap& item);
    [[nodiscard]] qsizetype retainedPayloadBytes() const { return retainedPayloadBytes_; }
    [[nodiscard]] int retainedPayloadCount() const { return payloads_.size(); }
    [[nodiscard]] int sourceRevision() const { return sourceRevision_; }
    // The rows one source entry renders as. Pure, so a worker can render a page
    // before it reaches the model.
    static QList<QVariantMap> renderedItems(const QVariantMap& item, int sourceIndex);

  signals:
    void countChanged();
    void sourceRevisionChanged();
    void groupExpansionChanged();
    void remoteGroupRequested(const QString& key, bool expanded);
    void materializingChanged();

  private:
    // Advances sourceRevision when the calling mutation returns.
    [[nodiscard]] auto sourceTouch() {
        return qScopeGuard([this] {
            ++sourceRevision_;
            emit sourceRevisionChanged();
        });
    }
    static QVariantMap normalized(QVariantMap item);
    static QList<QVariantMap> renderedItems(const QVariantList& items);
    static void appendChunks(QList<QVariantMap>* result, const QVariantMap& base,
                             const QString& body, const QString& originalKey, qsizetype offset,
                             bool fenced, bool forcedPlainContinuation);
    bool renderIncrementally(int row, const QVariantMap& old, const QVariantMap& updated,
                             int renderedRow, QList<QVariantMap>* tail, int* firstBlock) const;
    static bool needsDeferredMaterialization(const QVariantList& items);
    void publishDeferredMaterialization(const QVariantList& source, QList<QVariantMap> rendered,
                                        quint64 generation);
    void publishDeferredBatch(const QVariantList& source,
                              const std::shared_ptr<QList<QVariantMap>>& rendered, int offset,
                              quint64 generation);
    void applyRenderedUpdate(int sourceRow, const QVariantMap& updated,
                             QList<QVariantMap> renderedUpdate, quint64 generation,
                             int firstBlock = 0);
    void scheduleDeferredReset(const QVariantList& source, quint64 generation);
    void scheduleDeferredUpdate(int sourceRow, const QVariantMap& updated, quint64 generation);
    void retainInvalidatedDeferredUpdates(quint64 generation);
    void startDeferredWork();
    void finishDeferredReset(const QVariantList& source, QList<QVariantMap> rendered,
                             quint64 generation);
    void finishDeferredUpdates(const QMap<int, QVariantMap>& updates,
                               QMap<int, QList<QVariantMap>> rendered, quint64 generation);
    void settleMaterializing();
    // True while the whole row set is being rebuilt on a worker or published in
    // batches; a structural change then has no stable rows to apply to.
    bool deferredResetInFlight() const {
        return materializing_ && (deferredPendingReset_ || deferredResetActive_);
    }
    QStringList takeDeferredUpdateKeys();
    void replaceWindowRows(const QVariantList& items);
    void rescheduleDeferredUpdates(const QStringList& keys);
    struct RetainedPayload {
        QByteArray json;
        quint64 access{0};
    };
    static QString retainPayload(const QVariant& value, const QString& kind, QString* inlineJson);
    void evictPayloads(qsizetype requiredBytes);
    void clearPayloads();
    void rebuildKeys();
    // Hands the published row set to the geometry, or holds it until the row
    // change in progress has been reported.
    void publishGeometry();
    // items_ is the complete rendered list; visible_ selects the rows the view
    // sees. Every structural change mutates items_ and then calls syncVisible(),
    // which is the single place that turns an items_ change into model signals.
    int itemCount() const { return int(items_.size()); }
    bool memberOfCollapsedGroup(const QVariantMap& item) const;
    QList<int> computeVisible() const;
    QStringList keysOf(const QList<int>& rows) const;
    void syncVisible();
    int visibleRowOf(int row) const;
    int visibleRowAtOrAfter(int row) const;
    void republishVisible();
    void resetVisible();
    void pruneGroupExpansion();
    // Recomputes activity group roles around rendered rows [first, last] and emits dataChanged
    // for rows whose group roles changed (never a reset).
    void regroup(int first, int last);
    QList<int> groupRoleIds_;
    QList<QVariantMap> items_;
    QList<int> visible_;
    QStringList publishedKeys_;
    TranscriptRowGeometry* geometry_{nullptr};
    TranscriptWindowModel* window_{nullptr};
    // Open beginInsert/Remove/Reset/Move transactions, and whether the row set
    // changed inside one. The geometry is published when the last one closes.
    int rowTransactions_{0};
    bool geometryPending_{false};
    QHash<QString, qreal> pendingBodyHeights_;
    TranscriptRowMeasurer* measurer_{nullptr};
    // Everything about a row that decides the chrome around its body.
    static QString rowShape(const QVariantMap& item);
    QHash<QString, bool> groupExpansion_;
    QVariantList sourceItems_;
    QHash<QString, int> indexes_;
    // key -> items_ row, including rows withheld inside a collapsed group.
    QHash<QString, int> renderedIndexes_;
    // First rendered row of each source row (aligned while not materializing)
    // and source row by messageKey. Maintained on structural changes only.
    QList<int> renderedStart_;
    QHash<QString, int> sourceIndexes_;
    mutable qsizetype textBytes_{-1};
    QHash<int, QByteArray> roles_;
    quint64 materializationGeneration_{0};
    bool materializing_{false};
    int sourceRevision_{0};
    bool deferredWorkerActive_{false};
    bool deferredPending_{false};
    bool deferredPendingReset_{false};
    bool deferredResetActive_{false};
    quint64 deferredResetGeneration_{0};
    QVariantList deferredPendingSource_;
    QMap<int, QVariantMap> deferredPendingUpdates_;
    QMap<int, QVariantMap> deferredActiveUpdates_;
    quint64 deferredPendingGeneration_{0};
    int maximumDeferredOutstanding_{0};
    QHash<QString, RetainedPayload> payloads_;
    PayloadSource payloadSource_;
    qsizetype retainedPayloadBytes_{0};
    quint64 payloadAccess_{0};
    static constexpr qsizetype kInlinePayloadBytes = 32 * 1024;
    static constexpr qsizetype kMaximumPayloadBytes = 4 * 1024 * 1024;
    static constexpr int kMaximumPayloadCount = 64;
};

} // namespace ketplus
