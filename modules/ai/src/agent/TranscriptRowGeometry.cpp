#include "agent/TranscriptRowGeometry.h"

#include "agent/TranscriptTrace.h"

#include <algorithm>
#include <cmath>

namespace ketplus {
namespace {
// A measurement is only worth keeping while its row could plausibly return with
// the window. Past that the map would grow for the life of the session.
constexpr int kMaximumMeasurements = 4096;
} // namespace

TranscriptRowGeometry::TranscriptRowGeometry(QObject* parent) : QObject(parent) {}

// Best available answer for one row, in order of how much it is worth: a height
// a delegate reported, then a measured body plus its shape's learned chrome,
// then the constant. Each rests only on this row's own evidence.
void TranscriptRowGeometry::recomputeHeight(Row& row) const {
    const auto realized = measurements_.constFind(row.key);
    if (realized != measurements_.cend()) {
        row.height = *realized;
        row.realized = true;
        row.derived = false;
        return;
    }
    row.realized = false;
    const auto body = bodyMeasurements_.constFind(row.key);
    const auto chrome = chrome_.constFind(row.shape);
    if (body != bodyMeasurements_.cend() && chrome != chrome_.cend()) {
        row.height = *body + *chrome;
        row.derived = true;
        return;
    }
    const auto stale = staleHeights_.constFind(row.key);
    row.height = stale != staleHeights_.cend() ? *stale : provisionalHeight_;
    row.derived = false;
}

void TranscriptRowGeometry::applyCachedHeight(Row& row) const { recomputeHeight(row); }

void TranscriptRowGeometry::noteTotalCorrection(const qreal before) {
    largestCorrection_ = std::max(largestCorrection_, std::abs(totalHeight() - before));
}

void TranscriptRowGeometry::reset(const QStringList& keys, const QStringList& shapes) {
    rows_.clear();
    rows_.reserve(keys.size());
    rowsByKey_.clear();
    rowsByKey_.reserve(keys.size());
    for (int index = 0; index < keys.size(); ++index) {
        Row row;
        row.key = keys.at(index);
        row.shape = index < shapes.size() ? shapes.at(index) : QString{};
        recomputeHeight(row);
        rowsByKey_.insert(row.key, index);
        rows_.append(row);
    }
    topsDirty_ = true;
    emit geometryChanged();
}

void TranscriptRowGeometry::setRowHeight(const QString& key, const qreal height,
                                         const qreal bodyHeight) {
    if (key.isEmpty() || !(height >= 0) || !std::isfinite(height))
        return;
    const qreal previousTotal = totalHeight();
    if (measurements_.size() >= kMaximumMeasurements && !measurements_.contains(key))
        measurements_.clear();
    measurements_.insert(key, height);
    bool touched = false;
    for (Row& row : rows_) {
        if (row.key != key)
            continue;
        // Chrome is the part of the row its body did not account for. Learned
        // from this row so an unrealized row of the same shape can use it.
        if (bodyHeight >= 0 && !row.shape.isEmpty())
            chrome_.insert(row.shape, std::max(qreal(0), height - bodyHeight));
        if (row.realized && std::abs(row.height - height) < 0.01)
            continue;
        // A first real height that differs from the one the row was laid out
        // with moves every row below it: the size of the estimate's miss.
        if (!row.realized && std::abs(row.height - height) >= 2 && transcript_trace::enabled())
            transcript_trace::write(QStringLiteral("height-correction"),
                                    {{QStringLiteral("key"), key},
                                     {QStringLiteral("from"), row.height},
                                     {QStringLiteral("to"), height},
                                     {QStringLiteral("derived"), row.derived},
                                     {QStringLiteral("shape"), row.shape}});
        row.height = height;
        row.realized = true;
        row.derived = false;
        touched = true;
    }
    if (!touched)
        return;
    topsDirty_ = true;
    noteTotalCorrection(previousTotal);
    emit geometryChanged();
}

void TranscriptRowGeometry::setBodyHeight(const QString& key, const qreal bodyHeight) {
    setBodyHeights({{key, bodyHeight}});
}

void TranscriptRowGeometry::setBodyHeights(const QHash<QString, qreal>& bodyHeights) {
    const qreal previousTotal = totalHeight();
    bool touched = false;
    for (auto it = bodyHeights.cbegin(); it != bodyHeights.cend(); ++it) {
        const QString& key = it.key();
        const qreal bodyHeight = it.value();
        if (key.isEmpty() || !(bodyHeight >= 0) || !std::isfinite(bodyHeight))
            continue;
        if (bodyMeasurements_.size() >= kMaximumMeasurements && !bodyMeasurements_.contains(key))
            bodyMeasurements_.clear();
        bodyMeasurements_.insert(key, bodyHeight);
        const int index = rowsByKey_.value(key, -1);
        if (index < 0 || rows_.at(index).realized)
            continue;
        Row& row = rows_[index];
        const qreal before = row.height;
        recomputeHeight(row);
        touched = touched || std::abs(row.height - before) >= 0.01;
    }
    if (!touched)
        return;
    topsDirty_ = true;
    noteTotalCorrection(previousTotal);
    emit geometryChanged();
}

QStringList TranscriptRowGeometry::unmeasuredKeys(const int limit) const {
    QStringList keys;
    for (int index = rows_.size() - 1; index >= 0 && keys.size() < limit; --index) {
        const Row& row = rows_.at(index);
        if (!row.realized && !row.derived)
            keys.append(row.key);
    }
    return keys;
}

void TranscriptRowGeometry::setProvisionalHeight(const qreal height) {
    if (!(height > 0) || !std::isfinite(height) || std::abs(provisionalHeight_ - height) < 0.01)
        return;
    provisionalHeight_ = height;
    for (Row& row : rows_)
        recomputeHeight(row);
    topsDirty_ = true;
    emit geometryChanged();
}

void TranscriptRowGeometry::setLayoutFingerprint(const QString& fingerprint) {
    if (fingerprint == fingerprint_)
        return;
    fingerprint_ = fingerprint;
    staleHeights_.clear();
    for (const Row& row : std::as_const(rows_))
        if (row.realized || row.derived || measurements_.contains(row.key))
            staleHeights_.insert(row.key, row.height);
    for (auto it = measurements_.cbegin();
         it != measurements_.cend() && staleHeights_.size() < kMaximumMeasurements; ++it)
        staleHeights_.insert(it.key(), it.value());
    measurements_.clear();
    bodyMeasurements_.clear();
    // Chrome goes too: padding and header sizes are as width- and font-dependent
    // as the body they surround.
    chrome_.clear();
    for (Row& row : rows_)
        recomputeHeight(row);
    topsDirty_ = true;
    largestCorrection_ = 0;
    emit geometryChanged();
}

void TranscriptRowGeometry::rebuildTops() const {
    tops_.resize(rows_.size() + 1);
    qreal offset = 0;
    for (int row = 0; row < rows_.size(); ++row) {
        tops_[row] = offset;
        offset += rows_.at(row).height;
    }
    if (!tops_.isEmpty())
        tops_[rows_.size()] = offset;
    topsDirty_ = false;
}

qreal TranscriptRowGeometry::height(const int row) const {
    return row >= 0 && row < rows_.size() ? rows_.at(row).height : 0;
}

bool TranscriptRowGeometry::measured(const int row) const {
    return row >= 0 && row < rows_.size() && (rows_.at(row).realized || rows_.at(row).derived);
}

qreal TranscriptRowGeometry::top(const int row) const {
    if (row < 0 || row > rows_.size())
        return 0;
    if (topsDirty_)
        rebuildTops();
    return tops_.isEmpty() ? 0 : tops_.at(row);
}

qreal TranscriptRowGeometry::totalHeight() const {
    if (rows_.isEmpty())
        return 0;
    if (topsDirty_)
        rebuildTops();
    return tops_.at(rows_.size());
}

int TranscriptRowGeometry::measuredRowCount() const {
    return int(std::count_if(rows_.cbegin(), rows_.cend(),
                             [](const Row& row) { return row.realized || row.derived; }));
}

int TranscriptRowGeometry::indexAtY(const qreal y) const {
    if (rows_.isEmpty() || y < 0 || y >= totalHeight())
        return -1;
    if (topsDirty_)
        rebuildTops();
    // tops_ is ascending, so the row containing y is the last one starting at or
    // before it. Zero-height rows share a start, and upper_bound then lands past
    // the run, which is the row that actually occupies y.
    const auto above = std::upper_bound(tops_.cbegin(), tops_.cbegin() + rows_.size(), y);
    return std::max(0, int(above - tops_.cbegin()) - 1);
}

} // namespace ketplus
