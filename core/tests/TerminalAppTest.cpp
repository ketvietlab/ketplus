#include "terminal_app/TerminalWindow.h"

#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QTabWidget>
#include <QtTest>

namespace ketplus {

class TerminalAppTest final : public QObject {
    Q_OBJECT

  private slots:
    void createsAndClosesTabs();
};

void TerminalAppTest::createsAndClosesTabs() {
    ThemeManager theme(*qApp);
    TerminalWindow window(theme, QDir::tempPath());
    auto* tabs = window.findChild<QTabWidget*>();
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 1);

    QAction* newTabAction = nullptr;
    QAction* closeTabAction = nullptr;
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("New Tab")) {
            newTabAction = action;
        } else if (action->text() == QStringLiteral("Close Tab")) {
            closeTabAction = action;
        }
    }
    QVERIFY(newTabAction != nullptr);
    QVERIFY(closeTabAction != nullptr);

    newTabAction->trigger();
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentIndex(), 1);

    closeTabAction->trigger();
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentIndex(), 0);
}

} // namespace ketplus

QTEST_MAIN(ketplus::TerminalAppTest)

#include "TerminalAppTest.moc"
