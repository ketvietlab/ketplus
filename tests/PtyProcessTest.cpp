#include "terminal/PtyProcess.h"
#include "terminal/TerminalPanel.h"
#include "terminal/TerminalSession.h"
#include "terminal/TerminalView.h"
#include "ui/Theme.h"

#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

namespace ketplus {
namespace {

class PaintCounter final : public QObject {
  public:
    int count{0};

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++count;
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

class PtyProcessTest final : public QObject {
    Q_OBJECT

  private slots:
    void runsInteractiveShellInRequestedDirectory();
    void rendersAnsiOutputThroughVterm();
    void usesSystemFixedFont();
    void appliesTerminalTypography();
    void echoesTypedTextBeforeEnter();
    void restoresFocusAfterAnActionMenuCloses();
    void rendersInputMethodPreedit();
    void coalescesStreamingPaints();
    void controlCInterruptsForegroundCommand();
    void honorsSynchronizedOutputTransactions();
    void selectsAndCopiesTextWithMouse();
    void findsTextInTerminalBuffer();
    void boundsCoreScrollback();
    void emitsMouseProtocolFromCore();
};

void PtyProcessTest::runsInteractiveShellInRequestedDirectory() {
#if defined(Q_OS_UNIX)
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::outputReceived, this,
            [&output](const QByteArray& bytes) { output.append(bytes); });
    QSignalSpy exitSpy(&process, &PtyProcess::exited);

    QVERIFY(process.start(QDir::tempPath(), 24, 80));
    QVERIFY(process.isRunning());
    process.send(QByteArray("printf '__KETPLUS_PTY__%s\\n' \"$PWD\"; exit\n"));

    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 10000);
    QVERIFY2(output.contains("__KETPLUS_PTY__"), output.constData());
    QVERIFY2(output.contains(QDir::tempPath().toUtf8()), output.constData());
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::rendersAnsiOutputThroughVterm() {
#if defined(Q_OS_UNIX)
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    QSignalSpy exitSpy(&session, &TerminalSession::exited);

    terminal.start(QDir::tempPath());
    session.sendBytes(QByteArray("printf '\\033[31m__KETPLUS_VTERM__\\033[0m\\n'; exit\n"));

    QTRY_VERIFY_WITH_TIMEOUT(terminal.visibleText().contains("__KETPLUS_VTERM__"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 10000);
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::usesSystemFixedFont() {
    ThemeManager theme(*qApp);
    TerminalSession session;
    TerminalView terminal(session);
    terminal.ensurePolished();
    QCOMPARE(QFontInfo(terminal.font()).family(),
             QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family());
}

void PtyProcessTest::appliesTerminalTypography() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.setTypography(17, 29);
    QCOMPARE(terminal.font().pixelSize(), 17);
}

void PtyProcessTest::echoesTypedTextBeforeEnter() {
#if defined(Q_OS_UNIX)
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    QSignalSpy exitSpy(&session, &TerminalSession::exited);
    QSignalSpy outputSpy(&session, &TerminalSession::outputReceived);
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);

    terminal.start(QDir::tempPath());
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 10000);
    paints.count = 0;
    QTest::keyClicks(&terminal, QStringLiteral("ketplus_live_echo"));

    QTRY_VERIFY_WITH_TIMEOUT(terminal.visibleText().contains("ketplus_live_echo"), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(paints.count > 0, 200);
    session.sendBytes(QByteArray(1, '\x03'));
    QTest::qWait(100);
    session.sendBytes(QByteArray("exit\n"));
    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 10000);
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::restoresFocusAfterAnActionMenuCloses() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* editor = new QLineEdit(&window);
    auto* panel = new TerminalPanel(&window);
    layout->addWidget(editor);
    layout->addWidget(panel);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));

    panel->focusTerminal();
    editor->setFocus(Qt::OtherFocusReason); // Mimics QMenu restoring its old focus.

    auto* terminal = panel->findChild<TerminalView*>();
    QVERIFY(terminal != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(qApp->focusWidget(), terminal, 1000);
}

void PtyProcessTest::rendersInputMethodPreedit() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    terminal.setFocus();
    QTest::qWait(20);
    const QImage before = terminal.viewport()->grab().toImage();

    QInputMethodEvent preedit(QStringLiteral("đang gõ"), {});
    QApplication::sendEvent(&terminal, &preedit);
    const QImage after = terminal.viewport()->grab().toImage();

    QVERIFY(before != after);
}

void PtyProcessTest::coalescesStreamingPaints() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);
    QTest::qWait(20);
    paints.count = 0;

    for (int chunk = 0; chunk < 120; ++chunk) {
        session.feedOutput(QByteArray("x"));
    }
    QTest::qWait(80);

    QVERIFY(paints.count > 0);
    QVERIFY2(paints.count <= 8,
             qPrintable(QStringLiteral("120 output chunks caused %1 paints").arg(paints.count)));
}

