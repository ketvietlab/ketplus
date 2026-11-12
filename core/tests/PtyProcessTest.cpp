#include "terminal/PtyProcess.h"
#include "terminal/TerminalPanel.h"
#include "terminal/TerminalSession.h"
#include "terminal/TerminalTab.h"
#include "terminal/TerminalView.h"
#include "ui/Theme.h"
#include <QAbstractButton>
#include <QTabBar>
#include <QTabWidget>

#include "KetTokens.h"

#include "KetProfile.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <utility>

#include <cmath>
#include <cstdio>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/resource.h>
#endif

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

// A slow runner can deliver the first exposure paints long after show(), so a fixed wait does
// not say the view is idle. Wait for the first paint, then until 100 ms pass without another.
bool waitUntilPaintsSettle(const PaintCounter& paints) {
    if (!QTest::qWaitFor([&] { return paints.count > 0; }, 5000))
        return false;
    for (int round = 0; round < 50; ++round) {
        const int before = paints.count;
        QTest::qWait(100);
        if (paints.count == before)
            return true;
    }
    return false;
}

} // namespace

class PtyProcessTest final : public QObject {
    Q_OBJECT

  private slots:
    void runsInteractiveShellInRequestedDirectory();
    void typedTextWaitsForTheShellAndIsNotRun();
    void stoppedShellIsReaped_data();
    void stoppedShellIsReaped();
    void exitingRightAfterStopLeavesNoShell();
    void masterIsNotInherited();
    void rendersAnsiOutputThroughVterm();
    void drawsAnsiColoursFromKetTokens();
    void usesProfileFixedFont();
    void appliesTerminalTypography();
    void appliesTerminalFontFamily();
    void keepsTerminalFontFamilyUnderTheme();
    void echoesTypedTextBeforeEnter();
    void restoresFocusAfterAnActionMenuCloses();
    void tabStaysInTheTerminal();
    void embeddedTabsKeepIndependentSessions();
    void singleSessionHasNoTabChrome();
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

void PtyProcessTest::stoppedShellIsReaped_data() {
    QTest::addColumn<QByteArray>("setup");
    QTest::newRow("hangup") << QByteArray();
    QTest::newRow("ignores hangup") << QByteArray("trap '' HUP; ");
}

void PtyProcessTest::stoppedShellIsReaped() {
#if defined(Q_OS_UNIX)
    QFETCH(QByteArray, setup);
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::outputReceived, this,
            [&output](const QByteArray& bytes) { output.append(bytes); });
    QVERIFY(process.start(QDir::tempPath(), 24, 80));
    process.send(setup + "printf '__KETPLUS_PID_%s__\\n' \"$$\"\n");
    static const QRegularExpression pidLine(QStringLiteral("__KETPLUS_PID_([0-9]+)__"));
    QTRY_VERIFY_WITH_TIMEOUT(pidLine.match(QString::fromUtf8(output)).hasMatch(), 10000);
    const auto child =
        static_cast<pid_t>(pidLine.match(QString::fromUtf8(output)).captured(1).toLongLong());
    QVERIFY(child > 0);
    process.stop();
    // A zombie still answers kill(pid, 0); only a reaped child is gone.
    QTRY_VERIFY_WITH_TIMEOUT(::kill(child, 0) != 0 && errno == ESRCH, 5000);
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::exitingRightAfterStopLeavesNoShell() {
#if defined(Q_OS_UNIX)
    // The probe (this executable, see main) starts a shell that ignores the hangup, stops it
    // and returns from main at once; the shell must be gone by the time the probe has exited.
    QProcess probe;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("KETPLUS_PTY_EXIT_PROBE"), QStringLiteral("1"));
    probe.setProcessEnvironment(environment);
    probe.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(probe.waitForFinished(15000));
    QCOMPARE(probe.exitCode(), 0);
    const auto child = static_cast<pid_t>(probe.readAllStandardOutput().trimmed().toLongLong());
    QVERIFY(child > 0);
    const bool alive = ::kill(child, 0) == 0;
    if (alive)
        ::kill(child, SIGKILL);
    QVERIFY(!alive);
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::masterIsNotInherited() {
#if defined(Q_OS_UNIX)
    const auto masters = [] {
        QList<int> found;
        rlimit limit{};
        const int last = ::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 4096
                             ? static_cast<int>(limit.rlim_cur)
                             : 4096;
        for (int fd = 3; fd < last; ++fd) {
            if (::fcntl(fd, F_GETFD) >= 0 && ::ptsname(fd) != nullptr)
                found.append(fd);
        }
        return found;
    };
    const auto before = masters();
    PtyProcess process;
    QVERIFY(process.start(QDir::tempPath(), 24, 80));
    const auto after = masters();
    QList<int> opened;
    for (const int fd : after) {
        if (!before.contains(fd))
            opened.append(fd);
    }
    QCOMPARE(opened.size(), 1);
    QVERIFY(::fcntl(opened.first(), F_GETFD) & FD_CLOEXEC);
    process.stop();
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
}

void PtyProcessTest::typedTextWaitsForTheShellAndIsNotRun() {
#if defined(Q_OS_UNIX)
    // Type in Terminal (#170): the command is typed before the shell starts and left at the
    // prompt for the user to run with Return.
    TerminalPanel panel;
    panel.resize(640, 240);
    panel.show();
    panel.start(QDir::tempPath());
    // A line break would run it, so it is dropped (#449 review).
    panel.typeText(QStringLiteral("echo __KETPLUS_TYPED__\r\n"));
    auto* view = panel.findChild<TerminalView*>();
    QVERIFY(view != nullptr);
    // On failure, show what the terminal held, so a runner's shell can be told apart.
    QTRY_VERIFY2_WITH_TIMEOUT(view->visibleText().contains("echo __KETPLUS_TYPED__"),
                              qPrintable(view->visibleText()), 5000);
    QTest::qWait(500);
    QCOMPARE(view->visibleText().count(QStringLiteral("__KETPLUS_TYPED__")), 1);
    // Another command goes to a new tab, never into the program running in the open one.
    panel.typeTextInNewTab(QStringLiteral("echo __KETPLUS_SECOND__"));
    QCOMPARE(panel.findChildren<TerminalView*>().size(), 2);
    QVERIFY(!view->visibleText().contains(QStringLiteral("__KETPLUS_SECOND__")));
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

void PtyProcessTest::drawsAnsiColoursFromKetTokens() {
    // The 16 ANSI colours are Két DS tokens. Each reads at 4.5:1 on the terminal background
    // except the background's own slot, and a theme switch recolours output already on screen.
    ThemeManager theme(*qApp);
    TerminalSession session;
    TerminalView terminal(session);
    QByteArray output;
    for (int index = 0; index < 16; ++index) {
        output += QByteArray("\x1b[") + QByteArray::number(index < 8 ? 30 + index : 82 + index) +
                  QByteArray("mX");
    }
    output += QByteArray("\x1b[0m");

    const auto luminance = [](const QColor& colour) {
        const auto linear = [](const qreal value) {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(colour.redF()) + 0.7152 * linear(colour.greenF()) +
               0.0722 * linear(colour.blueF());
    };
    for (const auto mode : {ThemeManager::Mode::Dark, ThemeManager::Mode::Light}) {
        theme.setMode(mode);
        terminal.applyTheme(theme.palette());
        if (mode == ThemeManager::Mode::Dark) {
            session.feedOutput(output);
        }
        QCOMPARE(session.lineText(0), QStringLiteral("XXXXXXXXXXXXXXXX"));
        const ket::Theme kit = theme.palette().dark ? ket::Theme::Dark : ket::Theme::Light;
        const qreal background = luminance(QColor(theme.palette().panelSubtle));
        for (int index = 0; index < 16; ++index) {
            VTermColor colour = session.cellAt(0, index).fg;
            QVERIFY(VTERM_COLOR_IS_INDEXED(&colour));
            session.convertColorToRgb(colour);
            const QColor drawn(colour.rgb.red, colour.rgb.green, colour.rgb.blue);
            const QString name = QStringLiteral("terminal-ansi-%1").arg(index);
            QCOMPARE(drawn, ket::Tokens::color(name, kit));
            if (index == (theme.palette().dark ? 0 : 15)) {
                continue;
            }
            const qreal ink = luminance(drawn);
            const qreal ratio = (qMax(ink, background) + 0.05) / (qMin(ink, background) + 0.05);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("%1 is %2:1").arg(name).arg(ratio, 0, 'f', 2)));
        }
    }
}

void PtyProcessTest::usesProfileFixedFont() {
    ThemeManager theme(*qApp);
    TerminalSession session;
    TerminalView terminal(session);
    terminal.ensurePolished();
    QCOMPARE(QFontInfo(terminal.font()).family(),
             QFontInfo(QFont(ket::Profile::current().monoFamily())).family());
}

void PtyProcessTest::appliesTerminalTypography() {
    TerminalSession session;
    TerminalView terminal(session);
    terminal.setTypography(17, 29);
    QCOMPARE(terminal.font().pixelSize(), 17);
}

void PtyProcessTest::appliesTerminalFontFamily() {
    const QString fixedFamily =
        QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
    QString family;
    for (const QString& candidate : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(candidate) && candidate != fixedFamily &&
            QFontInfo(QFont(candidate)).family() == candidate) {
            family = candidate;
            break;
        }
    }
    if (family.isEmpty())
        QSKIP("No second fixed-pitch family is installed");

    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(800, 600);
    terminal.setTypography(16, 20, family);
    QCOMPARE(QFontInfo(terminal.font()).family(), family);
    QCOMPARE(terminal.font().pixelSize(), 16);
    const int rows = session.rows();
    const int columns = session.columns();

