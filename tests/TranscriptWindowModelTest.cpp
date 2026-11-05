#include "agent/TranscriptWindowModel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QtTest>

using ketplus::TranscriptWindowModel;

namespace {

constexpr int kKeyRole = Qt::UserRole + 1;

class KeyedModel final : public QStandardItemModel {
  public:
    QHash<int, QByteArray> roleNames() const override {
        return {{kKeyRole, QByteArrayLiteral("messageKey")}};
    }
};

class ResettableModel final : public QAbstractListModel {
  public:
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(keys_.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        return index.isValid() && role == kKeyRole ? keys_.value(index.row()) : QVariant{};
    }
    QHash<int, QByteArray> roleNames() const override {
        return {{kKeyRole, QByteArrayLiteral("messageKey")}};
    }
    void replace(const QStringList& keys) {
        beginResetModel();
        keys_ = keys;
        endResetModel();
    }

  private:
    QStringList keys_;
};

QStandardItem* row(const QString& key) {
    auto* item = new QStandardItem;
    item->setData(key, kKeyRole);
    return item;
}

QStringList keys(const TranscriptWindowModel& window) {
    QStringList result;
    for (int index = 0; index < window.rowCount(); ++index)
        result.append(window.index(index).data(kKeyRole).toString());
    return result;
}

QList<int> rows(const TranscriptWindowModel& window) {
    QList<int> result;
    for (int index = 0; index < window.rowCount(); ++index)
        result.append(window.index(index).data(TranscriptWindowModel::RowIndexRole).toInt());
    return result;
}

} // namespace

class TranscriptWindowModelTest final : public QObject {
    Q_OBJECT
  private slots:
    void exposesOnlyTheSpan();
    void slidingByOneRowIsOneInsertAndOneRemove();
    void prependRenumbersSurvivorsWithoutRecreatingThem();
    void removalInsideTheSpanRemovesOnlyThatRow();
    void spanIsClampedToTheSource();
    void delegateInsertionCanExpandTheRange();
    void historyResetCanChangeTheRangeWhileRemovingDelegates();
};

void TranscriptWindowModelTest::exposesOnlyTheSpan() {
    KeyedModel source;
    for (int index = 0; index < 10; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    QCOMPARE(window.rowCount(), 0);
    window.setRange(3, 5);
    QCOMPARE(keys(window), QStringList({"k3", "k4", "k5"}));
    QCOMPARE(rows(window), QList<int>({3, 4, 5}));
    QCOMPARE(window.windowRow(4), 1);
    QCOMPARE(window.windowRow(6), -1);
}

// A delegate is a laid-out Markdown document. Scrolling by one row must cost one
// new delegate, not a rebuild of every row that stayed on screen.
void TranscriptWindowModelTest::slidingByOneRowIsOneInsertAndOneRemove() {
    KeyedModel source;
    for (int index = 0; index < 10; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    window.setRange(3, 6);
    QSignalSpy inserted(&window, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&window, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&window, &QAbstractItemModel::modelReset);

    window.setRange(4, 7);
    QCOMPARE(keys(window), QStringList({"k4", "k5", "k6", "k7"}));
    QCOMPARE(removed.size(), 1);
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(reset.size(), 0);
}

// History arriving above the reader shifts every source row. The rows on screen
// are the same rows: they must keep their delegates and only learn new numbers.
void TranscriptWindowModelTest::prependRenumbersSurvivorsWithoutRecreatingThem() {
    KeyedModel source;
    for (int index = 0; index < 6; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    window.setRange(2, 4);
    QSignalSpy inserted(&window, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&window, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&window, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&window, &QAbstractItemModel::dataChanged);

    source.insertRow(0, row(QStringLiteral("older0")));
    source.insertRow(0, row(QStringLiteral("older1")));

    // The span followed the rows it held, before the view said anything.
    QCOMPARE(keys(window), QStringList({"k2", "k3", "k4"}));
    QCOMPARE(rows(window), QList<int>({4, 5, 6}));
    QCOMPARE(window.first(), 4);
    // Not one delegate rebuilt: only their row numbers changed.
    QCOMPARE(inserted.size(), 0);
    QCOMPARE(removed.size(), 0);
    QCOMPARE(reset.size(), 0);
    QVERIFY(!changed.isEmpty());

    // And the view arriving at the same span afterwards is a no-op.
    window.setRange(4, 6);
    QCOMPARE(inserted.size(), 0);
    QCOMPARE(removed.size(), 0);
}

void TranscriptWindowModelTest::removalInsideTheSpanRemovesOnlyThatRow() {
    KeyedModel source;
    for (int index = 0; index < 6; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    window.setRange(0, 5);
    QSignalSpy removed(&window, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&window, &QAbstractItemModel::modelReset);

    source.removeRow(2);
    QCOMPARE(keys(window), QStringList({"k0", "k1", "k3", "k4", "k5"}));
    QCOMPARE(rows(window), QList<int>({0, 1, 2, 3, 4}));
    QCOMPARE(removed.size(), 1);
    QCOMPARE(reset.size(), 0);
}

void TranscriptWindowModelTest::spanIsClampedToTheSource() {
    KeyedModel source;
    for (int index = 0; index < 3; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    window.setRange(-5, 50);
    QCOMPARE(keys(window), QStringList({"k0", "k1", "k2"}));
    source.clear();
    QCOMPARE(window.rowCount(), 0);
}

void TranscriptWindowModelTest::delegateInsertionCanExpandTheRange() {
    KeyedModel source;
    for (int index = 0; index < 6; ++index)
        source.appendRow(row(QStringLiteral("k%1").arg(index)));
    TranscriptWindowModel window(&source);
    QAbstractItemModelTester tester(&window,
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);
    window.setRange(0, 0);
    connect(&window, &QAbstractItemModel::rowsInserted, &window, [&] { window.setRange(0, 5); });
    window.setRange(0, 2);
    QCOMPARE(keys(window), QStringList({"k0", "k1", "k2", "k3", "k4", "k5"}));
    QCOMPARE(rows(window), QList<int>({0, 1, 2, 3, 4, 5}));
}

void TranscriptWindowModelTest::historyResetCanChangeTheRangeWhileRemovingDelegates() {
    ResettableModel source;
    source.replace({"old0", "old1", "old2"});
    TranscriptWindowModel window(&source);
    QAbstractItemModelTester tester(&window,
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);
    window.setRange(0, 2);
    connect(&window, &QAbstractItemModel::rowsRemoved, &window, [&] { window.setRange(0, 5); });
    source.replace({"k0", "k1", "k2", "k3", "k4", "k5"});
    QCOMPARE(keys(window), QStringList({"k0", "k1", "k2", "k3", "k4", "k5"}));
    QCOMPARE(rows(window), QList<int>({0, 1, 2, 3, 4, 5}));
}

QTEST_MAIN(TranscriptWindowModelTest)
#include "TranscriptWindowModelTest.moc"
