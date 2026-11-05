#include "agent/TranscriptWindowModel.h"

#include <QScopedValueRollback>
#include <QSet>

#include <algorithm>
#include <utility>

namespace ketplus {

TranscriptWindowModel::TranscriptWindowModel(QAbstractItemModel* source, QObject* parent)
    : QAbstractListModel(parent), source_(source) {
    keyRole_ = source_->roleNames().key(QByteArrayLiteral("messageKey"), -1);
    // Every structural change is answered the same way: work out which keys
    // the span now holds and move this model there with the fewest removals
    // and insertions. The view recomputes the span when geometry changes, which
    // may land a moment later; until then the old span is kept, clamped.
    const auto structural = [this] { resync(); };
    // A reset or layout change may also have replaced what a surviving key
    // holds, and nothing else will say so.
    const auto replaced = [this] { resync(true); };
    // Rows arriving or leaving above the span move the span with them, so the
    // rows on screen stay the rows on screen. Without this a page of history
    // would briefly push them out of the span and they would be rebuilt.
    connect(source_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, const int first, const int last) {
                const int count = last - first + 1;
                if (first <= first_) {
                    first_ += count;
                    last_ += count;
                }
                resync();
            });
    connect(source_, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex&, const int first, const int last) {
                const int count = last - first + 1;
                if (last < first_) {
                    first_ -= count;
                    last_ -= count;
                }
                resync();
            });
    connect(source_, &QAbstractItemModel::rowsMoved, this, structural);
    connect(source_, &QAbstractItemModel::modelReset, this, replaced);
    connect(source_, &QAbstractItemModel::layoutChanged, this, replaced);
    connect(source_, &QAbstractItemModel::dataChanged, this,
            &TranscriptWindowModel::forwardDataChanged);
}

int TranscriptWindowModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(keys_.size());
}

QVariant TranscriptWindowModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= sourceRows_.size())
        return {};
    const int sourceRow = sourceRows_.at(index.row());
    if (role == RowIndexRole)
        return sourceRow;
    return source_->data(source_->index(sourceRow, 0), role);
}

QHash<int, QByteArray> TranscriptWindowModel::roleNames() const {
    QHash<int, QByteArray> roles = source_->roleNames();
    roles.insert(RowIndexRole, QByteArrayLiteral("rowIndex"));
    return roles;
}

QString TranscriptWindowModel::keyOf(const int sourceRow) const {
    const QString key = source_->data(source_->index(sourceRow, 0), keyRole_).toString();
    // A row without a key still needs a stable identity inside one span.
    return key.isEmpty() ? QStringLiteral("#row:%1").arg(sourceRow) : key;
}

void TranscriptWindowModel::setRange(const int first, const int last) {
    if (first == first_ && last == last_)
        return;
    first_ = first;
    last_ = last;
    resync();
    emit rangeChanged();
}

int TranscriptWindowModel::windowRow(const int sourceRow) const {
    if (sourceRows_.isEmpty() || sourceRow < sourceRows_.constFirst() ||
        sourceRow > sourceRows_.constLast())
        return -1;
    const int row = sourceRow - sourceRows_.constFirst();
    return row < sourceRows_.size() && sourceRows_.at(row) == sourceRow ? row : -1;
}

void TranscriptWindowModel::resync(const bool contentReplaced) {
    resyncPending_ = true;
    contentReplacementPending_ |= contentReplaced;
    if (syncing_)
        return;
    // Delegate creation/destruction can synchronously change the viewport's
    // requested range from a model signal. Finish this transaction against its
    // snapshot before diffing the latest range; nested diffs invalidate both
    // the row indexes below and Qt's begin/end notifications.
    QScopedValueRollback guard(syncing_, true);
    while (resyncPending_) {
        resyncPending_ = false;
        resyncNow(std::exchange(contentReplacementPending_, false));
    }
}