    // A taller line and a larger size take effect on the grid and the PTY at once.
    terminal.setTypography(24, 40, family);
    QVERIFY(session.rows() < rows);
    QVERIFY(session.columns() < columns);

    // A family this system does not have falls back to the platform's fixed-pitch font.
    terminal.setTypography(16, 20, QStringLiteral("No Such Terminal Font 7f3a"));
    QCOMPARE(QFontInfo(terminal.font()).family(), fixedFamily);
    QCOMPARE(session.rows(), rows);
}

void PtyProcessTest::keepsTerminalFontFamilyUnderTheme() {
    // The application style sheet names the platform's fixed-pitch family for this view; the
    // requested family must survive polish and a theme change.
    const QString fixedFamily =
        QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
    QString family;
    for (const QString& candidate : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(candidate) && candidate != fixedFamily &&
            QFontInfo(QFont(candidate)).family() == candidate) {
            family = candidate;
            break;
        }
    }
    if (family.isEmpty())
        QSKIP("No second fixed-pitch family is installed");

    ThemeManager theme(*qApp);
    theme.setMode(ThemeManager::Mode::Light);
    TerminalSession session;
    TerminalView terminal(session);
    terminal.resize(800, 600);
    terminal.setTypography(16, 20, family);
    terminal.show();
    terminal.ensurePolished();
    QCoreApplication::processEvents();
    QCOMPARE(QFontInfo(terminal.font()).family(), family);
    QCOMPARE(terminal.font().pixelSize(), 16);
    const int columns = session.columns();

    theme.setMode(ThemeManager::Mode::Dark);
    QCoreApplication::processEvents();
    QCOMPARE(QFontInfo(terminal.font()).family(), family);
    QCOMPARE(terminal.font().pixelSize(), 16);
    QCOMPARE(session.columns(), columns);
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

    // A slow runner's shell can still be starting after its first output; echo is what is checked.
    QTRY_VERIFY2_WITH_TIMEOUT(terminal.visibleText().contains("ketplus_live_echo"),
                              qPrintable(terminal.visibleText()), 5000);
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