void PtyProcessTest::controlCInterruptsForegroundCommand() {
#if defined(Q_OS_UNIX)
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    QByteArray output;
    connect(&session, &TerminalSession::outputReceived, this,
            [&output](const QByteArray& bytes) { output.append(bytes); });
    QSignalSpy exitSpy(&session, &TerminalSession::exited);

    terminal.start(QDir::tempPath());
    session.sendBytes(QByteArray("stty -echo; printf '__KETPLUS_READY__\\n'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.count("__KETPLUS_READY__") >= 2, 5000);
    output.clear();
    session.sendBytes(
        QByteArray("printf '__SLEEP_STARTED__\\n'; sleep 30 && printf '__SLEEP_COMPLETED__\\n'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("__SLEEP_STARTED__"), 5000);
    output.clear();
#if defined(Q_OS_MACOS)
    QTest::keyClick(&terminal, Qt::Key_C, Qt::MetaModifier);
#else
    QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier);
#endif
    session.sendBytes(QByteArray("printf '__AFTER_INTERRUPT__\\n'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("__AFTER_INTERRUPT__"), 7000);
    session.sendBytes(QByteArray("exit\n"));

    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 7000);
    QVERIFY2(!output.contains("__SLEEP_COMPLETED__"), output.constData());
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::honorsSynchronizedOutputTransactions() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);
    QTest::qWait(80);
    paints.count = 0;

    session.feedOutput(QByteArray("\x1b[?2026hplaceholder"));
    QTest::qWait(30);
    QCOMPARE(paints.count, 0);

    session.feedOutput(QByteArray("\r\x1b[2Ktyped\x1b[?2026l"));
    QTRY_VERIFY_WITH_TIMEOUT(paints.count > 0, 100);
    QVERIFY(terminal.visibleText().contains(QStringLiteral("typed")));
    QVERIFY(!terminal.visibleText().contains(QStringLiteral("placeholder")));
}

void PtyProcessTest::selectsAndCopiesTextWithMouse() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    session.feedOutput(QByteArray("alpha beta\r\nsecond line"));
    QTest::qWait(20);

    const QFontMetrics metrics(terminal.font());
    const int cellWidth = qMax(1, metrics.horizontalAdvance(QLatin1Char('M')));
    const int cellHeight = qMax(metrics.height(), 20);
    const QPoint firstCell(10 + cellWidth / 2, 8 + cellHeight / 2);
    const QPoint fifthCell(10 + 4 * cellWidth + cellWidth / 2, 8 + cellHeight / 2);
    QTest::mousePress(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, firstCell);
    QTest::mouseMove(terminal.viewport(), fifthCell);
    QTest::mouseRelease(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, fifthCell);

    QCOMPARE(terminal.selectedText(), QStringLiteral("alpha"));
    terminal.copySelection();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha"));
}

void PtyProcessTest::findsTextInTerminalBuffer() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(640, 240);
    terminal.show();
    session.feedOutput(QByteArray("first needle\r\nsecond needle"));

    terminal.openSearch();
    auto* searchEdit = terminal.findChild<QLineEdit*>(QStringLiteral("terminalSearchEdit"));
    QVERIFY(searchEdit != nullptr);
    searchEdit->setText(QStringLiteral("needle"));

    auto* status = terminal.findChild<QLabel*>(QStringLiteral("terminalSearchStatus"));
    QVERIFY(status != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("1 / 2"), 500);
}

void PtyProcessTest::boundsCoreScrollback() {
    TerminalSession session;
    session.resizeTerminal(2, 20);
    QByteArray output;
    output.reserve(5100 * 3);
    for (int line = 0; line < 5100; ++line) {
        output.append("x\r\n");
    }
    session.feedOutput(output);

    QCOMPARE(session.historyLineCount(), 5000);
}

void PtyProcessTest::emitsMouseProtocolFromCore() {
    TerminalSession session;
    session.feedOutput(QByteArray("\x1b[?1000h\x1b[?1006h"));
    QVERIFY(session.mouseTrackingEnabled());
    QSignalSpy inputSpy(&session, &TerminalSession::inputGenerated);

    session.sendMouseMove(2, 3, VTERM_MOD_NONE);
    session.sendMouseButton(1, true, VTERM_MOD_NONE);
    session.sendMouseButton(1, false, VTERM_MOD_NONE);

    QVERIFY(!inputSpy.isEmpty());
    QByteArray generated;
    for (const QList<QVariant>& arguments : inputSpy) {
        generated.append(arguments.at(0).toByteArray());
    }
    QVERIFY(generated.contains("\x1b[<0;4;3M"));
    QVERIFY(generated.contains("\x1b[<0;4;3m"));

    session.feedOutput(QByteArray("\x1b[?1000l"));
    QVERIFY(!session.mouseTrackingEnabled());
}

} // namespace ketplus

QTEST_MAIN(ketplus::PtyProcessTest)

#include "PtyProcessTest.moc"
