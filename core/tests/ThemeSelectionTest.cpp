#include "ui/theme/ThemeSelection.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus;

class ThemeSelectionTest : public QObject {
    Q_OBJECT
  private slots:
    void migration_data() {
        QTest::addColumn<QString>("legacy");
        QTest::addColumn<bool>("follow");
        QTest::addColumn<QString>("scheme");
        QTest::newRow("light") << QStringLiteral("light") << false
                               << QStringLiteral("builtin-light");
        QTest::newRow("dark") << QStringLiteral("dark") << false << QStringLiteral("builtin-dark");
        QTest::newRow("system") << QStringLiteral("system") << true
                                << QStringLiteral("builtin-light");
        QTest::newRow("unknown") << QStringLiteral("unknown") << true
                                 << QStringLiteral("builtin-light");
    }
    void migration() {
        QFETCH(QString, legacy);
        QFETCH(bool, follow);
        QFETCH(QString, scheme);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore store(dir.path(), legacy);
        QVERIFY2(store.lastError().isEmpty(), qPrintable(store.lastError()));
        QCOMPARE(store.revision(), quint64(1));
        QCOMPARE(store.selection().followSystem, follow);
        QCOMPARE(store.selection().schemeId, scheme);
        ThemeSelectionStore reopened(dir.path(), legacy == u"dark" ? QStringLiteral("light")
                                                                   : QStringLiteral("dark"));
        QVERIFY(reopened.selection() == store.selection());
        QCOMPARE(reopened.revision(), quint64(1));
    }
    void commitAndWatchAtomicReplacement() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore first(dir.path()), second(dir.path());
        QSignalSpy firstChanged(&first, &ThemeSelectionStore::changed);
        QSignalSpy secondChanged(&second, &ThemeSelectionStore::changed);
        auto selection = first.selection();
        selection.followSystem = false;
        selection.schemeId = QStringLiteral("local-scheme");
        selection.lightSchemeId = QStringLiteral("local-light");
        selection.darkSchemeId = QStringLiteral("local-dark");
        QVERIFY(first.commit(selection));
        QCOMPARE(firstChanged.count(), 1);
        QTRY_COMPARE(secondChanged.count(), 1);
        QVERIFY(second.selection() == selection);
        QCOMPARE(second.revision(), quint64(2));
        first.refresh();
        second.refresh();
        QCOMPARE(firstChanged.count(), 1);
        QCOMPARE(secondChanged.count(), 1);
        QVERIFY(second.commit(selection));
        QCOMPARE(second.revision(), quint64(2));
        QCOMPARE(secondChanged.count(), 1);
        selection.followSystem = true;
        QVERIFY(second.commit(selection));
        QTRY_COMPARE(firstChanged.count(), 2);
        QCOMPARE(first.revision(), quint64(3));
        QVERIFY(first.selection() == selection);
    }
    void crossProcess() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore parent(dir.path());
        QSignalSpy changed(&parent, &ThemeSelectionStore::changed);
        for (int i = 0; i < 2; ++i) {
            const QString id = QStringLiteral("child-%1").arg(i);
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--commit"), dir.path(), id});
            QVERIFY(child.waitForFinished(10000));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
            QTRY_COMPARE(changed.count(), i + 1);
            QCOMPARE(parent.selection().schemeId, id);
            QCOMPARE(parent.revision(), quint64(i + 2));
        }
    }
    void lockingAndInvalidIds() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore store(dir.path());
        QFile existing(store.filePath());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        const auto before = existing.readAll();
        existing.close();
        auto changed = store.selection();
        changed.schemeId = QStringLiteral("new-id");
        QLockFile lock(dir.filePath(".selection.lock"));
        QVERIFY(lock.tryLock());
        QVERIFY(!store.commit(changed));
        QCOMPARE(store.revision(), quint64(1));
        lock.unlock();
        changed.schemeId = QStringLiteral("../escape");
        QVERIFY(!store.commit(changed));
        changed.schemeId = QStringLiteral("bad\n");
        QVERIFY(!store.commit(changed));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), before);
    }
    void malformedAndNewerConfigPreservesLastValid() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore store(dir.path());
        const auto previous = store.selection();
        QSignalSpy changed(&store, &ThemeSelectionStore::changed);
        QFile file(store.filePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto valid = file.readAll();
        file.close();
        for (const auto& bytes :
             {QByteArray("broken"), QByteArray("[]"), QByteArray(17 * 1024, ' ')}) {
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
            file.close();
            store.refresh();
            QVERIFY(!store.lastError().isEmpty());
            QVERIFY(store.selection() == previous);
            QCOMPARE(store.revision(), quint64(1));
            QVERIFY(!store.commit(previous));
            QCOMPARE(changed.count(), 0);
        }
        auto future = QJsonDocument::fromJson(valid).object();
        future.insert("schemaVersion", 2);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(future).toJson());
        file.close();
        ThemeSelectionStore restarted(dir.path());
        QVERIFY(!restarted.lastError().isEmpty());
        QVERIFY(!restarted.commit(previous));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(valid);
        file.close();
        store.refresh();
        QVERIFY(store.lastError().isEmpty());
        QVERIFY(store.commit(previous));
    }
    void invalidRevisions_data() {
        QTest::addColumn<QJsonValue>("revision");
        QTest::newRow("number") << QJsonValue(2);
        QTest::newRow("zero") << QJsonValue("0");
        QTest::newRow("negative") << QJsonValue("-1");
        QTest::newRow("fraction") << QJsonValue("1.5");
        QTest::newRow("overflow") << QJsonValue("18446744073709551616");
    }
    void invalidRevisions() {
        QFETCH(QJsonValue, revision);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ThemeSelectionStore store(dir.path());
        QFile file(store.filePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        object.insert("revision", revision);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(object).toJson());
        file.close();
        store.refresh();
        QVERIFY(!store.lastError().isEmpty());
        QCOMPARE(store.revision(), quint64(1));
    }
};
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 4 && arguments[1] == u"--commit") {
        ThemeSelectionStore store(arguments[2]);
        auto selected = store.selection();
        selected.followSystem = false;
        selected.schemeId = arguments[3];
        return store.commit(selected) ? 0 : 1;
    }
    ThemeSelectionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ThemeSelectionTest.moc"