void PtyProcessTest::tabStaysInTheTerminal() {
#if defined(Q_OS_UNIX)
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new TerminalPanel(&window);
    auto* after = new QLineEdit(&window);
    layout->addWidget(panel);
    layout->addWidget(after);
    window.resize(640, 320);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* terminal = panel->findChild<TerminalView*>();
    QVERIFY(terminal != nullptr);
    panel->focusTerminal();
    QTRY_COMPARE_WITH_TIMEOUT(qApp->focusWidget(), terminal, 1000);

    // Before the shell runs as well as while it runs: Tab and Shift+Tab never leave.
    QTest::keyClick(terminal, Qt::Key_Tab);
    QCOMPARE(qApp->focusWidget(), terminal);
    QTest::keyClick(terminal, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(qApp->focusWidget(), terminal);

    TerminalSession session;
    auto* running = new TerminalView(session, &window);
    layout->addWidget(running);
    QSignalSpy outputSpy(&session, &TerminalSession::outputReceived);
    running->start(QDir::tempPath());
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 10000);
    running->setFocus(Qt::OtherFocusReason);
    QTRY_COMPARE_WITH_TIMEOUT(qApp->focusWidget(), running, 1000);
    QTest::keyClick(running, Qt::Key_Tab);
    QCOMPARE(qApp->focusWidget(), running);
    QTest::keyClick(running, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(qApp->focusWidget(), running);
    // Tab may have opened the shell's completion list; stop it rather than type exit.
    session.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(), 10000);
    delete running; // It refers to `session`, which goes before `window`.
#else
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
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
    QVERIFY(waitUntilPaintsSettle(paints));
    paints.count = 0;

    for (int chunk = 0; chunk < 120; ++chunk) {
        session.feedOutput(QByteArray("x"));
    }
    // The output paints, late on a slow runner; once it settles, the chunks shared few paints.
    QVERIFY(waitUntilPaintsSettle(paints));
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
    QVERIFY(waitUntilPaintsSettle(paints));
    paints.count = 0;

    session.feedOutput(QByteArray("\x1b[?2026hplaceholder"));
    QTest::qWait(30);
    QCOMPARE(paints.count, 0);

    session.feedOutput(QByteArray("\r\x1b[2Ktyped\x1b[?2026l"));
    QTRY_VERIFY_WITH_TIMEOUT(paints.count > 0, 5000);
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

void PtyProcessTest::embeddedTabsKeepIndependentSessions() {
#if !defined(Q_OS_UNIX)
    QSKIP("The first PTY backend targets macOS and Linux.");
#endif
    TerminalPanel panel;
    panel.resize(800, 400);
    panel.show();
    panel.start(QDir::tempPath());
    auto* tabs = panel.findChild<QTabWidget*>(QStringLiteral("terminalPanelTabs"));
    QVERIFY(tabs);
    auto* first = qobject_cast<TerminalTab*>(tabs->currentWidget());
    QVERIFY(first);
    QTRY_VERIFY_WITH_TIMEOUT(first->isRunning(), 5000);
    auto* create = panel.findChild<QAbstractButton*>(QStringLiteral("newTerminalTab"));
    QVERIFY(create);
    create->click();
    QCOMPARE(tabs->count(), 2);
    auto* second = qobject_cast<TerminalTab*>(tabs->currentWidget());
    QVERIFY(second && first != second);
    QTRY_VERIFY_WITH_TIMEOUT(second->isRunning(), 5000);
    QVERIFY(first->isRunning());
    tabs->setCurrentIndex(0);
    QCOMPARE(qobject_cast<TerminalTab*>(tabs->currentWidget()), first);
    const QPointer<TerminalTab> closed = second;
    tabs->tabCloseRequested(1);
    QVERIFY(closed.isNull());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(first->isRunning());
}

void PtyProcessTest::singleSessionHasNoTabChrome() {
    // The Agent Workspace gives each terminal its own content tab; a second tab bar inside it
    // looked like two rows of tabs.
    TerminalPanel panel;
    panel.setSingleSession(true);
    panel.resize(800, 400);
    panel.show();
    auto* tabs = panel.findChild<QTabWidget*>(QStringLiteral("terminalPanelTabs"));
    QVERIFY(tabs);
    QVERIFY(!tabs->tabBar()->isVisible());
    auto* create = panel.findChild<QAbstractButton*>(QStringLiteral("newTerminalTab"));
    QVERIFY(create);
    QVERIFY(!create->isVisible());
    panel.typeTextInNewTab(QStringLiteral("echo"));
    QCOMPARE(tabs->count(), 1);
    panel.setSingleSession(false);
    QVERIFY(tabs->tabBar()->isVisible());
    QVERIFY(create->isVisible());
}

} // namespace ketplus

