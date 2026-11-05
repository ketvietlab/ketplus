#pragma once

#include <QAbstractListModel>
#include <QStringList>

namespace ketplus {

// The rows of a transcript that currently need a delegate: the viewport plus an
// over-range on either side, as a contiguous span of the message model.
//
// ListView decided this for itself, and to decide it had to place rows it had
// never laid out, which it did by extrapolating an average. The transcript view
// now places every row from TranscriptRowGeometry and only asks this model for
// the span it has already computed, so nothing here needs to know a height.
//
// Delegates are matched to rows by message key, not by position. When history
// is prepended, a group collapses, or the span slides by one row, a row that
// stays in the span keeps its delegate -- and so its laid-out rich text -- and
// only learns its new row number. Rebuilding a Markdown delegate because the
// row above it arrived is what would make scrolling pay for history loading.
class TranscriptWindowModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int first READ first NOTIFY rangeChanged)
    Q_PROPERTY(int last READ last NOTIFY rangeChanged)
  public:
    explicit TranscriptWindowModel(QAbstractItemModel* source, QObject* parent = nullptr);

    // The source row of each delegate, published beside the source's own roles.
    static constexpr int RowIndexRole = Qt::UserRole + 4096;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Inclusive source rows. An empty span is first > last.
    Q_INVOKABLE void setRange(int first, int last);
    int first() const { return first_; }
    int last() const { return last_; }

    // Source row -> row of this model, or -1 when that row has no delegate.
    Q_INVOKABLE int windowRow(int sourceRow) const;

  signals:
    void rangeChanged();

  private:
    void resync(bool contentReplaced = false);
    void resyncNow(bool contentReplaced);
    void forwardDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight,
                            const QList<int>& roles);
    QString keyOf(int sourceRow) const;

    QAbstractItemModel* source_{nullptr};
    int keyRole_{-1};
    int first_{0};
    int last_{-1};
    // What the view has been told: one key and one source row per row here.
    QStringList keys_;
    QList<int> sourceRows_;
    bool syncing_{false};
    bool resyncPending_{false};
    bool contentReplacementPending_{false};
};

} // namespace ketplus