void TranscriptWindowModel::resyncNow(const bool contentReplaced) {
    const int count = source_->rowCount();
    const int first = std::max(0, first_);
    const int last = std::min(count - 1, last_);
    QStringList next;
    QList<int> nextRows;
    for (int row = first; row <= last; ++row) {
        next.append(keyOf(row));
        nextRows.append(row);
    }

    // Keys leave from the back so earlier rows keep their numbers while later
    // ones go. Both lists follow source order, so what survives is already in
    // the right order and only needs the new arrivals slotted in around it.
    const QSet<QString> wanted(next.cbegin(), next.cend());
    for (int row = int(keys_.size()) - 1; row >= 0; --row) {
        if (wanted.contains(keys_.at(row)))
            continue;
        int start = row;
        while (start > 0 && !wanted.contains(keys_.at(start - 1)))
            --start;
        beginRemoveRows({}, start, row);
        keys_.remove(start, row - start + 1);
        sourceRows_.remove(start, row - start + 1);
        endRemoveRows();
        row = start;
    }

    const QSet<QString> present(keys_.cbegin(), keys_.cend());
    int cursor = 0;
    for (int index = 0; index < next.size();) {
        if (present.contains(next.at(index))) {
            // Guard against a key that moved relative to its neighbours: the
            // source then reordered rows, which a key diff cannot express, so
            // start over rather than attach a delegate to the wrong row.
            if (cursor >= keys_.size() || keys_.at(cursor) != next.at(index)) {
                beginResetModel();
                keys_ = next;
                sourceRows_ = nextRows;
                endResetModel();
                return;
            }
            ++cursor;
            ++index;
            continue;
        }
        int end = index;
        while (end + 1 < next.size() && !present.contains(next.at(end + 1)))
            ++end;
        beginInsertRows({}, cursor, cursor + end - index);
        for (int row = index; row <= end; ++row) {
            keys_.insert(cursor + row - index, next.at(row));
            sourceRows_.insert(cursor + row - index, nextRows.at(row));
        }
        endInsertRows();
        cursor += end - index + 1;
        index = end + 1;
    }

    // Survivors may now sit at different source rows. Their content is the
    // same row's content, so only the row number is announced, and only where
    // it changed.
    int changedFirst = -1;
    int changedLast = -1;
    for (int row = 0; row < sourceRows_.size(); ++row) {
        if (sourceRows_.at(row) == nextRows.at(row))
            continue;
        sourceRows_[row] = nextRows.at(row);
        if (changedFirst < 0)
            changedFirst = row;
        changedLast = row;
    }
    if (contentReplaced && !keys_.isEmpty())
        emit dataChanged(index(0), index(int(keys_.size()) - 1), {});
    else if (changedFirst >= 0)
        emit dataChanged(index(changedFirst), index(changedLast), {RowIndexRole});
}

void TranscriptWindowModel::forwardDataChanged(const QModelIndex& topLeft,
                                               const QModelIndex& bottomRight,
                                               const QList<int>& roles) {
    if (sourceRows_.isEmpty())
        return;
    const int first = std::max(topLeft.row(), sourceRows_.constFirst());
    const int last = std::min(bottomRight.row(), sourceRows_.constLast());
    if (first > last)
        return;
    // A key change is an identity change and is handled as one.
    if (roles.isEmpty() || roles.contains(keyRole_)) {
        bool renamed = false;
        for (int row = first; row <= last && !renamed; ++row)
            renamed = keyOf(row) != keys_.value(windowRow(row));
        if (renamed) {
            resync();
            if (roles.isEmpty() || roles.size() > 1) {
                const int windowFirst = windowRow(first);
                const int windowLast = windowRow(last);
                if (windowFirst >= 0 && windowLast >= 0)
                    emit dataChanged(index(windowFirst), index(windowLast), roles);
            }
            return;
        }
    }
    emit dataChanged(index(windowRow(first)), index(windowRow(last)), roles);
}

} // namespace ketplus