namespace {

#if defined(Q_OS_UNIX)
// Prints the pid of a shell that ignores SIGHUP, stops it and exits without waiting.
int runExitProbe(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    ketplus::PtyProcess process;
    QByteArray output;
    QObject::connect(&process, &ketplus::PtyProcess::outputReceived,
                     [&output](const QByteArray& bytes) { output.append(bytes); });
    if (!process.start(QDir::tempPath(), 24, 80))
        return 1;
    process.send("trap '' HUP; printf '__KETPLUS_PID_%s__\\n' \"$$\"\n");
    static const QRegularExpression pidLine(QStringLiteral("__KETPLUS_PID_([0-9]+)__"));
    QDeadlineTimer deadline(10000);
    while (!pidLine.match(QString::fromUtf8(output)).hasMatch() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto match = pidLine.match(QString::fromUtf8(output));
    if (!match.hasMatch())
        return 1;
    std::printf("%s\n", qPrintable(match.captured(1)));
    std::fflush(stdout);
    process.stop();
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_UNIX)
    if (qEnvironmentVariableIsSet("KETPLUS_PTY_EXIT_PROBE"))
        return runExitProbe(argc, argv);
#endif
#if defined(Q_OS_UNIX)
    // The shells start as login shells. A runner's own startup files can print, pause or read
    // the keyboard (zsh's new-user menu does), so the shell gets a home of its own ones. They
    // only set a short prompt: the default one holds the host name, and a runner's is long
    // enough that a typed command wraps at 80 columns and no longer reads as one line.
    QTemporaryDir home;
    if (!home.isValid())
        return 1;
    const QByteArray bashPrompt = "PS1='$ '\n";
    const QByteArray zshPrompt = "PROMPT='$ '\nRPROMPT=''\n";
    const std::pair<const char*, QByteArray> startupFiles[] = {{".zshenv", {}},
                                                               {".zprofile", {}},
                                                               {".zshrc", zshPrompt},
                                                               {".zlogin", {}},
                                                               {".bash_profile", bashPrompt},
                                                               {".bashrc", bashPrompt},
                                                               {".profile", bashPrompt}};
    for (const auto& [name, contents] : startupFiles) {
        QFile file(home.filePath(QString::fromLatin1(name)));
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
            return 1;
    }
    qputenv("HOME", home.path().toLocal8Bit());
    // macOS bash prints a "default shell is now zsh" notice first.
    qputenv("BASH_SILENCE_DEPRECATION_WARNING", "1");
    for (const char* variable : {"ZDOTDIR", "BASH_ENV", "ENV"})
        qunsetenv(variable);
#endif
    QApplication application(argc, argv);
    // As QTEST_MAIN sets it, so font metrics match the other GUI tests.
    application.setAttribute(Qt::AA_Use96Dpi, true);
    QTEST_SET_MAIN_SOURCE_PATH
    ketplus::PtyProcessTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PtyProcessTest.moc"
