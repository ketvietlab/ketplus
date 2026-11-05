#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

namespace ketplus {

// The transcript's row geometry, owned here instead of inferred by ListView.
//
// ListView sizes rows it has not realized from a running average over the rows
// it has, so realizing a row rewrites the assumed height of every unrealized
// row and the total never settles while the reader scrolls. Measured on a real
// conversation the total swung between roughly 40,000 px and 348,768 px while
// the tallest row that exists was 9,006 px. That average is
// QQuickItemViewPrivate::averageSize and QML cannot write it, so the only way
// to a total that means something is to keep our own.
//
// Here a row that has never been measured contributes one fixed constant, not a
// value derived from other rows, so realizing a row changes that row's height
// and nothing else. The total is then monotone with respect to realization: it
// moves by one row's correction at a time and converges as the reader scrolls,
// instead of being re-extrapolated on every measurement.
//
// This class deliberately does not lay anything out. In its first form it runs
// alongside ListView and is read only for diagnostics and for a scrollbar that
// does not lie; owning the layout is a later step that needs these numbers to
// be trustworthy first.
class TranscriptRowGeometry final : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal totalHeight READ totalHeight NOTIFY geometryChanged)
    Q_PROPERTY(int rowCount READ rowCount NOTIFY geometryChanged)
    Q_PROPERTY(int measuredRowCount READ measuredRowCount NOTIFY geometryChanged)
    Q_PROPERTY(qreal provisionalHeight READ provisionalHeight WRITE setProvisionalHeight NOTIFY
                   geometryChanged)
  public:
    explicit TranscriptRowGeometry(QObject* parent = nullptr);

    // The row set, republished whole on every model change. The window is
    // bounded at a few hundred rows and a measurement is cached by key, so
    // rebuilding costs a few hundred lookups and loses nothing: a row that was
    // already measured comes back measured, and rows above an insertion keep
    // their heights and so their positions. Incremental insert/remove would buy
    // nothing at this size and would add a second copy of the row set to drift
    // out of step with the model's.
    // Shapes run parallel to keys: a short signature of everything about a row
    // that decides the chrome around its body -- kind, category, whether it
    // leads a group, whether it continues a chunked message. Rows sharing a
    // shape share their chrome.
    Q_INVOKABLE void reset(const QStringList& keys, const QStringList& shapes = {});

    // A realized delegate reporting what it actually is, and how much of that
    // was its body. The difference is the chrome for this row's shape -- header,
    // padding, spacing -- which is learned from rows that laid themselves out
    // rather than reconstructed in C++, because a second copy of the QML layout
    // would drift out of step with the first one and we would not find out until
    // the numbers were already wrong.
    Q_INVOKABLE void setRowHeight(const QString& key, qreal height, qreal bodyHeight = -1);

    // A body measured away from its delegate. It becomes a row height only for a
    // shape whose chrome some realized row has already taught us; until then the
    // row keeps the constant, because a body height alone is not a row height
    // and guessing the difference is how an estimate creeps back in.
    Q_INVOKABLE void setBodyHeight(const QString& key, qreal bodyHeight);
    // Several at once, announced once. Heights and the announcement land
    // together: a row's height must never change without the view being told
    // in the same step, or anything that places rows in between would use the
    // new heights while the rows are still drawn at the old ones.
    void setBodyHeights(const QHash<QString, qreal>& bodyHeights);

    // Rows in the window that no delegate has reported and no measurement has
    // reached, newest first -- the order they are worth measuring in.
    Q_INVOKABLE QStringList unmeasuredKeys(int limit) const;

    Q_INVOKABLE qreal height(int row) const;
    Q_INVOKABLE qreal top(int row) const;
    Q_INVOKABLE bool measured(int row) const;
    // First row whose span contains y, or -1 when y falls outside the content.
    Q_INVOKABLE int indexAtY(qreal y) const;
    // Row currently holding this key, or -1. A reading position is held as a
    // key and an offset into that row, so it survives rows being inserted or
    // removed above it and heights changing anywhere.
    Q_INVOKABLE int rowForKey(const QString& key) const { return rowsByKey_.value(key, -1); }
    Q_INVOKABLE QString keyAt(int row) const {
        return row >= 0 && row < rows_.size() ? rows_.at(row).key : QString{};
    }

    // Width, font family, pixel size, line height and device pixel ratio: every
    // input a measurement depended on. A measurement taken at a different one is
    // not an approximation of this one, so the whole cache is discarded rather
    // than letting stale heights be applied at a geometry they were not taken
    // at.
    Q_INVOKABLE void setLayoutFingerprint(const QString& fingerprint);
    QString layoutFingerprint() const { return fingerprint_; }

    qreal totalHeight() const;
    int rowCount() const { return int(rows_.size()); }
    int measuredRowCount() const;
    qreal provisionalHeight() const { return provisionalHeight_; }
    void setProvisionalHeight(qreal height);

    // Diagnostics: how far the total has had to move as rows were realized. A
    // sum of independent row heights can only be corrected one row at a time, so
    // a large value here means the bookkeeping is wrong, not that the content
    // changed.
    Q_INVOKABLE qreal largestTotalCorrection() const { return largestCorrection_; }
    Q_INVOKABLE void resetDiagnostics() { largestCorrection_ = 0; }

  signals:
    void geometryChanged();

  private:
    struct Row {
        QString key;
        QString shape;
        qreal height{0};
        qreal bodyHeight{-1};
        // Realized: a delegate reported this exact row. Derived: its body was
        // measured and its shape's chrome was already known. Neither: the
        // constant.
        bool realized{false};
        bool derived{false};
    };
    void recomputeHeight(Row& row) const;
    // Cached measurements survive the rows that produced them, so a row leaving
    // and returning with the window keeps its real height instead of falling
    // back to the constant.
    void applyCachedHeight(Row& row) const;
    void noteTotalCorrection(qreal before);

    QList<Row> rows_;
    QHash<QString, int> rowsByKey_;
    QHash<QString, qreal> measurements_;
    QHash<QString, qreal> bodyMeasurements_;
    // shape -> chrome, the part of a row that is not its body.
    QHash<QString, qreal> chrome_;
    // Heights measured under the previous layout fingerprint. Wrong by the
    // re-wrap a width change causes, but far nearer than the constant, so a
    // resize does not collapse every unrealized row to it at once. Not
    // "measured": the measurer and delegates still replace them.
    QHash<QString, qreal> staleHeights_;
    QString fingerprint_;
    qreal provisionalHeight_{120};
    qreal largestCorrection_{0};
    // tops_ is a prefix sum rebuilt on demand. The transcript window is bounded
    // at a few hundred rows, so recomputing beats maintaining an incremental
    // structure that can drift out of step with rows_.
    mutable QList<qreal> tops_;
    mutable bool topsDirty_{true};
    void rebuildTops() const;
};

} // namespace ketplus
