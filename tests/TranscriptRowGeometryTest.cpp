#include "agent/TranscriptRowGeometry.h"

#include <QSignalSpy>
#include <QtTest>

using ketplus::TranscriptRowGeometry;

namespace {
QStringList keys(const std::initializer_list<const char*> names) {
    QStringList result;
    for (const char* name : names)
        result.append(QString::fromLatin1(name));
    return result;
}
} // namespace

class TranscriptRowGeometryTest final : public QObject {
    Q_OBJECT
  private slots:
    void unmeasuredRowsUseOneConstant();
    void measuringOneRowMovesOnlyThatRow();
    void totalIsMonotoneAsRowsAreRealized();
    void measurementsSurviveTheRowsThatProducedThem();
    void fingerprintChangeKeepsHeightsAsEstimates();
    void insertShiftsFollowingRowsOnly();
    void indexAtYFindsTheOccupyingRow();
};

// The property ListView cannot give us: an unrealized row's height does not
// depend on any other row, so it cannot be rewritten by a measurement elsewhere.
void TranscriptRowGeometryTest::unmeasuredRowsUseOneConstant() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"a", "b", "c"}));
    QCOMPARE(geometry.totalHeight(), 300.0);
    QCOMPARE(geometry.measuredRowCount(), 0);

    geometry.setRowHeight(QStringLiteral("b"), 5000);
    // A 5000 px row says nothing about a and c.
    QCOMPARE(geometry.height(0), 100.0);
    QCOMPARE(geometry.height(2), 100.0);
    QCOMPARE(geometry.totalHeight(), 5200.0);
}

void TranscriptRowGeometryTest::measuringOneRowMovesOnlyThatRow() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"a", "b", "c"}));
    QCOMPARE(geometry.top(2), 200.0);

    geometry.setRowHeight(QStringLiteral("a"), 400);
    // Only rows after the corrected one move, and by exactly its correction.
    QCOMPARE(geometry.top(0), 0.0);
    QCOMPARE(geometry.top(1), 400.0);
    QCOMPARE(geometry.top(2), 500.0);
    QCOMPARE(geometry.largestTotalCorrection(), 300.0);
}

// A sum of independent heights can only ever be corrected one row at a time, so
// every step the total takes is bounded by a single row's error. ListView's
// average has no such bound: it re-extrapolates every unrealized row at once.
void TranscriptRowGeometryTest::totalIsMonotoneAsRowsAreRealized() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    QStringList rows;
    for (int index = 0; index < 300; ++index)
        rows.append(QStringLiteral("row-%1").arg(index));
    geometry.reset(rows);
    QCOMPARE(geometry.totalHeight(), 30000.0);

    // Realize them in a scattered order, with heights spanning the range a real
    // transcript has: collapsed leaders next to multi-thousand-pixel messages.
    qreal previous = geometry.totalHeight();
    qreal largestStep = 0;
    for (int index = 0; index < 300; ++index) {
        const int row = (index * 97) % 300;
        const qreal height = row % 7 == 0 ? 4800.0 : 40.0;
        geometry.setRowHeight(QStringLiteral("row-%1").arg(row), height);
        largestStep = qMax(largestStep, qAbs(geometry.totalHeight() - previous));
        previous = geometry.totalHeight();
    }
    // No step exceeds the largest single row's departure from the constant.
    QVERIFY2(largestStep <= 4700.0 + 0.01,
             qPrintable(QStringLiteral("largest step %1").arg(largestStep)));
    QCOMPARE(geometry.measuredRowCount(), 300);
}

void TranscriptRowGeometryTest::measurementsSurviveTheRowsThatProducedThem() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"a", "b"}));
    geometry.setRowHeight(QStringLiteral("a"), 900);

    // Window moves off a and back onto it: the row returns at its real height,
    // so scrolling back over history does not re-inflate the total.
    geometry.reset(keys({"b", "c"}));
    QCOMPARE(geometry.totalHeight(), 200.0);
    geometry.reset(keys({"a", "b"}));
    QCOMPARE(geometry.height(0), 900.0);
    QVERIFY(geometry.measured(0));
}

void TranscriptRowGeometryTest::fingerprintChangeKeepsHeightsAsEstimates() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"a", "b"}));
    geometry.setRowHeight(QStringLiteral("a"), 900);
    QCOMPARE(geometry.totalHeight(), 1000.0);

    // A height taken at the old width is not a measurement at the new one, so
    // none counts as measured. It is still a far better estimate than the
    // constant: the extent must not collapse under the reader on a resize.
    geometry.setLayoutFingerprint(QStringLiteral("w=400,font=Inter,size=13,lh=1.4,dpr=2"));
    QCOMPARE(geometry.measuredRowCount(), 0);
    QCOMPARE(geometry.totalHeight(), 1000.0);
    QCOMPARE(geometry.unmeasuredKeys(10).size(), 2);
    // The estimate survives the row set being republished.
    geometry.reset(keys({"a", "b"}));
    QCOMPARE(geometry.height(0), 900.0);
    QVERIFY(!geometry.measured(0));

    geometry.setRowHeight(QStringLiteral("a"), 1500);
    QCOMPARE(geometry.totalHeight(), 1600.0);
    // Returning to the old fingerprint does not resurrect the old numbers as
    // measurements.
    geometry.setLayoutFingerprint(QStringLiteral("w=800,font=Inter,size=13,lh=1.4,dpr=2"));
    QCOMPARE(geometry.measuredRowCount(), 0);
    QCOMPARE(geometry.height(0), 1500.0);
}

void TranscriptRowGeometryTest::insertShiftsFollowingRowsOnly() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"c", "d"}));
    geometry.setRowHeight(QStringLiteral("d"), 700);
    const qreal dTop = geometry.top(1);

    // A page of older history arrives above. The row set is republished whole,
    // but the cache means nothing already measured is forgotten, so the loaded
    // rows keep their heights and move down by exactly what arrived above them.
    geometry.reset(keys({"a", "b", "c", "d"}));
    QCOMPARE(geometry.rowCount(), 4);
    QCOMPARE(geometry.height(3), 700.0);
    QVERIFY(geometry.measured(3));
    QCOMPARE(geometry.top(3), dTop + 200.0);

    geometry.reset(keys({"c", "d"}));
    QCOMPARE(geometry.top(1), dTop);
}

void TranscriptRowGeometryTest::indexAtYFindsTheOccupyingRow() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset(keys({"a", "b", "c"}));
    geometry.setRowHeight(QStringLiteral("b"), 500);

    QCOMPARE(geometry.indexAtY(0), 0);
    QCOMPARE(geometry.indexAtY(99.9), 0);
    QCOMPARE(geometry.indexAtY(100), 1);
    QCOMPARE(geometry.indexAtY(599.9), 1);
    QCOMPARE(geometry.indexAtY(600), 2);
    QCOMPARE(geometry.indexAtY(-1), -1);
    QCOMPARE(geometry.indexAtY(700), -1);

    // A zero-height row never claims a position: the row that occupies y does.
    geometry.setRowHeight(QStringLiteral("b"), 0);
    QCOMPARE(geometry.indexAtY(100), 2);
}

QTEST_MAIN(TranscriptRowGeometryTest)
#include "TranscriptRowGeometryTest.moc"
