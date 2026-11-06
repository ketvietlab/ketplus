#include "terminal_app/TerminalWindow.h"

#include "terminal/TerminalView.h"
#include "terminal_app/TerminalTab.h"

#include "KetAppearance.h"
#include "KetProfile.h"
#include "KetStyle.h"
#include "KetTokens.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QSettings>
#include <QTabWidget>
#include <QtTest>

namespace ketplus {

class TerminalAppTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void createsAndClosesTabs();
    void drawsTheTabStripActions();
    void followsTheKitAppearance();
    void savesAndRestoresTheAppearance();
    void usesTheSavedFontFamily();
};

void TerminalAppTest::initTestCase() {
    // Keep the developer's own KetPlus Terminal preferences out of the test.
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTerminalAppTest"));
    QCoreApplication::setApplicationName(QStringLiteral("TerminalAppTest"));
    QSettings().clear();
    // The app's own setup (main.cpp), so the window is tested under the profile and style it
    // ships with.
    ket::Profile::setCurrent(ket::Profile::ketPlus());
    ket::Style::install(*qApp);
}

void TerminalAppTest::cleanup() {
    QSettings().clear();
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
}

void TerminalAppTest::createsAndClosesTabs() {
    TerminalWindow window(QDir::tempPath());
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

void TerminalAppTest::drawsTheTabStripActions() {
    TerminalWindow window(QDir::tempPath());
    auto* actions = window.findChild<QWidget*>(QStringLiteral("terminalTabActions"));
    QVERIFY(actions != nullptr);
    QStringList labels;
    for (const QAbstractButton* button : actions->findChildren<QAbstractButton*>()) {
        labels.append(button->toolTip());
    }
    QCOMPARE(labels, QStringList({QStringLiteral("New tab"), QStringLiteral("Find in terminal"),
                                  QStringLiteral("Clear terminal scrollback"),
                                  QStringLiteral("Restart terminal session")}));

    auto* tabs = window.findChild<QTabWidget*>();
    actions->findChildren<QAbstractButton*>().constFirst()->click();
    QCOMPARE(tabs->count(), 2);
}

void TerminalAppTest::followsTheKitAppearance() {
    // The grid is drawn on page-bg and recolours when the kit appearance changes.
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
    TerminalWindow window(QDir::tempPath());
    window.resize(640, 400);
    window.show();
    TerminalView* view = window.currentTab()->view();
    const auto background = [view] {
        const QImage image = view->viewport()->grab().toImage();
        return QColor(image.pixel(image.width() - 4, image.height() - 4));
    };
    QTRY_COMPARE(background(), ket::Tokens::color(u"page-bg", ket::Theme::Light));

    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    QTRY_COMPARE(background(), ket::Tokens::color(u"page-bg", ket::Theme::Dark));
}

void TerminalAppTest::savesAndRestoresTheAppearance() {
    {
        TerminalWindow window(QDir::tempPath());
        QAction* dark = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Dark")) {
                dark = action;
            }
        }
        QVERIFY(dark != nullptr);
        dark->trigger();
    }
    QCOMPARE(QSettings().value(QStringLiteral("appearance/theme")).toString(),
             QStringLiteral("dark"));
    QCOMPARE(ket::Appearance::instance().mode(), ket::Appearance::Mode::Dark);

    // A value ThemeManager saved before the move to the kit still applies.
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
    restoreTerminalAppearance();
    QCOMPARE(ket::Appearance::instance().mode(), ket::Appearance::Mode::Light);

    QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("sepia"));
    restoreTerminalAppearance();
    QCOMPARE(ket::Appearance::instance().mode(), ket::Appearance::Mode::Light);
}

void TerminalAppTest::usesTheSavedFontFamily() {
    QString family;
    const QString fixedFamily =
        QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
    for (const QString& candidate : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(candidate) && candidate != fixedFamily &&
            QFontInfo(QFont(candidate)).family() == candidate) {
            family = candidate;
            break;
        }
    }
    if (family.isEmpty()) {
        QSKIP("No second fixed-pitch family is installed");
    }
    QSettings().setValue(QStringLiteral("terminal/fontFamily"), QStringLiteral(" %1 ").arg(family));
    TerminalWindow window(QDir::tempPath());
    QCOMPARE(QFontInfo(window.currentTab()->view()->font()).family(), family);
}

} // namespace ketplus

QTEST_MAIN(ketplus::TerminalAppTest)

#include "TerminalAppTest.moc"
