#include "terminal/PtyProcess.h"
#include "terminal/TerminalPanel.h"
#include "terminal/TerminalView.h"
#include "ui/Theme.h"

#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QInputMethodEvent>
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
    void echoesTypedTextBeforeEnter();
    void restoresFocusAfterAnActionMenuCloses();
    void rendersInputMethodPreedit();
    void coalescesStreamingPaints();
    void controlCInterruptsForegroundCommand();
    void honorsSynchronizedOutputTransactions();
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
    PtyProcess process;
    TerminalView terminal(process);
    terminal.resize(640, 240);
    terminal.show();
    QSignalSpy exitSpy(&process, &PtyProcess::exited);

    terminal.start(QDir::tempPath());
    process.send(QByteArray("printf '\\033[31m__KETPLUS_VTERM__\\033[0m\\n'; exit\n"));

    QTRY_VERIFY_WITH_TIMEOUT(terminal.visibleText().contains("__KETPLUS_VTERM__"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 10000);
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::usesSystemFixedFont() {
    ThemeManager theme(*qApp);
    PtyProcess process;
    TerminalView terminal(process);
    terminal.ensurePolished();
    QCOMPARE(QFontInfo(terminal.font()).family(),
             QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family());
}

void PtyProcessTest::echoesTypedTextBeforeEnter() {
#if defined(Q_OS_UNIX)
    PtyProcess process;
    TerminalView terminal(process);
    terminal.resize(640, 240);
    terminal.show();
    QSignalSpy exitSpy(&process, &PtyProcess::exited);
    QSignalSpy outputSpy(&process, &PtyProcess::outputReceived);
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);

    terminal.start(QDir::tempPath());
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 3000);
    paints.count = 0;
    QTest::keyClicks(&terminal, QStringLiteral("ketplus_live_echo"));

    QTRY_VERIFY_WITH_TIMEOUT(terminal.visibleText().contains("ketplus_live_echo"), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(paints.count > 0, 200);
    process.send(QByteArray(1, '\x03'));
    QTest::qWait(100);
    process.send(QByteArray("exit\n"));
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
    PtyProcess process;
    TerminalView terminal(process);
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
    PtyProcess process;
    TerminalView terminal(process);
    terminal.resize(640, 240);
    terminal.show();
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);
    QTest::qWait(20);
    paints.count = 0;

    for (int chunk = 0; chunk < 120; ++chunk) {
        process.outputReceived(QByteArray("x"));
    }
    QTest::qWait(80);

    QVERIFY(paints.count > 0);
    QVERIFY2(paints.count <= 8,
             qPrintable(QStringLiteral("120 output chunks caused %1 paints")
                            .arg(paints.count)));
}

void PtyProcessTest::controlCInterruptsForegroundCommand() {
#if defined(Q_OS_UNIX)
    PtyProcess process;
    TerminalView terminal(process);
    terminal.resize(640, 240);
    terminal.show();
    QByteArray output;
    connect(&process, &PtyProcess::outputReceived, this,
            [&output](const QByteArray& bytes) { output.append(bytes); });
    QSignalSpy exitSpy(&process, &PtyProcess::exited);

    terminal.start(QDir::tempPath());
    process.send(QByteArray("printf '__KETPLUS_READY__\\n'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("__KETPLUS_READY__"), 5000);
    output.clear();
    process.send(QByteArray("sleep 30; printf '__SLEEP_COMPLETED__\\n'\n"));
    QTest::qWait(100);
#if defined(Q_OS_MACOS)
    QTest::keyClick(&terminal, Qt::Key_C, Qt::MetaModifier);
#else
    QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier);
#endif
    QTest::qWait(100);
    process.send(QByteArray("printf '__AFTER_INTERRUPT__\\n'; exit\n"));

    QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 7000);
    QVERIFY2(output.contains("__AFTER_INTERRUPT__"), output.constData());
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::honorsSynchronizedOutputTransactions() {
    PtyProcess process;
    TerminalView terminal(process);
    terminal.resize(640, 240);
    terminal.show();
    PaintCounter paints;
    terminal.viewport()->installEventFilter(&paints);
    QTest::qWait(20);
    paints.count = 0;

    process.outputReceived(QByteArray("\x1b[?2026hplaceholder"));
    QTest::qWait(30);
    QCOMPARE(paints.count, 0);

    process.outputReceived(QByteArray("\r\x1b[2Ktyped\x1b[?2026l"));
    QTRY_VERIFY_WITH_TIMEOUT(paints.count > 0, 100);
    QVERIFY(terminal.visibleText().contains(QStringLiteral("typed")));
    QVERIFY(!terminal.visibleText().contains(QStringLiteral("placeholder")));
}

} // namespace ketplus

QTEST_MAIN(ketplus::PtyProcessTest)

#include "PtyProcessTest.moc"
