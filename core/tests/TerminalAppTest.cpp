#include "terminal_app/TerminalWindow.h"

#include "terminal/TerminalSession.h"
#include "terminal/TerminalTab.h"
#include "terminal/TerminalView.h"
#include "ui/KetFonts.h"

#include "KetAppearance.h"
#include "KetDialog.h"
#include "KetProfile.h"
#include "KetStyle.h"
#include "KetTokens.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTimer>
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
    void interfaceZoomPersistsWithoutChangingTerminalText();
    void showsTheKetPlusName();
    void aboutUsesTheKitDialog();
    void drawsTheTerminalInKetSansMono();
    void composingTextCoversTheCellsUnderIt();
    void inputMethodsRewriteTheWordBeingTyped();
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
    ket::Appearance::instance().setZoom(1);
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

void TerminalAppTest::interfaceZoomPersistsWithoutChangingTerminalText() {
    TerminalWindow window(QDir::tempPath());
    auto* zoom = window.findChild<QAction*>(QStringLiteral("terminalInterfaceZoom125"));
    QVERIFY(zoom);
    const int textSize = window.currentTab()->view()->font().pixelSize();
    zoom->trigger();
    QCOMPARE(ket::Appearance::instance().zoom(), 1.25);
    QCOMPARE(window.currentTab()->view()->font().pixelSize(), textSize);
    ket::Appearance::instance().setZoom(1);
    restoreTerminalAppearance();
    QCOMPARE(ket::Appearance::instance().zoom(), 1.25);
    QVERIFY(zoom->isChecked());
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

void TerminalAppTest::showsTheKetPlusName() {
    // ADR 0029: users see KétPlus Terminal; the application name keeps naming the settings.
    TerminalWindow window(QDir::tempPath());
    QVERIFY2(window.windowTitle().endsWith(QStringLiteral(" — KétPlus Terminal")),
             qPrintable(window.windowTitle()));
    QStringList texts;
    for (const QAction* action : window.findChildren<QAction*>()) {
        texts.append(action->text());
    }
    QVERIFY(texts.contains(QStringLiteral("Quit KétPlus Terminal")));
    QVERIFY(texts.contains(QStringLiteral("About KétPlus Terminal")));
    QVERIFY(!texts.join(QLatin1Char('\n')).contains(QStringLiteral("KetPlus Terminal")));
}

void TerminalAppTest::aboutUsesTheKitDialog() {
    TerminalWindow window(QDir::tempPath());
    window.show();
    QAction* about = nullptr;
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("About KétPlus Terminal"))
            about = action;
    }
    QVERIFY(about != nullptr);
    QString title;
    QString body;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<ket::Dialog*>();
        if (dialog == nullptr)
            return;
        title = dialog->title();
        if (auto* label = qobject_cast<QLabel*>(dialog->body()))
            body = label->text();
        QTest::keyClick(dialog, Qt::Key_Escape);
    });
    about->trigger();
    QCOMPARE(title, QStringLiteral("About KétPlus Terminal"));
    QVERIFY2(body.contains(QStringLiteral("Két Symbol")), qPrintable(body));
}

void TerminalAppTest::drawsTheTerminalInKetSansMono() {
    // The app registers the embedded KÉT fonts at startup (main.cpp); with no saved family the
    // terminal uses the profile's monospace, which is KÉT Sans Mono once it is installed.
    const QStringList families = registerKetFonts();
    QVERIFY2(families.contains(QStringLiteral("KET Sans")), qPrintable(families.join(u',')));
    QVERIFY2(families.contains(QStringLiteral("KET Sans Mono")), qPrintable(families.join(u',')));
    TerminalWindow window(QDir::tempPath());
    QCOMPARE(QFontInfo(window.currentTab()->view()->font()).family(),
             QStringLiteral("KET Sans Mono"));
}

void TerminalAppTest::composingTextCoversTheCellsUnderIt() {
    // An IME keeps the word being typed as preedit text while zsh-autosuggestions draws its
    // dimmed suggestion under the cursor. The preedit must hide that suggestion rather than be
    // painted on top of it, so the result matches composing over empty cells.
    const auto render = [](const QByteArray& shellOutput) {
        TerminalSession session;
        TerminalView view(session);
        view.setFocusPolicy(Qt::NoFocus);
        view.resize(320, 120);
        view.show();
        session.feedOutput(shellOutput);
        QInputMethodEvent compose(QStringLiteral("a"), {});
        QCoreApplication::sendEvent(&view, &compose);
        QCoreApplication::processEvents();
        return view.viewport()->grab().toImage();
    };
    const QImage overSuggestion = render(QByteArrayLiteral("\x1b[90mW\x1b[0m\r"));
    const QImage overBlank = render(QByteArrayLiteral("\r"));
    QCOMPARE(overSuggestion.size(), overBlank.size());
    QVERIFY(overSuggestion == overBlank);
}

void TerminalAppTest::inputMethodsRewriteTheWordBeingTyped() {
    // VNInput types each letter straight into the shell (so zsh completion sees it) and later
    // rewrites the word, for example "duy" -> "dụy". The terminal reports what was typed as the
    // text before the cursor and turns a replacement into backspaces plus the new text.
    TerminalSession session;
    TerminalView view(session);
    QByteArray sent;
    QObject::connect(&session, &TerminalSession::inputGenerated,
                     [&sent](const QByteArray& bytes) { sent += bytes; });
    const auto commit = [&view](const QString& text, int from = 0, int length = 0) {
        QInputMethodEvent event;
        event.setCommitString(text, from, length);
        QCoreApplication::sendEvent(&view, &event);
    };
    const auto query = [&view](Qt::InputMethodQuery property) {
        QInputMethodQueryEvent event(property);
        QCoreApplication::sendEvent(&view, &event);
        return event.value(property);
    };

    commit(QStringLiteral("d"));
    commit(QStringLiteral("u"));
    commit(QStringLiteral("y"));
    QCOMPARE(query(Qt::ImSurroundingText).toString(), QStringLiteral("duy"));
    QCOMPARE(query(Qt::ImCursorPosition).toInt(), 3);
    QCOMPARE(sent, QByteArrayLiteral("duy"));

    sent.clear();
    commit(QString::fromUtf8("dụy"), -3, 3);
    QCOMPARE(sent, QByteArray("\x7f\x7f\x7f") + QString::fromUtf8("dụy").toUtf8());
    QCOMPARE(query(Qt::ImSurroundingText).toString(), QString::fromUtf8("dụy"));

    // Replacing only the middle keeps the tail, which is typed again after it.
    sent.clear();
    commit(QStringLiteral("u"), -2, 1);
    QCOMPARE(sent, QByteArrayLiteral("\x7f\x7fuy"));
    QCOMPARE(query(Qt::ImSurroundingText).toString(), QStringLiteral("duy"));

    // A key that moves the shell cursor ends the word: nothing before it can be rewritten.
    QTest::keyClick(&view, Qt::Key_Left);
    QCOMPARE(query(Qt::ImSurroundingText).toString(), QString());
    sent.clear();
    commit(QStringLiteral("x"), -3, 3);
    QCOMPARE(sent, QByteArrayLiteral("x"));
}

} // namespace ketplus

QTEST_MAIN(ketplus::TerminalAppTest)

#include "TerminalAppTest.moc"
