#include "terminal/TerminalPanel.h"

#include "terminal/TerminalSession.h"
#include "terminal/TerminalView.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {

TerminalPanel::TerminalPanel(QWidget* parent)
    : QWidget(parent), session_(new TerminalSession(this)),
      terminal_(new TerminalView(*session_, this)),
      titleLabel_(new QLabel(QStringLiteral("TERMINAL"), this)), pathLabel_(new QLabel(this)),
      findButton_(new QToolButton(this)), clearButton_(new QToolButton(this)),
      restartButton_(new QToolButton(this)), closeButton_(new QToolButton(this)) {
    setProperty("kvRole", QStringLiteral("terminalPanel"));
    setMinimumHeight(120);
    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(terminal_);

    auto* header = new QWidget(this);
    header->setProperty("kvRole", QStringLiteral("terminalHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(11, 0, 6, 0);
    headerLayout->setSpacing(9);
    titleLabel_->setProperty("kvRole", QStringLiteral("terminalHeading"));
    pathLabel_->setProperty("kvRole", QStringLiteral("terminalPath"));
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(titleLabel_);
    headerLayout->addWidget(pathLabel_, 1);

    findButton_->setText(QString::fromUtf8("⌕"));
    findButton_->setAutoRaise(true);
    findButton_->setToolTip(QStringLiteral("Find in terminal"));
    findButton_->setAccessibleName(QStringLiteral("Find in terminal"));
    findButton_->setProperty("kvRole", QStringLiteral("terminalAction"));
    headerLayout->addWidget(findButton_);

    clearButton_->setText(QString::fromUtf8("⌫"));
    clearButton_->setAutoRaise(true);
    clearButton_->setToolTip(QStringLiteral("Clear terminal scrollback"));
    clearButton_->setAccessibleName(QStringLiteral("Clear terminal scrollback"));
    clearButton_->setProperty("kvRole", QStringLiteral("terminalAction"));
    headerLayout->addWidget(clearButton_);

    restartButton_->setText(QString::fromUtf8("↻"));
    restartButton_->setAutoRaise(true);
    restartButton_->setToolTip(QStringLiteral("Restart terminal session"));
    restartButton_->setAccessibleName(QStringLiteral("Restart terminal session"));
    restartButton_->setProperty("kvRole", QStringLiteral("terminalAction"));
    headerLayout->addWidget(restartButton_);

    closeButton_->setText(QString::fromUtf8("×"));
    closeButton_->setAutoRaise(true);
    closeButton_->setToolTip(QStringLiteral("Hide terminal"));
    closeButton_->setAccessibleName(QStringLiteral("Hide terminal"));
    closeButton_->setProperty("kvRole", QStringLiteral("terminalAction"));
    headerLayout->addWidget(closeButton_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(terminal_, 1);

    connect(closeButton_, &QToolButton::clicked, this, &TerminalPanel::closeRequested);
    connect(findButton_, &QToolButton::clicked, terminal_, &TerminalView::openSearch);
    connect(clearButton_, &QToolButton::clicked, terminal_, &TerminalView::clearScrollback);
    connect(restartButton_, &QToolButton::clicked, this, [this] {
        if (!terminal_->restart()) {
            emit statusMessageRequested(QStringLiteral("Unable to restart terminal session."));
        }
    });
    connect(terminal_, &TerminalView::statusMessageRequested, this,
            &TerminalPanel::statusMessageRequested);
    connect(terminal_, &TerminalView::titleChanged, this,
            [this](const QString& title) { titleLabel_->setToolTip(title); });
    connect(session_, &TerminalSession::started, this, [this] {
        titleLabel_->setText(QStringLiteral("TERMINAL"));
        titleLabel_->setProperty("terminalState", QStringLiteral("running"));
        titleLabel_->style()->unpolish(titleLabel_);
        titleLabel_->style()->polish(titleLabel_);
    });
    connect(session_, &TerminalSession::exited, this, [this](int) {
        titleLabel_->setText(QStringLiteral("TERMINAL · EXITED"));
        titleLabel_->setProperty("terminalState", QStringLiteral("exited"));
        titleLabel_->style()->unpolish(titleLabel_);
        titleLabel_->style()->polish(titleLabel_);
    });
}

void TerminalPanel::start(const QString& workingDirectory) {
    workingDirectory_ =
        QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    const QString homeRelative = QDir::home().relativeFilePath(workingDirectory_);
    pathLabel_->setText(homeRelative == QStringLiteral(".") ? QStringLiteral("~")
                        : homeRelative.startsWith(QStringLiteral("../"))
                            ? workingDirectory_
                            : QStringLiteral("~/%1").arg(homeRelative));
    pathLabel_->setToolTip(workingDirectory_);
    if (!session_->isRunning()) {
        terminal_->start(workingDirectory_);
    }
}

void TerminalPanel::focusTerminal() {
    terminal_->setFocus(Qt::ShortcutFocusReason);

    // QAction menus restore the previously focused widget after emitting triggered().
    // Re-apply terminal focus once that menu event has fully unwound.
    const QPointer<TerminalView> terminal = terminal_;
    QTimer::singleShot(0, terminal_, [terminal] {
        if (terminal != nullptr && terminal->isVisible()) {
            terminal->window()->activateWindow();
            terminal->setFocus(Qt::ShortcutFocusReason);
        }
    });
}

void TerminalPanel::applyTheme(const ThemePalette& palette) { terminal_->applyTheme(palette); }

void TerminalPanel::setTypography(const int fontSizePixels, const int lineHeightPixels) {
    terminal_->setTypography(fontSizePixels, lineHeightPixels);
}

bool TerminalPanel::isSessionRunning() const { return session_->isRunning(); }

} // namespace ketplus
